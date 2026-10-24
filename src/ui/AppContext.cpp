#include "AppContext.h"
#include "../core/FileSync.h"
#include "../core/Ravenna.h"
#include "TakeDisplay.h"
#include "TalkbackGlow.h"
#include "BusyWindow.h"
#include "TimeReadout.h"

namespace td
{
AppContext::AppContext()
{
    waveColors.folder = [this] { return project.projectFolder().getChildFile ("Waveforms"); };
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
    saver = std::make_unique<BackgroundSaver> ([this, alive = saverAlive]
    {
        juce::MessageManager::callAsync ([this, alive] { if (*alive && saver != nullptr) saver->drain(); });     // (what it wrote is taken in on this thread)
    });
    project.settleBackgroundSaves = [this] { waitForSaves(); };
    stallWatch = std::make_unique<StallWatch>();
    timereadout::shared().fromString (props.getUserSettings()->getValue ("timeReadout"));       // how the time readouts write the time (remembered on this computer)
    timereadout::shared().addChangeListener (this);
    project.structure.addChangeListener (this);
    devices.addChangeListener (this);
    startTimer (4000);
    project.onNewWindow = [this] { juce::MessageManager::callAsync ([this] { if (saveHold == 0 && project.dirty.load() && project.projectFile != juce::File() && ! engine.isRecording()) saveNow (true); }); };      // a new edit / take window / mixer gets its file at once
    watcher.fn = [this]
    {
        if (engine.playbackFinished()) stopPlayback (true);                  // tidy up when a play-through reaches its end
        engine.servicePreRoll();                                             // Pre-Rec's memory, made in the background, goes in place
        if (const auto done = engine.reapFinishedTakes(); ! done.empty())     // a stopped take's files are finished on the disk: draw their real waveforms
        {
            forgetWaveforms (done);
            project.sendChangeMessage();
        }
        if (++alignTicks % 3 == 0 && ! engine.isPlaying() && ! engine.isRecording())   // idle: the live inputs are aligned for the piece being worked on
            setAlignOwner (currentAlignOwner());
        if (engine.isPlaying() && ! discAlign.empty() && playInfo.kind == PlayInfo::Kind::Other && playInfo.id == discToken)
        {                                                                    // a disc through the mixers: each piece with its own spot delays, switched in the pause before it
            const double pos = playInfo.startSeconds + playedSeconds();
            for (auto& d : discAlign) if (pos < d.end) { setAlignOwner (d.owner); break; }
        }
    };
    watcher.startTimer (100);
    loadTicker.fn = [this] { tickLoad(); };
    loadTicker.startTimer (1000);
    undoTicker.fn = [this] { if (undoHold <= 0) { BusyLabel busy ("making an undo step"); project.undoTick (engine.isRecording()); } };
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
        crWanted = cr || remoteCr || crClickLatch;
        applyTalkback();
        pollTransportKeys();
    };
    spaceBelongsToRecordingHook() = [this] { return engine.isRecording() || spaceHeldAfterStop; };
    talkbackPoller.startTimerHz (60);
    preampTicker.fn = [this] { preampMemoryTick(); };
    preampTicker.startTimer (1000);
    if (auto* st = props.getUserSettings())
    {
        playheadFollows = st->getBoolValue ("playheadFollows", true);
        trimWaveZoom = juce::jlimit (0.25f, 40.0f, (float) st->getDoubleValue ("trimWaveZoom", 1.0));
        trimSlipLeft = st->getBoolValue ("trimSlipLeft", true);
        trimSlipRight = st->getBoolValue ("trimSlipRight", true);
        showInsertFlag = st->getBoolValue ("showInsertFlag", true);
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
    project.settleBackgroundSaves = nullptr;
    *saverAlive = false;                             // (what it finishes from here on is not taken in: the program is closing, and shutdown() saved)
    saver.reset();                                   // (waits until everything handed to it is written)
    stallWatch.reset();
    spaceBelongsToRecordingHook() = nullptr;
    timereadout::shared().removeChangeListener (this);
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

void AppContext::applyLtc (bool restartOutput)
{
    engine.setLtcInput (project.ltcInput);
    const auto now = juce::Time::getCurrentTime();
    const double tod = now.getHours() * 3600.0 + now.getMinutes() * 60.0 + now.getSeconds() + now.getMilliseconds() / 1000.0;
    engine.setLtcOutput (project.ltcOutput, project.ltcOutOn, project.ltcOutFormat, project.ltcOutStartMode == 0 ? tod : project.ltcOutStartSeconds, restartOutput);
}

TrackDef* AppContext::ltcTrack()
{
    for (auto& t : project.tracks) if (t.isLtc) return &t;
    return nullptr;
}

void AppContext::setLtcInput (int ch)
{
    project.ltcInput = ch;
    auto* t = ltcTrack();
    if (ch >= 0 && t == nullptr)
    {
        auto& nt = project.addTrack ("LTC", TrackFormat::Mono, ch);
        nt.isLtc = true; nt.colour = 0xff8a8f98; nt.setArmed (true);
        t = &nt;
    }
    if (t != nullptr && ch >= 0) { project.assignInputsFrom (*t, ch); }
    else if (t != nullptr) t->inputs.fill (-1);
    applyLtc();
    project.markDirty();
    project.structureChanged();
}

void AppContext::applyAux()
{
    auto& aux = engine.auxSounds();
    const auto& pp = project.pitchPipe;
    const auto& ck = project.click;
    aux.setPipeOutputs (pp.outputs); aux.pipe.setLevelDb (pp.levelDb); aux.pipe.setTone (pp.tone); aux.pipe.setKeyboard (pp.keyboard);
    aux.setClickOutputs (ck.outputs); aux.click.setLevelDb (ck.levelDb);
    aux.click.setTempo (ck.bpm); aux.click.setBeatsPerBar (ck.beatsPerBar); aux.click.setAccent (ck.accent); aux.click.setSound (ck.sound);
    aux.click.setBeatUnit (ck.beatUnit); aux.click.setQuaverTicks (ck.quaverTicks); aux.click.setTickDb (ck.tickDb);
    if (! hasProject()) { aux.click.setRunning (false); aux.pipe.allOff(); }
}

void AppContext::setAlignOwner (const juce::String& owner)
{
    if (owner == project.alignActive) return;
    project.alignActive = owner;
    engine.rebuildPlan();                                                    // (at once: playback may be about to start)
    project.sendChangeMessage();                                             // the mixer strips show the delays now in force
}

juce::String AppContext::currentAlignOwner()
{
    if (engine.isRecording()) return project.alignOwnerFor (false, recordTarget);
    if (engine.isPlaying())
    {
        if (playInfo.kind == PlayInfo::Kind::Take) return project.alignOwnerFor (false, playInfo.windowId);
        if (playInfo.kind == PlayInfo::Kind::Edit) return project.alignOwnerFor (true, playInfo.id);
        return project.alignActive;
    }
    const auto key = lastWorkWindowKey ? lastWorkWindowKey() : juce::String();
    const juce::Uuid id (key.fromFirstOccurrenceOf (":", false, false));
    if (key.startsWith ("take:")) return project.alignOwnerFor (false, id);
    if (key.startsWith ("edit:")) return project.alignOwnerFor (true, id);
    return project.alignActive;
}

void AppContext::applyTalkback()
{
    applyLtc();
    applyAux();
    engine.setTalkbackRouting (project.crMicInputs, project.tbOutputs);
    engine.setStageMix (project.stageGainDb, project.stagePan, project.stagePlaybackDb, project.stageOutputDb);
    engine.setCrMic (crMicOpen());
    engine.setTalkbackPlayback (tbPlaybackOn());
    // (the coloured borders that used to flash round the screen are gone: Pre-Rec, C-R and P-B are shown as squares in the menu bar)
    talkbackOverlay.reset();
}

void AppContext::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &timereadout::shared())       // a time readout's mode / frame rate / start timecode changed: remember it
    {
        props.getUserSettings()->setValue ("timeReadout", timereadout::shared().toString());
        props.saveIfNeeded();
        return;
    }
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
    freeRetiredMixersSoon();
    applyTalkback();
    if (refreshTitles) refreshTitles();
}

void AppContext::freeRetiredMixersSoon()
{
    const size_t count = project.retiredMixers.size();
    if (count == 0 || ! engine.audioThreadNeverStalled()) return;      // (a stalled audio thread may still be inside the old plan: those mixers stay, as they always did)
    juce::MessageManager::callAsync ([this, count]
    {
        // (by now the structure change that retired them has been handled: the windows have let go of them and the plan was rebuilt without them)
        if (! engine.audioThreadNeverStalled()) return;
        if (engine.isPlaying()) return;                                   // an edit's automation may be moving their faders: stopPlayback calls here again
        for (size_t i = 0; i < count && i < project.retiredMixers.size(); ++i)      // their plug-ins' windows first: a window must never outlive its plug-in
        {
            auto& m = *project.retiredMixers[i];
            auto slots = [this] (InsertSlot* sl, int n) { for (int k = 0; k < n; ++k) plugins.closeEditorsFor (sl[k].getProcessorUnsafe()); };
            for (auto& st : m.stripPool) { slots (st->slots, kNumSlots); for (auto& sd : st->sends.pool) slots (sd->slots, kSendSlots); slots (st->outSend.slots, kSendSlots); }
            for (auto& b : m.busPool)    { slots (b->slots, kNumSlots);  for (auto& sd : b->sends.pool) slots (sd->slots, kSendSlots); slots (b->outSend.slots, kSendSlots); }
        }
        project.freeRetiredMixers (count);
    });
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

juce::String AppContext::copyMix (const juce::Uuid& fromId, const juce::Uuid& toId, const Project::MixParts& parts)
{
    auto* from = findMixer (fromId);
    if (from == nullptr) return "No such mixer.";
    if (! (parts.faders || parts.sends || parts.pan || parts.inserts)) return "Tick at least one box to copy.";
    mixClipboard = project.mixSnapshot (*from, true); mixClipParts = parts; mixClipName = from->name; mixClipFrom = fromId;
    return pasteMix (toId);
}

juce::String AppContext::copyMixToClipboard (const juce::Uuid& fromId, const Project::MixParts& parts)
{
    auto* from = findMixer (fromId);
    if (from == nullptr) return "No such mixer.";
    mixClipboard = project.mixSnapshot (*from, true); mixClipParts = parts; mixClipName = from->name; mixClipFrom = fromId;
    project.structure.sendChangeMessage();                                          // the Paste buttons appear
    return "Copied \"" + from->name + "\". Press Paste in another mixer.";
}

juce::String AppContext::pasteMix (const juce::Uuid& toId)
{
    if (mixClipName.isEmpty()) return "Nothing has been copied yet.";
    const auto parts = mixClipParts; const bool withInserts = true;
    auto* from = findMixer (mixClipFrom);
    double sr = 48000.0; int block = 512;
    if (auto* d = devices.getCurrentAudioDevice()) { sr = d->getCurrentSampleRate(); block = juce::jmax (64, d->getCurrentBufferSizeSamples()); }
    const auto snap = mixClipboard;
    std::vector<std::pair<juce::Uuid, juce::var>> undo;
    juce::StringArray names;
    for (auto& m : project.mixers)
    {
        if (m.get() == from || (! toId.isNull() && m->id != toId)) continue;
        undo.push_back ({ m->id, project.mixSnapshot (*m, withInserts) });
        project.applyMix (*m, snap, sr, block, parts);
        names.add (m->name);
    }
    if (undo.empty()) return "Nothing to copy to.";
    mixUndo = std::move (undo);
    project.structure.sendChangeMessage();                                          // strips redraw with the new values
    juce::StringArray what;
    if (parts.faders) what.add ("faders");
    if (parts.sends) what.add ("sends");
    if (parts.pan) what.add ("pans");
    if (parts.inserts) what.add ("inserts");
    return "Copied the " + what.joinIntoString (", ") + " of \"" + mixClipName + "\" onto " + names.joinIntoString (", ") + ".";
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
    engine.checkLatency();
    autosaveIfIdle();
}

void AppContext::autosaveIfIdle()
{
    // The automatic save, only when nothing is going on (2.102): every press of Space or Stop must be answered at once, so saving gives way to
    // everything else. Saving less often was the price, and it is a small one: the snapshot takes a few tens of milliseconds and the writing
    // happens on a thread of its own, so a save never holds anything up now - this only keeps even the snapshot out of the way.
    static const bool trace = juce::SystemStats::getEnvironmentVariable ("FERMATA_LOG_AUTOSAVE", "0") == "1";      // (a development aid: why it waits)
    auto wait = [] (const char* why) { if (trace) juce::Logger::writeToLog (juce::String ("Automatic save waits: ") + why); };
    if (saveHold > 0) return;                                               // (a preview's files are in the take's file list: not what the project file should say)
    if (! project.dirty.load() || project.projectFile == juce::File()) return;
    if (engine.isRecording()) return wait ("recording");                                    // never while recording (Record and Stop save at once)
    if (saver != nullptr && saver->isBusy()) return wait ("the last one is being written");
    const auto now = juce::Time::getMillisecondCounter();
    const auto sinceSave = now - lastAutosaveMs;
    if (engine.isPlaying() && sinceSave < kAutosaveLatestMs) return wait ("playing");      // while it plays, only once 5 minutes have gone by (a long mixing session is never left unsaved)
    if (sinceSave < kAutosaveGapMs) return wait ("under 30 s since the last save");             // at most every 30 seconds
    if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown()) return wait ("a mouse button is down");     // not while something is being dragged
    if (sinceSave < kAutosaveLatestMs)                                      // (after 5 minutes it saves at the next quiet tick, whatever)
    {
        if (now - lastTransportMs < 3000) return wait ("Play / Stop just now");               // not just after Play / Stop: the next press may be coming
        if (now - project.undoDirtyAt < 2000) return wait ("a change just now");              // not in the middle of making changes
    }
    saveNow (true);
}

void AppContext::tickLoad()
{
    lastLoad = engine.takeLoadStats();
    if (! engine.isRunning() || lastLoad.blocks == 0) { minuteSeconds = 0; minuteLoad = {}; return; }
    // gather the minute: the average is weighted by blocks, the peaks are the highest
    const int nb = minuteLoad.blocks + lastLoad.blocks;
    minuteLoad.average = nb > 0 ? (minuteLoad.average * (float) minuteLoad.blocks + lastLoad.average * (float) lastLoad.blocks) / (float) nb : 0.0f;
    minuteLoad.blocks = nb;
    minuteLoad.peak = juce::jmax (minuteLoad.peak, lastLoad.peak);
    minuteLoad.late += lastLoad.late;
    minuteLoad.recordBufferPeak = juce::jmax (minuteLoad.recordBufferPeak, lastLoad.recordBufferPeak);
    const bool rec = engine.isRecording();
    const bool takeEnded = loadWasRecording && ! rec;
    loadWasRecording = rec;
    if (++minuteSeconds >= 60 || takeEnded)
    {
        auto* dev = devices.getCurrentAudioDevice();
        juce::String line;
        line << "Timing (last " << minuteSeconds << " s" << (takeEnded ? ", to the end of the take" : "") << "): audio thread load average "
             << juce::String (minuteLoad.average, 1) << "%, peak " << juce::String (minuteLoad.peak, 1) << "%, "
             << minuteLoad.late << " of " << minuteLoad.blocks << " blocks late";
        if (dev != nullptr) line << ", buffer " << dev->getCurrentBufferSizeSamples() << " at " << juce::String (dev->getCurrentSampleRate(), 0) << " Hz, driver xruns " << dev->getXRunCount();
        line << ", mixer helpers " << engine.mixerThreads();
        if (minuteLoad.recordBufferPeak > 0.0f || rec || takeEnded) line << ", recording buffer peak " << juce::String (minuteLoad.recordBufferPeak, 1) << "% full";
        if (engine.isPlaying()) line << ", playback underruns " << engine.playbackUnderruns();
        if (CoreReservation::get().active()) line << ", " << CoreReservation::get().reservedLogicalProcessors() << " reserved processors";
        juce::Logger::writeToLog (line);
        minuteSeconds = 0; minuteLoad = {};
    }
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

void AppContext::saveNow (bool quick)
{
    if (quick && saveHold > 0) return;                   // (a preview's files are in a take's file list: the project file must not say so; the save comes once they are out)
    if (project.projectFile == juce::File()) return;
    lastAutosaveMs = juce::Time::getMillisecondCounter();
    if (saver != nullptr) saver->drain();                // (what the saver has finished is taken in first: a failed save is written again)
    if (quick && saver != nullptr && ! wholeSaveNext)
    {
        // an automatic save: the snapshot here, the writing on the saver's thread - one at a time, so that a save which fails is known about
        // (and its windows written again) before the next snapshot is taken: one asked for meanwhile is taken once the one being written is done
        if (saver->isBusy()) { quickSaveWanted = true; return; }
        BusyLabel busy ("taking the snapshot for an automatic save");
        if (auto job = project.prepareSave (true))
        {
            if (job->prepareMs > 60.0)
                juce::Logger::writeToLog ("Automatic save: the snapshot took " + juce::String (juce::roundToInt (job->prepareMs)) + " ms (" + juce::String ((int) job->windows.size())
                                          + " window files to write)");
            const int gen = saveGeneration;
            saver->write (std::move (job), [this, gen] (BackgroundSaver::Result& r) { savedInBackground (r, gen); });
        }
    }
    else
    {
        BusyLabel busy ("saving the project");
        juce::String err;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const bool ok = project.save (err, quick);       // (waits for the saver first: Project::settleBackgroundSaves)
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        if (ms > 250.0) juce::Logger::writeToLog ("Saved the project in " + juce::String (juce::roundToInt (ms)) + " ms (written there and then)");
        if (ok) { wholeSaveNext = false; lastFailedJob.reset(); }
        else showSaveError (err, quick);
    }
    props.getUserSettings()->setValue ("lastProject", project.projectFile.getFullPathName());
    props.saveIfNeeded();
}

void AppContext::waitForSaves()
{
    if (saver == nullptr) return;
    if (saver->isBusy())
    {
        BusyLabel busy ("waiting for the automatic save to be written");
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int waited = 10; ! saver->waitUntilIdle (10000); waited += 10)       // (as long as it takes, as a save written here would: never two writes of the same files at once)
            juce::Logger::writeToLog ("Still waiting for the automatic save to be written (" + juce::String (waited) + " s): the drive is not answering");
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        if (ms > 100.0) juce::Logger::writeToLog ("Waited " + juce::String (juce::roundToInt (ms)) + " ms for the automatic save to be written");
    }
    saver->drain();
}

void AppContext::savedInBackground (BackgroundSaver::Result& r, int generation)
{
    if (generation != saveGeneration || r.job == nullptr) return;          // (another project since: not this one's)
    if (quickSaveWanted)                                                    // a save was asked for while this one was being written: now it can be
    {
        quickSaveWanted = false;
        juce::MessageManager::callAsync ([this, alive = saverAlive] { if (*alive && ! closingProgram && project.dirty.load()) saveNow (true); });
    }
    if (r.ok)
    {
        lastFailedJob.reset();
        if (r.job->writeMs > 2000.0)
            juce::Logger::writeToLog ("Automatic save: writing took " + juce::String (juce::roundToInt (r.job->writeMs)) + " ms (on its own thread: nothing waited for it)");
        return;
    }
    project.saveFailed (*r.job);                         // (unsaved again: the next save writes it all again)
    wholeSaveNext = wholeSaveNext || r.windowsFailed;
    juce::Logger::writeToLog ("Automatic save failed: " + r.error);
    lastFailedJob = std::move (r.job);                   // (written again when the project is closed without saving: see retryFailedSave)
    // said a moment later, and only if the project is still unsaved then (a Save that was waiting for this has written everything meanwhile)
    juce::MessageManager::callAsync ([this, alive = saverAlive, err = r.error] { if (*alive && ! closingProgram && project.dirty.load()) showSaveError (err, true); });
}

void AppContext::retryFailedSave()
{
    if (lastFailedJob == nullptr) return;
    juce::String err;
    const bool ok = Project::writeSave (*lastFailedJob, err);
    juce::Logger::writeToLog (ok ? juce::String ("The automatic save that had failed was written when closing")
                                 : "The automatic save that had failed could not be written when closing either: " + err);
    if (! ok && ! closingProgram) showError ("Could not save project", "The last automatic save could not be written:\n\n" + err);
    lastFailedJob.reset();
}

void AppContext::showSaveError (const juce::String& err, bool automatic)
{
    const auto now = juce::Time::getMillisecondCounter();
    if (automatic && lastSaveErrorMs != 0 && now - lastSaveErrorMs < 120000)     // (an automatic save that keeps failing says so every two minutes, not every time)
    {
        juce::Logger::writeToLog ("Could not save project (said less than two minutes ago, not shown again): " + err);
        return;
    }
    lastSaveErrorMs = now;
    showError ("Could not save project", err);
}

void AppContext::afterProjectReplaced()
{
    ++saveGeneration;                                  // (what a save handed over before this did is not this project's)
    wholeSaveNext = false; quickSaveWanted = false; lastFailedJob.reset();
    lastAutosaveMs = juce::Time::getMillisecondCounter();
    syncWithDevice();
    engine.rebuildPlan();
    freeRetiredMixersSoon();                           // the mixers (and plug-ins) of the project that was open before
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
    if (closeWindowByKey) { closeWindowByKey ("pitchpipe"); closeWindowByKey ("click"); }   // (their settings are the project's: they showed the old project's while the new one's sounded)
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
    engine.auxSounds().click.setRunning (false); engine.auxSounds().pipe.allOff();     // (the click and the pitch pipe belong to the old project's outputs)
    plugins.closeAllEditors();                       // (the old project's plug-ins go with it)
    saveNow();
    auto insertFactory = project.insertFactory;
    project.createDefaultDesign();
    project.addDefaultMainBus();                     // an Ext Bus "Main" on outputs 1-2, so a new project is heard at once
    project.name = file.getFileNameWithoutExtension();
    project.projectFile = file;
    juce::String err;
    if (! project.save (err)) { showError ("Could not create project", err); return false; }
    project.createFolders();                         // Recorded Media, Bounced Media, Bounced Media/Mastered Audio
    afterProjectReplaced();
    props.getUserSettings()->setValue ("lastProject", file.getFullPathName());
    return true;
}

bool AppContext::openBackupProject (const juce::File& bak)
{
    if (engine.isRecording()) { showError ("Recording", "Stop recording first."); return false; }
    const auto target = filesync::projectFileForBackup (bak);                            // Name.fermata.bak (or a "(replaced ...)" copy) -> Name.fermata
    if (target == juce::File()) { showError ("Open", bak.getFileName() + " is not the backup of a project file (that would be called Name.fermata.bak)."); return false; }
    const auto bakText = bak.loadFileAsString();                                         // (read first: saving the open project below may rewrite this very file)
    juce::var v;
    if (juce::JSON::parse (bakText, v).failed() || ! v.isObject()) { showError ("Open", bak.getFileName() + " could not be read as a Fermata project, so nothing was changed."); return false; }
    stopPlayback();
    if (hasProject())                              // the open project is saved first; if that fails nothing is touched (its unsaved changes would be lost)
    {
        juce::String serr;
        if (! project.save (serr)) { showError ("Open", "The project that is open could not be saved first, so nothing was changed.\n\n" + serr); return false; }
    }
    // the take windows, edits and mixers have files of their own, which are not put back: the lists of them stay as they are now
    const auto text = target.existsAsFile() ? Project::backupWithCurrentWindows (bakText, target.loadFileAsString()) : bakText;
    juce::String err; juce::File aside;
    if (! filesync::restoreBackupText (target, text, juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"), aside, err)) { showError ("Open", err); return false; }
    project.projectFile = juce::File();            // (saved above: opening must not save it again, over the file just put back)
    juce::Logger::writeToLog ("Opened the save before: " + bak.getFullPathName() + " put back as " + target.getFullPathName()
                              + (aside != juce::File() ? "; the file it replaced is kept as " + aside.getFileName() : juce::String()));
    return openProject (target);
}

bool AppContext::openProject (const juce::File& file)
{
    if (engine.isRecording()) { showError ("Recording", "Stop recording first."); return false; }
    // The "please wait" window: up at once, told what is being read as it goes, and closed only after the windows have caught up with the new project.
    busy::begin ("Opening project", file.getFileNameWithoutExtension());
    struct Closer { ~Closer() { busy::endAfterPendingMessages(); } } closer;
    stopPlayback();
    plugins.closeAllEditors();                       // (the old project's plug-ins go with it)
    if (project.projectFile != juce::File()) busy::step ("Saving " + project.name + " first");
    saveNow();
    juce::String err;
    // While the project is read: say which window is being read, and which plug-in is being loaded (a plug-in can take a few seconds to start).
    const auto realFactory = project.insertFactory;
    struct Restore
    {
        Project& p; InsertFactory f;
        ~Restore() { p.insertFactory = f; p.loadProgress = nullptr; }
    } restore { project, realFactory };
    project.loadProgress = [] (const juce::String& what, double progress) { busy::step (what, progress); };
    if (realFactory)
    {
        auto count = std::make_shared<int> (0);
        project.insertFactory = [this, realFactory, count] (const juce::String& desc, const juce::String& state) -> std::unique_ptr<InsertProcessor>
        {
            juce::String nm, pluginFile;
            if (auto xml = juce::XmlDocument::parse (desc)) { nm = xml->getStringAttribute ("name"); pluginFile = xml->getStringAttribute ("file"); }
            if (pluginFile.isNotEmpty() && plugins.isIgnored (pluginFile))          // on the list of ignored plug-ins: not loaded (the slot keeps it, with its settings)
            {
                juce::Logger::writeToLog ("Plug-in not loaded: " + (nm.isNotEmpty() ? nm : pluginFile) + " is on the list of ignored plug-ins (Audio settings > Plug-ins...)");
                return nullptr;
            }
            busy::step ("Starting plug-in " + juce::String (++*count) + (nm.isNotEmpty() ? ":  " + nm : juce::String()), -1.0);
            return realFactory (desc, state);
        };
    }
    const auto tOpen0 = juce::Time::getMillisecondCounterHiRes();
    if (! project.load (file, err)) { showError ("Could not open project", err); return false; }
    const auto tRead = juce::Time::getMillisecondCounterHiRes();
    project.insertFactory = realFactory;
    project.createFolders();
    if (! project.loadNotes.isEmpty())
    {
        juce::String text = "Not everything could be read as it was saved. Nothing was deleted. Where a file is missing, put it back in the project folder and open the project again; where a .bak was used, the file had gone wrong and the save before it was taken instead.\n\n";
        for (auto& l : project.loadNotes) text << "   " << l << "\n";
        juce::MessageManager::callAsync ([text] { juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Project files").withMessage (text).withButton ("OK"), nullptr); });
    }
    const auto mended = project.repairInterruptedRecordings();      // the program or the computer stopped while recording: keep everything that was recorded
    const auto tChecked = juce::Time::getMillisecondCounterHiRes();
    busy::step ("Getting the audio ready", -1.0);
    if (mended.size() > 0)
    {
        saveNow();
        juce::String text = "The program stopped while this project was recording, but nothing recorded was lost. The recordings were put right and are in the project:\n\n";
        for (auto& l : mended) text << "   " << l << "\n";
        juce::MessageManager::callAsync ([text] { juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle ("Recording recovered").withMessage (text).withButton ("OK"), nullptr); });
    }
    afterProjectReplaced();
    busy::step ("Checking for missing audio files", -1.0);
    if (const int missing = project.missingAudioCount (true); missing > 0)       // (quick: the takes found whole last time are not asked about again)
    {
        const auto list = project.missingAudioList (12, true);
        juce::String text = juce::String (missing) + (missing == 1 ? " audio file used by this project could not be found" : " audio files used by this project could not be found") + ":\n\n";
        for (auto& l : list) text << "   " << l << "\n";
        text << "\nPoint Fermata at the folder that holds them? It looks in that folder and all the folders inside it, by name; a file that was renamed is taken when "
                "its length, sample rate, bit depth and channels say it is the one. (Later: Takes or Edits menu > Find missing audio files...)";
        juce::Logger::writeToLog ("Missing audio files: " + juce::String (missing) + " - " + list.joinIntoString (" | "));
        juce::MessageManager::callAsync ([this, text]
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Audio files missing").withMessage (text)
                                              .withButton ("Find the audio files...").withButton ("Later"),
                                          [this] (int r) { if (r == 1 && findMissingAudio) findMissingAudio(); });
        });
    }
    const auto tReady = juce::Time::getMillisecondCounterHiRes();
    {   // how long opening took, by stage (the log line to look at when a big project is slow to open)
        size_t takes = 0; for (auto& w : project.takeWindows) takes += w->groups.size();
        juce::Logger::writeToLog ("Opened " + project.name + " in " + juce::String (juce::roundToInt (tReady - tOpen0)) + " ms: files read " + juce::String (juce::roundToInt (tRead - tOpen0))
                                  + " ms, recordings checked " + juce::String (juce::roundToInt (tChecked - tRead)) + " ms, audio and undo ready " + juce::String (juce::roundToInt (tReady - tChecked))
                                  + " ms (" + juce::String ((int) project.takeWindows.size()) + " take windows, " + juce::String ((int) takes) + " takes, " + juce::String ((int) project.edits.size()) + " edits)");
    }
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

bool AppContext::closeProject (bool save)
{
    if (engine.isRecording()) { showError ("Recording", "Stop recording first."); return false; }
    stopPlayback();
    engine.auxSounds().click.setRunning (false); engine.auxSounds().pipe.allOff();
    plugins.closeAllEditors();
    if (save) saveNow();
    else { waitForSaves(); retryFailedSave(); }      // (the automatic saves are kept: one being written finishes first, one that failed is written again)
    props.saveIfNeeded();
    project.projectFile = juce::File();              // from here on nothing is written to it (the windows closing below may still mark changes)
    if (closeAllWindows) closeAllWindows();
    juce::MessageManager::callAsync ([this] { startBlankProject(); });       // after the windows are gone (they close in the same queue, just before this)
    return true;
}

void AppContext::startBlankProject()
{
    project.windowFiles.clear(); project.missingWindowRefs.clear();
    project.createDefaultDesign();
    project.addDefaultMainBus();
    project.projectFile = juce::File();
    project.name = "(no project yet)";
    afterProjectReplaced();
}

juce::File AppContext::lastProjectFile()
{
    juce::File last (props.getUserSettings()->getValue ("lastProject"));
    return last.existsAsFile() || filesync::backupOf (last).existsAsFile() ? last : juce::File();      // (a project whose file went while it was being written still opens, from its .bak)
}

void AppContext::restoreLastProject()
{
    const auto last = lastProjectFile();
    if (last != juce::File() && openProject (last)) return;

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

int AppContext::armState() const
{
    int armed = 0;
    for (auto& t : project.tracks) if (t.isArmed()) ++armed;
    return armed == 0 ? 0 : armed == (int) project.tracks.size() ? 2 : 1;
}

void AppContext::remoteArmToggle()
{
    if (engine.isRecording() || project.tracks.empty()) return;
    const bool on = armState() != 2;                                         // all armed: none; otherwise all (the Arm all / Arm none buttons of the take window)
    for (auto& t : project.tracks) t.setArmed (on);
    project.changed();
}

void AppContext::remotePlayPause()
{
    if (isPlaying()) { stopPlayback (true); return; }
    if (engine.isRecording()) return;
    if (transportHandler) { transportHandler(); return; }
    auto* w = project.findTakeWindow (recordTarget);
    if (w == nullptr && ! project.takeWindows.empty()) w = project.takeWindows.front().get();
    if (w == nullptr) { setNotice ("Nothing to play: click in a take or edit window first."); return; }
    const auto err = playTakeWindow (w->id, w->playheadSeconds);
    if (err.isNotEmpty()) setNotice (err);
}

bool AppContext::typingInTextBox()
{
    for (auto* c = juce::Component::getCurrentlyFocusedComponent(); c != nullptr; c = c->getParentComponent())
        if (auto* te = dynamic_cast<juce::TextEditor*> (c)) return ! te->isReadOnly() && te->isShowing();
    return false;
}

bool AppContext::rKeyHasOwnJobHere()
{
    for (auto* c = juce::Component::getCurrentlyFocusedComponent(); c != nullptr; c = c->getParentComponent())
        if (c->getProperties().contains (ownsRKeyProperty())) return true;
    if (auto* w = juce::TopLevelWindow::getActiveTopLevelWindow())          // nothing has the keyboard: the window in front decides
    {
        std::function<bool (juce::Component&, int)> marked = [&] (juce::Component& c, int depth)
        {
            if (c.getProperties().contains (ownsRKeyProperty())) return true;
            if (depth >= 3) return false;
            for (auto* ch : c.getChildren()) if (marked (*ch, depth + 1)) return true;
            return false;
        };
        return marked (*w, 0);
    }
    return false;
}

void AppContext::pollTransportKeys()
{
    const bool fg = juce::Process::isForegroundProcess();
    const bool mods = juce::ModifierKeys::getCurrentModifiersRealtime().isAnyModifierKeyDown();
    const bool space = fg && juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::spaceKey);
    const bool r = fg && juce::KeyPress::isKeyCurrentlyDown ('R');
    if (! space) spaceHeldAfterStop = false;
    const bool spacePressed = space && ! spaceWasDown, rPressed = r && ! rWasDown;
    spaceWasDown = space; rWasDown = r;
    if ((! spacePressed && ! rPressed) || mods || typingInTextBox()) return;
    if (juce::TopLevelWindow::getActiveTopLevelWindow() == nullptr) return;     // a Windows dialog of its own (a file chooser ...) is in front: typing there is typing
    if (spacePressed && engine.isRecording())
    {
        spaceHeldAfterStop = true;                           // (the windows' own Space handlers see this and do nothing with the same press)
        toggleRecord (juce::Uuid::null());
        return;
    }
    if (rPressed && ! engine.isRecording() && ! rKeyHasOwnJobHere())
    {
        auto id = lastTakeWindowId ? lastTakeWindowId() : juce::Uuid::null();     // the take window in front, or the one that was in front last
        if (project.findTakeWindow (id) == nullptr) id = recordTarget;
        if (project.findTakeWindow (id) == nullptr) id = project.takeWindows.empty() ? juce::Uuid::null() : project.takeWindows.front()->id;
        if (id.isNull()) { setNotice ("Make a take window first (Takes menu), then R records from any window."); return; }
        toggleRecord (id);
    }
}

void AppContext::toggleRecord (const juce::Uuid& takeWindowId)
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        saveNow (true);
        return;
    }
    if (! hasProject()) { setNotice ("First choose where the project is saved (New project or Open project)."); return; }
    stopPlayback();
    auto* w = project.findTakeWindow (takeWindowId);
    if (w == nullptr) { showError ("Record", "Choose a take window to record into."); return; }
    recordTarget = takeWindowId;
    setAlignOwner (project.alignOwnerFor (false, takeWindowId));           // monitoring while recording: this piece's alignment
    auto err = engine.startRecording (*w);
    if (err.isNotEmpty()) showError ("Cannot record", err);
    else saveNow (true);   // write the project at once so the new take is never lost
}

// ---------------------------------------------------------------------------- playback
/** How far an open-ended play (a transport with nothing left to play) carries on before it stops by itself. */
static constexpr double kOpenEndSeconds = 6.0 * 3600.0;

static juce::String beginSession (AppContext& app, std::vector<PlaySegment> segs, juce::int64 start, juce::int64 end, double rate,
                                  std::shared_ptr<AutomationPlan> automation = nullptr, bool loop = false, bool allowEmpty = false,
                                  const juce::Uuid& editId = juce::Uuid::null(), juce::int64 continueTo = -1)
{
    if (segs.empty() && ! allowEmpty) return "There is nothing to play here";
    if (end <= start) return "Nothing to play (empty range)";
    app.noteTransport();
    std::vector<int> counts;
    for (auto& t : app.project.tracks) counts.push_back (t.channelCount());
    auto session = std::make_unique<PlaybackSession> (counts, std::move (segs), start, end, rate, app.engine.getMaxBlock());
    session->automation = std::move (automation);
    session->editId = editId;                                  // an Edit plays through its own mixer
    session->setLooping (loop);
    if (continueTo > end) session->setContinueTo (continueTo);              // a loop let go of carries on to there (see PlaybackSession::releaseLoop)
    return app.engine.startPlayback (std::move (session), false);           // (returns at once: the sound starts as soon as the first moments are read)
}

/** Times a press of Play on the message thread (for the Play / Stop timing line in the log) and labels it for the stall watch. */
struct PlayStartTimer
{
    explicit PlayStartTimer (AppContext& a) : app (a) {}
    ~PlayStartTimer() { app.lastPlayStartMs = juce::Time::getMillisecondCounterHiRes() - t0; }
    AppContext& app;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    BusyLabel label { "starting playback" };
};

/** Playing from the middle of something (or to a mark) would start and stop the sound abruptly: a 1 ms fade in at the start and out at the end of what is played. */
static void transportFades (std::vector<PlaySegment>& segs, juce::int64 a, juce::int64 b, double rate, bool fadeEnd = true)
{
    const auto len = (juce::int64) std::llround (0.001 * rate);                   // (always 1 ms: just enough to avoid a click when starting / stopping)
    if (len <= 0) return;
    for (auto& s : segs)
    {
        if (s.begin < a && s.end > a && (s.fadeInLen == 0 || s.fadeInStart + s.fadeInLen <= a)) { s.fadeInStart = a; s.fadeInLen = len; }
        if (fadeEnd && s.end > b && s.begin < b && b - len > a && (s.fadeOutLen == 0 || s.fadeOutStart >= b - len)) { s.fadeOutStart = b - len; s.fadeOutLen = len; }
    }
}

juce::String AppContext::playTakeWindow (const juce::Uuid& windowId, double fromSec, double loopToSec, const juce::Uuid& rateOfTake)
{
    PlayStartTimer timing (*this);
    auto* w = project.findTakeWindow (windowId);
    if (w == nullptr) return "That take window no longer exists";
    double rate = engine.getSampleRate();
    for (auto& g : w->groups) if (g.sampleRate > 0.0) { rate = g.sampleRate; break; }
    if (auto* lg = w->findGroup (rateOfTake); lg != nullptr && lg->sampleRate > 0.0) rate = lg->sampleRate;     // (a loop plays at the rate of its own take)
    if (rate <= 0.0) rate = 48000.0;
    std::vector<PlaySegment> segs;
    for (auto& g : w->groups)
    {
        if (std::abs (g.sampleRate - rate) > 0.5) continue;                        // another sample rate cannot be mixed in
        for (auto s : segmentsForTake (project, g, (juce::int64) std::llround (g.startSeconds * rate))) segs.push_back (std::move (s));
    }
    setAlignOwner (project.alignOwnerFor (false, windowId));                // the piece's own spot-mic alignment, before the first sample plays
    const juce::int64 a = (juce::int64) std::llround (juce::jmax (0.0, fromSec) * rate);
    const juce::int64 open = a + (juce::int64) std::llround (kOpenEndSeconds * rate);
    const bool loop = loopToSec > fromSec;
    const juce::int64 b = loop ? juce::jmax (a + 1, (juce::int64) std::llround (loopToSec * rate)) : open;
    transportFades (segs, a, b, rate, ! loop);                            // (a loop's end is faded by the session, so it can be let go of)
    auto err = beginSession (*this, std::move (segs), a, b, rate, nullptr, loop, true, juce::Uuid::null(), loop ? open : -1);
    if (err.isEmpty()) { playInfo = {}; playInfo.kind = PlayInfo::Kind::Take; playInfo.windowId = windowId; playInfo.startSeconds = (double) a / rate; playInfo.windowTimeline = true;
                         playInfo.loopLen = loop ? (double) (b - a) / rate : 0.0; }
    return err;
}

juce::String AppContext::playTake (const juce::Uuid& windowId, const juce::Uuid& takeId, double fromSec, double toSec, bool loop)
{
    PlayStartTimer timing (*this);
    auto* g = project.findTake (windowId, takeId);
    if (g == nullptr) return "That take no longer exists";
    const auto rate = g->sampleRate;
    const juce::int64 a = juce::jlimit ((juce::int64) 0, g->lengthSamples, (juce::int64) (fromSec * rate));
    const juce::int64 b = toSec < 0 ? g->lengthSamples : juce::jlimit (a, g->lengthSamples, (juce::int64) (toSec * rate));
    auto segs = segmentsForTake (project, *g);
    transportFades (segs, a, b, rate);
    loop = loop && toSec >= 0 && b > a;
    setAlignOwner (project.alignOwnerFor (false, windowId));
    auto err = beginSession (*this, std::move (segs), a, b, rate, nullptr, loop);
    if (err.isEmpty()) { playInfo = { PlayInfo::Kind::Take, windowId, takeId, (double) a / rate }; playInfo.loopLen = loop ? (double) (b - a) / rate : 0.0; }
    return err;
}

juce::String AppContext::playEdit (const juce::Uuid& editId, double fromSec, double toSec, bool loop, bool goOnAfterLoop)
{
    PlayStartTimer timing (*this);
    auto* e = project.findEdit (editId);
    if (e == nullptr) return "That edit no longer exists";
    // the edit may be empty, and you can start anywhere on its timeline, also after its last piece: with no end given it simply carries on (silent where there is no audio)
    double rate = e->sampleRate > 0.0 ? e->sampleRate : engine.getSampleRate();
    if (rate <= 0.0) rate = 48000.0;
    const juce::int64 a = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (fromSec * rate));
    const juce::int64 b = toSec < 0 ? a + (juce::int64) std::llround (kOpenEndSeconds * rate) : juce::jmax (a, (juce::int64) std::llround (toSec * rate));
    auto segs = segmentsForEdit (project, *e);
    loop = loop && toSec >= 0 && b > a;
    const bool goOn = loop && goOnAfterLoop;
    setAlignOwner (project.alignOwnerFor (true, editId));
    transportFades (segs, a, b, rate, ! goOn);                            // (a loop that may be let go of has its end faded by the session)
    project.resolveAllAutomation();
    auto err = beginSession (*this, std::move (segs), a, b, rate, automationPlanFor (project, *e), loop, true, editId,
                             goOn ? a + (juce::int64) std::llround (kOpenEndSeconds * rate) : -1);
    if (err.isEmpty()) { playInfo = { PlayInfo::Kind::Edit, e->windowId, editId, (double) a / rate }; playInfo.loopLen = loop ? (double) (b - a) / rate : 0.0; }
    return err;
}

juce::String AppContext::playEditGuide (const juce::Uuid& editId, double fromSec, double toSec, bool loop, bool goOnAfterLoop)
{
    PlayStartTimer timing (*this);
    auto* e = project.findEdit (editId);
    if (e == nullptr) return "That edit no longer exists";
    if (! e->guide.active()) return "This edit has no visual guide";
    const double rate = e->guide.sampleRate;
    const juce::int64 a = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (fromSec * rate));
    const juce::int64 b = toSec < 0 ? a + (juce::int64) std::llround (kOpenEndSeconds * rate) : juce::jmax (a, (juce::int64) std::llround (toSec * rate));
    auto segs = segmentsForGuide (project, *e);
    const int ti = guideTrackIndex (project, e->guide);
    if (segs.empty() || ti < 0) return "The guide has no track to play on (its track is gone, and no other track has as many channels)";
    loop = loop && toSec >= 0 && b > a;
    const bool goOn = loop && goOnAfterLoop;
    setAlignOwner (project.alignOwnerFor (true, editId));
    transportFades (segs, a, b, rate, ! goOn);
    auto err = beginSession (*this, std::move (segs), a, b, rate, nullptr, loop, true, editId, goOn ? a + (juce::int64) std::llround (kOpenEndSeconds * rate) : -1);
    if (err.isNotEmpty()) return err;
    playInfo = { PlayInfo::Kind::Edit, e->windowId, editId, (double) a / rate }; playInfo.loopLen = loop ? (double) (b - a) / rate : 0.0;
    // the guide plays on its track's strip in the edit's mixer: if that strip cannot be heard just now, say why (rather than play silence)
    if (auto* m = project.mixerOfEdit (editId))
        if (auto* st = m->stripFor (project.tracks[(size_t) ti].id))
        {
            bool otherSolo = m->vcaSoloMask() != 0u;
            for (auto& sp : m->stripPool) otherSolo = otherSolo || (sp.get() != st && sp->solo.get());
            for (auto& bp : m->busPool) otherSolo = otherSolo || bp->solo.get();
            const auto nm = project.tracks[(size_t) ti].name;
            if (st->mute.get()) setNotice ("The guide plays on the " + nm + " strip, which is muted in this edit's mixer: switch its M off to hear it.");
            else if (otherSolo && ! st->solo.get()) setNotice ("Another strip is soloed in this edit's mixer, so the guide (on " + nm + ") may not be heard.");
        }
    return {};
}

juce::String AppContext::setEditGuide (const juce::Uuid& windowId, const juce::Uuid& takeId, const juce::Uuid& trackId, juce::Uuid* editOut)
{
    auto* w = project.findTakeWindow (windowId);
    if (w == nullptr) return "No take window";
    auto* g = w->findGroup (takeId);
    if (g == nullptr) return "That take no longer exists";
    const TakeFile* tf = nullptr;
    for (auto& f : g->files) if (f.trackId == trackId) tf = &f;
    if (tf == nullptr) return "This take has no audio on that track.";
    const double rate = g->sampleRate;
    if (rate <= 0.0 || g->lengthSamples <= 0) return "This take has no audio yet.";
    juce::int64 a = 0, b = g->lengthSamples;
    if (w->editTake == takeId && w->editIn >= 0.0 && w->editOut > w->editIn)         // marks 1 and 2 in this take: only that part
    {
        a = juce::jlimit ((juce::int64) 0, g->lengthSamples, (juce::int64) std::llround (w->editIn * rate));
        b = juce::jlimit ((juce::int64) 0, g->lengthSamples, (juce::int64) std::llround (w->editOut * rate));
    }
    if (b - a < (juce::int64) (0.1 * rate)) return "That part is too short to be a guide.";
    // the edit: the one this take window sends to (3 / 4); else one made from it; else the edit window used last; else the only edit; else a new one
    EditDef* e = project.findEdit (w->targetEdit);
    if (e == nullptr) for (auto& ep : project.edits) if (ep->windowId == windowId) { e = ep.get(); break; }
    if (e == nullptr && lastWorkWindowKey) if (const auto k = lastWorkWindowKey(); k.startsWith ("edit:")) e = project.findEdit (juce::Uuid (k.fromFirstOccurrenceOf (":", false, false)));
    if (e == nullptr && project.edits.size() == 1) e = project.edits.front().get();
    if (e != nullptr && ! e->isEmpty() && std::abs (e->sampleRate - rate) > 0.5)
        return "This take is " + juce::String (rate, 0) + " Hz but the edit is " + juce::String (e->sampleRate, 0) + " Hz.";
    if (e == nullptr) e = &project.addEdit (w->name + " - EDIT", windowId);
    if (project.findEdit (w->targetEdit) == nullptr) w->targetEdit = e->id;          // (so what 3 and 4 send goes to the same edit as the guide)
    EditGuide gd;
    gd.windowId = windowId; gd.takeId = takeId; gd.trackId = trackId;
    gd.takeName = w->displayName (*g);
    gd.trackName = tf->trackName;
    if (auto* t = project.findTrack (trackId)) gd.trackName = t->name;
    gd.file = tf->file; gd.numChannels = juce::jmax (1, tf->numChannels); gd.sampleRate = rate;
    gd.srcIn = a; gd.srcOut = b; gd.start = 0;
    e->guide = gd; e->showGuide = true;
    project.changed();
    if (editOut != nullptr) *editOut = e->id;
    return {};
}

juce::String AppContext::playDisc (const std::vector<DiscPlayItem>& items, double fromSec, double toSec, const juce::Uuid& token)
{
    PlayStartTimer timing (*this);
    double rate = 0.0; juce::int64 endAll = 0;
    std::vector<PlaySegment> segs;
    auto plan = std::make_shared<AutomationPlan>();
    project.resolveAllAutomation();
    discAlign.clear();                                                       // each piece's spot delays, switched in the pause before it (see the watcher)
    for (auto& it : items)
        if (auto* e = project.findEdit (it.editId); e != nullptr && ! e->isEmpty() && e->sampleRate > 0.0)
            discAlign.push_back ({ it.startSeconds + (double) e->lengthSamples() / e->sampleRate, project.alignOwnerFor (true, it.editId) });
    std::sort (discAlign.begin(), discAlign.end(), [] (const DiscAlign& x, const DiscAlign& y) { return x.end < y.end; });
    discToken = token;
    for (auto& d : discAlign) if (fromSec < d.end) { setAlignOwner (d.owner); break; }      // the piece it starts in (or the next one)
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
    PlayStartTimer timing (*this);
    double rate = 0.0; juce::int64 endAll = 0;
    std::vector<PlaySegment> segs;
    for (auto& it : items)
    {
        if (! it.file.existsAsFile() || it.frames <= 0 || it.rate <= 0.0) continue;
        if (rate <= 0.0) rate = it.rate; else if (std::abs (it.rate - rate) > 0.5) continue;               // another sample rate cannot be played together with these
        const juce::int64 D = (juce::int64) std::llround (it.startSeconds * rate);
        PlaySegment s;
        s.trackIndex = 0; s.file = it.file; s.srcOffset = -D; s.begin = D; s.end = D + it.frames; s.rate = it.rate;
        segs.push_back (std::move (s));
        endAll = juce::jmax (endAll, D + it.frames);
    }
    if (segs.empty() || rate <= 0.0) return "There is nothing to play (the renders are not ready)";
    const juce::int64 a = juce::jlimit ((juce::int64) 0, endAll, (juce::int64) std::llround (fromSec * rate));
    const juce::int64 b = toSec < 0 ? endAll : juce::jlimit (a, endAll, (juce::int64) std::llround (toSec * rate));
    if (b <= a) return "Nothing to play (empty range)";
    transportFades (segs, a, b, rate);
    noteTransport();
    auto session = std::make_unique<PlaybackSession> (std::vector<int> { 2 }, std::move (segs), a, b, rate, engine.getMaxBlock());
    session->directOut = juce::jmax (0, outFirst);
    auto err = engine.startPlayback (std::move (session), false);
    if (err.isEmpty()) playInfo = { PlayInfo::Kind::Other, juce::Uuid::null(), token, (double) a / rate };
    return err;
}

juce::String AppContext::playRegionOriginal (const juce::Uuid& editId, const juce::Uuid& regionId, juce::int64 fromSample, juce::int64 toSample)
{
    PlayStartTimer timing (*this);
    auto* e = project.findEdit (editId);
    if (e == nullptr) return "That edit no longer exists";
    auto* r = e->find (regionId);
    if (r == nullptr) return "That piece no longer exists";
    const auto limit = r->sourceLength > 0 ? r->sourceLength : r->srcOut + (juce::int64) (60.0 * r->sampleRate);
    const juce::int64 a = juce::jlimit ((juce::int64) 0, limit, fromSample);
    const juce::int64 b = juce::jlimit (a, limit, toSample);
    setAlignOwner (project.alignOwnerFor (true, editId));
    auto err = beginSession (*this, segmentsForRegionOriginal (project, *r), a, b, r->sampleRate, nullptr, false, false, editId);
    if (err.isEmpty()) playInfo = { PlayInfo::Kind::Other, juce::Uuid::null(), regionId, (double) a / r->sampleRate };
    return err;
}

void AppContext::redrawWaveforms (const std::vector<juce::File>& files)
{
    const auto dir = project.projectFolder().getChildFile ("Waveforms");
    int n = 0;
    for (auto& f : files)
    {
        wavestore::fileFor (dir, f, ".fwave").deleteFile(); wavestore::fileFor (dir, f, ".fwave2").deleteFile();
        wavestore::fileFor (dir, f, ".fwcol").deleteFile();
        waveColors.forget (f);
        redrawLog.push_back ({ ++redrawStamp, f.getFullPathName() });
        ++n;
    }
    if (redrawLog.size() > 4000) redrawLog.erase (redrawLog.begin(), redrawLog.begin() + 2000);
    setNotice (n == 0 ? juce::String ("There are no waveforms to re-draw here") : "Re-drawing " + juce::String (n) + (n == 1 ? " waveform..." : " waveforms..."));
    project.sendChangeMessage();
}

void AppContext::forgetWaveforms (const std::vector<juce::File>& files)
{
    for (auto& f : files)
    {
        waveColors.forget (f);
        redrawLog.push_back ({ ++redrawStamp, f.getFullPathName() });
    }
    if (redrawLog.size() > 4000) redrawLog.erase (redrawLog.begin(), redrawLog.begin() + 2000);
}

std::vector<juce::File> AppContext::audioFilesOfWindow (const juce::String& key) const
{
    std::vector<juce::File> v;
    auto add = [&v] (const juce::File& f) { if (f != juce::File() && std::find (v.begin(), v.end(), f) == v.end()) v.push_back (f); };
    const auto id = juce::Uuid (key.fromFirstOccurrenceOf (":", false, false));
    if (key.startsWith ("edit:"))
    {
        if (auto* e = const_cast<Project&> (project).findEdit (id))
        {
            for (auto& r : e->regions) for (auto& f : r.files) add (f.file);
            for (auto& r : e->overdubs) for (auto& f : r.files) add (f.file);
        }
    }
    else if (key.startsWith ("take:"))
    {
        if (auto* w = const_cast<Project&> (project).findTakeWindow (id))
            for (auto& g : w->groups) for (auto& f : g.files) add (f.file);
    }
    return v;
}

void AppContext::setShowInsertFlag (bool on)
{
    showInsertFlag = on;
    if (auto* st = props.getUserSettings()) { st->setValue ("showInsertFlag", on); props.saveIfNeeded(); }
    project.sendChangeMessage();
}

void AppContext::setTrimSlip (bool left, bool right)
{
    trimSlipLeft = left; trimSlipRight = right;
    if (auto* st = props.getUserSettings()) { st->setValue ("trimSlipLeft", left); st->setValue ("trimSlipRight", right); props.saveIfNeeded(); }
}

void AppContext::changeTrimWaveZoom (float factor)
{
    trimWaveZoom = juce::jlimit (0.25f, 40.0f, trimWaveZoom * factor);
    if (auto* st = props.getUserSettings()) { st->setValue ("trimWaveZoom", (double) trimWaveZoom); props.saveIfNeeded(); }
    trimWaveZoomChanged.sendChangeMessage();           // only the Trim windows redraw
}

void AppContext::setPlayheadFollows (bool on)
{
    playheadFollows = on;
    if (auto* st = props.getUserSettings()) { st->setValue ("playheadFollows", on); props.saveIfNeeded(); }
    project.sendChangeMessage();                       // windows redraw their button
}

void AppContext::stopPlayback (bool quick)
{
    const bool wasPlaying = engine.isPlaying();
    BusyLabel busy ("stopping playback");
    if (wasPlaying) noteTransport();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const double firstSound = wasPlaying ? engine.playbackFirstSoundMs() : 0.0;
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
    engine.stopPlayback (quick);                       // (quick: a press of Space or Stop - the tidying up of the play is left to a thread of its own)
    playInfo = {};
    discAlign.clear();
    freeRetiredMixersSoon();                           // (mixers retired while it played waited for this)
    // (no save after Stop any more: the automatic save waits for a quiet moment - see autosaveIfIdle)
    const auto stopMs = juce::Time::getMillisecondCounterHiRes() - t0;
    static const bool logEvery = juce::SystemStats::getEnvironmentVariable ("FERMATA_LOG_PLAYSTOP", "0") == "1";      // (a development aid: every press)
    if (wasPlaying && (logEvery || lastPlayStartMs > 60.0 || firstSound > 600.0 || stopMs > 40.0))      // (only when something was slow: the log is not filled with every press)
        juce::Logger::writeToLog ("Play / Stop timing: Play took " + juce::String (juce::roundToInt (lastPlayStartMs)) + " ms of the screen's time, the first sound came after "
                                  + (firstSound > 0.0 ? juce::String (juce::roundToInt (firstSound)) + " ms" : juce::String ("(not yet)")) + ", Stop took " + juce::String (juce::roundToInt (stopMs)) + " ms");
    lastPlayStartMs = 0.0;
}

double AppContext::playheadSeconds() const
{
    if (! engine.isPlaying() || playInfo.kind == PlayInfo::Kind::None) return -1.0;
    return playInfo.startSeconds + playedSeconds();
}

double AppContext::playheadSecondsSmooth() const
{
    if (! engine.isPlaying() || playInfo.kind == PlayInfo::Kind::None) return -1.0;
    return playInfo.startSeconds + engine.playbackTimelineSeconds (true);       // (round a loop; straight on once the loop is let go of)
}

double AppContext::playedSeconds() const
{
    return engine.playbackTimelineSeconds (false);       // in a loop the playhead goes round and round; once the loop is let go of it goes straight on
}

juce::String AppContext::copyMarkedToEdit (const juce::Uuid& windowId, bool atEditPlayhead, juce::Uuid* editIdOut, bool insert)
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
    if (e == nullptr) { e = &project.addEdit (w->name + " - EDIT", windowId); w->targetEdit = e->id; }
    if (! e->isEmpty() && std::abs (e->sampleRate - rate) > 0.5)
        return "This take is " + juce::String (rate, 0) + " Hz but the edit is " + juce::String (e->sampleRate, 0) + " Hz.";

    // the edit keeps its own list of files, so it keeps working even if the take is later removed from the take window
    for (auto& f : g->files) r.files.push_back ({ f.trackId, f.trackName, f.file, f.numChannels });
    r.sourceLength = g->lengthSamples;
    const juce::int64 at = (juce::int64) std::llround (juce::jmax (0.0, e->playheadSeconds) * rate);
    const auto pieceLen = r.length();
    if (w->overdub)             e->addOverdub (r, at);
    else if (atEditPlayhead && insert)
    {
        e->insertRegionAt (r, at);
        setNotice ("Inserted at " + juce::String ((double) at / rate, 2) + " s: everything after it moved " + juce::String ((double) pieceLen / rate, 2) + " s later.");
    }
    else if (atEditPlayhead)
    {
        juce::int64 gone = 0;
        e->overwriteRegionAt (r, at, &gone);
        setNotice ("Placed at " + juce::String ((double) at / rate, 2) + " s: replaced " + juce::String ((double) gone / rate, 2) + " s of the edit (Shift + 4 inserts instead and moves the rest later).");
    }
    else                        e->insertRegion (r);
    // the edit may have its own track order: every track of the take goes to ITS row there (by identity, never by the row it had in the take window),
    // and a track the edit does not list yet is added at the bottom, so no audio is ever hidden or put on the wrong track
    if (! e->trackIds.empty())
        for (auto& f : g->files)
            if (std::find (e->trackIds.begin(), e->trackIds.end(), f.trackId) == e->trackIds.end()) e->trackIds.push_back (f.trackId);
    // (the 1 / 2 marks stay where they are after the take is sent: they are only replaced by new ones, or taken away with 5)
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
    closingProgram = true;
    if (! skipFinalSave) saveNow();
    else { waitForSaves(); retryFailedSave(); }      // "Quit without saving" keeps what was saved automatically: so it must really be on the disk
    if (auto* s = props.getUserSettings())
        if (auto xml = devices.createStateXml()) { s->setValue ("audioDevice", xml.get()); }
    props.saveIfNeeded();
    devices.removeAudioCallback (&engine);
    devices.closeAudioDevice();
}
} // namespace td
