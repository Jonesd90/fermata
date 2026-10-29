#include "ui/FermataLookAndFeel.h"
#include "ui/ThemeSwitch.h"
#include "ui/MainWindow.h"
#include "ui/DesignWindow.h"
#include "ui/TakeWindow.h"
#include "ui/MixerWindow.h"
#include "ui/ScopeWindow.h"
#include "ui/OrganiserWindow.h"
#include "ui/WinOwner.h"
#include "ui/TakeDisplay.h"
#include "ui/EditWindow.h"
#include "ui/TrimWindow.h"
#include "ui/MeterBridge.h"
#include "ui/BounceWindow.h"
#include "ui/MediaWindow.h"
#include "ui/MasteringWindow.h"
#include "ui/Splash.h"
#include "ui/BusyWindow.h"
#include "ui/RemoteControl.h"
#include "ui/RecordMonitor.h"
#include "ui/VideoExportWindow.h"
#include "ui/AafExportWindow.h"
#include "ui/VersionsWindow.h"
#include "ui/Surface.h"
#include "ui/SurfacePanel.h"
#include "ui/PitchPipeWindow.h"
#include "ui/ClickWindow.h"
#include "ui/AlignWindow.h"
#include "ui/CpuCoresPanel.h"
#include "ui/PluginManager.h"
#include "ui/HotKeysWindow.h"
#if JUCE_WINDOWS
 #include <shlobj.h>
#endif
#include <csignal>
#include "core/CrashReport.h"

#ifndef FERMATA_VERSION_STRING
 #define FERMATA_VERSION_STRING "0.0.0"      // only when built without CMake
#endif

using namespace td;

static juce::File logFolder() { return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata"); }

/** Called by Windows when the program crashes: a crash report (3.14.0: a minidump in Documents\Fermata\Crash reports, see CrashReport.h) first,
    then the kind of crash, where, and the stack trace into the log file. */
static void crashHandler (void* exceptionPointers)
{
    const auto report = crashreport::write (exceptionPointers, "crash");
    auto f = logFolder().getChildFile ("fermata-log.txt");
    juce::String text ("\n*** CRASH ***");
#if JUCE_WINDOWS
    if (const auto what = crashreport::describeException (exceptionPointers); what.isNotEmpty()) text << " " << what;
#endif
    text << "\n";
    if (report != juce::File()) text << "Crash report written: " << report.getFullPathName() << "\n";
    text << juce::SystemStats::getStackBacktrace() << "\n";
    f.appendText (text);
}

#if JUCE_WINDOWS
/** abort() (a C++ exception nobody caught, a pure virtual call, a failed check in a library) does not go through the crash handler: it is caught
    here, so it leaves a report and a log line too. */
static void abortHandler (int)
{
    const auto report = crashreport::write (nullptr, "abort");
    logFolder().getChildFile ("fermata-log.txt").appendText ("\n*** CRASH *** the program was aborted (an error it could not recover from)\n"
                                                           + (report != juce::File() ? "Crash report written: " + report.getFullPathName() + "\n" : juce::String())
                                                           + juce::SystemStats::getStackBacktrace() + "\n");
}
#endif

/** The .ico of a saved project: a sheet of paper with the logo. Windows reads PNG pictures inside an .ico. */
static bool writeProjectIcon (const juce::File& f)
{
    juce::MemoryOutputStream png;
    juce::PNGImageFormat fmt;
    if (! fmt.writeImageToStream (makeFermataFileIcon (256), png)) return false;
    juce::MemoryOutputStream ico;
    ico.writeShort (0); ico.writeShort (1); ico.writeShort (1);                 // reserved, type = icon, one picture
    ico.writeByte (0); ico.writeByte (0); ico.writeByte (0); ico.writeByte (0);   // 256 x 256, no palette
    ico.writeShort (1); ico.writeShort (32);                                      // planes, bits per pixel
    ico.writeInt ((int) png.getDataSize());
    ico.writeInt (22);                                                            // where the picture starts
    ico.write (png.getData(), png.getDataSize());
    f.deleteFile();
    return f.replaceWithData (ico.getData(), ico.getDataSize());
}

/** Makes Windows show saved projects (.fermata) with the paper-and-logo icon, and open them in this program when double-clicked. */
static void registerProjectFileType()
{
   #if JUCE_WINDOWS
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto ico = logFolder().getChildFile ("fermata-project.ico");
    if (! ico.existsAsFile() || ico.getLastModificationTime() < exe.getLastModificationTime()) writeProjectIcon (ico);
    if (! ico.existsAsFile()) return;
    const juce::String base = "HKEY_CURRENT_USER\\Software\\Classes\\";
    const juce::String command = "\"" + exe.getFullPathName() + "\" \"%1\"";
    bool changed = false;
    auto set = [&] (const juce::String& path, const juce::String& value)
    {
        if (juce::WindowsRegistry::getValue (base + path) != value) { juce::WindowsRegistry::setValue (base + path, value); changed = true; }
    };
    set (".fermata\\", "Fermata.Project");
    set (".takedaw\\", "Fermata.Project");          // projects from the program's old name still open
    set ("Fermata.Project\\", "Fermata project");
    set ("Fermata.Project\\DefaultIcon\\", "\"" + ico.getFullPathName() + "\"");
    set ("Fermata.Project\\shell\\open\\command\\", command);
    if (changed) SHChangeNotify (SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
   #endif
}

class FermataApplication : public juce::JUCEApplication, private juce::ChangeListener
{
public:
    const juce::String getApplicationName() override    { return "Fermata"; }
    const juce::String getApplicationVersion() override { return FERMATA_VERSION_STRING; }      // from FERMATA_VERSION in CMakeLists.txt
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        splash = std::make_unique<SplashScreen> (juce::String ("Version ") + getApplicationVersion());
        logFolder().createDirectory();
        registerProjectFileType();
        fileLogger.reset (new juce::FileLogger (logFolder().getChildFile ("fermata-log.txt"), "Fermata " + getApplicationVersion() + "  (built " + __DATE__ + " " + __TIME__ + ")", 512 * 1024));
        juce::Logger::setCurrentLogger (fileLogger.get());
        crashreport::install (logFolder().getChildFile ("Crash reports"), getApplicationVersion());       // 3.14.0
        juce::SystemStats::setApplicationCrashHandler (crashHandler);
       #if JUCE_WINDOWS
        std::signal (SIGABRT, abortHandler);
       #endif
        lookAndFeel.reset (new FermataLookAndFeel());
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());
        tooltips = std::make_unique<juce::TooltipWindow> (nullptr, 900);        // without this no tooltip would ever show
        ctx = std::make_unique<AppContext>();
        installThemeSwitch();                                                    // the sun / moon buttons switch the whole program
        themeSaveHook() = [this] (bool dark)
        {
            if (ctx == nullptr) return;
            if (auto* st = ctx->props.getUserSettings()) { st->setValue ("deskDark", dark); ctx->props.saveIfNeeded(); }
        };
        if (auto* st = ctx->props.getUserSettings()) if (const int us = st->getIntValue ("uiStyle", 0); us != 0) applyStyle (us);          // Project > UI style (the Manuscript UI)
        if (auto* st = ctx->props.getUserSettings()) if (const int dc = st->getIntValue ("deskColour", 0); dc != 0) applyDeskColour (dc);   // Project > Colour theme
        if (auto* st = ctx->props.getUserSettings()) if (st->getBoolValue ("deskDark", false)) switchTheme (true);     // the dark look (the desk a shade darker)
        if (auto* st = ctx->props.getUserSettings())
        {
            auto& cr = CoreReservation::get();
            juce::Logger::writeToLog ("CPU: " + cr.describeMachine());
            auto saved = loadCoreReservationSettings (*st);
            // 3.13.0: programs that a Fermata which crashed (or was ended from the Task Manager) left off its cores are set free first
            if (const int freed = cr.releaseLeftovers (saved); freed > 0)
                juce::Logger::writeToLog ("CPU cores: " + juce::String (freed) + (freed == 1 ? " program was" : " programs were") + " still kept off the audio cores by a Fermata that did not close properly - set free again");
            // 3.13.0: on a laptop, "ask other programs to stay off these cores" is switched off once (it left them too little of the PC); it can be ticked again
            if (CoreReservation::isLaptop() && ! st->containsKey ("cpuLaptopChecked"))
            {
                st->setValue ("cpuLaptopChecked", true);
                if (saved.keepOthersOff)
                {
                    saved.keepOthersOff = false;
                    saveCoreReservationSettings (*st, saved);
                    juce::Logger::writeToLog ("CPU cores: this is a laptop, so the other programs are no longer kept off the audio cores (Audio settings > CPU cores to change it)");
                }
                st->saveIfNeeded();
            }
            cr.configure (saved);                                                // the CPU cores kept for the audio (off unless switched on)
        }
        juce::Logger::writeToLog ("CPU cores: " + CoreReservation::get().statusText().replace ("\n", " "));
        ctx->showTakeWindow = [this] (const juce::Uuid& id) { openTakeWindow (id); };
        ctx->showMixer = [this] (const juce::Uuid& id) { openMixer (id); };
        ctx->showEdit = [this] (const juce::Uuid& id) { openEdit (id); };
        ctx->showEditBehind = [this] (const juce::Uuid& id, juce::Component* front) { openEdit (id, front); };
        ctx->focusRecent = [this] (const juce::String& kind) { focusRecent (kind); };
        ctx->activeKind = [this] { return activeKind(); };
        ctx->windowLight = [this] (const juce::String& kind) { return windowLight (kind); };
        ctx->lastWorkWindowKey = [this] { return lastWorkWindowKey(); };
        ctx->lastTakeWindowId = [this]
        {
            ToolWindow* best = nullptr; juce::String bestKey;
            for (auto& kv : windows)
                if (kv.second != nullptr && kv.first.startsWith ("take:") && kv.second->lastActive > 0 && (best == nullptr || kv.second->lastActive > best->lastActive)) { best = kv.second.get(); bestKey = kv.first; }
            return best != nullptr ? juce::Uuid (bestKey.fromFirstOccurrenceOf (":", false, false)) : recentTake;
        };
        ctx->showTrim = [this] (const juce::Uuid& e, const juce::Uuid& r, bool atEnd) { openTrim (e, r, atEnd); };
        ctx->closeWindowByKey = [this] (const juce::String& key) { closeWindowLater (key); };
        ctx->closeAllWindows = [this] { juce::MessageManager::callAsync ([this] { windows.clear(); }); };
        ctx->closeTrim = [this] (const juce::Uuid& e) { closeWindowLater ("trim:" + e.toString()); };
        ctx->showDesign = [this] { openDesign(); };
        ctx->showBridge = [this] { toggleBridge(); };
        ctx->showBounce = [this] (const BounceContext& c) { openBounce (c); };
        ctx->showVideoExport = [this] (const juce::Uuid& editId, juce::Component* parent) { td::showVideoExport (*ctx, editId, parent); };
        ctx->showAafExport = [this] (const juce::Uuid& editId, juce::Component* parent) { td::showAafExport (*ctx, editId, parent); };
        ctx->showVersions = [this] (const juce::Uuid& editId, juce::Component* parent) { td::showVersions (*ctx, editId, parent); };
        ctx->windowIsOpen = [this] (const juce::String& key) { return windows.find (key) != windows.end(); };
        focusKeeper = std::make_unique<ToolWindow::FocusKeeper>();              // the keys never go dead after a text box lets go of the keyboard
        globalKeyHook() = [this] (const juce::KeyPress& k)
        {
            if (! ctx->hasProject()) return false;                                   // (the program's keys do nothing until a project exists)
            {   // Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo (a text box that has the keyboard does its own)
                const auto m = k.getModifiers();
                const int kc = k.getKeyCode();
                if ((m.isCtrlDown() || m.isCommandDown()) && ! m.isAltDown())
                {
                    if ((kc == 'Z' || kc == 'z') && ! m.isShiftDown()) { ctx->undoStep(); return true; }
                    if (kc == 'Y' || kc == 'y' || ((kc == 'Z' || kc == 'z') && m.isShiftDown())) { ctx->redoStep(); return true; }
                }
            }
            if (k.getModifiers().isAnyModifierKeyDown()) return false;
            const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
            if (c == 'm') { toggleProcessingMixer(); return true; }
            if (c == 'b') { toggleBridge(); return true; }
            if (c == 'u') { deskWindowToggle ("pitchpipe"); return true; }      // 3.0.1: U = the pitch pipe, I = the click: opens it, or closes it (a minimised one comes back), as the Stream Deck's keys do
            if (c == 'i') { deskWindowToggle ("click"); return true; }
            return false;
        };
        ctx->toggleMixerAt = [this] (int i) { toggleMixerAt (i); };
        ctx->mixerOpenAt = [this] (int i)
        {
            if (i == 0) i = contextMixerIndex();
            else if (i == 1) i = ctx->project.altMixerIndex();                  // the Stream Deck's second key is the Alt Mixer (not applied to the Edit mixer just chosen above)
            if (! juce::isPositiveAndBelow (i, (int) ctx->project.mixers.size())) return false;
            auto it = windows.find ("mixer:" + ctx->project.mixers[(size_t) i]->id.toString());
            return it != windows.end() && it->second != nullptr && it->second->isVisible();
        };
        {
            int port = remote::kDefaultPort;                                       // 0 in the settings file (remotePort) switches the control port off
            if (auto* st = ctx->props.getUserSettings()) port = st->getIntValue ("remotePort", remote::kDefaultPort);
            remoteServer = std::make_unique<RemoteControlServer> (*ctx, port);
        }
        // ---- the control surface (Avid S1 / Mackie Control over MIDI): the mixer it follows, the keys it sends to the work window, the strip it selects
        ctx->frontMixerId = [this]
        {
            auto& P = ctx->project;
            if (P.mixers.empty()) return juce::Uuid::null();
            ToolWindow* best = nullptr; juce::String bestKey;
            for (auto& kv : windows)
                if (kv.second != nullptr && kv.first.startsWith ("mixer:") && kv.second->isVisible() && (best == nullptr || kv.second->lastActive > best->lastActive)) { best = kv.second.get(); bestKey = kv.first; }
            juce::Uuid id = best != nullptr ? juce::Uuid (bestKey.fromFirstOccurrenceOf (":", false, false)) : recentMixer;
            for (auto& m : P.mixers) if (m->id == id) return id;
            return P.mixers.front()->id;
        };
        ctx->keyToWorkWindow = [this] (const juce::KeyPress& k)
        {
            const auto key = lastWorkWindowKey();
            auto it = windows.find (key);
            if (key.isEmpty() || it == windows.end() || it->second == nullptr) return false;
            if (auto* c = it->second->getContentComponent()) return c->keyPressed (k);
            return false;
        };
        ctx->selectStripInMixer = [this] (const juce::Uuid& mixerId, const juce::Uuid& trackId)
        {
            auto it = windows.find ("mixer:" + mixerId.toString());
            if (it != windows.end() && it->second != nullptr) if (auto* mc = dynamic_cast<MixerComponent*> (it->second->getContentComponent())) mc->selectStrip (trackId);
        };
        ctx->isStripSelectedInMixer = [this] (const juce::Uuid& mixerId, const juce::Uuid& trackId)
        {
            auto it = windows.find ("mixer:" + mixerId.toString());
            if (it != windows.end() && it->second != nullptr) if (auto* mc = dynamic_cast<MixerComponent*> (it->second->getContentComponent())) return mc->isStripSelected (trackId);
            return false;
        };
        ctx->showSurfacePanel = [this] { showWindow ("surface", "Mixer controller", [this] { return new SurfacePanel (*ctx); }); };
        ctx->showPitchPipe = [this] { showWindow ("pitchpipe", "Pitch pipe", [this] { return new PitchPipeComponent (*ctx); }); };
        ctx->showClick = [this] { showWindow ("click", "Click", [this] { return new ClickComponent (*ctx); }); };
        ctx->showHotKeys = [this] { showWindow ("hotkeys", "HotKeys", [] { return new HotKeysComponent(); }); };
        ctx->showRecordMonitor = [this] { showRecordMonitor(); };
        // ---- the Stream Deck + XL: windows by name, the Loop button, "minimise all but the main window"
        ctx->deskWindowState = [this] (const juce::String& what) { return deskWindowState (what); };
        ctx->deskWindowToggle = [this] (const juce::String& what) { deskWindowToggle (what); };
        ctx->deskLoopState = [this]
        {
            const auto key = lastWorkWindowKey();
            auto it = windows.find (key);
            if (key.isEmpty() || it == windows.end() || it->second == nullptr) return -1;
            auto* c = it->second->getContentComponent();
            if (auto* t = dynamic_cast<TakeWindowComponent*> (c)) return t->loopIsOn() ? 1 : 0;
            if (auto* e = dynamic_cast<EditWindowComponent*> (c)) return e->loopIsOn() ? 1 : 0;
            return -1;
        };
        ctx->deskLoopToggle = [this]
        {
            auto it = windows.find (lastWorkWindowKey());
            if (it == windows.end() || it->second == nullptr) return;
            auto* c = it->second->getContentComponent();
            if (auto* t = dynamic_cast<TakeWindowComponent*> (c)) t->toggleLoop();
            else if (auto* e = dynamic_cast<EditWindowComponent*> (c)) e->toggleLoop();
        };
        ctx->deskActState = [this] (const juce::String& what)
        {
            if (what == "slipl") return ctx->trimSlipLeft ? 1 : 0;
            if (what == "slipr") return ctx->trimSlipRight ? 1 : 0;
            const auto key = lastWorkWindowKey();
            auto it = windows.find (key);
            if (key.isEmpty() || it == windows.end() || it->second == nullptr) return -1;
            auto* c = it->second->getContentComponent();
            if (auto* t = dynamic_cast<TakeWindowComponent*> (c)) return t->deskActState (what);
            if (auto* e = dynamic_cast<EditWindowComponent*> (c)) return e->deskActState (what);
            return -1;
        };
        ctx->deskAct = [this] (const juce::String& what)
        {
            if (what == "slipl") { ctx->setTrimSlip (! ctx->trimSlipLeft, ctx->trimSlipRight); return; }
            if (what == "slipr") { ctx->setTrimSlip (ctx->trimSlipLeft, ! ctx->trimSlipRight); return; }
            auto it = windows.find (lastWorkWindowKey());
            if (it == windows.end() || it->second == nullptr) return;
            auto* c = it->second->getContentComponent();
            if (auto* t = dynamic_cast<TakeWindowComponent*> (c)) t->deskAct (what);
            else if (auto* e = dynamic_cast<EditWindowComponent*> (c)) e->deskAct (what);
        };
        ctx->showAlign = [this] (bool isEdit, const juce::Uuid& id)
        {
            if (auto it = windows.find ("align"); it != windows.end() && it->second != nullptr)       // open already: it measures on the window it was asked from now
            {
                auto* c = it->second->getContentComponent();
                if (auto* host = dynamic_cast<CornerStripHost*> (c)) c = host->content.get();
                if (auto* ac = dynamic_cast<AlignComponent*> (c)) ac->setSource (isEdit, id);
            }
            showWindow ("align", "Align spot mics", [this, isEdit, id] { return new AlignComponent (*ctx, isEdit, id); });
        };
        surface = std::make_unique<SurfaceController> (*ctx);
        ctx->surface = surface.get();
        ctx->showAudioSettings = [this] { openAudioSettings(); };
        ctx->showMedia = [this] { openMedia(); };
        ctx->showMastering = [this] { showWindow ("mastering", "Mastering", [this] { return new MasteringComponent (*ctx); }); };
        ctx->showOrganiser = [this] { showWindow ("organiser", "Display Organiser", [this] { return new OrganiserComponent (*ctx); }); };
        ctx->showTakeDisplay = [this] { showWindow ("takedisplay", "Take Display", [this] { return new TakeDisplayControl (*ctx); }); };
        ctx->listWindows = [this]
        {
            std::vector<AppContext::WindowEntry> out;
            auto recencyOf = [this] (const juce::String& key, bool& open) { auto it = windows.find (key); open = it != windows.end() && it->second != nullptr; return open ? it->second->lastActive : -1; };
            auto add = [&] (const juce::String& key, const juce::String& title, const char* kind) { AppContext::WindowEntry e; e.key = key; e.title = title; e.kind = kind; e.recency = recencyOf (key, e.open); out.push_back (e); };
            { AppContext::WindowEntry e; e.key = "main"; e.title = "Main window"; e.kind = "main"; e.open = true; e.recency = 0; out.push_back (e); }
            // every take window, edit window and mixer of the project (open or not: choosing one that is closed opens it) ...
            for (auto& t : ctx->project.takeWindows) add ("take:" + t->id.toString(), t->name, "take");
            for (auto& e : ctx->project.edits)       add ("edit:" + e->id.toString(), e->name, "edit");
            for (auto& m : ctx->project.mixers)      add ("mixer:" + m->id.toString(), m->name, "mixer");
            // ... and the other windows that are open now (not the Organiser itself, nor a Take Display: that lives on its own screen)
            for (auto& [key, w] : windows)
            {
                if (w == nullptr || key == "organiser" || key == "takedisplay" || key.startsWith ("take:") || key.startsWith ("edit:") || key.startsWith ("mixer:")) continue;
                add (key, w->getName(), "other");
            }
            return out;
        };
        ctx->windowPlacesNow = [this]
        {
            AppContext::WindowPlaces p;
            if (mainWindow == nullptr) return p;
            p.area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (mainWindow->getBounds())->userArea;
            if (! mainWindow->isMinimised()) p.windows.push_back ({ "main", mainWindow->getBounds() });
            for (auto& [key, w] : windows)
            {
                if (w == nullptr || key == "organiser" || key == "takedisplay" || ! w->isVisible() || w->isMinimised() || DockHub::get().contains (w.get())) continue;
                const auto b = w->getBounds();
                if (b.getWidth() < 160 || b.getHeight() < 100 || ! b.intersects (p.area)) continue;
                if (b.getIntersection (p.area).getWidth() * b.getIntersection (p.area).getHeight() < b.getWidth() * b.getHeight() / 2) continue;   // mostly on another screen
                p.windows.push_back ({ key, b });
            }
            return p;
        };
        ctx->placeWindows = [this] (const std::vector<std::pair<juce::String, juce::Rectangle<float>>>& items)
        {
            auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (mainWindow->getBounds())->userArea;
            bool mainIncluded = false;
            for (auto& it : items) if (it.first == "main") mainIncluded = true;
            if (! mainIncluded)
            {
                // the main window stays exactly as it is (full screen or not); the other windows are arranged below its menu bar
                if (mainWindow->isMinimised()) mainWindow->setMinimised (false);
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                {
                    const int bottom = mc->menuBarBottomOnScreen();
                    if (bottom > area.getY() && bottom < area.getBottom()) area.removeFromTop (juce::jlimit (0, area.getHeight() / 3, bottom - area.getY() + 2));
                }
            }
            for (auto& it : items)                                         // windows that are not open yet are opened first
            {
                if (windows.find (it.first) != windows.end() || it.first == "main") continue;
                const auto id = juce::Uuid (it.first.fromFirstOccurrenceOf (":", false, false));
                if (it.first.startsWith ("take:")) openTakeWindow (id);
                else if (it.first.startsWith ("edit:")) openEdit (id);
                else if (it.first.startsWith ("mixer:")) openMixer (id);
            }
            for (auto& [key, cell] : items)
            {
                juce::DocumentWindow* w = key == "main" ? static_cast<juce::DocumentWindow*> (mainWindow.get()) : nullptr;
                if (w == nullptr) { auto it = windows.find (key); if (it != windows.end()) w = it->second.get(); }
                if (w == nullptr) continue;
                const juce::Rectangle<int> r (area.getX() + (int) std::round (cell.getX() * area.getWidth()), area.getY() + (int) std::round (cell.getY() * area.getHeight()),
                                              (int) std::round (cell.getWidth() * area.getWidth()), (int) std::round (cell.getHeight() * area.getHeight()));
                DockHub::get().show (w);                           // a minimised window comes back from the bar
                if (w->isMinimised()) w->setMinimised (false);
                if (w->isFullScreen()) w->setFullScreen (false);
                w->setBounds (r);
            }
        };
        ctx->showScope = [this] (const juce::Uuid& id)
        {
            juce::String nm = "mixer";
            for (auto& m : ctx->project.mixers) if (m->id == id) nm = m->name;
            showWindow ("scope:" + id.toString(), "Phase scope - " + nm, [this, id] { return new PhaseScopeComponent (*ctx, id); });
        };
        ctx->mediaReveal = [this] (const juce::File& f)
        {
            auto it = windows.find ("media");
            if (it != windows.end()) if (auto* mc = dynamic_cast<MediaComponent*> (it->second->getContentComponent())) mc->selectFile (f);   // the window need not be in front
        };
        ctx->refreshTitles = [this] { refreshWindows(); };
        busy::setAreaProvider ([this] { return mainWindow != nullptr && mainWindow->isVisible() ? mainWindow->getScreenBounds() : juce::Rectangle<int>(); });   // the "please wait" window sits over the main window

        // audio device: restore the last choice, otherwise prefer ASIO
        std::unique_ptr<juce::XmlElement> saved (ctx->props.getUserSettings()->getXmlValue ("audioDevice"));
        for (auto* type : ctx->devices.getAvailableDeviceTypes())
            juce::Logger::writeToLog ("Audio driver type available: " + type->getTypeName());
        juce::Logger::writeToLog ("Opening the saved / default audio device...");
        auto err = ctx->devices.initialise (256, 256, saved.get(), true, {}, nullptr);
        if (saved == nullptr)
            for (auto* type : ctx->devices.getAvailableDeviceTypes())
                if (type->getTypeName() == "ASIO") { ctx->devices.setCurrentAudioDeviceType ("ASIO", true); break; }

        juce::Logger::writeToLog ("Audio device init result: " + (err.isEmpty() ? juce::String ("OK") : err));
        {
            juce::File toOpen;                                         // a project double-clicked in Explorer
            for (auto& a : juce::StringArray::fromTokens (commandLine, true))
                if ((a.unquoted().endsWithIgnoreCase (".fermata") || a.unquoted().endsWithIgnoreCase (".takedaw")) && juce::File (a.unquoted()).existsAsFile()) toOpen = juce::File (a.unquoted());
            if (toOpen == juce::File() || ! ctx->openProject (toOpen)) ctx->startBlankProject();      // otherwise the start screen asks where the project goes
        }
        ctx->devices.addAudioCallback (&ctx->engine);

        mainWindow = std::make_unique<ToolWindowMain> (*this);
        mainWindow->onActivated = [this] { if (! ownAllWindows()) raiseToolWindows(); };
        ctx->plugins.adoptWindow = [this] (juce::Component& c) { return mainWindow != nullptr && makeOwnedBy (c, *mainWindow); };
        ctx->mainWindowArea = [this] { return mainWindow != nullptr ? mainWindow->getBounds() : juce::Rectangle<int>(); };
        mainWindow->setIcon (makeFermataIcon (256));
        mainWindow->setContentOwned (new MainComponent (*ctx), true);
        // never taller / wider than the screen (on a laptop the bottom of the window, with the arm buttons, would be off the edge)
        {
            auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea.reduced (8);
            int minW = 700;
            if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent())) minW = juce::jmax (minW, mc->menuBarWidth() + 4);     // the whole menu bar on one line
            mainWindow->setResizeLimits (juce::jmin (minW, area.getWidth()), 480, 8000, 8000);
            constexpr int titleBarH = 32;                                  // the window's own title bar sits above its content: leave room for it
            mainWindow->centreWithSize (juce::jmin (juce::jmax (mainWindow->getWidth(), minW), area.getWidth()), juce::jmin (mainWindow->getHeight(), area.getHeight() - titleBarH));
            mainWindow->setBounds (mainWindow->getBounds().constrainedWithin (area));
        }
        mainWindow->setVisible (true);
        mainWindow->setFullScreen (true);                              // opens maximised every time (the title bar and the taskbar still show); the size above is what "Restore" goes back to
        if (splash != nullptr)
            splash->startCountdown ([this] { juce::MessageManager::callAsync ([this] { splash.reset(); }); });
        if (err.isNotEmpty()) showError ("Audio device", err + "\n\nOpen \"Audio settings\" to choose your ASIO device.");
        announceCrashReports();
    }

    /** 3.14.0: crash or freeze reports written since the last start are mentioned once (in the log, and in a message after the splash). */
    void announceCrashReports()
    {
        auto* st = ctx != nullptr ? ctx->props.getUserSettings() : nullptr;
        if (st == nullptr) return;
        const juce::Time last (st->getValue ("crashCheck", "0").getLargeIntValue());
        st->setValue ("crashCheck", juce::String (juce::Time::getCurrentTime().toMilliseconds()));
        st->saveIfNeeded();
        const auto fresh = crashreport::reportsSince (last);
        crashreport::prune (5);
        if (last.toMilliseconds() == 0 || fresh.isEmpty()) return;          // (the first start of 3.14.0 has nothing to compare with)
        juce::StringArray names; for (auto& f : fresh) names.add (f.getFileName());
        juce::Logger::writeToLog ("Crash reports written since the last start: " + names.joinIntoString (", "));
        const bool froze = fresh.getFirst().getFileName().contains (" freeze ");
        const auto newest = fresh.getFirst();
        juce::Timer::callAfterDelay (4000, [newest, froze]
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Crash report")
                                              .withMessage (juce::String (froze ? "Last time, Fermata got stuck for a long time" : "Last time, Fermata closed unexpectedly")
                                                            + ", and it saved a report of what it was doing:\n" + newest.getFileName()
                                                            + "\n\nTo help find the cause, send that file together with fermata-log.txt (both are in Documents\\Fermata). Help > Show the crash reports finds it again later.")
                                              .withButton ("Show the report").withButton ("OK"),
                                          [newest] (int r) { if (r == 1) newest.revealToUser(); });
        });
    }

    void shutdown() override
    {
        closing::begin();                                                         // 3.15.0: every step into the log, with its time (ClosingLog.h)
        busy::setAreaProvider (nullptr);
        splash.reset();
        remoteServer.reset();
        closing::step ("Stream Deck / remote connections closed");
        // The fix tools' windows (ReTouch, De-click, Noise mode ...) are modal dialogs that JUCE would delete only after this object - and the
        // AppContext their destructors talk to (stop playback, put back what was not written, the undo hold) - had gone. Delete them first.
        // (Only DialogWindows: those are always "deleted when dismissed", so nothing else owns them.)
        for (int i = juce::ModalComponentManager::getInstance()->getNumModalComponents(); --i >= 0;)
            if (auto* dw = dynamic_cast<juce::DialogWindow*> (juce::ModalComponentManager::getInstance()->getModalComponent (i))) delete dw;
        closing::step ("tool windows closed");
        if (ctx) ctx->surface = nullptr;
        surface.reset();                                                          // (the surface is blanked: faders down, lamps off)
        closing::step ("control surface blanked");
        if (ctx) ctx->shutdown();
        CoreReservation::get().shutdown();                                       // other programs are given all the cores back
        closing::step ("CPU cores handed back");
        windows.clear();
        closing::step ("windows closed");
        mainWindow.reset();
        closing::step ("main window closed");
        ctx.reset();
        tooltips.reset();
        focusKeeper.reset();
        globalKeyHook() = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
        closing::end();
        juce::Logger::setCurrentLogger (nullptr);
        fileLogger.reset();
    }

    void systemRequestedQuit() override
    {
        if (ctx && ctx->engine.isRecording())
        {
            showError ("Recording in progress", "Stop recording before quitting.");
            return;
        }
        if (quitAsking) return;                                                  // the question is already on screen
        if (! ctx || ctx->project.projectFile == juce::File()) { quit(); return; }
        quitAsking = true;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Quit Fermata")
                                          .withMessage ("Do you want to save your work before quitting?\n\n(The project is also saved automatically as you work; \"Quit without saving\" skips only the final save.)")
                                          .withButton ("Save and quit").withButton ("Quit without saving").withButton ("Cancel"),
                                      [this] (int result)
                                      {
                                          quitAsking = false;
                                          if (result == 1) saveAndQuit();                                          // Save and quit
                                          else if (result == 2) { if (ctx) ctx->skipFinalSave = true; quit(); }   // Quit without saving
                                          // anything else (Cancel): carry on working
                                      });
    }
    /** "Save and quit": the program quits only once the project is on the disk. A save that fails used to go unseen (the message box came after
        quit(), so it was never shown): now Fermata stays open and asks - stay, or quit without this last save (what was saved automatically before is untouched). */
    void saveAndQuit()
    {
        juce::String err;
        if (ctx == nullptr || ctx->saveNow (false, &err)) { quit(); return; }
        quitAsking = true;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Could not save project")
                                          .withMessage ("The project could not be saved:\n\n" + err + "\n\nFermata stays open so nothing is lost: free the drive and save again (Project > Save, or Save as... to another drive), "
                                                        "or quit without this last save. What was saved automatically before is on the disk as it was.")
                                          .withButton ("Stay").withButton ("Quit without saving"),
                                      [this] (int r)
                                      {
                                          quitAsking = false;
                                          if (r == 2) { if (ctx) ctx->skipFinalSave = true; quit(); }
                                      });
    }
    bool quitAsking = false;

private:
    class ToolWindowMain : public juce::DocumentWindow
    {
    public:
        explicit ToolWindowMain (FermataApplication&) : juce::DocumentWindow (juce::String ("Fermata"), theme::window, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, true);
        }
        /** The main window is only a background: whenever it comes to the front, all the other windows go back in front of it. */
        void activeWindowStatusChanged() override
        {
            juce::DocumentWindow::activeWindowStatusChanged();
            if (isActiveWindow() && onActivated) juce::MessageManager::callAsync ([cb = onActivated] { cb(); });
        }
        std::function<void()> onActivated;
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        void resized() override { juce::DocumentWindow::resized(); placeThemeToggle (*this, themeToggle); }
        ThemeToggleButton themeToggle;
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (globalKeyHook() && globalKeyHook() (k)) return true;
            if (k.getKeyCode() == juce::KeyPress::tabKey)        // (as in ToolWindow: Tab from the window as a whole must not land in its first text box)
                if (auto* f = juce::Component::getCurrentlyFocusedComponent(); f == nullptr || f == this || f == getContentComponent()) return true;
            if (ToolWindow::isPlainSpace (k))                    // the main window has no transport of its own: Space plays / stops the take or edit window you were last in
                if (auto* t = ToolWindow::lastTransportWindow())
                    if (auto* c = t->getContentComponent()) if (c->keyPressed (k)) return true;
            return juce::DocumentWindow::keyPressed (k);
        }
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override {}

    /** Windows: every tool window is OWNED by the main window, so none can ever go behind it and they minimise / restore with it. */
    bool ownAllWindows()
    {
        if (mainWindow == nullptr) return false;
        bool ok = true;
        for (auto& [key, w] : windows) if (w != nullptr) ok = makeOwnedBy (*w, *mainWindow) && ok;
        if (audioSettings != nullptr) ok = makeOwnedBy (*audioSettings, *mainWindow) && ok;
        ctx->plugins.forEachEditor ([&] (juce::Component& c) { ok = makeOwnedBy (c, *mainWindow) && ok; });
        return ok;
    }
    /** The fallback (where windows cannot be owned): when the main window comes forward, every window goes back in front of it, in the
        order they were last used, so the one you were working in stays on top. */
    void raiseToolWindows()
    {
        std::vector<ToolWindow*> list;
        for (auto& [key, w] : windows) if (w != nullptr && w->isVisible()) list.push_back (w.get());
        if (audioSettings != nullptr && audioSettings->isVisible()) list.push_back (audioSettings.get());
        std::stable_sort (list.begin(), list.end(), [] (ToolWindow* a, ToolWindow* b) { return a->lastActive < b->lastActive; });
        for (auto* w : list) w->toFront (false);
        ctx->plugins.forEachEditor ([] (juce::Component& c) { c.toFront (false); });         // plug-in windows stay above the rest
    }
    void closeWindowLater (const juce::String& key)
    {
        juce::MessageManager::callAsync ([this, key] { windows.erase (key); });
    }
    /** A key that opens / closes a window (the Stream Deck, the control surface, M, B) closes it - but never while a menu, a message or a call-out is up.
        Those are modal, one of them may belong to that window, and its answer would then come back to a window that has been deleted (2.98.0: the
        Stream Deck's keys arrive at any time, also while a menu is open). */
    void closeFromKey (const juce::String& key)
    {
        if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0) { ctx->setNotice ("Close the open menu or message first, then press the key again."); return; }
        if (! transportWindowMayClose (key)) return;
        closeWindowLater (key);
    }
    /** A take or edit window is being closed (its X, or a key): what it is playing stops with it (the engine plays on its own, so the sound went on with
        nothing left to stop it from), and the take window being recorded into is not closed at all. False = leave it open. */
    bool transportWindowMayClose (const juce::String& key)
    {
        const bool take = key.startsWith ("take:"), edit = key.startsWith ("edit:");
        if (! take && ! edit) return true;
        const juce::Uuid id (key.fromFirstOccurrenceOf (":", false, false));
        if (take && ctx->engine.isRecording() && (ctx->recordTarget == id || ctx->project.findTake (id, ctx->engine.currentTakeId()) != nullptr))
        {
            showError ("Recording", "This take window is recording. Stop the recording first, then close the window.");
            return false;
        }
        if (ctx->isPlaying())
        {
            const auto& pi = ctx->playInfo;
            if ((take && pi.kind == AppContext::PlayInfo::Kind::Take && pi.windowId == id) || (edit && pi.kind == AppContext::PlayInfo::Kind::Edit && pi.id == id))
                ctx->stopPlayback();
        }
        return true;
    }

    /** 'stayInFront' (optional): a window the user is working in. An already open window is then left exactly where it is; a NEW window always opens on top. */
    template <typename Make>
    void showWindow (const juce::String& key, const juce::String& title, Make make, juce::Component* stayInFront = nullptr)
    {
        auto it = windows.find (key);
        if (it != windows.end())
        {
            it->second->setName (title);
            if (stayInFront == nullptr) DockHub::get().show (it->second.get());     // when sent to from another window it is left exactly as it is: behind, minimised, wherever
            return;
        }
        if (stayInFront != nullptr) return;                                          // not open, and the user is working elsewhere: do not open it (the work is already in the project)
        juce::Component* content = make();
        if (key == "organiser" || key == "takedisplay" || key == "bridge" || key == "pitchpipe" || key == "click" || key == "align") content = new CornerStripHost (content);     // (their pin and sun / moon buttons get a strip of their own)
        auto w = std::make_unique<ToolWindow> (title, [this, key] (ToolWindow*)
        {
            if (! transportWindowMayClose (key)) return;
            juce::MessageManager::callAsync ([this, key] { windows.erase (key); });
        });
        w->setContentOwned (content, true);
        // every window's contents take the keyboard themselves (the keys then come to them, and on to the window), so a window never hands it to
        // the first text box it finds instead: JUCE does that when a window is activated with nothing in it holding the keyboard - a click on a
        // mixer's title bar put the keys into its name box, and R / Space were "typing" from then on
        content->setWantsKeyboardFocus (true);
        if (auto* host = dynamic_cast<CornerStripHost*> (content)) host->content->setWantsKeyboardFocus (true);
        w->setIcon (makeFermataIcon (256));
        w->isTransportWindow = key.startsWith ("take:") || key.startsWith ("edit:");
        w->forwardKeys = key.startsWith ("mixer:") || key.startsWith ("scope:") || key == "bridge" || key == "media" || key == "takedisplay" || key == "organiser" || key == "pitchpipe" || key == "click" || key == "align" || key == "hotkeys";
        juce::Rectangle<int> mixerRemembered;                                         // a mixer's size when it was last closed (put back by fitWindowToStrips)
        {   // where it was left last time (kept in the project), or else centred; never wider than the screen, nor taller
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto area = (mainWindow != nullptr ? displays.getDisplayForRect (mainWindow->getBounds()) : displays.getPrimaryDisplay())->userArea;
            juce::Rectangle<int> saved;
            if (auto sit = ctx->project.windowBounds.find (key); sit != ctx->project.windowBounds.end()) saved = juce::Rectangle<int>::fromString (sit->second);
            bool onScreen = false;                                                   // a remembered place counts only if its title bar can still be reached
            for (auto& d : displays.displays) if (d.userArea.intersects (juce::Rectangle<int> (saved.getX() + 20, saved.getY() - 10, 120, 30))) onScreen = true;
            const bool isMixer = key.startsWith ("mixer:");
            if (saved.getWidth() >= 120 && saved.getHeight() >= 80 && onScreen)
            {
                if (isMixer) { w->setTopLeftPosition (saved.getPosition()); mixerRemembered = saved; }     // a mixer: its size is put back once it knows its strips (below)
                else w->setBounds (saved);
            }
            else
            {
                const bool mixer = key.startsWith ("mixer:");
                w->setSize (juce::jmin (w->getWidth(), area.getWidth() - 20), mixer ? w->getHeight() : juce::jmin (w->getHeight(), area.getHeight() - 100));
                w->centreWithSize (w->getWidth(), w->getHeight());
                if (mixer) w->setTopLeftPosition (w->getX(), juce::jmax (w->getY(), area.getY() + 44));
            }
        }
        const bool owned = mainWindow != nullptr && makeOwnedBy (*w, *mainWindow);
        w->setVisible (true);
        if (auto* mc = dynamic_cast<MixerComponent*> (content)) mc->fitWindowToStrips (mixerRemembered);    // (the size it was left at, within the screen)
        w->onBoundsChanged = [this, key] (const juce::String& t) { ctx->project.windowBounds[key] = t; };       // from now on, every move / resize is remembered in the project
        auto* raw = w.get();
        windows[key] = std::move (w);
        juce::ignoreUnused (stayInFront);                     // a NEW window always opens on top of the others, even over a maximised one
        if (! owned) raiseToolWindows();
        raw->toFront (true);                                  // the new window always ends up in front, with the keyboard
        raw->grabContentFocus();
        // A window that opens where it covers the menu bar (a remembered place from a bigger screen, or opened maximised) is moved below it, and the bar is
        // brought back in front: it must always be visible. (keepTitleBarClear only ran when a window was moved or resized, not when it first appeared.)
        juce::Component::SafePointer<ToolWindow> safeNew (raw);
        juce::MessageManager::callAsync ([safeNew]
        {
            if (auto* nw = safeNew.getComponent()) nw->keepTitleBarClear();
            if (menuBarRaise()) menuBarRaise()();
        });
    }

    // ---- the T / E / M squares of the menu bar
    juce::Uuid recentTake, recentEdit, recentMixer;                                    // what was opened last (for when no such window is open any more)
    /** The window of this kind ("take", "edit", "mixer") that was last in front: brought back to the front (restored if minimised, opened if it was closed). */
    void focusRecent (const juce::String& kind)
    {
        const juce::String prefix = kind + ":";
        ToolWindow* best = nullptr; juce::String bestKey;
        for (auto& kv : windows)
            if (kv.second != nullptr && kv.first.startsWith (prefix) && (best == nullptr || kv.second->lastActive > best->lastActive)) { best = kv.second.get(); bestKey = kv.first; }
        juce::Uuid id = best != nullptr ? juce::Uuid (bestKey.fromFirstOccurrenceOf (":", false, false)) : juce::Uuid::null();
        auto& P = ctx->project;
        if (id.isNull())
        {
            if (kind == "take")       { id = recentTake;  if (P.findTakeWindow (id) == nullptr) id = P.takeWindows.empty() ? juce::Uuid::null() : P.takeWindows.front()->id; }
            else if (kind == "edit")  { id = recentEdit;  if (P.findEdit (id) == nullptr)       id = P.edits.empty()       ? juce::Uuid::null() : P.edits.front()->id; }
            else                      { id = recentMixer; bool ok = false; for (auto& m : P.mixers) ok = ok || m->id == id; if (! ok) id = P.mixers.empty() ? juce::Uuid::null() : P.mixers.front()->id; }
        }
        if (id.isNull()) return;
        if (kind == "take") openTakeWindow (id); else if (kind == "edit") openEdit (id); else openMixer (id);
        if (auto it = windows.find (prefix + id.toString()); it != windows.end() && it->second != nullptr) { DockHub::get().show (it->second.get()); it->second->toFront (true); it->second->grabContentFocus(); }
    }
    /** The edit or take window that was last in front ("edit:<id>" / "take:<id>"), whether or not Fermata is the program in use right now. */
    juce::String lastWorkWindowKey() const
    {
        ToolWindow* best = nullptr; juce::String bestKey;
        for (auto& kv : windows)
        {
            auto* w = kv.second.get();
            if (w == nullptr || ! (kv.first.startsWith ("edit:") || kv.first.startsWith ("take:"))) continue;
            if (best == nullptr || w->lastActive > best->lastActive) { best = w; bestKey = kv.first; }
        }
        return bestKey;
    }
    /** Which kind of window the person is working in right now: the Fermata window that was last in front, while Fermata is the program in use. "" = none of the three. */
    /** 0: no window of this kind is open, 1: one is open, 2: one is the window in use, 3: open but minimised (all of them). */
    int windowLight (const juce::String& kind) const
    {
        const auto prefix = kind + ":";
        bool any = false, shown = false;
        for (auto& kv : windows)
        {
            auto* w = kv.second.get();
            if (w == nullptr || ! kv.first.startsWith (prefix)) continue;
            any = true;
            if (w->isVisible() && ! w->isMinimised() && ! DockHub::get().contains (w)) shown = true;
        }
        if (! any) return 0;
        if (activeKind() == kind) return 2;
        return shown ? 1 : 3;
    }

    juce::String activeKind() const
    {
        if (! juce::Process::isForegroundProcess()) return {};
        ToolWindow* best = nullptr; juce::String bestKey;
        for (auto& kv : windows)
        {
            auto* w = kv.second.get();
            if (w == nullptr || ! w->isVisible() || DockHub::get().contains (w)) continue;
            if (best == nullptr || w->lastActive > best->lastActive) { best = w; bestKey = kv.first; }
        }
        if (best == nullptr || best->lastActive == 0 || ! best->isActiveWindow()) return {};
        if (bestKey.startsWith ("take:")) return "take";
        if (bestKey.startsWith ("edit:")) return "edit";
        if (bestKey.startsWith ("mixer:")) return "mixer";
        return {};
    }

    void openTakeWindow (const juce::Uuid& id)
    {
        auto* d = ctx->project.findTakeWindow (id);
        if (d == nullptr) return;
        recentTake = id;
        showWindow ("take:" + id.toString(), "Takes - " + d->name, [this, id] { return new TakeWindowComponent (*ctx, id); });
    }
    void openMixer (const juce::Uuid& id)
    {
        for (auto& m : ctx->project.mixers)
            if (m->id == id)
            {
                recentMixer = id;
                showWindow ("mixer:" + id.toString(), "Mixer - " + m->name, [this, id] { return new MixerComponent (*ctx, id); });
                return;
            }
    }
    /** What M and the first Stream Deck key mean right now: the mixer of the Edit you are working in. "Working in" = the Edit window, Trim window or Edit mixer that was
        most recently in front of all the other Fermata windows (not just the one that is in front this very moment, which can be the menu bar, another program, or the Stream Deck's focus).
        If the most recent one is an Edit mixer that is still open, the key closes it again. Anywhere else (a take window, the processing mixer ...) it is the processing mixer (index 0). */
    int contextMixerIndex() const
    {
        auto& P = ctx->project;
        ToolWindow* best = nullptr; juce::String bestKey;
        for (auto& kv : windows)
        {
            auto* w = kv.second.get();
            if (w == nullptr || ! w->isVisible() || DockHub::get().contains (w)) continue;
            if (best == nullptr || w->lastActive > best->lastActive) { best = w; bestKey = kv.first; }
        }
        if (best == nullptr || best->lastActive == 0) return 0;
        if (bestKey.startsWith ("edit:") || bestKey.startsWith ("trim:"))
        {
            if (auto* m = P.mixerOfEdit (juce::Uuid (bestKey.fromFirstOccurrenceOf (":", false, false))))
                for (size_t i = 0; i < P.mixers.size(); ++i) if (P.mixers[i].get() == m) return (int) i;
        }
        else if (bestKey.startsWith ("mixer:"))
        {
            const juce::Uuid id (bestKey.fromFirstOccurrenceOf (":", false, false));
            for (size_t i = 0; i < P.mixers.size(); ++i) if (P.mixers[i]->id == id && ! P.mixers[i]->editId.isNull()) return (int) i;
        }
        return 0;
    }
    /** M: opens the processing mixer (or the mixer of the Edit window you are in), or closes it if it is open. */
    void toggleProcessingMixer() { toggleMixerAt (0); }
    /** The mixer with this index (0 = the processing mixer, 1 = the next one, usually the producer's): opens it, or closes it if it is open. */
    void toggleMixerAt (int index)
    {
        if (index == 0) index = contextMixerIndex();
        else if (index == 1) index = ctx->project.altMixerIndex();             // the second key is the Alt Mixer (the one ticked in its window, else the first cue mixer); never applied to an Edit mixer chosen by the line above, which can itself be number 1
        if (! juce::isPositiveAndBelow (index, (int) ctx->project.mixers.size())) return;
        const auto id = ctx->project.mixers[(size_t) index]->id;
        const auto key = "mixer:" + id.toString();
        auto it = windows.find (key);
        if (it != windows.end() && ! DockHub::get().contains (it->second.get()))
        {
            bool frontmost = it->second->isVisible();                          // only close it if it is the window you were last in; if it is hidden behind others, bring it forward instead
            for (auto& kv : windows) if (kv.second != nullptr && kv.second->isVisible() && kv.second->lastActive > it->second->lastActive) frontmost = false;
            if (frontmost) closeFromKey (key); else DockHub::get().show (it->second.get());
        }
        else openMixer (id);
    }
    /** B: opens the meter bridge, a floating window with a meter for every driver input and output, or closes it. */
    void toggleBridge()
    {
        const juce::String key = "bridge";
        if (auto bi = windows.find (key); bi != windows.end()) { if (DockHub::get().contains (bi->second.get())) DockHub::get().show (bi->second.get()); else closeFromKey (key); return; }
        showWindow (key, "Meter bridge", [this] { return new MeterBridgeComponent (*ctx); });
        if (auto it = windows.find (key); it != windows.end())
        {
            it->second->setAlwaysOnTop (true);                 // floats above the other windows (of every program)
            bridgeTop = std::make_unique<StayOnTop> (*it->second);
            if (ctx->project.windowBounds.find (key) == ctx->project.windowBounds.end()) { it->second->setSize (1100, 420); it->second->setTopLeftPosition (20, 20); }
        }
    }
    /** The Recording monitor (3.2.0): the three lamps and the take's events, floating above every window like the meter bridge. */
    void showRecordMonitor()
    {
        const juce::String key = "recmon";
        if (auto it = windows.find (key); it != windows.end()) { DockHub::get().show (it->second.get()); return; }
        showWindow (key, "Recording monitor", [this] { return new RecordMonitorComponent (*ctx); });
        if (auto it = windows.find (key); it != windows.end())
        {
            it->second->setAlwaysOnTop (true);
            recMonTop = std::make_unique<StayOnTop> (*it->second);
            if (ctx->project.windowBounds.find (key) == ctx->project.windowBounds.end()) it->second->setTopLeftPosition (40, 60);
        }
    }
    // ---- the Stream Deck + XL's window keys
    /** Minimising: the window is hidden and shows as a title bar along the bottom of the main window (what its own minimise button does). */
    static void minimiseToDock (juce::DocumentWindow* w)
    {
        if (w == nullptr) return;
        if (w->isMinimised()) w->setMinimised (false);
        w->setVisible (false);
        DockHub::get().add (w);
    }
    /** The take window the "take" key means: the one used last (else the one opened last, else the first). */
    juce::Uuid deskTakeWindowId() const
    {
        juce::Uuid id = ctx->lastTakeWindowId ? ctx->lastTakeWindowId() : recentTake;
        if (ctx->project.findTakeWindow (id) == nullptr) id = ctx->project.takeWindows.empty() ? juce::Uuid::null() : ctx->project.takeWindows.front()->id;
        return id;
    }
    juce::String deskKeyFor (const juce::String& what) const
    {
        if (what == "editwin")
        {
            const ToolWindow* best = nullptr; juce::String bestKey;               // the edit window used last, as for the take key
            for (auto& kv : windows)
                if (kv.second != nullptr && kv.first.startsWith ("edit:") && (best == nullptr || kv.second->lastActive > best->lastActive)) { best = kv.second.get(); bestKey = kv.first; }
            if (best != nullptr) return bestKey;
            juce::Uuid id = recentEdit;
            if (ctx->project.findEdit (id) == nullptr) id = ctx->project.edits.empty() ? juce::Uuid::null() : ctx->project.edits.front()->id;
            return id.isNull() ? juce::String() : "edit:" + id.toString();
        }
        if (what != "take") return what;              // "bridge", "pitchpipe", "click", "media" ... are their own keys
        const auto id = deskTakeWindowId();
        return id.isNull() ? juce::String() : "take:" + id.toString();
    }
    /** 0 not open, 1 open and shown, 3 open but minimised. */
    int deskWindowState (const juce::String& what) const
    {
        if (what == "minimiseall") return deskMinimisedSome() ? 3 : 0;
        const auto key = deskKeyFor (what);
        auto it = key.isEmpty() ? windows.end() : windows.find (key);
        if (it == windows.end() || it->second == nullptr) return 0;
        return it->second->isVisible() && ! DockHub::get().contains (it->second.get()) ? 1 : 3;
    }
    void deskWindowToggle (const juce::String& what)
    {
        if (! ctx->hasProject()) return;
        if (what == "bridge") { toggleBridge(); return; }                   // opens it, or closes it
        if (what == "pitchpipe" || what == "click")
        {
            if (auto it = windows.find (what); it != windows.end() && it->second != nullptr)
            {
                if (DockHub::get().contains (it->second.get())) DockHub::get().show (it->second.get());       // minimised: back
                else closeFromKey (what);
            }
            else if (what == "pitchpipe") ctx->showPitchPipe(); else ctx->showClick();
            return;
        }
        if (what == "take")
        {
            const auto id = deskTakeWindowId();
            if (id.isNull()) return;
            auto it = windows.find ("take:" + id.toString());
            if (it == windows.end() || it->second == nullptr) { openTakeWindow (id); return; }                // not open: open it
            auto* w = it->second.get();
            if (w->isVisible() && ! DockHub::get().contains (w)) minimiseToDock (w);                           // shown: minimise it (never close it)
            else { DockHub::get().show (w); w->grabContentFocus(); }                                           // minimised or hidden: bring it back
            return;
        }
        if (what == "editwin" || what == "takedisplay" || what == "organiser" || what == "media" || what == "mastering")   // open / close
        {
            const auto key = deskKeyFor (what);
            auto it = key.isEmpty() ? windows.end() : windows.find (key);
            if (it != windows.end() && it->second != nullptr)
            {
                if (DockHub::get().contains (it->second.get()) || ! it->second->isVisible()) { DockHub::get().show (it->second.get()); it->second->grabContentFocus(); }   // minimised: back
                else closeFromKey (key);                                                                                                                             // shown: close
                return;
            }
            if (what == "editwin") { if (key.isNotEmpty()) openEdit (juce::Uuid (key.fromFirstOccurrenceOf (":", false, false))); }
            else if (what == "takedisplay") ctx->showTakeDisplay();
            else if (what == "organiser") ctx->showOrganiser();
            else if (what == "media") openMedia();
            else if (what == "mastering") ctx->showMastering();
            return;
        }
        if (what == "minimiseall") minimiseAllButMain();
    }
    /** Every window but the main one goes to the bar along the bottom of the main window. Pressed again, the ones it hid come back (the most recently used on top). */
    void minimiseAllButMain()
    {
        if (deskMinimisedSome())
        {
            std::vector<ToolWindow*> back;
            for (auto& p : deskMinimised) if (auto* w = p.getComponent()) if (DockHub::get().contains (w)) back.push_back (dynamic_cast<ToolWindow*> (w));
            std::stable_sort (back.begin(), back.end(), [] (ToolWindow* a, ToolWindow* b) { return (a != nullptr ? a->lastActive : 0) < (b != nullptr ? b->lastActive : 0); });
            for (auto& p : deskMinimised) if (auto* w = p.getComponent()) if (DockHub::get().contains (w) && dynamic_cast<ToolWindow*> (w) == nullptr) DockHub::get().show (w);     // (plug-in windows)
            for (auto* w : back) if (w != nullptr) DockHub::get().show (w);
            deskMinimised.clear();
            return;
        }
        deskMinimised.clear();
        auto hide = [this] (juce::DocumentWindow* w)
        {
            if (w == nullptr || ! w->isVisible() || DockHub::get().contains (w)) return;
            deskMinimised.push_back (juce::Component::SafePointer<juce::DocumentWindow> (w));
            minimiseToDock (w);
        };
        for (auto& kv : windows) hide (kv.second.get());
        hide (audioSettings.get());
        ctx->plugins.forEachEditor ([&hide] (juce::Component& c) { hide (dynamic_cast<juce::DocumentWindow*> (&c)); });
        if (mainWindow != nullptr) { if (mainWindow->isMinimised()) mainWindow->setMinimised (false); mainWindow->toFront (true); }
    }
    bool deskMinimisedSome() const
    {
        for (auto& p : deskMinimised) if (auto* w = p.getComponent()) if (DockHub::get().contains (w)) return true;
        return false;
    }
    std::vector<juce::Component::SafePointer<juce::DocumentWindow>> deskMinimised;      // the windows the "minimise all" key hid

    /** Bounce Out: one window per edit / take window. */
    void openBounce (const BounceContext& c)
    {
        const auto key = "bounce:" + c.id.toString();
        auto it = windows.find (key);
        if (it != windows.end()) { DockHub::get().show (it->second.get()); return; }
        showWindow (key, "Bounce Out - " + c.defaultName, [this, c] { return new BounceComponent (*ctx, c); });
    }
    void openEdit (const juce::Uuid& id, juce::Component* stayInFront = nullptr)
    {
        if (ctx->project.findEdit (id) == nullptr) return;
        if (stayInFront == nullptr) recentEdit = id;
        showWindow ("edit:" + id.toString(), "Edit - " + ctx->project.findEdit (id)->name, [this, id] { return new EditWindowComponent (*ctx, id); }, stayInFront);     // (3.16.0: its name, also on the taskbar when minimised)
    }
    void openTrim (const juce::Uuid& editId, const juce::Uuid& regionId, bool atEnd)
    {
        if (ctx->project.findEdit (editId) == nullptr) return;
        const auto key = "trim:" + editId.toString();
        auto it = windows.find (key);
        if (it != windows.end())                                   // one trim window per edit: move it to the requested join
        {
            if (auto* tc = dynamic_cast<TrimComponent*> (it->second->getContentComponent())) tc->gotoJoin (regionId, atEnd);
            DockHub::get().show (it->second.get());
            return;
        }
        showWindow (key, "Trim - " + ctx->project.findEdit (editId)->name, [this, editId, regionId, atEnd] { auto* c = new TrimComponent (*ctx, editId, regionId, atEnd); return c; });
    }
    void openMedia()
    {
        showWindow ("media", "Media - where the files are", [this] { return new MediaComponent (*ctx); });
    }
    void openDesign()
    {
        showWindow ("design", "Project Designer", [this] { return new DesignComponent (*ctx); });
    }
    /** Audio settings: the device chooser plus Apply and Close buttons. */
    struct AudioSettingsPanel : public juce::Component, private juce::Timer
    {
        AudioSettingsPanel (AppContext& c, std::function<void()> closeFn)
            : app (c), selector (c.devices, 0, 256, 0, 256, false, false, false, false)
        {
            addAndMakeVisible (selector);
            startTimer (500);                                           // (the chooser is switched off while a take records: see timerCallback)
            addAndMakeVisible (apply); addAndMakeVisible (close); addAndMakeVisible (note); addAndMakeVisible (scan); addAndMakeVisible (cpuCores); addAndMakeVisible (pluginList);
            addChildComponent (rateNote);                               // (3.8.0: shown while the live rate converter is on)
            rateNote.setColour (juce::Label::textColourId, theme::warn);
            rateNote.setFont (juce::FontOptions (13.0f));
            rateNote.setJustificationType (juce::Justification::centredLeft);
            pluginList.setTooltip ("Every plug-in Fermata has found: ignore the ones you never want (they are not scanned, loaded or offered), and try again the ones left out after a crash");
            pluginList.onClick = [this] { showPluginManager (app); };
            note.setText ("Choose the driver and settings above, then press Apply.", juce::dontSendNotification);
            apply.onClick = [this] { app.applyAudioSettings(); note.setText ("Applied: " + juce::String (app.engine.getNumInputs()) + " inputs / "
                                                                             + juce::String (app.engine.getNumOutputs()) + " outputs from the driver.", juce::dontSendNotification); };
            close.onClick = [closeFn] { closeFn(); };
            scan.setTooltip ("Look for VST3 plug-ins to use as inserts in the mixers");
            scan.onClick = [this]
            {
                if (app.plugins.isScanning()) return;
                note.setText ("Scanning for VST3 plug-ins...", juce::dontSendNotification);
                juce::Component::SafePointer<AudioSettingsPanel> safe (this);
                app.plugins.startScan ([safe]
                {
                    if (safe != nullptr)
                        safe->note.setText ("Plug-in scan finished: " + juce::String (safe->app.plugins.getKnownPlugins().getNumTypes()) + " plug-ins known.", juce::dontSendNotification);
                });
            };
            cpuCores.setTooltip ("Keep some CPU cores for Fermata's audio only, so that other programs cannot interrupt a recording");
            cpuCores.onClick = [this] { showCpuCoresPanel (app); };
            apply.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
            setSize (680, 560);
            timerCallback();                                            // (opened while a take records: locked from the start)
        }
        /** The device chooser applies a change the moment it is made (JUCE's AudioDeviceSelectorComponent): a new rate or buffer size restarts the device,
            and a device restarted under a take ends it (see AudioEngine::audioDeviceAboutToStart). So while a take records the chooser is switched off. */
        void timerCallback() override
        {
            const auto conv = app.converterNote();                       // 3.8.0: the device at another rate than the project, converted live
            if (conv != rateNote.getText())
            {
                rateNote.setText (conv, juce::dontSendNotification);
                rateNote.setVisible (conv.isNotEmpty());
                resized();
            }
            const bool rec = app.engine.isRecording();
            if (rec == lockedForRecording) return;
            lockedForRecording = rec;
            selector.setEnabled (! rec); apply.setEnabled (! rec);
            note.setText (rec ? "Recording: the audio device cannot be changed until the take has stopped." : "Choose the driver and settings above, then press Apply.", juce::dontSendNotification);
        }
        bool lockedForRecording = false;
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto bottom = r.removeFromBottom (40);
            if (rateNote.isVisible()) rateNote.setBounds (r.removeFromBottom (44));
            selector.setBounds (r);
            close.setBounds (bottom.removeFromRight (110).reduced (3));
            apply.setBounds (bottom.removeFromRight (110).reduced (3));
            scan.setBounds (bottom.removeFromLeft (118).reduced (3));
            pluginList.setBounds (bottom.removeFromLeft (92).reduced (3));
            cpuCores.setBounds (bottom.removeFromLeft (104).reduced (3));
            note.setBounds (bottom);
        }
        AppContext& app;
        juce::AudioDeviceSelectorComponent selector;
        juce::TextButton apply { "Apply" }, close { "Close" }, scan { "Scan plug-ins" }, pluginList { "Plug-ins..." }, cpuCores { "CPU cores..." };
        juce::Label note, rateNote;
    };

    void openAudioSettings()
    {
        juce::Logger::writeToLog ("Opening the Audio settings window");
        if (audioSettings != nullptr) { audioSettings->toFront (true); return; }
        auto* dw = new ToolWindow ("Audio settings - choose the ASIO device (e.g. Merging Audio Device / MADPanel)", [this] (ToolWindow*)
        {
            juce::MessageManager::callAsync ([this] { audioSettings.reset(); });
        });
        dw->setContentOwned (new CornerStripHost (new AudioSettingsPanel (*ctx, [this] { juce::MessageManager::callAsync ([this] { audioSettings.reset(); }); })), true);
        dw->setResizable (false, false);
        dw->centreWithSize (dw->getWidth(), dw->getHeight());
        if (mainWindow != nullptr) makeOwnedBy (*dw, *mainWindow);
        dw->setVisible (true);
        audioSettings.reset (dw);
    }

    /** Keep window titles right and close windows whose mixer / take window no longer exists. */
    void refreshWindows()
    {
        std::vector<juce::String> dead;
        for (auto& [key, w] : windows)
        {
            if (key.startsWith ("take:"))
            {
                if (auto* d = ctx->project.findTakeWindow (juce::Uuid (key.fromFirstOccurrenceOf (":", false, false)))) w->setName ("Takes - " + d->name);
                else dead.push_back (key);
            }
            else if (key.startsWith ("edit:") || key.startsWith ("trim:"))       // (3.16.0: titled with the edit's name, kept up to date)
            {
                if (auto* e = ctx->project.findEdit (juce::Uuid (key.fromFirstOccurrenceOf (":", false, false))))
                    w->setName ((key.startsWith ("edit:") ? "Edit - " : "Trim - ") + e->name);
                else dead.push_back (key);
            }
            else if (key.startsWith ("scope:"))
            {
                bool found = false;
                for (auto& m : ctx->project.mixers) if (m->id.toString() == key.fromFirstOccurrenceOf (":", false, false)) found = true;
                if (! found) dead.push_back (key);
            }
            else if (key.startsWith ("mixer:"))
            {
                bool found = false;
                for (auto& m : ctx->project.mixers)
                    if (m->id.toString() == key.fromFirstOccurrenceOf (":", false, false)) { w->setName ("Mixer - " + m->name); found = true; }
                if (! found) dead.push_back (key);
            }
        }
        if (! dead.empty())
            juce::MessageManager::callAsync ([this, dead] { for (auto& k : dead) windows.erase (k); });
    }

    std::unique_ptr<SplashScreen> splash;
    std::unique_ptr<juce::FileLogger> fileLogger;
    std::unique_ptr<FermataLookAndFeel> lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    std::unique_ptr<ToolWindow> audioSettings;
    std::unique_ptr<AppContext> ctx;
    std::unique_ptr<RemoteControlServer> remoteServer;       // the control port for a Stream Deck (127.0.0.1 only)
    std::unique_ptr<SurfaceController> surface;              // the Avid S1 / Mackie Control surface (MIDI)
    std::unique_ptr<ToolWindowMain> mainWindow;
    std::unique_ptr<ToolWindow::FocusKeeper> focusKeeper;
    std::map<juce::String, std::unique_ptr<ToolWindow>> windows;
    std::unique_ptr<StayOnTop> bridgeTop;                    // keeps the meter bridge in front of everything (see StayOnTop)
    std::unique_ptr<StayOnTop> recMonTop;                    // the same for the Recording monitor (3.2.0)
};

START_JUCE_APPLICATION (FermataApplication)
