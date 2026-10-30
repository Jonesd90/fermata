#pragma once
#include "AppContext.h"

namespace td
{
/** 3.18.0: the Pre length - how many seconds before Record a take starts (1 to 10, 5 unless chosen) - as a slider. In the pop-up of the Pre half of the
    Rec | Pre button (right-click) and in Audio settings. Follows a change made in the other place. */
class PreLengthControl : public juce::Component, private juce::Timer
{
public:
    explicit PreLengthControl (AppContext& a) : app (a)
    {
        caption.setText ("Pre: takes start", juce::dontSendNotification);
        after.setText ("before Record", juce::dontSendNotification);
        for (auto* l : { &caption, &after }) { l->setColour (juce::Label::textColourId, theme::text); l->setFont (juce::FontOptions (13.0f)); addAndMakeVisible (l); }
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setRange (AudioEngine::kPreRollMin, AudioEngine::kPreRollMax, 1.0);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22);
        slider.setTextValueSuffix (" s");
        slider.setValue (app.engine.getPreRollSeconds(), juce::dontSendNotification);
        slider.onValueChange = [this] { app.setPreRollSeconds (juce::roundToInt (slider.getValue())); };
        slider.setTooltip ("How far back a take starts when Pre is on: 1 to 10 seconds (5 unless you change it). Fermata keeps the inputs for this long all the time; "
                           "longer costs only memory (about 25 MB per second for 64 inputs at 96 kHz). Changed during a take, it applies from the next one.");
        addAndMakeVisible (slider);
        startTimer (500);
    }
    static constexpr int kWidth = 420, kHeight = 30;
    void resized() override
    {
        auto r = getLocalBounds();
        caption.setBounds (r.removeFromLeft (112));
        after.setBounds (r.removeFromRight (96));
        slider.setBounds (r.reduced (2, 3));
    }
private:
    void timerCallback() override
    {
        if (! slider.isMouseButtonDown() && juce::roundToInt (slider.getValue()) != app.engine.getPreRollSeconds())
            slider.setValue (app.engine.getPreRollSeconds(), juce::dontSendNotification);
    }
    AppContext& app;
    juce::Label caption, after;
    juce::Slider slider;
};

/** The pop-up of the Pre half: Pre on / off and its length. */
class PrePopup : public juce::Component, private juce::Timer
{
public:
    explicit PrePopup (AppContext& a) : app (a), length (a)
    {
        addAndMakeVisible (onOff); addAndMakeVisible (length); addAndMakeVisible (note);
        onOff.setClickingTogglesState (false);
        onOff.onClick = [this] { app.setSessionMode (! app.sessionMode()); timerCallback(); };
        note.setFont (juce::FontOptions (12.0f));
        note.setColour (juce::Label::textColourId, theme::dimText);
        note.setText ("Pre keeps the inputs for the last few seconds all the time, so a take starts that long before Record was pressed - "
                      "a start is never missed. It works with or without Rec.", juce::dontSendNotification);
        note.setJustificationType (juce::Justification::topLeft);
        setSize (PreLengthControl::kWidth + 20, 118);
        startTimer (300);
        timerCallback();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        onOff.setBounds (r.removeFromTop (26).removeFromLeft (150));
        r.removeFromTop (6);
        length.setBounds (r.removeFromTop (PreLengthControl::kHeight));
        r.removeFromTop (4);
        note.setBounds (r);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }
private:
    void timerCallback() override
    {
        const bool on = app.sessionMode();
        onOff.setToggleState (on, juce::dontSendNotification);
        onOff.setButtonText (on ? "Pre is ON" : "Pre is off");
        onOff.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2e9e4f));
    }
    AppContext& app;
    juce::TextButton onOff;
    PreLengthControl length;
    juce::Label note;
};

inline void showPrePopup (AppContext& app, juce::Component& anchor)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<PrePopup> (app), anchor.getScreenBounds(), nullptr);
}
} // namespace td
