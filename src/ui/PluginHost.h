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
    void startScan (std::function<void()> whenFinished = {});
    juce::String scanStatus() const { const juce::ScopedLock sl (statusLock); return status; }

    /** Message thread. Returns null (with an error message) if the plug-in can't be loaded. */
    std::unique_ptr<InsertProcessor> create (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error);
    InsertFactory makeFactory (std::function<double()> sampleRate, std::function<int()> blockSize);

    void showEditor (InsertProcessor*);
    void closeEditorsFor (InsertProcessor*);
    void closeAllEditors();
    /** Set by the application: makes a plug-in window belong to the main window (so it cannot go behind it). Returns false if that is not possible here. */
    std::function<bool (juce::Component&)> adoptWindow;
    /** Every open plug-in window (to keep them in front of the main window). */
    void forEachEditor (const std::function<void (juce::Component&)>& fn);

    juce::PopupMenu buildPluginMenu (int baseId, std::vector<juce::PluginDescription>& outDescs) const;

private:
    void run() override;
    class EditorWindow;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    std::atomic<bool> scanning { false };
    std::function<void()> finished;
    mutable juce::CriticalSection statusLock;
    juce::String status;
    juce::File listFile;
    std::vector<std::unique_ptr<EditorWindow>> editors;
};
} // namespace td
