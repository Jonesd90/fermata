#pragma once
#include "AppContext.h"
#include "OutputPicker.h"
#include "FermataLookAndFeel.h"

namespace td
{
/** The click: a steady beat on the outputs you choose, switched on and off by hand (it is not tied to Play or Record). Tempo (typed, dragged or
    tapped), the bar (the first beat accented if wanted) and one of four sounds - among them a soft, rounded one that is easier to play with than a
    hard tick. Never recorded. */
class ClickComponent : public juce::Component, private juce::Timer
{
public:
    explicit ClickComponent (AppContext& a)
        : app (a), outputs (a, [this]() -> std::vector<int>& { return def().outputs; }, [this] { changed(); })
    {
        auto caption = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (12.5f, juce::Font::bold)); l.setColour (juce::Label::textColourId, theme::text);
            l.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (l);
        };
        caption (tempoCap, "Tempo"); caption (barCap, "Bar"); caption (soundCap, "Sound"); caption (outCap, "Plays to"); caption (levelCap, "Level");
        startButton.setClickingTogglesState (false);
        startButton.onClick = [this]
        {
            auto& ck = app.engine.auxSounds().click;
            app.applyAux();
            ck.setRunning (! ck.running());
            if (ck.running() && def().outputs.empty()) status.setText ("It is running, but goes nowhere: choose its outputs under Plays to.", juce::dontSendNotification);
            else status.setText ({}, juce::dontSendNotification);
            update();
        };
        startButton.setTooltip ("Start or stop the click");
        addAndMakeVisible (startButton);

        tempo.setSliderStyle (juce::Slider::LinearHorizontal);
        tempo.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 24);
        tempo.setRange (20.0, 300.0, 0.5);
        tempo.setTextValueSuffix (" bpm");
        tempo.setValue (def().bpm, juce::dontSendNotification);
        tempo.onValueChange = [this] { def().bpm = tempo.getValue(); changed(); };
        tempo.setTooltip ("Beats per minute (of the beat below the bar's line: a quarter note in 4/4, an eighth in 6/8). Drag, or click the number and type.");
        addAndMakeVisible (tempo);
        tapButton.setTooltip ("Tap it in time (four taps or more): the tempo follows your taps");
        tapButton.onClick = [this] { tap(); };
        tapButton.setWantsKeyboardFocus (false);
        addAndMakeVisible (tapButton);

        for (int b = 1; b <= 16; ++b) beatsBox.addItem (juce::String (b), b);
        beatsBox.setSelectedId (def().beatsPerBar, juce::dontSendNotification);
        beatsBox.onChange = [this] { def().beatsPerBar = juce::jmax (1, beatsBox.getSelectedId()); changed(); };
        for (int u : { 2, 4, 8, 16 }) unitBox.addItem (juce::String (u), u);
        unitBox.setSelectedId (def().beatUnit, juce::dontSendNotification);
        unitBox.onChange = [this] { def().beatUnit = juce::jmax (1, unitBox.getSelectedId()); changed(); };
        beatsBox.setTooltip ("Beats in a bar"); unitBox.setTooltip ("The note that gets a beat (4 = a quarter note, 8 = an eighth)");
        slash.setText ("/", juce::dontSendNotification); slash.setJustificationType (juce::Justification::centred); slash.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (beatsBox); addAndMakeVisible (slash); addAndMakeVisible (unitBox);
        accentToggle.setToggleState (def().accent, juce::dontSendNotification);
        accentToggle.onClick = [this] { def().accent = accentToggle.getToggleState(); changed(); };
        addAndMakeVisible (accentToggle);

        for (int s = 0; s < ClickTrack::NumSounds; ++s)
        {
            auto* b = soundButtons.add (new juce::TextButton (ClickTrack::soundName (s)));
            b->setRadioGroupId (4711);
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, theme::accent);
            b->setTooltip (ClickTrack::soundInfo (s));
            b->setToggleState (def().sound == s, juce::dontSendNotification);
            b->onClick = [this, s] { if (soundButtons[s]->getToggleState()) { def().sound = s; changed(); } };
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        soundInfo.setFont (juce::FontOptions (12.0f)); soundInfo.setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (soundInfo);

        addAndMakeVisible (outputs);
        level.setSliderStyle (juce::Slider::LinearHorizontal);
        level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
        level.setRange (-40.0, 0.0, 0.5);
        level.setTextValueSuffix (" dB");
        level.setValue (def().levelDb, juce::dontSendNotification);
        level.onValueChange = [this] { def().levelDb = (float) level.getValue(); changed(); };
        addAndMakeVisible (level);
        status.setFont (juce::FontOptions (12.0f)); status.setColour (juce::Label::textColourId, theme::warn);
        addAndMakeVisible (status);
        info.setText ("A click on the outputs ticked under Plays to (the performers' headphones or the stage speakers, say). Start and stop it with the big button: "
                      "it runs on its own, not with Play or Record, and is never recorded.\n\n"
                      "Tempo: type it, drag it, or tap it with Tap. Bar: beats in a bar and the note that gets the beat; Accent puts a stronger, higher sound on "
                      "the first beat of every bar.\n\n"
                      "Sounds: Click (a short tick), Woodblock, Soft (a rounded \"bum\" with a slight swell into it, easier to play with than a hard tick) and "
                      "Low pulse (a low thud, felt as much as heard). Changes are heard at once, even while it runs.");
        addAndMakeVisible (info);
        update();
        startTimerHz (30);
        setSize (640, 360);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::text); g.setFont (FermataLookAndFeel::displayFont (17.0f));
        g.drawText ("Click", 16, 10, getWidth() - 60, 24, juce::Justification::centredLeft, false);
        // the beat lights: one per beat of the bar, the current one lit (the first of the bar in its own colour)
        const auto& ck = app.engine.auxSounds().click;
        const int n = juce::jlimit (1, 16, def().beatsPerBar);
        const bool flash = ck.running() && juce::Time::getMillisecondCounter() - lastBeatAt < 120;
        const float d = 18.0f;
        for (int i = 0; i < n; ++i)
        {
            const juce::Rectangle<float> c ((float) lights.getX() + (float) i * (d + 8.0f), (float) lights.getCentreY() - d * 0.5f, d, d);
            const bool on = flash && i == shownBeat;
            g.setColour (on ? (i == 0 && def().accent ? juce::Colour (0xffe8b923) : theme::accent) : theme::field);
            g.fillEllipse (c);
            g.setColour (theme::border.brighter (0.3f)); g.drawEllipse (c, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 10);
        info.placeTopRight (*this, 8);
        r.removeFromTop (34);
        {
            auto row = r.removeFromTop (46);
            startButton.setBounds (row.removeFromLeft (150)); row.removeFromLeft (20);
            lights = row.reduced (0, 4);
        }
        r.removeFromTop (12);
        {
            auto row = r.removeFromTop (26);
            tempoCap.setBounds (row.removeFromLeft (60)); tapButton.setBounds (row.removeFromRight (70)); row.removeFromRight (8);
            tempo.setBounds (row);
        }
        r.removeFromTop (8);
        {
            auto row = r.removeFromTop (26);
            barCap.setBounds (row.removeFromLeft (60)); beatsBox.setBounds (row.removeFromLeft (60)); slash.setBounds (row.removeFromLeft (16)); unitBox.setBounds (row.removeFromLeft (60));
            row.removeFromLeft (20); accentToggle.setBounds (row.removeFromLeft (220));
        }
        r.removeFromTop (8);
        {
            auto row = r.removeFromTop (28);
            soundCap.setBounds (row.removeFromLeft (60));
            for (auto* b : soundButtons) { b->setBounds (row.removeFromLeft (100).reduced (2, 0)); }
        }
        soundInfo.setBounds (r.removeFromTop (22).withTrimmedLeft (60));
        r.removeFromTop (8);
        const int outH = outputs.heightFor (r.getWidth() - 60);
        {
            auto row = r.removeFromTop (outH);
            outCap.setBounds (row.removeFromLeft (60).withHeight (24)); outputs.setBounds (row);
        }
        r.removeFromTop (8);
        {
            auto row = r.removeFromTop (26);
            levelCap.setBounds (row.removeFromLeft (60)); level.setBounds (row.removeFromLeft (300));
        }
        status.setBounds (r.removeFromTop (22));
    }

private:
    ClickDef& def() { return app.project.click; }
    void changed() { app.project.markDirty(); app.applyAux(); update(); }
    void update()
    {
        const bool on = app.engine.auxSounds().click.running();
        startButton.setButtonText (on ? "Stop" : "Start");
        startButton.setColour (juce::TextButton::buttonColourId, on ? juce::Colour (0xffb3261e) : juce::Colour (0xff1f7a46));
        startButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        accentToggle.setButtonText ("Accent the first beat of the bar");
        soundInfo.setText (ClickTrack::soundInfo (def().sound), juce::dontSendNotification);
        for (int s = 0; s < soundButtons.size(); ++s) soundButtons[s]->setToggleState (def().sound == s, juce::dontSendNotification);
        repaint();
    }
    void tap()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (! taps.empty() && now - taps.back() > 2500.0) taps.clear();          // a long pause starts a new count
        taps.push_back (now);
        if (taps.size() > 8) taps.erase (taps.begin());
        if (taps.size() >= 3)
        {
            const double avg = (taps.back() - taps.front()) / (double) (taps.size() - 1);
            if (avg > 150.0) { def().bpm = juce::jlimit (20.0, 300.0, std::round (60000.0 / avg * 2.0) / 2.0); tempo.setValue (def().bpm, juce::dontSendNotification); changed(); }
        }
    }
    void timerCallback() override
    {
        const auto& ck = app.engine.auxSounds().click;
        const int c = ck.beatCount();
        if (c != lastCount) { lastCount = c; shownBeat = ck.beatInBar(); lastBeatAt = juce::Time::getMillisecondCounter(); repaint (lights.expanded (4)); }
        else if (juce::Time::getMillisecondCounter() - lastBeatAt < 200) repaint (lights.expanded (4));
        if (startButton.getButtonText() != (ck.running() ? "Stop" : "Start")) update();
        if (std::abs (tempo.getValue() - def().bpm) > 1.0e-6) tempo.setValue (def().bpm, juce::dontSendNotification);
    }

    AppContext& app;
    juce::Label tempoCap, barCap, soundCap, outCap, levelCap, slash, soundInfo, status;
    juce::TextButton startButton { "Start" }, tapButton { "Tap" };
    juce::Slider tempo, level;
    juce::ComboBox beatsBox, unitBox;
    juce::ToggleButton accentToggle { "Accent the first beat of the bar" };
    juce::OwnedArray<juce::TextButton> soundButtons;
    OutputPicker outputs;
    InfoNote info;
    juce::Rectangle<int> lights;
    std::vector<double> taps;
    int lastCount = 0, shownBeat = 0;
    juce::uint32 lastBeatAt = 0;
};
} // namespace td
