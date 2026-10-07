#pragma once
#include "AppContext.h"

namespace td
{
class MediaTreeItem;

/** The Media window: a folder tree of exactly where the project's audio files are on disk (the project folder, plus the folder of any
    file that lives somewhere else). Clicking a take or a piece in the Take / Edit windows highlights its file here, even when this window is behind. */
class MediaComponent : public juce::Component, private juce::Timer
{
public:
    explicit MediaComponent (AppContext&);
    ~MediaComponent() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    /** Opens the folders down to this file and highlights it. */
    void selectFile (const juce::File&);
    void refresh();

private:
    void timerCallback() override;
    juce::String signature() const;
    std::vector<juce::File> rootFolders() const;

    AppContext& app;
    juce::TreeView tree;
    std::unique_ptr<MediaTreeItem> root;
    juce::TextButton refreshButton { "Refresh" }, revealButton { "Show in Explorer" };
    juce::Label info;
    juce::String lastSignature;
    std::vector<juce::File> extraRoots;       // folders added because a selected file is not under any other root
    juce::File pending;
    int contentWidth = 600;                    // what the rows are told their width is
    friend class MediaTreeItem;
};
} // namespace td
