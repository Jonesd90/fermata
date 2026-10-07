#pragma once
#include "AppContext.h"

namespace td
{
/** What the Take Display shows, drawn to fill whatever size it is given (everything scales with the size): the project line, a logo,
    the piece name, the take number (green "Next take", red "This take" while recording), the time of day and the time left in the session. */
class TakeDisplayView : public juce::Component, private juce::Timer
{
public:
    explicit TakeDisplayView (AppContext&);
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    bool showTalkback = true;                    // the little preview in the control window does not

private:
    void timerCallback() override { repaint(); }
    void loadLogoIfNeeded();

    AppContext& app;
    juce::Image logo;
    juce::String logoLoadedFor;
};

/** A borderless window covering one display. Esc or a double-click closes it. */
class TakeDisplayWindow : public juce::Component
{
public:
    TakeDisplayWindow (AppContext&, TakeDisplayHub&, int displayIndex, juce::Rectangle<int> area);
    ~TakeDisplayWindow() override;
    void resized() override { view.setBounds (getLocalBounds()); }
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void closeSelf();
    TakeDisplayHub& hub;
    int index;
    TakeDisplayView view;
};

/** Which displays currently show the Take Display. Lives in the AppContext, so the displays stay up when the control window is closed. */
class TakeDisplayHub
{
public:
    explicit TakeDisplayHub (AppContext& a) : app (a) {}
    bool isOpen (int displayIndex) const { return windows.count (displayIndex) > 0; }
    void setOpen (int displayIndex, bool open);
    void closeAll() { windows.clear(); changes.sendChangeMessage(); }
    juce::ChangeBroadcaster changes;

private:
    AppContext& app;
    std::map<int, std::unique_ptr<TakeDisplayWindow>> windows;
};

/** The window where the Take Display is set up: project line, logo, session timer, which screens, and a preview. */
class TakeDisplayControl : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit TakeDisplayControl (AppContext&);
    ~TakeDisplayControl() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { syncScreens(); }
    void rebuildScreenButtons();
    void syncScreens();
    void applyTimer();
    void refreshTimerStatus();

    AppContext& app;
    juce::Label lineCaption, logoCaption, logoPathLabel, timerCaption, timerStatus, screensCaption, previewCaption, hint;
    juce::TextEditor lineEdit, timerText;
    juce::ComboBox logoBox, timerMode;
    juce::TextButton chooseLogo { "Choose image..." }, startTimer { "Start" }, clearTimer { "No countdown" };
    juce::OwnedArray<juce::TextButton> screenButtons;
    int shownDisplays = -1;
    TakeDisplayView preview;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace td
