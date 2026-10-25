#include "PitchPipeWindow.h"
#include "FermataLookAndFeel.h"

namespace td
{
// ============================================================================================================================ PianoKeys
juce::Rectangle<float> PianoKeys::keyRect (int midi) const
{
    const float ww = whiteW(), h = (float) getHeight();
    int whites = 0;                                                   // white keys before this one
    for (int m = lowest; m < midi; ++m) if (! isBlack (m)) ++whites;
    if (! isBlack (midi)) return { (float) whites * ww, 0.0f, ww, h };
    const float bw = ww * 0.62f;
    return { (float) whites * ww - bw * 0.5f, 0.0f, bw, h * 0.62f };
}

int PianoKeys::keyAt (juce::Point<float> p) const
{
    for (int m = lowest; m <= highest(); ++m) if (isBlack (m) && keyRect (m).contains (p)) return m;     // the black keys lie on top
    for (int m = lowest; m <= highest(); ++m) if (! isBlack (m) && keyRect (m).contains (p)) return m;
    return -1;
}

void PianoKeys::paint (juce::Graphics& g)
{
    const juce::Colour ivory (0xfff4f1ea), ebony (0xff1c1f24), lit (0xffe8b923), on = theme::accent;
    for (int m = lowest; m <= highest(); ++m)
    {
        if (isBlack (m)) continue;
        auto r = keyRect (m).reduced (0.5f, 0.0f);
        const bool snd = sounding.count (m) > 0, sel = selected.count (m) > 0;
        g.setColour (snd ? on.brighter (0.25f) : sel ? lit : ivory);
        g.fillRoundedRectangle (r.withTrimmedTop (-4.0f), 4.0f);
        g.setColour (juce::Colour (0xff6b7480)); g.drawRoundedRectangle (r.withTrimmedTop (-4.0f), 4.0f, 1.0f);
        const int pc = ((m % 12) + 12) % 12;
        g.setColour (snd || sel ? juce::Colours::black : juce::Colour (pc == 0 ? 0xff30343b : 0xff8a919b));
        g.setFont (juce::FontOptions (pc == 0 ? 14.0f : 12.0f, pc == 0 ? juce::Font::bold : juce::Font::plain));
        g.drawText (pc == 0 ? tuning::noteName (m) : juce::String (tuning::pitchClassName (pc)), r.removeFromBottom (28.0f).toNearestInt(), juce::Justification::centred, false);
    }
    for (int m = lowest; m <= highest(); ++m)
    {
        if (! isBlack (m)) continue;
        auto r = keyRect (m);
        const bool snd = sounding.count (m) > 0, sel = selected.count (m) > 0;
        g.setColour (snd ? on : sel ? lit.darker (0.15f) : ebony);
        g.fillRoundedRectangle (r.withTrimmedTop (-4.0f), 3.0f);
        if (! snd && ! sel) { g.setColour (juce::Colours::white.withAlpha (0.12f)); g.fillRoundedRectangle (r.reduced (r.getWidth() * 0.18f, 0.0f).withTrimmedBottom (r.getHeight() * 0.25f).withTrimmedTop (-2.0f), 2.0f); }
    }
}

void PianoKeys::mouseDown (const juce::MouseEvent& e)
{
    const int k = keyAt (e.position);
    if (k < 0) return;
    if (chordMode) { if (onToggle) onToggle (k); return; }
    down = k;
    if (onPress) onPress (k);
}
void PianoKeys::mouseDrag (const juce::MouseEvent& e)
{
    if (chordMode || down < 0) return;
    const int k = keyAt (e.position);
    if (k >= 0 && k != down)                                         // sliding along the keys: one note after another
    {
        if (onRelease) onRelease (down);
        down = k;
        if (onPress) onPress (k);
    }
}
void PianoKeys::mouseUp (const juce::MouseEvent&)
{
    if (chordMode || down < 0) return;
    if (onRelease) onRelease (down);
    down = -1;
}

// ============================================================================================================================ PitchPipeComponent
PitchPipeComponent::PitchPipeComponent (AppContext& a)
    : app (a), outputs (a, [this]() -> std::vector<int>& { return def().outputs; }, [this] { settingsChanged(); })
{
    auto caption = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (12.5f, juce::Font::bold)); l.setColour (juce::Label::textColourId, theme::text);
        l.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (l);
    };
    caption (refCap, "A ="); caption (hzCap, "Hz"); caption (tuningCap, "Tuning"); caption (keyCap, "Key"); caption (outCap, "Plays to"); caption (levelCap, "Level"); caption (toneCap, "Tone"); caption (softCap, "Soft"); caption (brightCap, "Bright");
    for (auto* l : { &softCap, &brightCap }) { l->setFont (juce::FontOptions (11.5f)); l->setColour (juce::Label::textColourId, theme::dimText); }
    softCap.setJustificationType (juce::Justification::centredRight);
    for (auto* l : { &tuningInfo, &readout, &offsets })
    {
        l->setFont (juce::FontOptions (12.0f)); l->setColour (juce::Label::textColourId, theme::dimText);
        l->setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (l);
    }
    readout.setFont (juce::FontOptions (14.0f, juce::Font::bold)); readout.setColour (juce::Label::textColourId, theme::text);

    // the reference: the usual pitches, or type any other
    refBox.setEditableText (true);
    int id = 1;
    for (double hz : { 415.0, 430.0, 432.0, 435.0, 440.0, 442.0, 443.0, 466.0 }) refBox.addItem (juce::String (hz, 0), id++);
    refBox.setText (juce::String (def().refA, def().refA == std::floor (def().refA) ? 0 : 1), juce::dontSendNotification);
    refBox.setTooltip ("The reference pitch: the A above middle C. Choose one, or type any (380 - 500 Hz). Every tuning keeps A exactly here.");
    refBox.onChange = [this]
    {
        const double v = refBox.getText().retainCharacters ("0123456789.").getDoubleValue();
        if (v >= 380.0 && v <= 500.0) { def().refA = v; settingsChanged(); }
        refBox.setText (juce::String (def().refA, def().refA == std::floor (def().refA) ? 0 : 1), juce::dontSendNotification);
    };
    addAndMakeVisible (refBox);
    for (int t = 0; t < tuning::NumTemperaments; ++t) tuningBox.addItem (tuning::temperamentName (t), t + 1);
    tuningBox.setSelectedId (def().temperament + 1, juce::dontSendNotification);
    tuningBox.onChange = [this] { def().temperament = juce::jlimit (0, tuning::NumTemperaments - 1, tuningBox.getSelectedId() - 1); settingsChanged(); };
    tuningBox.setTooltip ("How the notes are tuned. Every tuning except Equal sounds different in each key: choose the key below.");
    addAndMakeVisible (tuningBox);
    for (int k = 0; k < 12; ++k) keyBox.addItem (tuning::pitchClassName (k), k + 1);
    keyBox.setSelectedId (def().key + 1, juce::dontSendNotification);
    keyBox.onChange = [this] { def().key = juce::jlimit (0, 11, keyBox.getSelectedId() - 1); settingsChanged(); };
    keyBox.setTooltip ("The key the tuning is built on (the piece's key). Equal temperament is the same in every key.");
    addAndMakeVisible (keyBox);

    chordToggle.setToggleState (def().chordMode, juce::dontSendNotification);
    chordToggle.setTooltip ("Chord mode: click the keys of a chord (they light up), then hold Play to hear them together. Click a lit key to take it out.");
    chordToggle.onClick = [this]
    {
        stopChord();
        for (int m : std::set<int> (held)) noteOff (m);
        def().chordMode = chordToggle.getToggleState();
        keys.chordMode = def().chordMode;
        settingsChanged();
    };
    addAndMakeVisible (chordToggle);
    for (int c = 0; c < tuning::NumChordTunings; ++c) chordTuningBox.addItem (tuning::chordTuningName (c), c + 1);
    chordTuningBox.setSelectedId (def().chordTuning + 1, juce::dontSendNotification);
    chordTuningBox.setTooltip ("As the tuning: every note as the tuning has it. Pure major / minor chord: the chord tuned pure on its root - a major third 13.7 cents "
                               "below the piano's, a minor third 15.6 cents above, the fifth 2 cents wide - whatever the tuning (the root stays where the tuning puts it).");
    chordTuningBox.onChange = [this] { def().chordTuning = juce::jlimit (0, tuning::NumChordTunings - 1, chordTuningBox.getSelectedId() - 1); settingsChanged(); };
    addAndMakeVisible (chordTuningBox);
    playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    playButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    playButton.setTooltip ("Hold to play the chord; let go to stop");
    playButton.onPress = [this] { playChord(); };
    playButton.onRelease = [this] { stopChord(); };
    playButton.setWantsKeyboardFocus (false);
    addAndMakeVisible (playButton);
    clearButton.setTooltip ("Put out all the lit keys");
    clearButton.onClick = [this] { stopChord(); selected.clear(); keys.setSelected (selected); updateText(); };
    addAndMakeVisible (clearButton);
    octDown.onClick = [this] { if (def().lowestC > 24) { def().lowestC -= 12; keys.lowest = def().lowestC; keys.repaint(); app.project.markDirty(); } };
    octUp.onClick   = [this] { if (def().lowestC < 84) { def().lowestC += 12; keys.lowest = def().lowestC; keys.repaint(); app.project.markDirty(); } };
    octDown.setTooltip ("The two octaves an octave lower"); octUp.setTooltip ("The two octaves an octave higher");
    for (auto* b : { &octDown, &octUp, &clearButton }) { b->setWantsKeyboardFocus (false); addAndMakeVisible (b); }

    keys.lowest = def().lowestC;
    keys.chordMode = def().chordMode;
    keys.onPress = [this] (int m) { noteOn (m); };
    keys.onRelease = [this] (int m) { noteOff (m); };
    keys.onToggle = [this] (int m)
    {
        if (selected.count (m)) selected.erase (m); else selected.insert (m);
        keys.setSelected (selected);
        if (chordPlaying) playChord();                           // (changing the chord while it sounds: the notes glide to their new pitches)
        updateText();
    };
    addAndMakeVisible (keys);

    addAndMakeVisible (outputs);
    setUpTalkbackButton (tbButton, "the pitch pipe");
    tbButton.setToggleState (def().toTalkback, juce::dontSendNotification);
    tbButton.onClick = [this]
    {
        def().toTalkback = tbButton.getToggleState(); settingsChanged (true);
        if (def().toTalkback && app.project.tbOutputs.empty()) app.setNotice ("To talkback is on, but no talkback output is chosen: press TB under one of the main window's outputs.");
    };
    addAndMakeVisible (tbButton);
    level.setSliderStyle (juce::Slider::LinearHorizontal);
    level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
    level.setRange (-40.0, 0.0, 0.5);
    level.setTextValueSuffix (" dB");
    level.setValue (def().levelDb, juce::dontSendNotification);
    level.onValueChange = [this] { def().levelDb = (float) level.getValue(); settingsChanged(); };
    addAndMakeVisible (level);
    tone.setSliderStyle (juce::Slider::LinearHorizontal);
    tone.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    tone.setRange (0.0, 1.0, 0.01);
    tone.setValue (def().tone, juce::dontSendNotification);
    tone.setDoubleClickReturnValue (true, 0.35);
    tone.setTooltip ("How soft or bright the reed sounds. Soft: mostly the note and its first few overtones. Bright: the reed as it comes. It is the same softness at every pitch, "
                     "and the note is just as loud. You hear it change at once. Double-click for the usual setting.");
    tone.onValueChange = [this] { def().tone = (float) tone.getValue(); settingsChanged(); };
    addAndMakeVisible (tone);
    keyboardToggle.setToggleState (def().keyboard, juce::dontSendNotification);
    keyboardToggle.setTooltip ("A simple keyboard note instead of the pitch pipe's reed: it strikes, then settles to a steady level while you hold the key, "
                               "and stops quickly when you let go. The same notes and tunings; nothing is recorded.");
    keyboardToggle.onClick = [this] { def().keyboard = keyboardToggle.getToggleState(); settingsChanged(); };
    keyboardToggle.setWantsKeyboardFocus (false);
    addAndMakeVisible (keyboardToggle);

    info.setText ("The pitch pipe plays a soft reed note - like a real pitch pipe - on the outputs ticked under Plays to: the stage speakers for the performers, "
                  "or your control-room speakers to check a note yourself while they record, without them hearing it. Nothing it plays is recorded.\n\n"
                  "To talkback (beside Plays to): it also goes to the talkback speakers, with the talkback mic and Playback. Its level there is the "
                  "Click/Pipe box at the top right of the main window.\n\n"
                  "Click a key and hold it: it sounds while you hold it (a quick click still gives a short note); slide along the keys for one note after another. "
                  "The keyboard shows two octaves; the Octave buttons move it.\n\n"
                  "Chord: tick it, click the keys of the chord (they light up), then hold Play. Pure major / pure minor chord tunes the chord pure on its root: "
                  "the major third 13.7 cents lower than a piano's, the minor third 15.6 cents higher, the fifth 2 cents wide - the way a choir or a string "
                  "quartet tunes a chord. The root stays where the tuning puts it, and Fermata finds the root even in an inversion.\n\n"
                  "Tuning: equal temperament, just intonation (pure in the key, poor in distant keys), Pythagorean, quarter-comma meantone, and the "
                  "well temperaments of Werckmeister (III, 1691), Vallotti and Kirnberger (III). Every tuning but equal temperament is built on the Key, "
                  "and A is always exactly the reference (A = 440, 415 for Baroque pitch ...). The line under the keyboard gives each note's offset in cents.\n\n"
                  "A note keeps its pitch from the moment it starts to the end of its fade-out: letting go never bends it. If you change the tuning, the key or the "
                  "lit keys while a note is held, it glides to its new pitch in a twentieth of a second, so you can hear one tuning against another.\n\n"
                  "Tone: Soft is mostly the note and its first few overtones; Bright is the reed as it comes. Same loudness either way.\n\n"
                  "Keyboard sound: a simple keyboard note instead of the reed - it strikes, settles to a steady level while the key is held, and stops quickly when it is let go.");
    addAndMakeVisible (info);

    settingsChanged (false);
    startTimer (250);
    setSize (980, 620);
}

PitchPipeComponent::~PitchPipeComponent()
{
    app.engine.auxSounds().pipe.allOff();                        // a note never hangs on after the window is gone
}

void PitchPipeComponent::settingsChanged (bool userChange)
{
    if (userChange) app.project.markDirty();                        // (opening the window changes nothing)
    app.applyAux();
    chordTuningBox.setEnabled (def().chordMode);
    playButton.setEnabled (def().chordMode);
    clearButton.setEnabled (def().chordMode);
    keys.chordMode = def().chordMode;
    if (! def().chordMode && ! selected.empty()) { selected.clear(); keys.setSelected (selected); }
    retune();
    updateText();
    outputs.refresh();
}

void PitchPipeComponent::noteOn (int midi)
{
    auto& pipe = app.engine.auxSounds().pipe;
    pipe.setSlot (slotFor (midi), hzFor (midi), true);
    held.insert (midi);
    pressedAt[midi] = juce::Time::getMillisecondCounter();
    ++pressGen[midi];
    lastNote = midi;
    keys.setSounding (held);
    updateText();
    if (def().outputs.empty()) readout.setText ("Choose where it plays (Plays to, below): nothing is ticked yet.", juce::dontSendNotification);
}

void PitchPipeComponent::noteOff (int midi)
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto since = now - pressedAt[midi];
    const int gen = pressGen[midi];
    auto off = [this, midi]
    {
        app.engine.auxSounds().pipe.setSlot (slotFor (midi), hzFor (midi), false);
        held.erase (midi);
        keys.setSounding (held);
    };
    if (since >= 400) { off(); return; }
    juce::Component::SafePointer<PitchPipeComponent> safe (this);
    juce::Timer::callAfterDelay ((int) (400 - since), [safe, midi, gen, off]
    {
        if (safe == nullptr) return;
        if (safe->pressGen[midi] != gen || safe->pressedAt[midi] == 0) return;     // pressed again since: that press decides
        off();
    });
}

void PitchPipeComponent::playChord()
{
    auto& pipe = app.engine.auxSounds().pipe;
    const auto notes = chordNotes();
    if (notes.empty()) { readout.setText ("Click the keys of the chord first (they light up), then hold Play.", juce::dontSendNotification); return; }
    const auto c = tuning::chordFrequencies (notes, def().refA, def().temperament, def().key, def().chordTuning);
    std::set<int> now (notes.begin(), notes.end());
    for (int m : held) if (! now.count (m)) pipe.setSlot (slotFor (m), hzFor (m), false);      // notes taken out of the chord while it sounds
    for (size_t i = 0; i < notes.size(); ++i) pipe.setSlot (slotFor (notes[i]), c.freqs[i], true);
    held = now;
    chordPlaying = true;
    keys.setSounding (held);
    updateText();
}

void PitchPipeComponent::stopChord()
{
    if (! chordPlaying) return;
    auto& pipe = app.engine.auxSounds().pipe;
    for (int m : held) pipe.setSlot (slotFor (m), hzFor (m), false);
    held.clear();
    chordPlaying = false;
    keys.setSounding (held);
    updateText();
}

void PitchPipeComponent::retune()
{
    auto& pipe = app.engine.auxSounds().pipe;
    if (chordPlaying) { playChord(); return; }
    for (int m : held) pipe.setSlot (slotFor (m), hzFor (m), true);
}

void PitchPipeComponent::updateText()
{
    const auto& d = def();
    tuningInfo.setText (juce::String (tuning::temperamentInfo (d.temperament)), juce::dontSendNotification);
    keyBox.setEnabled (d.temperament != tuning::Equal);
    // the offsets of the twelve notes in this tuning
    {
        juce::String s = juce::String ("Cents from equal temperament") + (d.temperament == tuning::Equal ? juce::String (":") : juce::String (" (") + tuning::temperamentName (d.temperament) + " on " + tuning::pitchClassName (d.key) + "):");
        for (int pc = 0; pc < 12; ++pc)
        {
            const double c = tuning::offsetCents (d.temperament, d.key, pc);
            s << "   " << tuning::pitchClassName (pc) << " " << (c >= 0.05 ? "+" : "") << juce::String (std::abs (c) < 0.05 ? 0.0 : c, 1);
        }
        offsets.setText (s, juce::dontSendNotification);
    }
    // what is sounding (or the chord that will)
    auto describe = [] (int m, double hz, double refA)
    {
        const double c = tuning::centsFromEqual (hz, m, refA);
        return tuning::noteName (m) + "  " + juce::String (hz, 2) + " Hz  (" + (c >= 0.05 ? "+" : "") + juce::String (std::abs (c) < 0.05 ? 0.0 : c, 1) + ")";
    };
    if (d.chordMode)
    {
        const auto notes = chordNotes();
        if (notes.empty()) { readout.setText ("Chord: click the keys of the chord, then hold Play.", juce::dontSendNotification); return; }
        const auto c = tuning::chordFrequencies (notes, d.refA, d.temperament, d.key, d.chordTuning);
        juce::StringArray parts;
        for (size_t i = 0; i < notes.size(); ++i) parts.add (describe (notes[i], c.freqs[i], d.refA));
        juce::String tail;
        if (d.chordTuning != tuning::AsTuned && notes.size() >= 2)
            tail = juce::String ("    pure ") + (d.chordTuning == tuning::PureMinor ? "minor" : "major") + " chord on " + tuning::noteName (c.root)
                 + (c.triadFound ? juce::String() : juce::String (" (no ") + (d.chordTuning == tuning::PureMinor ? "minor" : "major") + " triad in these notes: tuned on the lowest note)");
        readout.setText ((chordPlaying ? "Playing:  " : "Chord:  ") + parts.joinIntoString ("   ") + tail, juce::dontSendNotification);
    }
    else if (lastNote >= 0)
        readout.setText ((held.empty() ? "Last note:  " : "Playing:  ") + describe (lastNote, hzFor (lastNote), d.refA) + "  cents from equal temperament", juce::dontSendNotification);
    else
        readout.setText ("Click a key and hold it to hear the note.", juce::dontSendNotification);
}

void PitchPipeComponent::timerCallback()
{
    // the reference / tuning changed elsewhere (another window, a project load): follow it
    const auto& d = def();
    if (tuningBox.getSelectedId() != d.temperament + 1) tuningBox.setSelectedId (d.temperament + 1, juce::dontSendNotification);
    if (keyBox.getSelectedId() != d.key + 1) keyBox.setSelectedId (d.key + 1, juce::dontSendNotification);
    if (keys.lowest != d.lowestC) { keys.lowest = d.lowestC; keys.repaint(); }
    if (keyboardToggle.getToggleState() != d.keyboard) keyboardToggle.setToggleState (d.keyboard, juce::dontSendNotification);
    if (tbButton.getToggleState() != d.toTalkback) tbButton.setToggleState (d.toTalkback, juce::dontSendNotification);
}

void PitchPipeComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::text); g.setFont (FermataLookAndFeel::displayFont (17.0f));
    g.drawText ("Pitch pipe", 16, 10, getWidth() - 60, 24, juce::Justification::centredLeft, false);
    g.setColour (theme::border);
    if (rule1 > 0) g.fillRect (16, rule1, getWidth() - 32, 1);
    if (rule2 > 0) g.fillRect (16, rule2, getWidth() - 32, 1);
}

void PitchPipeComponent::resized()
{
    auto r = getLocalBounds().reduced (28, 16);
    info.placeTopRight (*this, 10);
    r.removeFromTop (38);                                                    // the title
    {
        auto row = r.removeFromTop (32);
        refCap.setBounds (row.removeFromLeft (36)); refBox.setBounds (row.removeFromLeft (96)); hzCap.setBounds (row.removeFromLeft (38)); row.removeFromLeft (30);
        tuningCap.setBounds (row.removeFromLeft (60)); tuningBox.setBounds (row.removeFromLeft (240)); row.removeFromLeft (30);
        keyCap.setBounds (row.removeFromLeft (40)); keyBox.setBounds (row.removeFromLeft (84));
    }
    r.removeFromTop (6);
    tuningInfo.setBounds (r.removeFromTop (22));
    r.removeFromTop (16);
    {
        auto row = r.removeFromTop (34);
        chordToggle.setBounds (row.removeFromLeft (300)); row.removeFromLeft (14);
        chordTuningBox.setBounds (row.removeFromLeft (210).reduced (0, 2)); row.removeFromLeft (18);
        playButton.setBounds (row.removeFromLeft (120)); row.removeFromLeft (10);
        clearButton.setBounds (row.removeFromLeft (80));
        octUp.setBounds (row.removeFromRight (96)); row.removeFromRight (8); octDown.setBounds (row.removeFromRight (96));
    }
    r.removeFromTop (16);
    // the bottom: Plays to, and the Level and Tone sliders on one line, under a rule
    {
        auto lv = r.removeFromBottom (30);
        levelCap.setBounds (lv.removeFromLeft (64)); level.setBounds (lv.removeFromLeft (290)); lv.removeFromLeft (36);
        toneCap.setBounds (lv.removeFromLeft (46)); softCap.setBounds (lv.removeFromLeft (36)); lv.removeFromLeft (4);
        tone.setBounds (lv.removeFromLeft (200)); lv.removeFromLeft (4); brightCap.setBounds (lv.removeFromLeft (50)); lv.removeFromLeft (24);
        keyboardToggle.setBounds (lv.removeFromLeft (juce::jmin (160, lv.getWidth())));
    }
    r.removeFromBottom (14);
    {
        const int outH = juce::jmax (26, outputs.heightFor (r.getWidth() - 80 - 140));
        auto row = r.removeFromBottom (outH);
        outCap.setBounds (row.removeFromLeft (80).withHeight (24));
        tbButton.setBounds (row.removeFromRight (126).withHeight (26)); row.removeFromRight (14);               // To talkback, beside Plays to (and separate from it)
        outputs.setBounds (row);
    }
    r.removeFromBottom (14);
    rule2 = r.getBottom() - 1;
    r.removeFromBottom (14);
    offsets.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (4);
    readout.setBounds (r.removeFromBottom (28));
    r.removeFromBottom (12);
    keys.setBounds (r);
    rule1 = 0;
}
} // namespace td
