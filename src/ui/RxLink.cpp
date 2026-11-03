// Export for Processing, sending to a program: see RxLink.h. The linked programs (a list kept in this computer's settings), the Export for Processing
// menu, sending the marked part out (FixTools.cpp: planSendOut, shared with "Create files to edit"), starting the program with the files, the
// once-a-second look for files saved there (RxSaveWatch, src/core/RxWatch.h), and bringing them in (EditDef::takeInWaitingFile / useRxSave).
// 3.22.0 did this for iZotope RX alone; 3.24.0 for any program (Melodyne writes the WAV back with File > Replace Audio).
#include "RxLink.h"
#include "FixToolsCommon.h"
#include "WinOwner.h"
#include "../core/RxWatch.h"

namespace td { namespace rxlink
{
using fixtools::say;
using fixtools::formats;

static const juce::String kTitle ("Export for Processing");
static constexpr double kHandleSeconds = 2.0;          // the extra audio each side of the part, so it can be heard (and mended) in context

static bool usable (const juce::File& f) { return f != juce::File() && f.exists(); }      // (exists: on a Mac the program is a folder, the .app)
static bool isMelodyne (const juce::String& s) { return s.containsIgnoreCase ("Melodyne"); }

// ======================================================================================================== the linked programs
std::vector<Program> programs (AppContext& app)
{
    std::vector<Program> v;
    auto* st = app.props.getUserSettings();
    if (st == nullptr) return v;
    if (auto xml = st->getXmlValue ("externalPrograms"))
    {
        for (auto* e : xml->getChildIterator())
        {
            Program p; p.name = e->getStringAttribute ("name").trim();
            const auto path = e->getStringAttribute ("path");
            if (p.name.isNotEmpty() && juce::File::isAbsolutePath (path)) { p.file = juce::File (path); v.push_back (p); }
        }
    }
    else                                                       // (3.22.0 / 3.23.0 kept one RX location)
    {
        const auto old = st->getValue ("rxProgram");
        if (old.isNotEmpty() && juce::File::isAbsolutePath (old)) v.push_back ({ "iZotope RX", juce::File (old) });
    }
    return v;
}

void setPrograms (AppContext& app, const std::vector<Program>& v)
{
    auto* st = app.props.getUserSettings();
    if (st == nullptr) return;
    juce::XmlElement x ("PROGRAMS");
    for (auto& p : v) { auto* e = x.createNewChildElement ("PROGRAM"); e->setAttribute ("name", p.name); e->setAttribute ("path", p.file.getFullPathName()); }
    st->setValue ("externalPrograms", &x);
    app.props.saveIfNeeded();
}

/** A name for the menu from the program's file: "iZotope RX", "Melodyne", or the file's own name. */
static juce::String suggestedName (const juce::File& f)
{
    const auto n = f.getFileNameWithoutExtension();
    if (n.containsIgnoreCase ("RX") && (n.containsIgnoreCase ("iZotope") || n.containsIgnoreCase ("Audio Editor"))) return "iZotope RX";
    if (isMelodyne (n)) return "Melodyne";
    if (n.containsIgnoreCase ("SpectraLayers")) return "SpectraLayers";
    return n;
}

/** The short name a job's folder and files are called by: "RX" for "iZotope RX", "Melodyne", "SpectraLayers Pro" ... */
static juce::String jobLabel (const juce::String& programName)
{
    auto l = programName.trim();
    for (auto* vendor : { "iZotope ", "Celemony ", "Steinberg ", "Adobe ", "Magix ", "Acon " })
        if (l.startsWithIgnoreCase (vendor)) l = l.substring (juce::String (vendor).length()).trim();
    l = sanitiseForFile (l);
    if (l.length() > 20) l = l.substring (0, 20).trim();
    return l.isNotEmpty() ? l : juce::String ("Edit");
}

/** How a file gets back from this program, as a sentence fragment ("press Save (Ctrl+S) on each tab"). */
static juce::String howToSave (const juce::String& programName)
{
    if (isMelodyne (programName)) return "choose File > Replace Audio for each one (Melodyne's own Save only keeps a Melodyne document; Replace Audio writes the WAV and keeps the original as an 'orig' copy)";
    if (programName.containsIgnoreCase ("RX")) return "press Save (Ctrl+S) on each tab";
    return "save each file over itself (File > Save, the same name and format)";
}

// ---- finding the programs where their installers put them
static int numberAfter (const juce::String& name, const juce::String& word)          // "iZotope RX 10 Audio Editor", "RX" -> 10
{
    return name.fromFirstOccurrenceOf (word, false, true).trimStart().getIntValue();
}

#if JUCE_WINDOWS
/** A program that came with the editor but is not the editor itself. */
static bool isHelper (const juce::String& n)
{
    for (auto* w : { "uninst", "connect", "monitor", "helper", "crash", "update", "portal", "setup", "install", "plugin", "plug-in", "service", "report", "licen" })
        if (n.containsIgnoreCase (w)) return true;
    return false;
}
static juce::Array<juce::File> vendorFolders (const juce::String& vendor)
{
    juce::Array<juce::File> roots;
    for (auto* var : { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" })
    {
        const auto v = juce::SystemStats::getEnvironmentVariable (var, {});
        if (v.isNotEmpty() && juce::File::isAbsolutePath (v)) roots.addIfNotAlreadyThere (juce::File (v).getChildFile (vendor));
    }
    roots.addIfNotAlreadyThere (juce::File ("C:\\Program Files").getChildFile (vendor));
    return roots;
}
#endif

juce::File findRx()
{
    juce::File best; int bestVer = -1;
    auto consider = [&] (const juce::File& f, const juce::String& name) { const int v = numberAfter (name, "RX"); if (v > bestVer) { best = f; bestVer = v; } };
   #if JUCE_WINDOWS
    for (int pass = 0; pass < 2 && best == juce::File(); ++pass)          // the audio editor by its name first; failing that any RX program that is not a helper
        for (auto& root : vendorFolders ("iZotope"))
        {
            if (! root.isDirectory()) continue;
            for (auto& dir : root.findChildFiles (juce::File::findDirectories, false, "*RX*"))
                for (auto& exe : dir.findChildFiles (juce::File::findFiles, true, "*.exe"))
                {
                    const auto n = exe.getFileNameWithoutExtension();
                    if (! n.containsIgnoreCase ("RX") || isHelper (n)) continue;
                    if (pass == 0 && ! n.containsIgnoreCase ("Audio Editor")) continue;
                    consider (exe, n);
                }
        }
   #elif JUCE_MAC
    for (auto& d : juce::File ("/Applications").findChildFiles (juce::File::findDirectories, false, "*RX*"))
    {
        if (d.hasFileExtension ("app")) { if (d.getFileName().containsIgnoreCase ("Audio Editor")) consider (d, d.getFileNameWithoutExtension()); continue; }
        for (auto& a : d.findChildFiles (juce::File::findDirectories, false, "*.app"))
            if (a.getFileName().containsIgnoreCase ("RX") && a.getFileName().containsIgnoreCase ("Audio Editor")) consider (a, a.getFileNameWithoutExtension());
    }
   #else
    juce::ignoreUnused (consider);
   #endif
    return best;
}

juce::File findMelodyne()
{
    juce::File best; int bestVer = -1;
    auto consider = [&] (const juce::File& f, const juce::String& name) { const int v = numberAfter (name, "Melodyne"); if (v > bestVer) { best = f; bestVer = v; } };
   #if JUCE_WINDOWS
    for (auto& root : vendorFolders ("Celemony"))
    {
        if (! root.isDirectory()) continue;
        for (auto& exe : root.findChildFiles (juce::File::findFiles, true, "*.exe"))
        {
            const auto n = exe.getFileNameWithoutExtension();
            if (! isMelodyne (n) || isHelper (n)) continue;
            consider (exe, exe.getParentDirectory().getFileName() + " " + n);         // ("Melodyne 5\Melodyne.exe": the folder says the version)
        }
    }
   #elif JUCE_MAC
    for (auto& a : juce::File ("/Applications").findChildFiles (juce::File::findDirectories, false, "*Melodyne*.app"))
        consider (a, a.getFileNameWithoutExtension());
   #else
    juce::ignoreUnused (consider);
   #endif
    return best;
}

/** Adds (or replaces, by its file or its name) one program in the list. */
static void linkProgram (AppContext& app, const Program& p)
{
    auto list = programs (app);
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Program& o) { return o.file == p.file || o.name.equalsIgnoreCase (p.name); }), list.end());
    list.push_back (p);
    setPrograms (app, list);
}

void addProgram (AppContext& app, juce::Component* parent, std::function<void (bool)> done)
{
   #if JUCE_WINDOWS
    const auto start = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
    const juce::String pattern ("*.exe");
   #elif JUCE_MAC
    const juce::File start ("/Applications");
    const juce::String pattern ("*.app");
   #else
    const auto start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const juce::String pattern ("*");
   #endif
    juce::Component::SafePointer<juce::Component> sp (parent);
    auto* fc = new juce::FileChooser ("Choose the editing program to link (its .exe, usually in Program Files)", start, pattern);
    fc->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [&app, fc, done, sp] (const juce::FileChooser& c)
    {
        const auto f = c.getResult();
        juce::MessageManager::callAsync ([fc] { delete fc; });          // (not inside its own callback)
        if (f == juce::File() || ! f.exists()) { if (done) done (false); return; }
        auto* aw = new juce::AlertWindow ("Link a program", "What should Export for Processing's menu call it?\n\n" + f.getFullPathName(), juce::MessageBoxIconType::NoIcon, sp.getComponent());
        aw->addTextEditor ("name", suggestedName (f), "Name");
        aw->addButton ("Link it", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([&app, aw, f, done] (int r)
        {
            const auto name = aw->getTextEditorContents ("name").trim();
            delete aw;
            if (r != 1) { if (done) done (false); return; }
            linkProgram (app, { name.isNotEmpty() ? name : suggestedName (f), f });
            app.setNotice ("Linked " + (name.isNotEmpty() ? name : suggestedName (f)) + ": it is in Export for Processing's menu (edit window).");
            if (done) done (true);
        }));
    });
}

static void removeProgram (AppContext& app, const Program& p)
{
    auto list = programs (app);
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Program& o) { return o.file == p.file && o.name == p.name; }), list.end());
    setPrograms (app, list);
    app.setNotice ("Removed " + p.name + " from Export for Processing's menu. (Parts already out in it still come back when saved.)");
}

void askAtFirstStart (AppContext& app)
{
    auto* st = app.props.getUserSettings();
    if (st == nullptr || st->getBoolValue ("extProgramsAsked", false)) return;
    juce::Timer::callAfterDelay (6000, [&app]
    {
        auto* s = app.props.getUserSettings();
        if (s == nullptr || s->getBoolValue ("extProgramsAsked", false)) return;
        s->setValue ("extProgramsAsked", true); app.props.saveIfNeeded();
        const auto list = programs (app);
        auto found = std::make_shared<std::vector<Program>>();
        auto offer = [&] (const juce::File& f, const juce::String& name)
        {
            if (! usable (f)) return;
            for (auto& p : list) if (p.file == f) return;
            found->push_back ({ name, f });
        };
        offer (findRx(), "iZotope RX");
        offer (findMelodyne(), "Melodyne");
        if (found->empty() && ! list.empty()) return;                  // (already linked: nothing to ask)
        juce::String msg ("New: Export for Processing (in the edit window) can open the part between marks 1 and 2 straight in your editing programs - "
                          "iZotope RX, Melodyne or any other - and bring each file back into the edit by itself when you save it there.");
        if (! found->empty())
        {
            msg << "\n\nFound on this computer:";
            for (auto& f : *found) msg << "\n" << f.name << "   (" << f.file.getFullPathName() << ")";
            msg << "\n\nLink " << (found->size() == 1 ? "it" : "them") << "?";
        }
        else msg << "\n\nNo editing program was found where the usual installers put them. Link one now?";
        auto* aw = new juce::AlertWindow ("Editing programs", msg, juce::MessageBoxIconType::QuestionIcon, nullptr);
        if (! found->empty()) aw->addButton (found->size() == 1 ? "Link it" : "Link them", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Choose a program...", 2);
        aw->addButton ("Skip", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([&app, found] (int r)
        {
            if (r == 1) { for (auto& p : *found) linkProgram (app, p); app.setNotice ("Linked: they are in Export for Processing's menu (edit window)."); return; }
            if (r == 2) { addProgram (app, nullptr, nullptr); return; }
            app.setNotice ("You can link a program at any time: Export for Processing > Add a program...");
        }), true);
    });
}

// ======================================================================================================== the menus
void showExportMenu (AppContext& app, const juce::Uuid& eid, juce::Component* button)
{
    const auto list = programs (app);
    juce::PopupMenu m;
    m.addItem (1, "Create files to edit  (in Processing Media - re-link them when they are done)");
    m.addSeparator();
    if (list.empty()) m.addItem (-1, "Open in a program: none linked yet", false);
    for (size_t i = 0; i < list.size(); ++i)
        m.addItem (100 + (int) i, "Open in " + list[i].name + (usable (list[i].file) ? juce::String() : juce::String ("  (not found: remove it and add it again)")), usable (list[i].file));
    m.addSeparator();
    m.addItem (2, "Add a program...");
    if (! list.empty())
    {
        juce::PopupMenu rm;
        for (size_t i = 0; i < list.size(); ++i) rm.addItem (200 + (int) i, list[i].name);
        m.addSubMenu ("Remove a program", rm);
    }
    juce::Component::SafePointer<juce::Component> sp (button);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (button), [&app, eid, sp, list] (int r)
    {
        if (r == 1) { fixtools::exportForProcessingEdit (app, eid, sp.getComponent()); return; }
        if (r == 2) { addProgram (app, sp.getComponent(), nullptr); return; }
        if (r >= 200 && r < 200 + (int) list.size()) { removeProgram (app, list[(size_t) r - 200]); return; }
        if (r >= 100 && r < 100 + (int) list.size()) sendToProgram (app, eid, list[(size_t) r - 100], sp.getComponent());
    });
}

void showProgramsMenu (AppContext& app, juce::Component* target)
{
    const auto list = programs (app);
    juce::PopupMenu m;
    m.addSectionHeader ("Programs that Export for Processing opens");
    if (list.empty()) m.addItem (-1, "(none linked yet)", false);
    for (auto& p : list) m.addItem (-1, p.name + "   " + p.file.getFullPathName() + (usable (p.file) ? juce::String() : juce::String ("  (not found)")), false);
    m.addSeparator();
    m.addItem (2, "Add a program...");
    if (! list.empty())
    {
        juce::PopupMenu rm;
        for (size_t i = 0; i < list.size(); ++i) rm.addItem (200 + (int) i, list[i].name);
        m.addSubMenu ("Remove a program", rm);
    }
    juce::Component::SafePointer<juce::Component> sp (target);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [&app, sp, list] (int r)
    {
        if (r == 2) { addProgram (app, sp.getComponent(), nullptr); return; }
        if (r >= 200 && r < 200 + (int) list.size()) removeProgram (app, list[(size_t) r - 200]);
    });
}

// ======================================================================================================== starting the program
static bool openInProgram (const juce::File& exe, const std::vector<juce::File>& files, juce::String& error)
{
    if (! usable (exe)) { error = "The program is not there any more: " + exe.getFullPathName(); return false; }
    if (files.empty()) { error = "There are no files to open."; return false; }
   #if JUCE_MAC
    juce::StringArray args { "open", "-a", exe.getFullPathName() };
    for (auto& f : files) args.add (f.getFullPathName());
    return startProgram (args, error);
   #else
    // The files go on the program's command line (RX takes them into the copy that is already open, one tab each). A very long list goes in several calls
    // (a Windows command line holds about 32000 characters: some 250 files).
    size_t i = 0;
    while (i < files.size())
    {
        juce::StringArray args; args.add (exe.getFullPathName());
        int len = exe.getFullPathName().length() + 3;
        while (i < files.size() && (args.size() == 1 || len + files[i].getFullPathName().length() + 3 < 30000))
        {
            len += files[i].getFullPathName().length() + 3;
            args.add (files[i++].getFullPathName());
        }
        if (! startProgram (args, error)) return false;
    }
    return true;
   #endif
}

static void explainOnce (AppContext& app, const Program& p, const juce::File& folder)
{
    auto* st = app.props.getUserSettings();
    const auto key = "extExplained_" + p.name.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
    if (st == nullptr || st->getBoolValue (key, false)) return;
    st->setValue (key, true); app.props.saveIfNeeded();
    const juce::String text ("The marked part is opening in " + p.name + ", one file per track, with 2 seconds extra each side so you hear it in context "
                             "(markers in the files show where the marked part starts and ends).\n\n"
                             "Mend it there, then " + howToSave (p.name) + ". Each file that comes back replaces only the marked part of its track - "
                             "a second or two after it is saved - and Ctrl+Z takes it out again. Do it again later and the new version comes in.\n\n"
                             "Keep the length the same (no stretching or cutting). Until a file comes back, its part shows as 'In " + jobLabel (p.name) + " ...' and plays as it was. "
                             "Right-click that block to bring in what has been saved, open it again, show the files, or cancel and keep the original audio."
                             + (isMelodyne (p.name) ? juce::String ("\n\nIf Melodyne opens without the files, drag them in from their folder (Show the files).") : juce::String()));
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle ("Open in " + p.name).withMessage (text)
                                      .withButton ("Show the files").withButton ("OK"),
                                  [folder] (int r) { if (r == 1) folder.revealToUser(); });        // (the first button is 1; OK and Escape are 0)
}

// ======================================================================================================== sending the part out
namespace
{
struct SendJob
{
    struct Part { fixtools::SendPart p; juce::String name; std::vector<juce::File> dest; juce::int64 lo = 0, hi = 0; };
    std::vector<Part> parts;
    juce::File folder;
    juce::String job;
};
}

/** "RX 1", "Melodyne 2" ...: the first name whose folder is not in Processing Media yet. */
static juce::String nextJobName (AppContext& app, const juce::String& label, juce::File& folder)
{
    const auto base = app.project.processingFolder();
    for (int n = 1; n < 100000; ++n)
    {
        const auto name = label + " " + juce::String (n);
        if (! base.getChildFile (name).exists()) { folder = base.getChildFile (name); return name; }
    }
    folder = fixtools::processingJobFolder (app, label);
    return label;
}

static void send (AppContext& app, const juce::Uuid& eid, std::shared_ptr<std::vector<fixtools::SendPart>> parts, bool subset, juce::Component* parent, const Program& program)
{
    auto* e = app.project.findEdit (eid);
    if (e == nullptr || parts == nullptr || parts->empty()) return;
    const juce::String title ("Open in " + program.name);
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto handle = (juce::int64) std::llround (kHandleSeconds * rate);
    auto job = std::make_shared<SendJob>();
    job->job = nextJobName (app, jobLabel (program.name), job->folder);
    const bool streams = parts->front().label.isNotEmpty();
    std::vector<juce::File> taken;
    for (auto& sp : *parts)
    {
        SendJob::Part q; q.p = sp;
        q.name = streams ? job->job + (sp.label.startsWithIgnoreCase ("Out") ? " OUT" : " IN") : job->job;
        for (size_t i = 0; i < sp.src.size(); ++i)
        {
            const auto stem = sanitiseForFile (sp.names[i] + " - " + q.name);      // the track first: it is what a program's tab shows
            auto d = job->folder.getChildFile (stem + ".wav");
            for (int k = 2; std::find (taken.begin(), taken.end(), d) != taken.end(); ++k) d = job->folder.getChildFile (stem + " " + juce::String (k) + ".wav");
            taken.push_back (d); q.dest.push_back (d);
        }
        job->parts.push_back (std::move (q));
    }
    AudioJob::Work work = [job, handle] (AudioJob& aj) -> juce::String
    {
        job->folder.createDirectory();
        size_t done = 0, total = 0;
        for (auto& q : job->parts) total += q.dest.size();
        for (auto& q : job->parts)
        {
            // the handles: up to 2 s each side, as far as every file of the part has audio (the files of one part all hold the same stretch, so they line up)
            juce::int64 lo = q.p.a - handle, hi = q.p.b + handle;
            for (size_t i = 0; i < q.p.src.size(); ++i)
            {
                auto rd = audioops::openReader (formats(), q.p.src[i]);
                if (rd == nullptr) { job->folder.deleteRecursively(); return "Could not read " + q.p.src[i].getFileName() + "."; }
                lo = juce::jmax (lo, q.p.fstart[i]); hi = juce::jmin (hi, q.p.fstart[i] + rd->lengthInSamples);
            }
            q.lo = juce::jmin (lo, q.p.a); q.hi = juce::jmax (hi, q.p.b);              // (the part itself always goes, as Create files to edit sends it)
            const auto marks = audioops::cueMarkers ({ { q.p.a - q.lo, "Fermata: the marked part starts" }, { q.p.b - q.lo, "Fermata: the marked part ends" } });
            for (size_t i = 0; i < q.p.src.size(); ++i)
            {
                if (aj.cancelled()) { job->folder.deleteRecursively(); return "Cancelled."; }
                aj.setProgress ((float) done++ / (float) juce::jmax ((size_t) 1, total));
                juce::String err;
                if (! audioops::exportRange (formats(), q.p.src[i], q.p.fstart[i], q.lo, q.hi, q.dest[i], err, true, &marks)) { job->folder.deleteRecursively(); return err; }
            }
        }
        return {};
    };
    auto onSuccess = [&app, eid, job, subset, program, title]
    {
        auto* e2 = app.project.findEdit (eid);
        const auto old = e2 != nullptr ? e2->regions : std::vector<EditRegion>();
        bool ok = e2 != nullptr;
        if (ok)
            for (auto& q : job->parts)
            {
                WaitingPiece w;
                w.name = q.name; w.files = q.dest; w.tracks = subset ? q.p.trk : std::vector<juce::Uuid>();
                w.rx = true; w.program = program.name; w.fileFrom = q.lo; w.fileLen = q.hi - q.lo;
                ok = e2->markWaiting (q.p.rid, q.p.a, q.p.b, std::move (w)) && ok;
            }
        if (! ok)
        {
            if (e2 != nullptr) e2->regions = old;
            job->folder.deleteRecursively();
            say (title, "The edit changed while the audio was being written, so nothing was changed. Please try again.");
            return;
        }
        std::vector<juce::File> all;
        for (auto& q : job->parts)
            for (auto& d : q.dest) { app.project.rxSeen[d.getFullPathName()] = d.getLastModificationTime().toMilliseconds(); all.push_back (d); }   // (as written: not a save)
        fixtools::setUndoForEdit (app, eid, old, "open in " + program.name);
        app.project.changed();
        juce::String err;
        if (! openInProgram (program.file, all, err))
        {
            say (title, err + "\n\nThe files are in\n" + job->folder.getFullPathName() + "\n\nOpen them in " + program.name + " yourself: when you " + howToSave (program.name)
                        + ", they still come back into the edit by themselves.");
            return;
        }
        app.setNotice ("Opened in " + program.name + ": " + juce::String ((int) all.size()) + (all.size() == 1 ? " file" : " files") + ". Mend it there and "
                       + (isMelodyne (program.name) ? juce::String ("use File > Replace Audio") : juce::String ("save it")) + ": it comes back into the edit by itself.");
        explainOnce (app, program, job->folder);
    };
    fixtools::runSimpleJob ("Writing the marked part for " + program.name + "...", work, onSuccess, title, parent);
}

void sendToProgram (AppContext& app, const juce::Uuid& eid, const Program& program, juce::Component* parent)
{
    const juce::String title ("Open in " + program.name);
    if (fixtools::heldByRec (app, title, parent, [ap = &app, eid, program] (juce::Component* p) { sendToProgram (*ap, eid, program, p); })) return;
    auto* e = app.project.findEdit (eid);
    if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn)
    {
        say (title, "First mark the part to open in " + program.name + " with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key. "
                    "Ctrl + Alt + drag marks only the tracks you drag over; otherwise every track goes.");
        return;
    }
    if (! usable (program.file))
    {
        say (title, program.name + " is not where it was linked:\n" + program.file.getFullPathName() + "\n\nRemove it from Export for Processing's menu and add it again (Add a program...).");
        return;
    }
    juce::Component::SafePointer<juce::Component> sp (parent);
    fixtools::planSendOut (app, eid, title, [&app, eid, sp, program] (std::shared_ptr<std::vector<fixtools::SendPart>> parts, bool subset, const juce::String&)
    {
        send (app, eid, parts, subset, sp.getComponent(), program);
    });
}

// ======================================================================================================== bringing saves back
/** "Vln 1 - RX 3 (saved 1).wav", "(saved 2)" ...: where a save of 'f' is copied to. Each save is a file of its own, so Ctrl+Z can go back to the one before. */
static juce::File copyFor (const juce::File& f)
{
    for (int n = 1; n < 100000; ++n)
    {
        const auto c = f.getSiblingFile (f.getFileNameWithoutExtension() + " (saved " + juce::String (n) + ").wav");
        if (! c.exists()) return c;
    }
    return audioops::uniqueFile (f.getSiblingFile (f.getFileNameWithoutExtension() + " (saved).wav"));
}

static juce::String secondsText (juce::int64 frames, double rate) { return juce::String ((double) frames / (rate > 0 ? rate : 48000.0), 3) + " s"; }

/** Brings in what has been saved of these files (full paths). In every edit, a waiting piece's track takes a copy of it, and every track that already plays
    an earlier save of it plays the new one. One undo step for the lot, a notice for what came back and one message for what could not. 'watch': the
    watcher's (it learns what was dealt with, and a file that cannot be read yet is tried again later); nullptr from the menu (everything is reported at once). */
static void bringIn (AppContext& app, const std::vector<juce::String>& paths, RxSaveWatch* watch)
{
    juce::StringArray came, problems;
    bool ticked = false;
    std::set<EditDef*> changedEdits;
    for (auto& path : paths)
    {
        const juce::File f (path);
        const auto modified = f.getLastModificationTime().toMilliseconds();
        auto dealt = [&] { if (watch != nullptr) watch->done (path, modified, app.project.rxSeen); else app.project.rxSeen[path] = modified; };
        auto notYet = [&] (const juce::String& why)                       // could not be read or copied: perhaps the program is still writing it
        {
            if (watch != nullptr && ! watch->later (path)) return;
            problems.add (why); dealt();
        };
        // where it goes: pieces waiting for it, and tracks that play an earlier save of it
        struct Spot { EditDef* e; int i; size_t k; };
        std::vector<Spot> waiting; std::vector<EditDef*> following;
        juce::int64 frames = -1; int channels = 0; double rate = 0.0;
        for (auto& ep : app.project.edits)
        {
            auto& e = *ep;
            for (int i = 0; i < (int) e.regions.size(); ++i)
            {
                const auto& r = e.regions[(size_t) i];
                if (! r.waiting.active() || ! r.waiting.rx) continue;
                for (size_t k = 0; k < r.waiting.files.size(); ++k)
                    if (r.waiting.files[k] == f)
                    {
                        waiting.push_back ({ &e, i, k });
                        const auto fi = e.waitingFileIndex (i, k);
                        if (frames < 0 && fi < r.files.size()) { frames = r.waiting.filesLength(); channels = r.files[fi].numChannels; rate = r.sampleRate; }
                    }
            }
            bool follows = false;
            for (auto* list : { &e.regions, &e.overdubs })
                for (auto& r : *list)
                    for (auto& rf : r.files)
                        if (rf.rxFrom == f)
                        {
                            follows = true;
                            if (frames < 0)                              // a later save: it must match the copy of the earlier one
                                if (auto rd = audioops::openReader (formats(), rf.file)) { frames = rd->lengthInSamples; channels = (int) rd->numChannels; rate = rd->sampleRate; }
                        }
            if (follows) following.push_back (&e);
        }
        if (waiting.empty() && following.empty()) { dealt(); continue; }
        if (frames < 0) { problems.add (f.getFileName() + ": the audio it should replace could not be read, so it was not brought in."); dealt(); continue; }
        // check it
        {
            auto rd = audioops::openReader (formats(), f);
            if (rd == nullptr) { notYet (f.getFileName() + ": it could not be read as audio, so it was not brought in."); continue; }
            juce::String bad;
            if ((int) rd->numChannels != channels) bad = "it now has " + juce::String ((int) rd->numChannels) + " channel(s) but " + juce::String (channels) + " were sent";
            else if (std::abs (rd->sampleRate - rate) > 0.5) bad = "it is now at " + juce::String (rd->sampleRate, 0) + " Hz but it was sent at " + juce::String (rate, 0) + " Hz";
            else if (rd->lengthInSamples != frames) bad = "it is now " + secondsText (rd->lengthInSamples, rate) + " long but it must stay " + secondsText (frames, rate)
                                                          + " (the marked part plus the extra each side). Something in the program changed its length";
            if (bad.isNotEmpty()) { problems.add (f.getFileName() + ": " + bad + "."); dealt(); continue; }
        }
        // copy it (the program goes on owning its file: the edit plays the copy, and each save is a copy of its own)
        const auto copy = copyFor (f);
        if (! f.copyFileTo (copy)) { copy.deleteFile(); notYet (f.getFileName() + ": it could not be copied (is the drive full?), so it was not brought in."); continue; }
        // (3.27.1: the copy is checked header against bytes - a body shorter than its header says is a file still being written, and is tried again later)
        if (const auto e = audioops::checkWrittenLength (&formats(), copy, frames); e.isNotEmpty()) { copy.deleteFile(); notYet (f.getFileName() + ": it changed while it was being copied; it was not brought in."); continue; }
        if (! ticked) { app.project.undoTick (false, true); ticked = true; }       // what was done before is a step of its own
        bool used = false;
        for (auto& s : waiting)
            if (s.e->takeInWaitingFile (s.i, s.k, copy, f)) { used = true; changedEdits.insert (s.e); }
        for (auto* e : following)
            if (e->useRxSave (f, copy) > 0) { used = true; changedEdits.insert (e); }
        if (used) came.add (f.getFileNameWithoutExtension());
        else
        {
            copy.deleteFile();
            if (! waiting.empty())                                                   // (3.27.1: say so - it used to be turned down without a word, and the block stayed "In RX" for ever)
                problems.add (f.getFileName() + ": the piece it belongs to has changed since it was sent (a track was removed, or it was moved past the files' ends), so it was not brought in. Right-click the block and Cancel, then send it again.");
        }
        dealt();
    }
    if (! changedEdits.empty())
    {
        app.project.changed();
        app.project.undoTick (false, true);                                         // one undo step: Ctrl+Z takes this save out again
        app.setNotice ("Came back into the edit: " + came.joinIntoString (", ") + ". Ctrl+Z takes it out again.");
    }
    if (! problems.isEmpty())
        say (kTitle, juce::String (problems.size() == 1 ? "A file saved in an editing program could not come back into the edit:" : "Some files saved in an editing program could not come back into the edit:")
                     + "\n\n" + problems.joinIntoString ("\n\n")
                     + "\n\nPut it right there (Undo, or keep the same length, channels and sample rate) and save again: it comes back then. Until then the edit plays what it had.");
}

namespace
{
class Watcher : public juce::Timer
{
public:
    explicit Watcher (AppContext& a) : app (a) { startTimer (1000); }
    ~Watcher() override { stopTimer(); }
    void timerCallback() override
    {
        if (! app.hasProject()) { watch = RxSaveWatch(); return; }
        if (app.project.projectFile != projectFile) { watch = RxSaveWatch(); projectFile = app.project.projectFile; }
        if (app.engine.isRecording() || app.undoHold > 0 || app.saveHold > 0) return;            // nothing changes under a take, or a window trying something out
        if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0) return;     // nor under an open dialog (some put the edit back as they found it)
        if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown()) return;    // 3.27.1: nor in the middle of a drag (the Trim window puts the regions back from its mouse-down copy on every drag event)
        // 3.27.1: with nothing out in a program - only files that came back earlier and are followed for later saves - every fifth second is enough
        // (every such file is asked about three times a tick otherwise, for the life of the project)
        bool anyOut = false;
        for (auto& e : app.project.edits) for (auto& r : e->regions) anyOut = anyOut || (r.waiting.active() && r.waiting.rx);
        if (! anyOut && ++slowTick % 5 != 0) return;
        std::vector<RxSaveWatch::Look> looks;
        std::set<juce::String> paths;
        auto add = [&] (const juce::File& f)
        {
            RxSaveWatch::Look l; l.path = f.getFullPathName();
            if (! paths.insert (l.path).second) return;
            l.exists = f.existsAsFile();
            if (l.exists) { l.modified = f.getLastModificationTime().toMilliseconds(); l.size = f.getSize(); }
            looks.push_back (l);
        };
        for (auto& e : app.project.edits)
        {
            if (app.windowIsOpen && app.windowIsOpen ("trim:" + e->id.toString())) continue;   // (3.27.1: its Trim window puts the regions back as they were when it opened; the save comes in once it is closed)
            for (auto& r : e->regions)
            {
                if (r.waiting.active() && r.waiting.rx) for (auto& f : r.waiting.files) add (f);
                for (auto& f : r.files) if (f.rxFrom != juce::File()) add (f.rxFrom);
            }
            for (auto& r : e->overdubs) for (auto& f : r.files) if (f.rxFrom != juce::File()) add (f.rxFrom);
        }
        if (looks.empty()) { watch.pending.clear(); return; }
        const auto ready = watch.ready (looks, app.project.rxSeen);
        if (! ready.empty()) bringIn (app, ready, &watch);
    }
private:
    AppContext& app;
    RxSaveWatch watch;
    juce::File projectFile;
    int slowTick = 0;
};
}

std::unique_ptr<juce::Timer> makeWatcher (AppContext& app) { return std::make_unique<Watcher> (app); }

/** The waiting piece of piece 'rid' (nullptr when it is not out in a program). */
static const WaitingPiece* outIn (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid)
{
    auto* e = app.project.findEdit (eid); if (e == nullptr) return nullptr;
    const int i = e->indexOf (rid); if (i < 0) return nullptr;
    const auto& w = e->regions[(size_t) i].waiting;
    return w.active() && w.rx ? &w : nullptr;
}

void bringInNow (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid)
{
    const auto* w = outIn (app, eid, rid);
    if (w == nullptr) return;
    std::vector<juce::String> paths;
    for (auto& f : w->files) paths.push_back (f.getFullPathName());
    bringIn (app, paths, nullptr);
}

void openAgain (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid)
{
    const auto* w = outIn (app, eid, rid);
    if (w == nullptr) return;
    const auto programName = w->program.isNotEmpty() ? w->program : juce::String ("iZotope RX");          // (3.22.0's pieces went to RX)
    std::vector<juce::File> files; juce::StringArray missing;
    for (auto& f : w->files) { if (f.existsAsFile()) files.push_back (f); else missing.add (f.getFileName()); }
    if (! missing.isEmpty())
    {
        say (kTitle, "These files are no longer there, so they cannot be opened again:\n" + missing.joinIntoString ("\n") + "\n\nRight-click the block and choose Cancel to keep the original audio, then send it again.");
        if (files.empty()) return;
    }
    const Program* p = nullptr;
    const auto list = programs (app);
    for (auto& o : list) if (o.name.equalsIgnoreCase (programName)) p = &o;
    if (p == nullptr)
    {
        say (kTitle, programName + " is no longer in Export for Processing's list of programs. Add it again (Add a program...), or choose Show the files and open them yourself: "
                     "saved there, they still come back.");
        return;
    }
    juce::String err;
    if (! openInProgram (p->file, files, err)) say (kTitle, err);
}

void showFiles (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid)
{
    const auto* w = outIn (app, eid, rid);
    if (w == nullptr || w->files.empty()) return;
    for (auto& f : w->files) if (f.existsAsFile()) { f.revealToUser(); return; }
    w->files.front().getParentDirectory().revealToUser();
}
}} // namespace td::rxlink
