#pragma once
#include "Uikit.h"

namespace td
{
/** Hosts VST3 plug-ins in mixer insert slots. */
class PluginHost : private juce::Thread
{
public:
    PluginHost();
    ~PluginHost() override;

    juce::KnownPluginList& getKnownPlugins() noexcept { return known; }
    bool isScanning() const noexcept { return scanning.load(); }
    /** False if a scan is already running (or the scan thread could not be started). */
    bool startScan (std::function<void()> whenFinished = {});
    juce::String scanStatus() const { const juce::ScopedLock sl (statusLock); return status; }

    /** Message thread. Returns null (with an error message) if the plug-in can't be loaded. */
    std::unique_ptr<InsertProcessor> create (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error);
    /** The factory a project (or a render copy of one) uses to bring its saved plug-ins back. A plug-in that cannot be started is logged, with the
        reason, and the reason is kept in lastFactoryError (message thread) for whoever called the factory to tell the person. */
    InsertFactory makeFactory (std::function<double()> sampleRate, std::function<int()> blockSize);
    /** Why the last plug-in the factory was asked for could not be started (empty: it could). Message thread. */
    juce::String lastFactoryError;

    void showEditor (InsertProcessor*);
    void closeEditorsFor (InsertProcessor*);
    void closeAllEditors();
    /** Set by the application: makes a plug-in window belong to the main window (so it cannot go behind it). Returns false if that is not possible here. */
    std::function<bool (juce::Component&)> adoptWindow;
    /** Every open plug-in window (to keep them in front of the main window). */
    void forEachEditor (const std::function<void (juce::Component&)>& fn);

    juce::PopupMenu buildPluginMenu (int baseId, std::vector<juce::PluginDescription>& outDescs) const;

    // ---- the plug-in manager (Audio settings > Plug-ins...). It works on plug-in FILES: a file that holds several plug-ins (a shell) goes with all of them.
    /** One plug-in file as the manager lists it. */
    struct FileEntry
    {
        enum class State { found, ignored, crashed };
        juce::String file, names;                 // the file (or identifier) and the plug-ins in it ("Name (Maker)", ...)
        State state = State::found;
    };
    std::vector<FileEntry> listFiles() const;                 // (not while scanning: JUCE's lists are not locked against the scan thread)
    /** A file the user chose to ignore: it is never scanned, never loaded with a project (the slot keeps it, with its settings, unloaded) and never
        offered. A copy already loaded keeps working (bounces, copies) until the project is opened again. */
    bool isIgnored (const juce::String& fileOrIdentifier) const { return ignored.contains (fileOrIdentifier, true); }
    /** Message thread, not while scanning (returns false then). Ignoring takes the file's plug-ins out of the lists at once; using it again scans the file
        again (then 'done' is called, on the message thread). */
    bool setIgnored (const juce::String& fileOrIdentifier, bool ignore, std::function<void()> done = {});
    /** A file left out because it crashed Fermata while it was being scanned: taken off that list and scanned again now ('done' afterwards). If it crashes
        again, the next scan leaves it out again. */
    bool tryAgain (const juce::String& fileOrIdentifier, std::function<void()> done = {});

private:
    void run() override;
    void saveLists();
    /** Takes 'file' out of the scan's crash note (plugin-scan-in-progress.txt), which would otherwise leave it out again before it is tried. */
    void forgetCrashNote (const juce::String& file);
    juce::File pedalFile() const { return listFile.getSiblingFile ("plugin-scan-in-progress.txt"); }
    class EditorWindow;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    std::atomic<bool> scanning { false };
    std::function<void()> finished;
    juce::StringArray onlyFiles;                  // a scan of just these files (try again / use again), set before the thread starts
    juce::StringArray ignored, ignoredNames;      // the ignored files and the names of the plug-ins that were in them (message thread)
    mutable juce::CriticalSection statusLock;
    juce::String status;
    juce::File listFile, ignoredFile;
    std::vector<std::unique_ptr<EditorWindow>> editors;
};
} // namespace td
