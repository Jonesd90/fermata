#include "AppContext.h"
#include "../core/Ravenna.h"
#include "TakeDisplay.h"
#include "TalkbackGlow.h"

namespace td
{
AppContext::AppContext()
{
    waveColors.onReady = [this] { if (waveColour) project.sendChangeMessage(); };          // a file's colours are ready: draw them
    remoteCrKey.holdMs = 1000;                     // the remote Talk key: under 1 second latches, longer is momentary
    project.newStripsToMain = true;
    juce::PropertiesFile::Options o;
    // the program used to be called TakeDAW: carry its saved settings (audio device, plug-in list, last project...) over to the new name, once
    {
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        const auto oldDir = appData.getChildFile ("TakeDAW"), newDir = appData.getChildFile ("Fermata");
        if (oldDir.isDirectory() && ! newDir.exists() && oldDir.copyDirectoryTo (newDir))
        {
            const auto oldSettings = newDir.getChildFile ("TakeDAW.settings");
            if (oldSettings.existsAsFile()) oldSettings.moveFileTo (newDir.getChildFile ("Fermata.settings"));
        }
    }
    o.applicationName = "Fermata";
    o.filenameSuffix = ".settings";
    o.folderName = "Fermata";
    o.osxLibrarySubFolder = "Application Support";
    props.setStorageParameters (o);

    project.insertFactory = plugins.makeFactory ([this] { return engine.getSampleRate(); }, [this] { return engine.getMaxBlock(); });
    project.structure.addChangeListener (this);
    devices.addChangeListener (this);
    startTimer (4000);
    project.onNewWindow = [this] { juce::MessageManager::callAsync ([this] { if (project.dirty.load() && project.projectFile != juce::File() && ! engine.isRecording()) saveNow(); }); };      // a new edit / take window / mixer gets its file at once
    watcher.fn = [this] { if (engine.playbackFinished()) stopPlayback(); };   // tidy up when a play-through reaches its end
    watcher.startTimer (100);
    undoTicker.fn = [this] { if (undoHold <= 0) project.undoTick (engine.isRecording()); };
    undoTicker.startTimer (250);
    peakPoller.fn = [this]
    {
        inPeaks.resize (project.inputs.size());   outPeaks.resize (project.outputs.size());
        for (size_t i = 0; i < inPeaks.size(); ++i)  inPeaks[i]  = engine.takeInputPeak ((int) i);     // reading resets, so it is done once, here
        for (size_t i = 0; i < outPeaks.size(); ++i) outPeaks[i] = engine.takeOutputPeak ((int) i);
        peakListeners.call ([] (PeakListener& l) { l.peaksUpdated(); });
    };
    peakPoller.startTimerHz (30);
    talkbackPoller.fn = [this]
    {
        // The two keypad keys are read straight from the keyboard, whichever of our windows has the focus (only while this program is in front).
        // The main keyboard's + and - are different keys and do nothing here.
        const bool fg = juce::Process::isForegroundProcess();
        const bool plus  = fg && juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::numberPadAdd);
        const bool minus = fg && juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::numberPadSubtract);
        const bool cr = crKey.update (plus, juce::Time::getMillisecondCounter());
        if (minus && ! minusWasDown) tbWanted = ! tbWanted;
        minusWasDown = minus;
        const bool remoteCr = remoteCrKey.update (remoteCrDown, juce::Time::getMillisecondCounter());
        crWanted = cr || remoteCr;
        applyTalkback();
    };
    talkbackPoller.startTimerHz (60);
    preampTicker.fn = [this] { preampMemoryTick(); };
    preampTicker.startTimer (1000);
    if (auto* st = props.getUserSettings())
    {
        playheadFollows = st->getBoolValue ("playheadFollows", true);
        takeDisplay.line = st->getValue ("tdLine");
        takeDisplay.logoMode = st->getIntValue ("tdLogoMode", 0);
        takeDisplay.logoPath = st->getValue ("tdLogoPath");
        takeDisplay.timerMode = st->getIntValue ("tdTimerMode", 0);
        takeDisplay.endAtText = st->getValue ("tdEndAt", "17:00");
        takeDisplay.lengthText = st->getValue ("tdLength", "03:00");
        takeDisplay.endMs = st->getValue ("tdEndMs", "0").getLargeIntValue();
    }
}

AppContext::~AppContext()
{
    *anemanAlive = false;
    preampTicker.stopTimer();
    talkbackPoller.stopTimer();
    talkbackOverlay.reset();
    engine.setCrMic (false); engine.setTalkbackPlayback (false);
    takeDisplayHub.reset();
    project.structure.removeChangeListener (this);
    devices.removeChangeListener (this);
}

void AppContext::setCrMicInput (int i, bool on)
{
    if (i < 0 || i >= (int) project.inputs.size() || project.isCrMic (i) == on) return;
    auto& v = project.crMicInputs;
    if (on) { v.push_back (i); std::sort (v.begin(), v.end()); }
    else v.erase (std::remove (v.begin(), v.end(), i), v.end());
    project.markDirty(); applyTalkback(); project.sendChangeMessage();
}

void AppContext::setTbPair (int first, bool on)
{
    auto& v = project.tbOutputs;
    const bool has = std::find (v.begin(), v.end(), first) != v.end();
    if (on == has || first < 0 || first >= (int) project.outputs.size()) return;
    if (on) { v.push_back (first); std::sort (v.begin(), v.end()); }
    else v.erase (std::remove (v.begin(), v.end(), first), v.end());
    project.markDirty(); applyTalkback(); project.sendChangeMessage();
}

void AppContext::applyTalkback()
{
    engine.setTalkbackRouting (project.crMicInputs, project.tbOutputs);
    engine.setStageMix (project.stageGainDb, project.stagePan, project.stagePlaybackDb, project.stageOutputDb);
    engine.setCrMic (crMicOpen());
    engine.setTalkbackPlayback (tbPlaybackOn());
    // (the coloured borders that used to flash round the screen are gone: Pre-Rec, C-R and P-B are shown as squares in the menu bar)
    talkbackOverlay.reset();
}

void AppContext::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &devices)     // remember the audio device choice for next launch
    {
        juce::Logger::writeToLog ("Audio device setup changed");
        syncWithDevice();
        if (auto xml = devices.createStateXml()) props.getUserSettings()->setValue ("audioDevice", xml.get());
        props.saveIfNeeded();
        return;
    }
    if (! auditionId.isNull() && (findMixer (auditionId) == nullptr || isEngineerMixer (auditionId))) setAudition ({});
    engine.rebuildPlan();
    applyTalkback();
    if (refreshTitles) refreshTitles();
}

MixerState* AppContext::findMixer (const juce::Uuid& id) const
{
    for (auto& m : project.mixers) if (m->id == id) return m.get();
    return nullptr;
}

SendResult AppContext::setSendLevel (MixerState& m, const juce::Uuid& src, const juce::Uuid& dest, float db)
{
    const auto r = project.setSendLevel (m, src, dest, db);
    if (r == SendResult::RoutingChanged) engine.rebuildPlan();
    return r;
}

void AppContext::setAudition (const juce::Uuid& id)
{
    auto* m = (id.isNull() || isEngineerMixer (id)) ? nullptr : findMixer (id);
    if (m != nullptr && ! m->editId.isNull()) m = nullptr;                          // an Edit's mixer is not a cue mixer
    auditionId = m != nullptr && auditionId != id ? id : juce::Uuid::null();     // pressing the active one again switches it off
    engine.setAuditionMixer (auditionId.isNull() ? nullptr : m);
    project.structure.sendChangeMessage();                                          // windows update their buttons
}

bool AppContext::deleteMixer (const juce::Uuid& id)
{
    if (isEngineerMixer (id)) return false;                       // the processing mixer stays
    for (int i = 1; i < project.cueEnd(); ++i)                    // (an Edit's mixer is only removed together with its Edit)
        if (project.mixers[(size_t) i]->id == id)
        {
            if (auditionId == id) { auditionId = juce::Uuid::null(); engine.setAuditionMixer (nullptr); }
            project.removeMixer (i);
            return true;
        }
    return false;
}

juce::String AppContext::copyMix (const juce::Uuid& fromId, const juce::Uuid& toId, bool withInserts)
{
    auto* from = findMixer (fromId);
    if (from == nullptr) return "No such mixer.";
    double sr = 48000.0; int block = 512;
    if (auto* d = devices.getCurrentAudioDevice()) { sr = d->getCurrentSampleRate(); block = juce::jmax (64, d->getCurrentBufferSizeSamples()); }
    const auto snap = project.mixSnapshot (*from, withInserts);
    std::vector<std::pair<juce::Uuid, juce::var>> undo;
    juce::StringArray names;
    for (auto& m : project.mixers)
    {
        if (m.get() == from || (! toId.isNull() && m->id != toId)) continue;
        undo.push_back ({ m->id, project.mixSnapshot (*m, withInserts) });
        project.applyMix (*m, snap, sr, block);
        names.add (m->name);
    }
    if (undo.empty()) return "Nothing to copy to.";
    mixUndo = std::move (undo);
    project.structure.sendChangeMessage();                                          // strips redraw with the new values
    return "Copied the mix of \"" + from->name + "\" onto " + names.joinIntoString (", ") + ".";
}

juce::String AppContext::undoCopyMix()
{
    if (mixUndo.empty()) return "Nothing to undo.";
    double sr = 48000.0; int block = 512;
    if (auto* d = devices.getCurrentAudioDevice()) { sr = d->getCurrentSampleRate(); block = juce::jmax (64, d->getCurrentBufferSizeSamples()); }
    juce::StringArray names;
    for (auto& [id, snap] : mixUndo)
        if (auto* m = findMixer (id)) { project.applyMix (*m, snap, sr, block); names.add (m->name); }
    mixUndo.clear();
    project.structure.sendChangeMessage();
    return "Restored " + names.joinIntoString (", ") + " to how it was before the copy.";
}

void AppContext::syncWithDevice()
{
    auto* dev = devices.getCurrentAudioDevice();
    if (dev == nullptr) return;
    const auto ins = dev->getInputChannelNames(), outs = dev->getOutputChannelNames();

    // the DAW always uses every channel the driver offers, so input N here is channel N of the driver
    auto setup = devices.getAudioDeviceSetup();
    juce::BigInteger allIn, allOut;
    allIn.setRange (0, ins.size(), true); allOut.setRange (0, outs.size(), true);
    if (setup.useDefaultInputChannels || setup.useDefaultOutputChannels || setup.inputChannels != allIn || setup.outputChannels != allOut)
    {
        setup.useDefaultInputChannels = false; setup.useDefaultOutputChannels = false;
        setup.inputChannels = allIn; setup.outputChannels = allOut;
        juce::Logger::writeToLog ("Enabling all " + juce::String (ins.size()) + " inputs and " + juce::String (outs.size()) + " outputs of the driver");
        auto err = devices.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty()) juce::Logger::writeToLog ("Could not enable all channels: " + err);
        return;     // the device restarts, which calls us again
    }
    project.syncWithDevice (ins, outs);
}

void AppContext::applyAudioSettings()
{
    juce::Logger::writeToLog ("Audio settings applied");
    syncWithDevice();
    if (auto xml = devices.createStateXml()) props.getUserSettings()->setValue ("audioDevice", xml.get());
    props.saveIfNeeded();
    engine.rebuildPlan();
    if (refreshTitles) refreshTitles();
}

void AppContext::timerCallback()
{
    if (project.dirty.load() && project.projectFile != juce::File() && ! engine.isRecording())
        saveNow();
}

void AppContext::undoStep()
{
    const auto live = engine.isRecording() ? engine.currentTakeId() : juce::Uuid::null();
    if (project.undo (live)) setNotice ("Undone (" + juce::String (project.undoDepth()) + " more step" + (project.undoDepth() == 1 ? "" : "s") + " back; Ctrl+Y puts it back)");
    else setNotice ("Nothing to undo");
    if (refreshTitles) refreshTitles();
}

void AppContext::redoStep()
{
    const auto live = engine.isRecording() ? engine.currentTakeId() : juce::Uuid::null();
    if (project.redo (live)) setNotice ("Redone (" + juce::String (project.redoDepth()) + " more to redo)");
    else setNotice ("Nothing to redo");
    if (refreshTitles) refreshTitles();
}

void AppContext::saveNow()
{
    juce::String err;
    if (project.projectFile != juce::File())
    {
        if (! project.save (err)) showError ("Could not save project", err);
        props.getUserSettings()->setValue ("lastProject", project.projectFile.getFullPathName());
        props.saveIfNeeded();
    }
}

void AppContext::afterProjectReplaced()
{
    syncWithDevice();
    engine.rebuildPlan();
    recordTarget = project.takeWindows.empty() ? juce::Uuid::null() : project.takeWindows.front()->id;
    if (project.preampDevices.empty())                 // a new project: use the devices of the rig, as last set up
    {
        auto v = juce::JSON::parse (props.getUserSettings()->getValue ("preampDevices"));
        if (auto* a = v.getArray())
            for (auto& d : *a) project.preampDevices.push_back (PreampDeviceCfg::fromVar (d));
    }
    project.undoReset();
    rebuildPreampDriver();
    refreshPreampsFromAneman();
    preampChecked = false; preampAsking = false; preampWaitTicks = 0; haveWrittenPreamps = false;     // compare with the stored state once the devices have answered
    if (refreshTitles) refreshTitles();
}

juce::File AppContext::preampMemoryFile() const
{
    if (project.projectFile == juce::File()) return {};
    return project.projectFile.getSiblingFile (project.projectFile.getFileNameWithoutExtension() + " - preamp memory.json");
}

void AppContext::preampMemoryTick()
{
    auto* d = dynamic_cast<RavennaPreampDriver*> (project.preampDriver.get());
    const auto file = preampMemoryFile();
    if (d == nullptr || file == juce::File() || preampAsking) return;

    if (! preampChecked)
    {
        // wait (up to about 12 s) for the devices to be read before comparing
        bool all = true;
        for (auto& inf : d->info()) all = all && inf.connected;
        if (! all && ++preampWaitTicks < 12) return;
        preampChecked = true;
        PreampMemory stored;
        if (PreampMemory::load (file, stored) && ! stored.inputs.empty())
        {
            std::vector<int> differing;
            for (auto& [i, s] : stored.inputs) { PreampSettings cur; if (d->read (i, cur) && ! PreampMemory::same (cur, s)) differing.push_back (i); }
            if (! differing.empty()) { askRestorePreamps (stored, differing); return; }
        }
    }

    // the current full state of every preamp the devices can report (an input that cannot be read keeps its old entry)
    PreampMemory now = haveWrittenPreamps ? lastWrittenPreamps : PreampMemory();
    if (! haveWrittenPreamps) PreampMemory::load (file, now);
    bool any = false;
    for (int i = 0; i < (int) project.inputs.size(); ++i) { PreampSettings s; if (d->read (i, s)) { now.inputs[i] = s; any = true; } }
    if (! any) return;
    bool changed = ! haveWrittenPreamps || now.inputs.size() != lastWrittenPreamps.inputs.size() || ! file.existsAsFile();
    if (! changed) for (auto& [i, s] : now.inputs) { auto it = lastWrittenPreamps.inputs.find (i); if (it == lastWrittenPreamps.inputs.end() || ! PreampMemory::same (it->second, s)) { changed = true; break; } }
    if (changed && now.save (file)) { lastWrittenPreamps = now; haveWrittenPreamps = true; }
    else if (! changed) { lastWrittenPreamps = now; haveWrittenPreamps = true; }
}

void AppContext::askRestorePreamps (const PreampMemory& stored, const std::vector<int>& differing)
{
    preampAsking = true;                                   // nothing is written until the choice has been made, so the stored state is not lost
    auto* w = new juce::AlertWindow ("Preamp settings",
                                     "Keep preamp settings or load stored preamp settings from last time this project was used?\n\n(" + juce::String ((int) differing.size())
                                         + (differing.size() == 1 ? " input differs.)" : " inputs differ.)"),
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addComboBox ("choice", { "Keep the preamp settings as they are now", "Load the stored preamp settings from last time this project was used" });
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Close (keep as they are)", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<juce::AlertWindow> safe (w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, safe, stored, differing] (int result)
    {
        const bool load = result == 1 && safe != nullptr && safe->getComboBoxComponent ("choice") != nullptr && safe->getComboBoxComponent ("choice")->getSelectedItemIndex() == 1;
        if (load)
            for (int i : differing) { auto it = stored.inputs.find (i); if (it != stored.inputs.end()) project.setPreamp (i, it->second); }
        preampAsking = false; haveWrittenPreamps = false;
    }), true);
}

void AppContext::rebuildPreampDriver()
{
    project.preampDriver = std::make_shared<NullPreampDriver>();      // stops the old connections first
    if (project.preampDevices.empty()) return;
    auto d = std::make_shared<RavennaPreampDriver> (project.preampDevices);
    d->onHardware = [this] (int input, const PreampSettings& s) { project.adoptPreamp (input, s); };
    project.preampDriver = d;
}

void AppContext::setPreampDevices (const std::vector<PreampDeviceCfg>& devs, bool chosenByUser)
{
    project.preampDevices = devs;
    juce::Array<juce::var> arr;
    for (auto& d : devs)
    {
        arr.add (d.toVar());
    }
    props.getUserSettings()->setValue ("preampDevices", juce::JSON::toString (juce::var (arr), true));
    props.saveIfNeeded();
    project.changed();
    rebuildPreampDriver();
    if (chosenByUser) preampChecked = true;                // the devices were just chosen by the user: nothing to compare with
}

void AppContext::refreshPreampsFromAneman()
{
    auto alive = anemanAlive;
    const auto preferred = props.getUserSettings()->getValue ("anemanHost");
    juce::Thread::launch ([this, alive, preferred]
    {
        auto plan = fetchAnemanPlan (preferred);
        if (! plan.ok) return;
        juce::MessageManager::callAsync ([this, alive, plan]
        {
            if (! *alive) return;
            props.getUserSettings()->setValue ("anemanHost", plan.source); props.saveIfNeeded();
            if (plan.devices != project.preampDevices) setPreampDevices (plan.devices, false);
        });
    });
}

bool AppContext::newProject (const juce::File& file)
{
    if (engine.isRecording()) { showError ("Recording", "Stop recording first."); return false; }
    stopPlayback();
    saveNow();
    auto insertFactory = project.insertFactory;
    project.createDefaultDesign();
    project.name = file.getFileNameWithoutExtension();
    project.projectFile = file;
    juce::String err;
    if (! project.save (err)) { showError ("Could not create project", err); return false; }
    project.createFolders();                         // Recorded Media, Bounced Media, Bounced Media/Mastered Audio
    afterProjectReplaced();
    props.getUserSettings()->setValue ("lastProject", file.getFullPathName());
    return true;
}

bool AppContext::openProject (const juce::File& file)
{
    if (engine.isRecording()) { showError ("Recording", "Stop recording first."); return false; }
    stopPlayback();
    saveNow();
    juce::String err;
    if (! project.load (file, err)) { showError ("Could not open project", err); return false; }
    project.createFolders();
    if (! project.loadNotes.isEmpty())
    {
        juce::String text = "Some edits or take windows could not be read, because their files are missing or damaged. Nothing was deleted: put the files back in the project folder and open the project again.\n\n";
        for (auto& l : project.loadNotes) text << "   " << l << "\n";
        juce::MessageManager::callAsync ([text] { juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Files missing").withMessage (text).withButton ("OK"), nullptr); });
    }
    const auto mended = project.repairInterruptedRecordings();      // the program or the computer stopped while recording: keep everything that was recorded
    if (mended.size() > 0)
    {
        saveNow();
        juce::String text = "The program stopped while this project was recording, but nothing recorded was lost. The recordings were put right and are in the project:\n\n";
        for (auto& l : mended) text << "   " << l << "\n";
        juce::MessageManager::callAsync ([text] { juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle ("Recording recovered").withMessage (text).withButton ("OK"), nullptr); });
    }
    afterProjectReplaced();
    props.getUserSettings()->setValue ("lastProject", file.getFullPathName());
    return true;
}

void AppContext::deleteWindowAndFile (bool isEdit, const juce::Uuid& id)
{
    if (engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
    stopPlayback();
    if (closeWindowByKey)
    {
        const auto key = id.toString();
        if (isEdit) { closeWindowByKey ("edit:" + key); closeWindowByKey ("trim:" + key); closeWindowByKey ("bounce:" + key); }
        else        { closeWindowByKey ("take:" + key); closeWindowByKey ("bounce:" + key); }
    }
    juce::MessageManager::callAsync ([this, isEdit, id]                // after the windows are gone (they close in the same queue, just before this)
    {
        if (isEdit) project.deleteEditAndFile (id);
        else
        {
            project.deleteTakeWindowAndFile (id);
            if (recordTarget == id) recordTarget = project.takeWindows.empty() ? juce::Uuid::null() : project.takeWindows.front()->id;
        }
        saveNow();
    });
}

void AppContext::startBlankProject()
{
    project.windowFiles.clear(); project.missingWindowRefs.clear();
    project.createDefaultDesign();
    project.projectFile = juce::File();
    project.name = "(no project yet)";
    afterProjectReplaced();
}

juce::File AppContext::lastProjectFile()
{
    juce::File last (props.getUserSettings()->getValue ("lastProject"));
    return last.existsAsFile() ? last : juce::File();
}

void AppContext::restoreLastProject()
{
    juce::File last (props.getUserSettings()->getValue ("lastProject"));
    if (last.existsAsFile() && openProject (last)) return;

    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata")
                      .getChildFile ("Session " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M"));
    newProject (folder.getChildFile ("Session.fermata"));
}

void AppContext::setSessionMode (bool on)
{
    engine.setSessionMode (on);
    if (auto* st = props.getUserSettings()) { st->setValue ("sessionMode", on); props.saveIfNeeded(); }
    project.sendChangeMessage();
}

void AppContext::saveTakeDisplay()
{
    if (auto* st = props.getUserSettings())
    {
        st->setValue ("tdLine", takeDisplay.line);
        st->setValue ("tdLogoMode", takeDisplay.logoMode);
        st->setValue ("tdLogoPath", takeDisplay.logoPath);
        st->setValue ("tdTimerMode", takeDisplay.timerMode);
        st->setValue ("tdEndAt", takeDisplay.endAtText);
        st->setValue ("tdLength", takeDisplay.lengthText);
        st->setValue ("tdEndMs", juce::String (takeDisplay.endMs));
        st->saveIfNeeded();
    }
}

juce::String AppContext::setSessionEndAt (const juce::String& hhmm)
{
    int h = 0, m = 0;
    if (! sessionclock::parseHHMM (hhmm, h, m, false)) return "Type the time the session ends as HH:MM, for example 17:30";
    takeDisplay.endAtText = juce::String::formatted ("%02d:%02d", h, m);
    takeDisplay.endMs = sessionclock::endAtMs (juce::Time::currentTimeMillis(), h, m);
    saveTakeDisplay();
    return {};
}

juce::String AppContext::setSessionLength (const juce::String& hhmm)
{
    int h = 0, m = 0;
    if (! sessionclock::parseHHMM (hhmm, h, m, true) || (h == 0 && m == 0)) return "Type the length of the session as HH:MM, for example 03:00";
    takeDisplay.lengthText = juce::String::formatted ("%02d:%02d", h, m);
    takeDisplay.endMs = juce::Time::currentTimeMillis() + ((juce::int64) h * 3600 + (juce::int64) m * 60) * 1000;
    saveTakeDisplay();
    return {};
}

void AppContext::clearSessionTimer() { takeDisplay.endMs = 0; saveTakeDisplay(); }

bool AppContext::sessionSecondsLeft (juce::int64& seconds) const
{
    if (takeDisplay.endMs <= 0) return false;
    seconds = sessionclock::remainingSeconds (takeDisplay.endMs, juce::Time::currentTimeMillis());
    return true;
}

bool AppContext::takeBoxInfo (int& number)
{
    number = 1;
    if (engine.isRecording())
    {
        for (auto& w : project.takeWindows)
            if (auto* g = w->findGroup (engine.currentTakeId())) { number = g->number; return true; }
        return true;
    }
    auto* w = project.findTakeWindow (recordTarget);
    if (w == nullptr && ! project.takeWindows.empty()) w = project.takeWindows.front().get();
    if (w != nullptr) number = w->nextNumber;
    return false;
}

void AppContext::remoteTalk (bool down)
{
    remoteCrDown = down;
    remoteCrKey.update (down, juce::Time::getMillisecondCounter());          // at once, so even a very short tap is never missed between two ticks
}

void AppContext::remoteRecord()
{
    if (engine.isRecording()) { toggleRecord (juce::Uuid::null()); return; }
    auto id = recordTarget;
    if (project.findTakeWindow (id) == nullptr) id = project.takeWindows.empty() ? juce::Uuid::null() : project.takeWindows.front()->id;
    if (id.isNull()) { setNotice ("Make a take window first (Takes menu), then Record works from the Stream Deck."); return; }
    toggleRecord (id);
}

void AppContext::remotePlayPause()
{
    if (isPlaying()) { stopPlayback(); return; }
    if (engine.isRecording()) return;
    if (transportHandler) { transportHandler(); return; }
    auto* w = project.findTakeWindow (recordTarget);
    if (w == nullptr && ! project.takeWindows.empty()) w = project.takeWindows.front().get();
    if (w == nullptr) { setNotice ("Nothing to play: click in a take or edit window first."); return; }
    const auto err = playTakeWindow (w->id, w->playheadSeconds);
    if (err.isNotEmpty()) setNotice (err);
}

void AppContext::toggleRecord (const juce::Uuid& takeWindowId)
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        saveNow();
        return;
    }
    if (! hasProject()) { setNotice ("First choose where the project is saved (New project or Open project)."); return; }
    stopPlayback();
    auto* w = project.findTakeWindow (takeWindowId);
    if (w == nullptr) { showError ("Record", "Choose a take window to record into."); return; }
    recordTarget = takeWindowId;
    auto err = engine.startRecording (*w);
    if (err.isNotEmpty()) showError ("Cannot record", err);
    else saveNow();   // write the project at once so the new take is never lost
}

// ---------------------------------------------------------------------------- playback
/** How far an open-ended play (a transport with nothing left to play) carries on before it stops by itself. */
static constexpr double kOpenEndSeconds = 6.0 * 3600.0;

static juce::String beginSession (AppContext& app, std::vector<PlaySegment> segs, juce::int64 start, juce::int64 end, double rate,
                                  std::shared_ptr<AutomationPlan> automation = nullptr, bool loop = false, bool allowEmpty = false,
                                  const juce::Uuid& editId = juce::Uuid::null())
{
    if (segs.empty() && ! allowEmpty) return "There is nothing to play here";
    if (end <= start) return "Nothing to play (empty range)";
    std::vector<int> counts;
    for (auto& t : app.project.tracks) counts.push_back (t.channelCount());
    auto session = std::make_unique<PlaybackSession> (counts, std::move (segs), start, end, rate, app.engine.getMaxBlock());
    session->automation = std::move (automation);
    session->editId = editId;                                  // an Edit plays through its own mixer
    session->setLooping (loop);
    return app.engine.startPlayback (std::move (session));
}

/** Playing from the middle of something (or to a mark) would start and stop the sound abruptly: a 1 ms fade in at the start and out at the end of what is played. */
static void transportFades (std::vector<PlaySegment>& segs, juce::int64 a, juce::int64 b, double rate)
{
    const auto len = (juce::int64) std::llround (0.001 * rate);                   // (always 1 ms: just enough to avoid a click when starting / stopping)
    if (len <= 0) return;
    for (auto& s : segs)
    {
        if (s.begin < a && s.end > a && (s.fadeInLen == 0 || s.fadeInStart + s.fadeInLen <= a)) { s.fadeInStart = a; s.fadeInLen = len; }
        if (s.end > b && s.begin < b && b - len > a && (s.fadeOutLen == 0 || s.fadeOutStart >= b - len)) { s.fadeOutStart = b - len; s.fadeOutLen = len; }
    }
}

juce::String AppContext::playTakeWindow (const juce::Uuid& windowId, double fromSec)
{
    auto* w = project.findTakeWindow (windowId);
    if (w == nullptr) return "That take window no longer exists";
    double rate = engine.getSampleRate();
    for (auto& g : w->groups) if (g.sampleRate > 0.0) { rate = g.sampleRate; break; }
    if (rate <= 0.0) rate = 48000.0;
    std::vector<PlaySegment> segs;
    for (auto& g : w->groups)
    {
        if (std::abs (g.sampleRate - rate) > 0.5) continue;                        // another sample rate cannot be mixed in
        for (auto s : segmentsForTake (project, g, (juce::int64) std::llround (g.startSeconds * rate))) segs.push_back (std::move (s));
    }
    const juce::int64 a = (juce::int64) std::llround (juce::jmax (0.0, fromSec) * rate);
    const juce::int64 b = a + (juce::int64) std::llround (kOpenEndSeconds * rate);
    transportFades (segs, a, b, rate);
    auto err = beginSession (*this, std::move (segs), a, b, rate, nullptr, false, true);
    if (err.isEmpty()) { playInfo = {}; playInfo.kind = PlayInfo::Kind::Take; playInfo.windowId = windowId; playInfo.startSeconds = (double) a / rate; playInfo.windowTimeline = true; }
    return err;
}

juce::String AppContext::playTake (const juce::Uuid& windowId, const juce::Uuid& takeId, double fromSec, double toSec, bool loop)
{
    auto* g = project.findTake (windowId, takeId);
    if (g == nullptr) return "That take no longer exists";
    const auto rate = g->sampleRate;
    const juce::int64 a = juce::jlimit ((juce::int64) 0, g->lengthSamples, (juce::int64) (fromSec * rate));
    const juce::int64 b = toSec < 0 ? g->lengthSamples : juce::jlimit (a, g->lengthSamples, (juce::int64) (toSec * rate));
    auto segs = segmentsForTake (project, *g);
    transportFades (segs, a, b, rate);
    loop = loop && toSec >= 0 && b > a;
    auto err = beginSession (*this, std::move (segs), a, b, rate, nullptr, loop);
    if (err.isEmpty()) { playInfo = { PlayInfo::Kind::Take, windowId, takeId, (double) a / rate }; playInfo.loopLen = loop ? (double) (b - a) / rate : 0.0; }
    return err;
}

juce::String AppContext::playEdit (const juce::Uuid& editId, double fromSec, double toSec, bool loop)
{
    auto* e = project.findEdit (editId);
    if (e == nullptr) return "That edit no longer exists";
    // the edit may be empty, and you can start anywhere on its timeline, also after its last piece: with no end given it simply carries on (silent where there is no audio)
    double rate = e->sampleRate > 0.0 ? e->sampleRate : engine.getSampleRate();
    if (rate <= 0.0) rate = 48000.0;
    const juce::int64 a = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (fromSec * rate));
    const juce::int64 b = toSec < 0 ? a + (juce::int64) std::llround (kOpenEndSeconds * rate) : juce::jmax (a, (juce::int64) std::llround (toSec * rate));
    auto segs = segmentsForEdit (project, *e);
    transportFades (segs, a, b, rate);
    project.resolveAllAutomation();
    loop = loop && toSec >= 0 && b > a;
    auto err = beginSession (*this, std::move (segs), a, b, rate, automationPlanFor (project, *e), loop, true, editId);
    if (err.isEmpty()) { playInfo = { PlayInfo::Kind::Edit, e->windowId, editId, (double) a / rate }; playInfo.loopLen = loop ? (double) (b - a) / rate : 0.0; }
    return err;
}

juce::String AppContext::playDisc (const std::vector<DiscPlayItem>& items, double fromSec, double toSec, const juce::Uuid& token)
{
    double rate = 0.0; juce::int64 endAll = 0;
    std::vector<PlaySegment> segs;
    auto plan = std::make_shared<AutomationPlan>();
    project.resolveAllAutomation();
    int used = 0;
    for (auto& it : items)
    {
        auto* e = project.findEdit (it.editId);
        if (e == nullptr || e->isEmpty()) continue;
        if (rate <= 0.0) rate = e->sampleRate; else if (std::abs (e->sampleRate - rate) > 0.5) continue;        // an edit at another sample rate cannot be played together with these
        const juce::int64 D = (juce::int64) std::llround (it.startSeconds * rate);
        for (auto s : segmentsForEdit (project, *e))
        {
            s.srcOffset -= D; s.begin += D; s.end += D;
            if (s.fadeInLen > 0) s.fadeInStart += D;
            if (s.fadeOutLen > 0) s.fadeOutStart += D;
            for (auto& g : s.gainSteps) g.at += D;
            segs.push_back (std::move (s));
        }
        endAll = juce::jmax (endAll, D + e->lengthSamples());
        ++used;
        if (auto p = automationPlanFor (project, *e, true))      // the disc preview plays through the processing mixer, so the automation moves that one
            for (auto lane : p->items)
            {
                if (lane.lane.pts.empty()) continue;
                for (auto& pt : lane.lane.pts) pt.time += D;
                AutomationPlan::Item* same = nullptr;
                for (auto& q : plan->items) if (q.target == lane.target && q.slot == lane.slot && q.paramIndex == lane.paramIndex) same = &q;
                if (same == nullptr) { plan->items.push_back (std::move (lane)); continue; }
                const auto firstT = lane.lane.pts.front().time;                                             // a step between the clips: hold the old value until the new clip starts
                if (same->lane.pts.back().time < firstT - 1) { auto hold = same->lane.pts.back(); hold.time = firstT - 1; same->lane.pts.push_back (hold); }
                for (auto& pt : lane.lane.pts) same->lane.pts.push_back (pt);
            }
    }
    if (used == 0 || rate <= 0.0) return "There is nothing to play (the clips are empty)";
    const juce::int64 a = juce::jlimit ((juce::int64) 0, endAll, (juce::int64) std::llround (fromSec * rate));
    const juce::int64 b = toSec < 0 ? endAll : juce::jlimit (a, endAll, (juce::int64) std::llround (toSec * rate));
    transportFades (segs, a, b, rate);
    auto err = beginSession (*this, std::move (segs), a, b, rate, plan->items.empty() ? nullptr : plan);
    if (err.isEmpty()) playInfo = { PlayInfo::Kind::Other, juce::Uuid::null(), token, (double) a / rate };
    return err;
}

juce::String AppContext::playRenders (const std::vector<RenderPlayItem>& items, double fromSec, double toSec, const juce::Uuid& token, int outFirst)
{
    double rate = 0.0; juce::int64 endAll = 0;
    std::vector<PlaySegment> segs;
    for (auto& it : items)
    {
        if (! it.file.existsAsFile() || it.frames <= 0 || it.rate <= 0.0) continue;
        if (rate <= 0.0) rate = it.rate; else if (std::abs (it.rate - rate) > 0.5) continue;               // another sample rate cannot be played together with these
        const juce::int64 D = (juce::int64) std::llround (it.startSeconds * rate);
        PlaySegment s;
        s.trackIndex = 0; s.file = it.file; s.srcOffset = -D; s.begin = D; s.end = D + it.frames;
        segs.push_back (std::move (s));
        endAll = juce::jmax (endAll, D + it.frames);
    }
    if (segs.empty() || rate <= 0.0) return "There is nothing to play (the renders are not ready)";
    const juce::int64 a = juce::jlimit ((juce::int64) 0, endAll, (juce::int64) std::llround (fromSec * rate));
    const juce::int64 b = toSec < 0 ? endAll : juce::jlimit (a, endAll, (juce::int64) std::llround (toSec * rate));
    if (b <= a) return "Nothing to play (empty range)";
    transportFades (segs, a, b, rate);
    auto session = std::make_unique<PlaybackSession> (std::vector<int> { 2 }, std::move (segs), a, b, rate, engine.getMaxBlock());
    session->directOut = juce::jmax (0, outFirst);
    auto err = engine.startPlayback (std::move (session));
    if (err.isEmpty()) playInfo = { PlayInfo::Kind::Other, juce::Uuid::null(), token, (double) a / rate };
    return err;
}

juce::String AppContext::playRegionOriginal (const juce::Uuid& editId, const juce::Uuid& regionId, juce::int64 fromSample, juce::int64 toSample)
{
    auto* e = project.findEdit (editId);
    if (e == nullptr) return "That edit no longer exists";
    auto* r = e->find (regionId);
    if (r == nullptr) return "That piece no longer exists";
    const auto limit = r->sourceLength > 0 ? r->sourceLength : r->srcOut + (juce::int64) (60.0 * r->sampleRate);
    const juce::int64 a = juce::jlimit ((juce::int64) 0, limit, fromSample);
    const juce::int64 b = juce::jlimit (a, limit, toSample);
    auto err = beginSession (*this, segmentsForRegionOriginal (project, *r), a, b, r->sampleRate, nullptr, false, false, editId);
    if (err.isEmpty()) playInfo = { PlayInfo::Kind::Other, juce::Uuid::null(), regionId, (double) a / r->sampleRate };
    return err;
}

void AppContext::setPlayheadFollows (bool on)
{
    playheadFollows = on;
    if (auto* st = props.getUserSettings()) { st->setValue ("playheadFollows", on); props.saveIfNeeded(); }
    project.sendChangeMessage();                       // windows redraw their button
}

void AppContext::stopPlayback()
{
    if (playheadFollows && engine.isPlaying() && ! engine.playbackFinished() && ! playInfo.noFollow)      // stopped by hand: the playhead stays where it got to
    {
        const double pos = playInfo.startSeconds + playedSeconds();
        if (playInfo.kind == PlayInfo::Kind::Take)
        {
            if (auto* w = project.findTakeWindow (playInfo.windowId))
            {
                if (playInfo.windowTimeline) { w->setPlayhead (pos); project.sendChangeMessage(); }
                else if (auto* g = w->findGroup (playInfo.id)) { w->setPlayhead (g->startSeconds + pos); project.sendChangeMessage(); }
            }
        }
        else if (playInfo.kind == PlayInfo::Kind::Edit)
        {
            if (auto* e = project.findEdit (playInfo.id)) { e->playheadSeconds = pos; project.sendChangeMessage(); }
        }
    }
    engine.stopPlayback();
    playInfo = {};
}

double AppContext::playheadSeconds() const
{
    if (! engine.isPlaying() || playInfo.kind == PlayInfo::Kind::None) return -1.0;
    return playInfo.startSeconds + playedSeconds();
}

double AppContext::playedSeconds() const
{
    const double played = engine.playbackSeconds();
    return playInfo.loopLen > 0.0 ? std::fmod (played, playInfo.loopLen) : played;      // in a loop the playhead goes round and round
}

juce::String AppContext::copyMarkedToEdit (const juce::Uuid& windowId, bool atEditPlayhead, juce::Uuid* editIdOut)
{
    auto* w = project.findTakeWindow (windowId);
    if (w == nullptr) return "No take window";
    auto* g = w->findGroup (w->editTake);
    if (g == nullptr || w->editIn < 0.0 || w->editOut <= w->editIn)
        return "Press 1 (edit IN) and 2 (edit OUT) in the same take first.";

    const double rate = g->sampleRate;
    EditRegion r;
    r.windowId = windowId; r.takeId = g->id; r.takeName = w->displayName (*g); r.sampleRate = rate; r.barIn = g->barIn; r.barOut = g->barOut;
    r.srcIn  = juce::jlimit ((juce::int64) 0, g->lengthSamples, (juce::int64) std::llround (w->editIn * rate));
    r.srcOut = juce::jlimit ((juce::int64) 0, g->lengthSamples, (juce::int64) std::llround (w->editOut * rate));
    if (r.srcOut - r.srcIn < (juce::int64) (0.001 * rate)) return "The marked part is too short.";

    EditDef* e = project.findEdit (w->targetEdit);
    if (e == nullptr) { e = &project.addEdit (w->name + " - edit", windowId); w->targetEdit = e->id; }
    if (! e->isEmpty() && std::abs (e->sampleRate - rate) > 0.5)
        return "This take is " + juce::String (rate, 0) + " Hz but the edit is " + juce::String (e->sampleRate, 0) + " Hz.";

    // the edit keeps its own list of files, so it keeps working even if the take is later removed from the take window
    for (auto& f : g->files) r.files.push_back ({ f.trackId, f.trackName, f.file, f.numChannels });
    r.sourceLength = g->lengthSamples;
    const juce::int64 at = (juce::int64) std::llround (juce::jmax (0.0, e->playheadSeconds) * rate);
    if (w->overdub)             e->addOverdub (r, at);
    else if (atEditPlayhead)    e->insertRegionAt (r, at);
    else                        e->insertRegion (r);
    // the edit may have its own track order: every track of the take goes to ITS row there (by identity, never by the row it had in the take window),
    // and a track the edit does not list yet is added at the bottom, so no audio is ever hidden or put on the wrong track
    if (! e->trackIds.empty())
        for (auto& f : g->files)
            if (std::find (e->trackIds.begin(), e->trackIds.end(), f.trackId) == e->trackIds.end()) e->trackIds.push_back (f.trackId);
    w->editTake = juce::Uuid::null(); w->editIn = w->editOut = -1.0;
    project.changed();
    if (editIdOut) *editIdOut = e->id;
    return {};
}

EditDef& AppContext::makeEdit (const juce::String& name)
{
    auto& e = project.addEdit (name, juce::Uuid::null());
    project.changed();
    return e;
}

void AppContext::shutdown()
{
    takeDisplayHub.reset();
    if (engine.isRecording()) engine.stopRecording();
    stopPlayback();
    project.preampDriver = std::make_shared<NullPreampDriver>();
    plugins.closeAllEditors();
    if (! skipFinalSave) saveNow();
    if (auto* s = props.getUserSettings())
        if (auto xml = devices.createStateXml()) { s->setValue ("audioDevice", xml.get()); }
    props.saveIfNeeded();
    devices.removeAudioCallback (&engine);
    devices.closeAudioDevice();
}
} // namespace td
