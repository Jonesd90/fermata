#pragma once
#include "AppContext.h"
#include "OutputPicker.h"
#include "../core/Tuning.h"
#include <map>
#include <set>

namespace td
{
/** A piano keyboard of two octaves (C to C). In normal mode a key sounds while it is held (a quick click still gives a short note); in chord mode a click
    lights a key or puts it out, and the lit keys sound together while Play is held. */
class PianoKeys : public juce::Component
{
public:
    std::function<void (int midi)> onPress, onRelease;      // normal mode
    std::function<void (int midi)> onToggle;                 // chord mode
    bool chordMode = false;
    int lowest = 48;                                         // a C
    static constexpr int kOctaves = 2;
    int highest() const noexcept { return lowest + 12 * kOctaves; }
    void setSounding (const std::set<int>& s) { if (s != sounding) { sounding = s; repaint(); } }
    void setSelected (const std::set<int>& s) { if (s != selected) { selected = s; repaint(); } }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static bool isBlack (int midi) noexcept { const int pc = ((midi % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

private:
    int whiteCount() const noexcept { return 7 * kOctaves + 1; }
    float whiteW() const noexcept { return (float) getWidth() / (float) whiteCount(); }
    juce::Rectangle<float> keyRect (int midi) const;
    int keyAt (juce::Point<float>) const;
    std::set<int> sounding, selected;
    int down = -1;
};

/** A button that does something while it is held down (the chord's Play). */
class HoldButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    std::function<void()> onPress, onRelease;
    void buttonStateChanged() override
    {
        const bool isDown = getState() == juce::Button::buttonDown;
        if (isDown && ! held) { held = true; if (onPress) onPress(); }
        else if (! isDown && held) { held = false; if (onRelease) onRelease(); }
    }
private:
    bool held = false;
};

/** The pitch pipe: a keyboard that sounds a soft reed note (or a chord) on the outputs you choose - the stage speakers for the performers, or the
    control-room speakers for you and the producer alone - in any of seven tunings, on any reference A. Nothing it plays is ever recorded. */
class PitchPipeComponent : public juce::Component, private juce::Timer
{
public:
    explicit PitchPipeComponent (AppContext&);
    ~PitchPipeComponent() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PitchPipeDef& def() { return app.project.pitchPipe; }
    void settingsChanged (bool userChange = true);     // save, tell the engine, retune what is sounding, update the text
    void noteOn (int midi);
    void noteOff (int midi);
    void playChord();
    void stopChord();
    void retune();
    void updateText();
    int slotFor (int midi) const noexcept { return ((midi - 21) % 48 + 48) % 48; }
    double hzFor (int midi) const { return tuning::frequency (midi, app.project.pitchPipe.refA, app.project.pitchPipe.temperament, app.project.pitchPipe.key); }
    std::vector<int> chordNotes() const { return std::vector<int> (selected.begin(), selected.end()); }
    void timerCallback() override;

    AppContext& app;
    juce::Label refCap, tuningCap, keyCap, hzCap, tuningInfo, readout, offsets, outCap, levelCap, toneCap, softCap, brightCap;
    juce::ComboBox refBox, tuningBox, keyBox, chordTuningBox;
    juce::ToggleButton chordToggle { "Chord: choose the notes, then hold Play" };
    juce::ToggleButton keyboardToggle { "Keyboard sound" };
    HoldButton playButton { "Play" };
    juce::TextButton clearButton { "Clear" }, octDown { "< Octave" }, octUp { "Octave >" }, tbButton { "To talkback" };
    PianoKeys keys;
    OutputPicker outputs;
    juce::Slider level, tone;
    InfoNote info;
    std::set<int> held;                          // normal mode: the keys sounding
    std::set<int> selected;                      // chord mode: the lit keys
    bool chordPlaying = false;
    std::map<int, juce::uint32> pressedAt;       // when each key went down (a quick click still sounds for 0.4 s)
    std::map<int, int> pressGen;                 // so a late "off" does not stop a key pressed again since
    int lastNote = -1;
    int rule1 = 0, rule2 = 0;
};
} // namespace td
