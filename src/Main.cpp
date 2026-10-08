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
#include "ui/RemoteControl.h"
#if JUCE_WINDOWS
 #include <shlobj.h>
#endif

using namespace td;

static juce::File logFolder() { return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata"); }

/** Called by Windows when the program crashes: writes where it happened into the log file. */
static void crashHandler (void*)
{
    auto f = logFolder().getChildFile ("fermata-log.txt");
    f.appendText ("\n*** CRASH ***\n" + juce::SystemStats::getStackBacktrace() + "\n");
}


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
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        splash = std::make_unique<SplashScreen> (juce::String ("Version ") + getApplicationVersion());
        logFolder().createDirectory();
        registerProjectFileType();
        fileLogger.reset (new juce::FileLogger (logFolder().getChildFile ("fermata-log.txt"), "Fermata " + getApplicationVersion() + "  (built " + __DATE__ + " " + __TIME__ + ")", 512 * 1024));
        juce::Logger::setCurrentLogger (fileLogger.get());
        juce::SystemStats::setApplicationCrashHandler (crashHandler);
        lookAndFeel.reset (new FermataLookAndFeel());
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());
        tooltips = std::make_unique<juce::TooltipWindow> (nullptr, 500);        // without this no tooltip would ever show
        ctx = std::make_unique<AppContext>();
        installThemeSwitch();                                                    // the sun / moon buttons switch the whole program
        themeSaveHook() = [this] (bool dark)
        {
            if (ctx == nullptr) return;
            if (auto* st = ctx->props.getUserSettings()) { st->setValue ("appDark", dark); ctx->props.saveIfNeeded(); }
        };
        if (auto* st = ctx->props.getUserSettings()) if (! st->getBoolValue ("appDark", true)) switchTheme (false);
        ctx->showTakeWindow = [this] (const juce::Uuid& id) { openTakeWindow (id); };
        ctx->showMixer = [this] (const juce::Uuid& id) { openMixer (id); };
        ctx->showEdit = [this] (const juce::Uuid& id) { openEdit (id); };
        ctx->showEditBehind = [this] (const juce::Uuid& id, juce::Component* front) { openEdit (id, front); };
        ctx->showTrim = [this] (const juce::Uuid& e, const juce::Uuid& r, bool atEnd) { openTrim (e, r, atEnd); };
        ctx->closeTrim = [this] (const juce::Uuid& e) { closeWindowLater ("trim:" + e.toString()); };
        ctx->showDesign = [this] { openDesign(); };
        ctx->showBridge = [this] { toggleBridge(); };
        ctx->showBounce = [this] (const BounceContext& c) { openBounce (c); };
        globalKeyHook() = [this] (const juce::KeyPress& k)
        {
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
        ctx->showAudioSettings = [this] { openAudioSettings(); };
        ctx->showMedia = [this] { openMedia(); };
        ctx->showMastering = [this] { showWindow ("mastering", "Mastering", [this] { return new MasteringComponent (*ctx); }); };
        ctx->showOrganiser = [this] { showWindow ("organiser", "Display Organiser", [this] { return new OrganiserComponent (*ctx); }); };
        ctx->showTakeDisplay = [this] { showWindow ("takedisplay", "Take Display", [this] { return new TakeDisplayControl (*ctx); }); };
        ctx->listWindows = [this]
        {
            std::vector<std::pair<juce::String, juce::String>> out;
            out.push_back ({ "main", "Main window" });
            // every take window, edit window and mixer of the project (open or not: choosing one that is closed opens it) ...
            for (auto& t : ctx->project.takeWindows) out.push_back ({ "take:" + t->id.toString(), "Takes - " + t->name });
            for (auto& e : ctx->project.edits)       out.push_back ({ "edit:" + e->id.toString(), "Edit - " + e->name });
            for (auto& m : ctx->project.mixers)      out.push_back ({ "mixer:" + m->id.toString(), "Mixer - " + m->name });
            // ... and the other windows that are open now
            for (auto& [key, w] : windows)
            {
                if (w == nullptr || key == "organiser" || key.startsWith ("take:") || key.startsWith ("edit:") || key.startsWith ("mixer:")) continue;
                out.push_back ({ key, w->getName() });
            }
            return out;
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
            mainWindow->centreWithSize (juce::jmin (juce::jmax (mainWindow->getWidth(), minW), area.getWidth()), juce::jmin (mainWindow->getHeight(), area.getHeight()));
            mainWindow->setBounds (mainWindow->getBounds().constrainedWithin (area));
        }
        mainWindow->setVisible (true);
        if (splash != nullptr)
            splash->startCountdown ([this] { juce::MessageManager::callAsync ([this] { splash.reset(); }); });
        if (err.isNotEmpty()) showError ("Audio device", err + "\n\nOpen \"Audio settings\" to choose your ASIO device.");
    }

    void shutdown() override
    {
        splash.reset();
        remoteServer.reset();
        if (ctx) ctx->shutdown();
        windows.clear();
        mainWindow.reset();
        ctx.reset();
        tooltips.reset();
        globalKeyHook() = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
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
                                          if (result == 1) { if (ctx) ctx->saveNow(); quit(); }               // Save and quit
                                          else if (result == 2) { if (ctx) ctx->skipFinalSave = true; quit(); }   // Quit without saving
                                          // anything else (Cancel): carry on working
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

    /** 'stayInFront' (optional): a window the user is working in. An already open window is then left exactly where it is; a NEW window always opens on top. */
    template <typename Make>
    void showWindow (const juce::String& key, const juce::String& title, Make make, juce::Component* stayInFront = nullptr)
    {
        auto it = windows.find (key);
        if (it != windows.end())
        {
            it->second->setName (title);
            if (stayInFront == nullptr || DockHub::get().contains (it->second.get())) DockHub::get().show (it->second.get());     // an open window stays where it is when sent to from another
            return;
        }
        auto* content = make();
        auto w = std::make_unique<ToolWindow> (title, [this, key] (ToolWindow*)
        {
            juce::MessageManager::callAsync ([this, key] { windows.erase (key); });
        });
        w->setContentOwned (content, true);
        w->setIcon (makeFermataIcon (256));
        w->isTransportWindow = key.startsWith ("take:") || key.startsWith ("edit:");
        w->forwardKeys = key.startsWith ("mixer:") || key.startsWith ("scope:") || key == "bridge" || key == "media" || key == "takedisplay" || key == "organiser";
        {   // where it was left last time (kept in the project), or else centred; never wider than the screen, nor taller (except a mixer, which opens tall enough to show the whole strip)
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto area = (mainWindow != nullptr ? displays.getDisplayForRect (mainWindow->getBounds()) : displays.getPrimaryDisplay())->userArea;
            juce::Rectangle<int> saved;
            if (auto sit = ctx->project.windowBounds.find (key); sit != ctx->project.windowBounds.end()) saved = juce::Rectangle<int>::fromString (sit->second);
            bool onScreen = false;                                                   // a remembered place counts only if its title bar can still be reached
            for (auto& d : displays.displays) if (d.userArea.intersects (juce::Rectangle<int> (saved.getX() + 20, saved.getY() - 10, 120, 30))) onScreen = true;
            const bool isMixer = key.startsWith ("mixer:");
            if (saved.getWidth() >= 120 && saved.getHeight() >= 80 && onScreen)
            {
                if (isMixer) w->setTopLeftPosition (saved.getPosition());           // a mixer always opens at the size that shows every strip; only its place is remembered
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
        if (auto* mc = dynamic_cast<MixerComponent*> (content)) mc->fitWindowToStrips();
        w->onBoundsChanged = [this, key] (const juce::String& t) { ctx->project.windowBounds[key] = t; };       // from now on, every move / resize is remembered in the project
        auto* raw = w.get();
        windows[key] = std::move (w);
        juce::ignoreUnused (stayInFront);                     // a NEW window always opens on top of the others, even over a maximised one
        if (! owned) raiseToolWindows();
        raw->toFront (true);                                  // the new window always ends up in front, with the keyboard
        raw->grabContentFocus();
    }

    void openTakeWindow (const juce::Uuid& id)
    {
        auto* d = ctx->project.findTakeWindow (id);
        if (d == nullptr) return;
        showWindow ("take:" + id.toString(), "Takes - " + d->name, [this, id] { return new TakeWindowComponent (*ctx, id); });
    }
    void openMixer (const juce::Uuid& id)
    {
        for (auto& m : ctx->project.mixers)
            if (m->id == id)
            {
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
            if (frontmost) closeWindowLater (key); else DockHub::get().show (it->second.get());
        }
        else openMixer (id);
    }
    /** B: opens the meter bridge, a floating window with a meter for every driver input and output, or closes it. */
    void toggleBridge()
    {
        const juce::String key = "bridge";
        if (auto bi = windows.find (key); bi != windows.end()) { if (DockHub::get().contains (bi->second.get())) DockHub::get().show (bi->second.get()); else closeWindowLater (key); return; }
        showWindow (key, "Meter bridge", [this] { return new MeterBridgeComponent (*ctx); });
        if (auto it = windows.find (key); it != windows.end())
        {
            it->second->setAlwaysOnTop (true);                 // floats above the other windows
            if (ctx->project.windowBounds.find (key) == ctx->project.windowBounds.end()) { it->second->setSize (1100, 420); it->second->setTopLeftPosition (20, 20); }
        }
    }
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
        showWindow ("edit:" + id.toString(), "Edit", [this, id] { return new EditWindowComponent (*ctx, id); }, stayInFront);
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
        showWindow (key, "Trim", [this, editId, regionId, atEnd] { auto* c = new TrimComponent (*ctx, editId, regionId, atEnd); return c; });
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
    struct AudioSettingsPanel : public juce::Component
    {
        AudioSettingsPanel (AppContext& c, std::function<void()> closeFn)
            : app (c), selector (c.devices, 0, 256, 0, 256, false, false, false, false)
        {
            addAndMakeVisible (selector);
            addAndMakeVisible (apply); addAndMakeVisible (close); addAndMakeVisible (note); addAndMakeVisible (scan);
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
            apply.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
            setSize (580, 560);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto bottom = r.removeFromBottom (40);
            selector.setBounds (r);
            close.setBounds (bottom.removeFromRight (110).reduced (3));
            apply.setBounds (bottom.removeFromRight (110).reduced (3));
            scan.setBounds (bottom.removeFromLeft (130).reduced (3));
            note.setBounds (bottom);
        }
        AppContext& app;
        juce::AudioDeviceSelectorComponent selector;
        juce::TextButton apply { "Apply" }, close { "Close" }, scan { "Scan plug-ins" };
        juce::Label note;
    };

    void openAudioSettings()
    {
        juce::Logger::writeToLog ("Opening the Audio settings window");
        if (audioSettings != nullptr) { audioSettings->toFront (true); return; }
        auto* dw = new ToolWindow ("Audio settings - choose the ASIO device (e.g. Merging Audio Device / MADPanel)", [this] (ToolWindow*)
        {
            juce::MessageManager::callAsync ([this] { audioSettings.reset(); });
        });
        dw->setContentOwned (new AudioSettingsPanel (*ctx, [this] { juce::MessageManager::callAsync ([this] { audioSettings.reset(); }); }), true);
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
            else if (key.startsWith ("edit:"))
            {
                if (ctx->project.findEdit (juce::Uuid (key.fromFirstOccurrenceOf (":", false, false))) == nullptr) dead.push_back (key);
            }
            else if (key.startsWith ("trim:"))
            {
                if (ctx->project.findEdit (juce::Uuid (key.fromFirstOccurrenceOf (":", false, false))) == nullptr) dead.push_back (key);
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
    std::unique_ptr<ToolWindowMain> mainWindow;
    std::map<juce::String, std::unique_ptr<ToolWindow>> windows;
};

START_JUCE_APPLICATION (FermataApplication)
