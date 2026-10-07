#pragma once
#include "AppContext.h"
#include "../core/Bounce.h"

namespace td
{
class BounceSourceList;

/** Bounce Out: makes a master file (or several) from an edit or a take, through the processing mixer's settings (or another mixer's). */
class BounceComponent : public juce::Component, private juce::Timer
{
public:
    BounceComponent (AppContext&, const BounceContext&);
    ~BounceComponent() override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void start();
    void finished (const BounceResult&);
    bool buildSettings (BounceSettings&, juce::String& error) const;
    void updateEnabled();
    void chooseFolder();
    void setBusy (bool);

    AppContext& app;
    BounceContext ctx;
    bool isEdit = true;
    juce::Label title, rangeCaption, leadCaption, tailCaption, mixerCaption, sourcesCaption, nameCaption, folderCaption, status, normCaption;
    juce::ToggleButton fullButton, ioButton, selectedButton, allTakesButton;
    juce::ComboBox leadBox, tailBox, mixerBox;
    std::unique_ptr<BounceSourceList> sources;
    juce::Viewport sourcesViewport;
    juce::TextButton allTracks { "All audio tracks" }, allInt { "All Int buses" }, allExt { "All Ext buses" }, clearAll { "Clear" };
    juce::ToggleButton normToggle { "Normalise so the loudest peak is" };
    juce::ToggleButton togetherToggle { "Normalise all the files together (keeps their balance, for stems)" },
                       outNameToggle { "Add the output's name to each file name" };
    juce::TextEditor peakEditor, nameEditor, folderEditor;
    juce::TextButton chooseButton { "Choose folder..." }, bounceButton { "Bounce" }, closeButton { "Close" }, showButton { "Show in folder" };
    juce::ProgressBar progressBar;
    double progressValue = 0.0;
    std::unique_ptr<BounceJob> job;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastFile;
};
} // namespace td
