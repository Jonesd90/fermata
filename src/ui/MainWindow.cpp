#include "MainWindow.h"
#include <set>
#include "WinOwner.h"
#include "FixTools.h"
#include "PreampSetup.h"
#include "ThemeSwitch.h"

namespace td
{
/** One driver input, drawn as a strip of the desk: a coloured bar, a black display with your own second name and the driver's name, the gain knob with its value,
    the preamp keys (MIC / 48V / phase / low cut / PAD / Z HI / BOOST, with little pictures), a live meter, the monitor keys E / S / S-L / T and CR.
    The preamp controls belong to the physical input, so there is exactly one set no matter how many mixers or tracks use it. */
class InputRow : public juce::Component
{
public:
    InputRow (AppContext& a, int index) : app (a), idx (index)
    {
        using desk::Ic;
        colour = desk::stripColour (idx);
        driver.setJustificationType (juce::Justification::centredLeft);
        driver.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain));
        driver.setColour (juce::Label::textColourId, colour);
        driver.setMinimumHorizontalScale (0.7f);
        driver.setInterceptsMouseClicks (false, false);
        driver.setTooltip ("The name the audio driver gives this input (it cannot be changed)");
        alias.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        alias.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        alias.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        alias.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        alias.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        alias.setIndents (0, 0); alias.setBorder (juce::BorderSize<int> (0));
        alias.setTextToShowWhenEmpty ("name", juce::Colours::white.withAlpha (0.35f));
        alias.setTooltip ("Your own name for this input (click to type it). It is shown next to the driver name everywhere.");
        auto commit = [this]
        {
            if (idx >= (int) app.project.inputs.size()) return;
            app.project.setInputName (idx, alias.getText());
        };
        alias.onReturnKey = [this, commit] { commit(); alias.giveAwayKeyboardFocus(); };       // Enter: done (the box no longer has the keyboard)
        alias.onEscapeKey = [this]
        {
            if (idx < (int) app.project.inputs.size()) alias.setText (app.project.inputs[(size_t) idx].name, juce::dontSendNotification);
            alias.giveAwayKeyboardFocus();
        };
        alias.onFocusLost = commit;
        for (auto* c : std::initializer_list<juce::Component*> { &driver, &alias, &meter, &scale, &readout }) addAndMakeVisible (c);

        gain.setRange (0.0, 66.0, 0.5);
        gain.setDoubleClickReturnValue (true, 20.0);
        gain.setRing (colour);
        gain.setTooltip ("Preamp gain: drag up or down (double-click for 20 dB; the mouse wheel never changes it)");
        gain.onValueChange = [this] { updateReadout(); push(); };
        addAndMakeVisible (gain);
        readout.setTooltip ("Preamp gain in dB. Double-click to type a value.");
        readout.onDouble = [this]
        {
            juce::Component::SafePointer<InputRow> safe (this);
            valueentry::ask (readout, "Gain in dB (0 to 66)", juce::String (gain.getValue(), 1), [safe] (const juce::String& t)
            {
                double v = 0.0;
                if (safe != nullptr && valueentry::parseDb (t, 0.0, 0.0, 66.0, v)) safe->gain.setValue (v, juce::sendNotificationSync);
            });
        };

        // the monitor mode applies to every track that is fed by this input (ARM lives in the Take window)
        static const char* tips[] = { "E - Editing: the live input is only heard while it is ARMed.",
                                      "S - Session: always heard, armed or not. While a take plays back it is muted and you hear the recording.",
                                      "S-L - Session Live: always heard, even while a take plays back.",
                                      "T - Talkback (conductor's microphone): always heard between takes, completely off while recording." };
        for (int m = 0; m < 4; ++m)
        {
            auto* b = monButtons.add (new desk::DeskKey (monitorLabel ((Monitor) m), Ic::None, 3.0f));
            b->setRadioGroupId (4700 + idx);
            b->setTooltip (tips[m]);
            b->setLed (m == 3 ? juce::Colour (0xffff8a2e) : juce::Colour (0xff35e07a));
            b->onClick = [this, m] { if (updating) return; for (auto* t : tracksOnInput()) t->setMonitor ((Monitor) m); app.project.markDirty(); };
            addAndMakeVisible (b);
        }

        crButton.setTooltip ("Talkback mic: this input is part of the control-room (talkback) microphone. Switch it on for as many inputs as you like: they are all heard together. Numpad + opens it to the TB (talkback) outputs: tap to latch, hold for as long as the key is held.");
        crButton.onClick = [this] { if (! updating) app.setCrMicInput (idx, crButton.getToggleState()); };
        addAndMakeVisible (crButton);
        mic = make ("MIC", Ic::Mic, "Mic / Line: lit = LINE input, off = MIC input", juce::Colour (0xffffb02e));
        v48 = make ("48V", Ic::Bolt, "Phantom power (+48 V)", juce::Colour (0xffff4a3d));
        pol = make (juce::String (juce::CharPointer_UTF8 ("\xc3\x98")), Ic::None, "Polarity invert", juce::Colour (0xffffb02e));
        lc  = make ("LC", Ic::Ramp, "Low cut filter", juce::Colour (0xffffb02e));
        pad = make ("PAD", Ic::Pillow, "Pad: the mic preamp's input attenuator", juce::Colour (0xffffb02e));
        zhi = make ("Z HI", Ic::None, "High input impedance (the device's Z in switch)", juce::Colour (0xffffb02e));
        boost = make ("BOOST", Ic::Rocket, "Boost: the mic preamp's extra-gain setting (Hapi: Mic / Boost). Sent to the device as its 'lift' switch.", juce::Colour (0xffffb02e));
        refreshFromModel();
        checkPreampShown();
    }

    void refreshFromModel()
    {
        using desk::Ic;
        if (idx >= (int) app.project.inputs.size()) return;
        auto& in = app.project.inputs[(size_t) idx];
        updating = true;
        const auto drv = juce::String (idx + 1).paddedLeft ('0', 2) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + in.driverName;
        if (driver.getText() != drv) driver.setText (drv, juce::dontSendNotification);
        if (! alias.hasKeyboardFocus (false) && alias.getText() != in.name) { alias.setText (in.name, juce::dontSendNotification); alias.moveCaretToTop (false); }
        if (std::abs (gain.getValue() - in.preamp.gainDb) > 0.01 && ! gain.isMouseButtonDown()) gain.setValue (in.preamp.gainDb, juce::dontSendNotification);
        updateReadout();
        const bool lo = in.preamp.lineOnly;                                         // a fixed line input (Anubis jack 3/4): only gain, phase, low cut and Line / Instrument
        if (lo)                                                                     // the first key is the Line / Instrument switch: LINE (plain) or INSTR (lit)
        {
            mic->setToggleState (in.preamp.zHigh, juce::dontSendNotification);
            mic->setButtonText (in.preamp.zHigh ? "INSTR" : "LINE");
            mic->setIcon (Ic::Jack);
            mic->setEnabled (in.preamp.hasInstrument);
            mic->setTooltip ("Line / Instrument: lit = INSTRUMENT input, plain = LINE input");
        }
        else
        {
            mic->setToggleState (in.preamp.line, juce::dontSendNotification);
            mic->setButtonText (in.preamp.line ? "LINE" : "MIC");
            mic->setIcon (in.preamp.line ? Ic::Jack : Ic::Mic);
            mic->setEnabled (true);
            mic->setTooltip ("Mic / Line: lit = LINE input, off = MIC input");
        }
        for (auto* b : { v48, pad, boost, zhi }) b->setVisible (preampShown && ! lo);
        v48->setToggleState (in.preamp.phantom, juce::dontSendNotification);
        pol->setToggleState (in.preamp.polarity, juce::dontSendNotification);
        lc->setToggleState (in.preamp.lowCut, juce::dontSendNotification);
        boost->setToggleState (in.preamp.boost, juce::dontSendNotification);
        pad->setToggleState (in.preamp.pad, juce::dontSendNotification);
        zhi->setToggleState (in.preamp.zHigh, juce::dontSendNotification);
        crButton.setToggleState (app.project.isCrMic (idx), juce::dontSendNotification);
        updating = false;
    }

    void tick (const std::vector<float>& inPk)
    {
        checkPreampShown();
        meter.setLevel (juce::isPositiveAndBelow (idx, (int) inPk.size()) ? inPk[(size_t) idx] : 0.0f);
        const auto fed = tracksOnInput();
        const bool any = ! fed.empty();
        updating = true;
        for (int m = 0; m < monButtons.size(); ++m)
        {
            monButtons[m]->setEnabled (any);
            const bool on = any && (int) fed.front()->monitor() == m;
            if (monButtons[m]->getToggleState() != on) monButtons[m]->setToggleState (on, juce::dontSendNotification);
        }
        updating = false;
    }

    /** The strip's measurements: full size (squeeze 0) down to the most compact (squeeze 1), for a screen without the height for the full strip
        (for example 1920 x 1080 at 150 %). In between, everything shrinks evenly. */
    struct Sizes { int top, disp, gainCap, knob, afterKnob, readout, gap1, key, keyGap, gap2, lower, monGap, gap3, talk, bottom; };
    static Sizes sizesFor (float t)
    {
        auto l = [t] (int full, int small) { return juce::roundToInt ((float) full + ((float) small - (float) full) * juce::jlimit (0.0f, 1.0f, t)); };
        return { l (12, 9), l (50, 40), l (16, 13), l (72, 48), l (2, 1), l (22, 18), l (10, 6), l (32, 26), l (3, 2), l (10, 6), l (105, 72), l (3, 2), l (8, 5), l (24, 20), l (10, 8) };
    }
    /** The height of a strip at a given squeeze. */
    static int heightFor (float t)
    {
        const auto z = sizesFor (t);
        return z.top + z.disp + z.gainCap + z.knob + z.afterKnob + z.readout + z.gap1 + 4 * z.key + 3 * z.keyGap + z.gap2 + z.lower + z.gap3 + z.talk + z.bottom;
    }
    void setSqueeze (float t) { if (std::abs (t - squeeze) > 0.001f) { squeeze = t; resized(); repaint(); } }

    void resized() override
    {
        constexpr int padX = 6;
        const auto z = sizesFor (squeeze);
        auto r = getLocalBounds().reduced (padX, 0);
        r.removeFromTop (z.top);
        dispArea = r.removeFromTop (z.disp);
        alias.setBounds (dispArea.reduced (6, 0).removeFromTop (juce::roundToInt (z.disp * 0.6f)).withTrimmedTop (z.disp >= 46 ? 5 : 3));
        driver.setBounds (dispArea.reduced (5, 0).removeFromBottom (juce::jmin (18, z.disp / 3)));
        gainCapArea = r.removeFromTop (z.gainCap);                                     // "GAIN" is drawn here
        gain.setBounds (r.removeFromTop (z.knob).withSizeKeepingCentre (z.knob, z.knob));
        r.removeFromTop (z.afterKnob);
        readout.setBounds (r.removeFromTop (z.readout));
        r.removeFromTop (z.gap1);
        {   // the preamp keys: two columns, BOOST across both
            auto keys = r.removeFromTop (4 * z.key + 3 * z.keyGap);
            const int w = (keys.getWidth() - 3) / 2;
            auto row = [&] (desk::DeskKey* a, desk::DeskKey* b)
            {
                auto line = keys.removeFromTop (z.key); keys.removeFromTop (z.keyGap);
                a->setBounds (line.removeFromLeft (w)); line.removeFromLeft (3); b->setBounds (line);
            };
            row (mic, v48); row (pol, lc); row (pad, zhi);
            boost->setBounds (keys.removeFromTop (z.key));
        }
        r.removeFromTop (z.gap2);
        {   // the meter with its scale, and the four monitor keys
            auto lower = r.removeFromTop (z.lower);
            meter.setBounds (lower.removeFromLeft (12));
            scale.setBounds (lower.removeFromLeft (21).withTrimmedLeft (1));
            lower.removeFromLeft (2);
            const int mh = (z.lower - 3 * z.monGap) / 4;
            for (auto* b : monButtons) { b->setBounds (lower.removeFromTop (mh)); lower.removeFromTop (z.monGap); }
        }
        r.removeFromTop (z.gap3);
        crButton.setBounds (r.removeFromTop (z.talk));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = desk::pal();
        auto b = getLocalBounds();
        g.setColour (p.strip); g.fillRect (b);
        g.setColour (p.stripEdge); g.drawRect (b, 1);
        g.setColour (colour); g.fillRect (0, 0, b.getWidth(), 4); g.fillRect (0, b.getHeight() - 4, b.getWidth(), 4);
        g.setColour (p.disp); g.fillRect (dispArea);
        g.setColour (p.dispEdge); g.drawRect (dispArea, 1);
        g.setColour (p.cream); g.setFont (juce::FontOptions (juce::jmin (10.5f, (float) gainCapArea.getHeight() * 0.75f), juce::Font::bold));
        g.drawText ("GAIN", 0, gainCapArea.getY() + 1, b.getWidth(), gainCapArea.getHeight() - 2, juce::Justification::centred, false);
    }

    static constexpr int kStripW = 84;
private:
    /** The tracks that record from this input (any channel of the track). */
    std::vector<TrackDef*> tracksOnInput()
    {
        std::vector<TrackDef*> v;
        for (auto& t : app.project.tracks)
            for (int c = 0; c < t.channelCount(); ++c)
                if (t.inputOf (c) == idx) { v.push_back (&t); break; }
        return v;
    }
    desk::DeskKey* make (const juce::String& text, desk::Ic ic, const juce::String& tip, juce::Colour led)
    {
        auto* b = buttons.add (new desk::DeskKey (text, ic, 10.0f));
        b->setLed (led);
        b->setTooltip (tip);
        b->onClick = [this] { push(); };
        addAndMakeVisible (b);
        return b;
    }
    void updateReadout() { readout.setValueText (juce::String (gain.getValue(), 1)); }
    void push()
    {
        if (updating || idx >= (int) app.project.inputs.size()) return;
        PreampSettings s;
        const bool lo = app.project.inputs[(size_t) idx].preamp.lineOnly;
        s.gainDb = (float) gain.getValue(); s.line = lo ? true : mic->getToggleState(); s.phantom = v48->getToggleState();
        s.polarity = pol->getToggleState(); s.lowCut = lc->getToggleState(); s.boost = boost->getToggleState(); s.pad = pad->getToggleState(); s.zHigh = lo ? mic->getToggleState() : zhi->getToggleState();
        s.lineOnly = app.project.inputs[(size_t) idx].preamp.lineOnly; s.hasInstrument = app.project.inputs[(size_t) idx].preamp.hasInstrument;
        app.project.setPreamp (idx, s);
    }

    bool hasPreamp() const { return app.project.preampDriver != nullptr && app.project.preampDriver->controls (idx); }
    void checkPreampShown()
    {
        const bool hw = hasPreamp(); if (hw == preampShown) return;
        preampShown = hw; gain.setVisible (hw); readout.setVisible (hw);
        for (auto* b : { mic, v48, pol, lc, pad, zhi, boost }) b->setVisible (hw);
        refreshFromModel(); resized();
    }

    AppContext& app; int idx; bool updating = false, preampShown = true;
    float squeeze = 0.0f;                                   // 0 = full size, 1 = the most compact (see sizesFor)
    juce::Rectangle<int> dispArea, gainCapArea;
    juce::Colour colour;
    juce::Label driver; juce::TextEditor alias; LevelMeter meter { true }; desk::DeskScale scale; desk::DeskEncoder gain; desk::DeskReadout readout;
    juce::OwnedArray<desk::DeskKey> buttons, monButtons;
    desk::TalkKey crButton;
    desk::DeskKey *mic = nullptr, *v48 = nullptr, *pol = nullptr, *lc = nullptr, *boost = nullptr, *pad = nullptr, *zhi = nullptr;
};

/** One driver output, drawn as a wider desk strip: a coloured bar, a black display with your own second name, the driver's name, the TB (talkback) key with its
    megaphone, which mixer feeds it, and a live meter. */
class OutputRow : public juce::Component
{
public:
    OutputRow (AppContext& a, int index) : app (a), idx (index)
    {
        colour = desk::stripColour (idx + 3);
        driver.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::plain));
        driver.setColour (juce::Label::textColourId, desk::pal().cream);
        driver.setMinimumHorizontalScale (0.7f);
        driver.setInterceptsMouseClicks (false, false);
        driver.setTooltip ("The name the audio driver gives this output (it cannot be changed)");
        alias.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        alias.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        alias.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        alias.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        alias.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        alias.setIndents (0, 0); alias.setBorder (juce::BorderSize<int> (0));
        alias.setTextToShowWhenEmpty ("name", juce::Colours::white.withAlpha (0.35f));
        alias.setTooltip ("Your own name for this output (click to type it).");
        auto commit = [this]
        {
            if (idx >= (int) app.project.outputs.size()) return;
            if (app.project.outputs[(size_t) idx].name != alias.getText()) { app.project.outputs[(size_t) idx].name = alias.getText(); app.project.changed(); }
        };
        alias.onReturnKey = [this, commit] { commit(); alias.giveAwayKeyboardFocus(); };
        alias.onEscapeKey = [this]
        {
            if (idx < (int) app.project.outputs.size()) alias.setText (app.project.outputs[(size_t) idx].name, juce::dontSendNotification);
            alias.giveAwayKeyboardFocus();
        };
        alias.onFocusLost = commit;
        fedBy.setColour (juce::Label::textColourId, desk::pal().cyan.withMultipliedBrightness (0.85f));
        fedBy.setFont (juce::FontOptions (10.5f));
        fedBy.setBorderSize (juce::BorderSize<int> (0));
        fedBy.setMinimumHorizontalScale (0.8f);
        fedBy.setInterceptsMouseClicks (false, false);
        for (auto* c : std::initializer_list<juce::Component*> { &driver, &alias, &meter, &fedBy }) addAndMakeVisible (c);
        tbButton.setLed (juce::Colour (0xffff8a2e));
        tbButton.setTooltip ("TB: make this output and the next one a stereo talkback pair (to the studio). Any outputs, any number of pairs; on the last output it is mono. Only the talkback mic and recorded playback can ever reach it - never live microphones. Click again to turn the pair off.");
        tbButton.onClick = [this] { if (! updating) app.setTbPair (tbFirst(), ! isTbNow()); };
        addAndMakeVisible (tbButton);
        refreshFromModel();
    }
    void refreshFromModel()
    {
        if (idx >= (int) app.project.outputs.size()) return;
        auto& o = app.project.outputs[(size_t) idx];
        updating = true;
        const bool isTb = isTbNow(), asLeft = hasPair (idx);
        tbButton.setToggleState (isTb, juce::dontSendNotification);
        tbButton.setButtonText (isTb ? (asLeft ? "TB L" : "TB R") : "TB");
        tbButton.setEnabled (true);
        fedBy.setColour (juce::Label::textColourId, desk::pal().cyan.withMultipliedBrightness (0.85f));   // (follows the desk colour)
        updating = false;
        const auto drv = juce::String (idx + 1) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + o.driverName;
        if (driver.getText() != drv) driver.setText (drv, juce::dontSendNotification);
        if (! alias.hasKeyboardFocus (false) && alias.getText() != o.name) { alias.setText (o.name, juce::dontSendNotification); alias.moveCaretToTop (false); }
        const auto fed = app.project.outputFedBy (idx);
        const auto t = fed.isEmpty() ? juce::String() : juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x90 ")) + fed;
        if (fedBy.getText() != t) fedBy.setText (t, juce::dontSendNotification);
    }
    void tick (const std::vector<float>& outPk) { meter.setLevel (juce::isPositiveAndBelow (idx, (int) outPk.size()) ? outPk[(size_t) idx] : 0.0f); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 0);
        r.removeFromTop (9);
        auto disp = r.removeFromTop (36).reduced (7, 0);                // the display: your name, and under it which mixer feeds the output
        alias.setBounds (disp.removeFromTop (20).withTrimmedTop (4));
        fedBy.setBounds (disp.withTrimmedBottom (2));
        r.removeFromTop (4);
        {
            auto line = r.removeFromTop (24);
            tbButton.setBounds (line.removeFromRight (58));
            driver.setBounds (line);
        }
        r.removeFromTop (4);
        meter.setBounds (r.removeFromTop (6));
    }
    void paint (juce::Graphics& g) override
    {
        const auto& p = desk::pal();
        auto b = getLocalBounds();
        g.setColour (p.strip); g.fillRect (b);
        g.setColour (p.stripEdge); g.drawRect (b, 1);
        g.setColour (colour); g.fillRect (0, 0, b.getWidth(), 4);
        auto disp = juce::Rectangle<int> (8, 9, b.getWidth() - 16, 36);
        g.setColour (p.disp); g.fillRect (disp);
        g.setColour (p.dispEdge); g.drawRect (disp, 1);
    }
    static constexpr int kStripW = 170, kStripH = 88;
private:
    bool hasPair (int first) const { auto& v = app.project.tbOutputs; return std::find (v.begin(), v.end(), first) != v.end(); }
    bool isTbNow() const { return hasPair (idx) || hasPair (idx - 1); }
    int tbFirst() const { return hasPair (idx) ? idx : hasPair (idx - 1) ? idx - 1 : idx; }       // the right-hand output of a pair switches that pair off
    AppContext& app; int idx; bool updating = false;
    juce::Colour colour;
    desk::DeskKey tbButton { "TB", desk::Ic::Mega, 10.0f };
    juce::Label driver, fedBy; juce::TextEditor alias; LevelMeter meter { false };
};

/** Shown when Fermata starts: nothing can be done until the project's folder is chosen (or an existing project opened). */
class StartPanel : public juce::Component
{
public:
    std::function<void()> onNew, onOpen, onLast, onQuit;
    StartPanel (const juce::File& last)
    {
        newBtn.setButtonText ("New project...");
        openBtn.setButtonText ("Open an existing project...");
        quitBtn.setButtonText ("Quit");
        for (auto* c : std::initializer_list<juce::Component*> { &newBtn, &openBtn, &quitBtn }) addAndMakeVisible (c);
        if (last != juce::File())
        {
            lastBtn.setButtonText ("Open the last project:  " + last.getFileNameWithoutExtension());
            addAndMakeVisible (lastBtn); hasLast = true;
        }
        newBtn.onClick = [this] { if (onNew) onNew(); };
        openBtn.onClick = [this] { if (onOpen) onOpen(); };
        lastBtn.onClick = [this] { if (onLast) onLast(); };
        quitBtn.onClick = [this] { if (onQuit) onQuit(); };
        setWantsKeyboardFocus (false);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
    bool keyPressed (const juce::KeyPress&) override { return true; }       // the program's keys do nothing until a project exists
    void resized() override
    {
        auto r = getLocalBounds().withSizeKeepingCentre (juce::jmin (760, getWidth() - 40), (hasLast ? 4 : 3) * 54);
        const int bw = juce::jmin (420, r.getWidth());
        auto place = [&] (juce::Component& c) { c.setBounds (r.removeFromTop (44).withSizeKeepingCentre (bw, 44)); r.removeFromTop (10); };
        place (newBtn); place (openBtn);
        if (hasLast) place (lastBtn);
        place (quitBtn);
    }
private:
    juce::TextButton newBtn, openBtn, lastBtn, quitBtn; bool hasLast = false;
};

void MainComponent::showStartPanel()
{
    auto p = std::make_unique<StartPanel> (app.lastProjectFile());
    p->onNew = [this] { newProjectDialog(); };
    p->onOpen = [this] { openProjectDialog(); };
    p->onLast = [this] { const auto f = app.lastProjectFile(); if (f != juce::File()) app.openProject (f); };
    p->onQuit = [] { if (auto* a = juce::JUCEApplicationBase::getInstance()) a->systemRequestedQuit(); };
    startPanel = std::move (p);
    addAndMakeVisible (*startPanel);
    startPanel->setBounds (getLocalBounds());
    startPanel->toFront (false);
}

MainComponent::MainComponent (AppContext& a) : app (a)
{
    addAndMakeVisible (auditionIcon);
    addAndMakeVisible (logo);
    addChildComponent (stagePanel);
    takeBox.setCompact (true);                                    // the take counter lives in the menu bar
    addAndMakeVisible (dock);
    dock.onChange = [this] { resized(); };
    menuStrip = std::make_unique<MenuBarStrip>();                  // the menu bar floats above every window (see MenuBarStrip)
    for (auto* b : menuItems()) menuStrip->addAndMakeVisible (b);
    menuStrip->addAndMakeVisible (takeBox);
    for (auto* f : { &flagPre, &flagCr, &flagPb }) menuStrip->addAndMakeVisible (f);
    for (auto* f : { &flagT, &flagE, &flagM }) menuStrip->addAndMakeVisible (f);
    flagT.onClick = [this] { if (app.focusRecent) app.focusRecent ("take"); };
    flagE.onClick = [this] { if (app.focusRecent) app.focusRecent ("edit"); };
    flagM.onClick = [this] { if (app.focusRecent) app.focusRecent ("mixer"); };
    menuStrip->addAndMakeVisible (ltcInLamp); menuStrip->addAndMakeVisible (ltcOutLamp);
    ltcInLamp.onClick = [this] { showLtcInMenu(); };
    ltcOutLamp.onClick = [this] { showLtcOutMenu(); };
    menuBarRaise() = [this] { if (menuStrip != nullptr && menuStrip->isVisible() && juce::Process::isForegroundProcess()) menuStrip->toFront (false); };
    menuBarAvoidArea() = [this] { return menuStrip != nullptr && menuStrip->isVisible() ? menuStrip->getBounds() : juce::Rectangle<int>(); };
    editsButton.onClick = [this] { showEditsMenu(); };
    projectButton.onClick = [this] { showProjectMenu(); };
    audioButton.onClick   = [this] { if (app.showAudioSettings) app.showAudioSettings(); };
    designButton.onClick  = [this] { if (app.showDesign) app.showDesign(); };
    takesButton.onClick   = [this] { showTakesMenu(); };
    mixersButton.onClick  = [this] { showMixersMenu(); };
    bridgeButton.onClick  = [this] { if (app.showBridge) app.showBridge(); };
    preampsButton.onClick = [this] { showPreampSetup (app); };
    mediaButton.onClick   = [this] { if (app.showMedia) app.showMedia(); };
    masteringButton.onClick = [this] { if (app.showMastering) app.showMastering(); };
    masteringButton.setTooltip ("Mastering: export the edits as audio files (Virtual Master), or lay them out as a CD and make a DDP");
    takeDisplayButton.onClick = [this] { if (app.showTakeDisplay) app.showTakeDisplay(); };
    takeDisplayButton.setTooltip ("Show the take number, time of day and time left in the session, full screen, on another display");
    organiserButton.onClick = [this] { if (app.showOrganiser) app.showOrganiser(); };
    organiserButton.setTooltip ("Let the computer arrange your open windows on the screen: 2, 3 or 4 windows, each in its own part");
    sessionButton.setClickingTogglesState (false);
    sessionButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd32f2f));
    sessionButton.setTooltip ("Pre-Rec (session mode): the inputs are kept for the last 6 seconds all the time, so when you press Record the take starts 6 seconds BEFORE you pressed it. "
                              "The red Pre-Rec square at the right of this bar shows it is on. Listening is never interrupted.");
    // the three squares at the right of the bar are buttons
    flagPre.onClick = [this] { app.setSessionMode (! app.sessionMode()); };
    flagCr.onClick  = [this]
    {
        if (! app.talkbackReady()) { juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Talkback mic", "Set up the talkback first: choose at least one control-room mic input and one talkback output pair in the Project Designer."); return; }
        app.crClickLatch = ! app.crClickLatch;
    };
    flagPb.onClick  = [this]
    {
        if (app.project.tbOutputs.empty()) { juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Talkback playback", "Set up a talkback output pair first in the Project Designer."); return; }
        app.remotePlayback();
    };
    if (auto* st = app.props.getUserSettings()) if (st->getBoolValue ("sessionMode", false)) app.engine.setSessionMode (true);
    mediaButton.setTooltip ("A folder tree of where every file of the project is saved");
    preampsButton.setTooltip ("Set up the Anubis / Hapi / MT48 devices whose preamps Fermata controls");
    bridgeButton.setTooltip ("Meter bridge: a meter for every driver input and output (key B)");
    mixersButton.setTooltip ("Mixers. Key M opens or closes the processing mixer.");

    projectBox.setFontHeight (15.0f);
    for (auto* b : { &projectBox, &rateBox, &depthBox, &diskBox }) addAndMakeVisible (b);
    if (auto* st = app.props.getUserSettings())                     // the second recording drive (Project menu)
    {
        const auto path = st->getValue ("backupFolder");
        if (path.isNotEmpty() && juce::File::isAbsolutePath (path)) app.engine.setBackupFolder (juce::File (path));
    }
    statusLabel.setFont (juce::FontOptions (12.5f));
    addAndMakeVisible (statusLabel);
    inputsCaption.setText ("DRIVER INPUTS", juce::dontSendNotification);
    inputsCaption.setJustificationType (juce::Justification::centredLeft);
    outputsCaption.setText ("DRIVER OUTPUTS", juce::dontSendNotification);
    for (auto* l : { &inputsCaption, &outputsCaption })
    {
        l->setColour (juce::Label::textColourId, desk::pal().cream);
        l->setFont (juce::FontOptions (12.5f, juce::Font::bold));
        addAndMakeVisible (l);
    }
    for (auto* v : { &inputsViewport, &outputsViewport }) { v->setScrollBarsShown (true, true); v->setScrollBarThickness (10); addAndMakeVisible (v); }
    inputsViewport.setViewedComponent (&inputsHolder, false);
    outputsViewport.setViewedComponent (&outputsHolder, false);
    rebuildRows(); rebuildAuditionBar();
    app.project.structure.addChangeListener (this);
    app.project.addChangeListener (this);
    startTimerHz (30);
    app.peakListeners.add (this);
    if (! app.hasProject()) showStartPanel();                      // nothing can be done until the project's folder is chosen
    setSize (juce::jmax (1440, menuBarWidth() + 4), fullHeight());    // the full-size layout (on a smaller screen Main.cpp makes it fit, and the strips shrink)
}

void MainComponent::parentHierarchyChanged()
{
    auto* top = getTopLevelComponent();
    if (top != nullptr && top != this && top != watchedTop.getComponent())
    {
        if (watchedTop != nullptr) watchedTop->removeComponentListener (this);
        watchedTop = top;
        top->addComponentListener (this);
    }
    syncMenuStrip();
}

void MainComponent::syncMenuStrip()
{
    if (menuStrip == nullptr) return;
    auto* peer = getPeer();
    if (peer == nullptr) return;
    if (peer->isMinimised()) return;                                // the bar stays where it was while the window is minimised
    const auto r = localAreaToGlobal (juce::Rectangle<int> (0, 0, getWidth(), menuBarH));
    if (menuStrip->getBounds() != r) menuStrip->setBounds (r);
    if (! menuStrip->isVisible()) menuStrip->setVisible (true);
    if (auto* top = getTopLevelComponent(); top != nullptr && top != this) makeOwnedBy (*menuStrip, *top);       // the main window owns the strip: it minimises and hides with it, and is never above another program
    if (juce::Process::isForegroundProcess()) menuStrip->toFront (false);
}

MainComponent::~MainComponent()
{
    menuBarAvoidArea() = nullptr; menuBarRaise() = nullptr;
    if (watchedTop != nullptr) watchedTop->removeComponentListener (this);
    app.peakListeners.remove (this);
    app.project.structure.removeChangeListener (this);
    app.project.removeChangeListener (this);
}

static constexpr int kLampW = 13;                 // the window lights after Take Window / Edit Window / Mixer

/** The width the menu bar's entries need at a given writing size and space round each entry (the squares and lamps included). */
static int menuNeed (const std::vector<MenuBarButton*>& items, const std::vector<WindowLamp*>& squares, const std::vector<StatusFlag*>& flags,
                     const std::vector<LtcLamp*>& lamps, int takeBoxW, int lineH, float font, int pad)
{
    const auto keep = menuStyle();
    menuStyle() = { font, pad };
    int w = 6 + takeBoxW;
    for (auto* b : items) w += MenuBarButton::widthFor (b->getButtonText()) + 1;
    w += (int) squares.size() * (kLampW + 2);
    juce::ignoreUnused (lineH);
    for (auto* l : lamps) w += l->wantedWidth() + 3;
    for (auto* f : flags) w += f->wantedWidth() + 3;
    menuStyle() = keep;
    return w + 8;
}

int MainComponent::menuBarWidth() const
{
    auto* self = const_cast<MainComponent*> (this);
    return menuNeed (self->menuItems(), { &self->flagT, &self->flagE, &self->flagM }, { &self->flagPre, &self->flagCr, &self->flagPb },
                     { &self->ltcInLamp, &self->ltcOutLamp }, kTakeBoxW, 30, 10.5f, 4);       // the smallest the bar can be made
}

void MainComponent::layoutMenuBar()
{
    constexpr int lineH = 30;
    menuBarH = lineH + 4;
    const std::vector<WindowLamp*> squares { &flagT, &flagE, &flagM };
    const std::vector<StatusFlag*> flags { &flagPre, &flagCr, &flagPb };
    const std::vector<LtcLamp*> lamps { &ltcInLamp, &ltcOutLamp };
    // as large as the width allows: first the space round each entry gets smaller, then the writing
    float font = 14.0f; int pad = 14;
    while (menuNeed (menuItems(), squares, flags, lamps, kTakeBoxW, lineH, font, pad) > getWidth() && (pad > 4 || font > 10.5f))
    {
        if (pad > 6) pad -= 2;
        else if (font > 10.5f) { font -= 0.5f; pad = juce::jmax (4, pad - 1); }
        else pad = 4;
    }
    menuStyle() = { font, pad };
    std::vector<int> seps;
    int x = 4;
    const int y = 2;
    takeBox.setBounds (x, y + 1, kTakeBoxW - 6, lineH - 2);      // the take counter: a small box at the left end of the bar
    x += kTakeBoxW - 2;
    // the status squares and the LTC lamps at the right end
    int rx = getWidth() - 3;
    for (auto it = flags.rbegin(); it != flags.rend(); ++it) { const int w = (*it)->wantedWidth(); (*it)->setBounds (rx - w, y, w, lineH); rx -= w + 3; }
    for (auto it = lamps.rbegin(); it != lamps.rend(); ++it) { const int w = (*it)->wantedWidth(); (*it)->setBounds (rx - w, y, w, lineH); rx -= w + 3; }
    // every entry from the left, in order (Media and Mastering are the last two); the space left over sits before the lamps at the right
    auto widthOf = [] (MenuBarButton* b) { return MenuBarButton::widthFor (b->getButtonText()); };
    for (auto* b : menuItems())
    {
        seps.push_back (x); x += 1;
        const int w = widthOf (b);
        b->setBounds (x, y, w, lineH);
        x += w;
        if (auto* lamp = flagFor (b)) { lamp->setBounds (x - menuStyle().pad + 2, y, kLampW, lineH); x += kLampW + 2 - menuStyle().pad + 2; }   // its light, just after the words
    }
    seps.push_back (x);
    juce::ignoreUnused (rx);
    menuStrip->separators = seps;
    menuStrip->setSize (getWidth(), menuBarH);
    menuStrip->repaint();
    syncMenuStrip();
}

int MainComponent::fullHeight() const
{
    return 34 + 4 + StageSpeakerPanel::kHeight + 7 + InputRow::heightFor (0.0f) + 4 + 26 + OutputRow::kStripH + 4 + kRailFull;
}

int MainComponent::menuBarBottomOnScreen() const { return localAreaToGlobal (juce::Rectangle<int> (0, 0, 1, menuBarH)).getBottom(); }

void MainComponent::paint (juce::Graphics& g)
{
    const auto& p = desk::pal();
    g.fillAll (p.window);
    g.setColour (p.menu); g.fillRect (0, 0, getWidth(), menuBarH);                       // the menu bar
    g.setColour (p.menuEdge); g.fillRect (0, menuBarH - 1, getWidth(), 1);
    // the line under the top row (the Fermata mark, project and displays, the stage speaker boxes)
    if (headerLineY > 0)
    {
        g.setColour (p.cream.withAlpha (0.55f)); g.fillRect (kCheekW + 10, headerLineY, getWidth() - 2 * (kCheekW + 10), 1);
        g.setColour (juce::Colours::black.withAlpha (0.25f)); g.fillRect (kCheekW + 10, headerLineY + 1, getWidth() - 2 * (kCheekW + 10), 1);
    }
    // the wooden side panels, and the rail along the bottom with the maker's name
    const int top = menuBarH, bottom = getHeight() - railH;
    desk::paintCheek (g, { 0, top, kCheekW, bottom - top }, true);
    desk::paintCheek (g, { getWidth() - kCheekW, top, kCheekW, bottom - top }, false);
    desk::paintRail (g, { 0, bottom, getWidth(), railH });
    desk::paintNameplate (g, { kCheekW + 12, bottom + (railH - 22) / 2, getWidth() - 2 * kCheekW - 24, 22 });
}

void MainComponent::resized()
{
    if (startPanel != nullptr) startPanel->setBounds (getLocalBounds());
    layoutMenuBar();                                                    // the menu bar: always one line
    // ---- does everything fit at full size? If not (a 16:9 screen at 150 %, say) the rail gets thinner and the input strips shrink to fit
    const bool showStage = stagePanel.wanted();
    const int headH = StageSpeakerPanel::kHeight;                      // always the stage speaker boxes' height, so the top row never moves when they come and go
    const int innerW = getWidth() - 2 * (kCheekW + 10);
    const bool outScroll = (int) outputRows.size() * (OutputRow::kStripW + 2) > innerW;
    const bool inScroll  = (int) inputRows.size()  * (InputRow::kStripW + 2)  > innerW;
    constexpr int sb = 10;
    const int outH = OutputRow::kStripH + (outScroll ? sb : 0);
    const int fixedH = menuBarH + 4 + headH + 7 + (inScroll ? sb : 0) + 4 + 26 + outH + 4;     // everything but the input strips and the rail
    railH = getHeight() >= fixedH + InputRow::heightFor (0.0f) + kRailFull ? kRailFull : kRailCompact;
    {
        const int room = getHeight() - fixedH - railH;                  // what the input strips can have
        const int full = InputRow::heightFor (0.0f), small = InputRow::heightFor (1.0f);
        squeeze = room >= full ? 0.0f : juce::jlimit (0.0f, 1.0f, (float) (full - room) / (float) (full - small));
        for (auto* row : inputRows) row->setSqueeze (squeeze);
    }
    // ---- the wooden rail along the bottom: the minimised windows sit on it, the maker's name at its right
    {
        auto rail = getLocalBounds().removeFromBottom (railH).reduced (kCheekW + 12, 0);
        rail.removeFromRight (190);
        dock.setBounds (rail.withSizeKeepingCentre (rail.getWidth(), juce::jmin (30, railH - 8)));
    }
    auto r = getLocalBounds().withTrimmedTop (menuBarH).withTrimmedBottom (railH).reduced (kCheekW + 10, 0);
    r.removeFromTop (4); r.removeFromBottom (4);
    // ---- the top row: the Fermata mark and name | project | the driver and recording state | DRIVER INPUTS, and the stage speaker boxes at the right
    headerSeps.clear();
    stagePanel.setVisible (showStage);
    auto head = r.removeFromTop (headH);
    headerLine = head;
    {   // left to right: the mark, the displays, the driver line, DRIVER INPUTS; everything to the right of that is the stage speaker boxes' room
        // (they sit at the right-hand end and only scroll sideways when they fill all of it)
        auto line = head;
        logo.setBounds (line.removeFromLeft (148).withSizeKeepingCentre (148, 40));
        line.removeFromLeft (12);
        // the displays: the project on top, the rate, depth and free disk space under it
        const int colW = juce::jmax (rateBox.wantedWidth() + depthBox.wantedWidth() + diskBox.wantedWidth() + 24, juce::jmin (460, projectBox.wantedWidth()));
        auto col = line.removeFromLeft (juce::jmin (colW, line.getWidth() / 2)).withSizeKeepingCentre (juce::jmin (colW, line.getWidth() / 2), 56);
        projectBox.setBounds (col.removeFromTop (28).withWidth (juce::jmin (col.getWidth(), juce::jmax (180, projectBox.wantedWidth()))));
        col.removeFromTop (6);
        for (auto* b : { &rateBox, &depthBox, &diskBox }) { b->setBounds (col.removeFromLeft (b->wantedWidth()).withSizeKeepingCentre (b->wantedWidth(), 22)); col.removeFromLeft (12); }
        line.removeFromLeft (12);
        const int minStage = juce::jmin (StageSpeakerPanel::reservedWidth(), line.getWidth() / 3);          // the boxes always have at least this much
        const int statusW = juce::jmin (statusWidth(), juce::jmax (60, line.getWidth() - minStage - 10 - 122 - 12));
        statusLabel.setBounds (line.removeFromLeft (statusW).withSizeKeepingCentre (statusW, 24));
        line.removeFromLeft (14);
        inputsCaption.setBounds (line.removeFromLeft (112).withSizeKeepingCentre (112, 24));
        line.removeFromLeft (10);
        if (showStage) stagePanel.setBounds (line.removeFromRight (juce::jmin (line.getWidth(), stagePanel.wantedWidth())));
        projectName.setBounds ({});
    }
    headerLineY = head.getBottom() + 3;                                // the line that separates the top row from the strips (in the middle of the gap)
    r.removeFromTop (7);
    // ---- the driver outputs at the bottom, with the audition buttons on their caption line
    const int stripH = InputRow::heightFor (squeeze);
    const int inH  = juce::jmin (stripH + (inScroll ? sb : 0), juce::jmax (120, r.getHeight() - outH - 30));
    inputsViewport.setBounds (r.removeFromTop (inH));
    r.removeFromTop (4);
    {
        auto capRow = r.removeFromTop (26);
        outputsCaption.setBounds (capRow.removeFromLeft (140));
        auditionIcon.setVisible (! auditionButtons.isEmpty());
        if (! auditionButtons.isEmpty())
        {
            capRow.removeFromLeft (8);
            auditionIcon.setBounds (capRow.removeFromLeft (24).withSizeKeepingCentre (20, 20));
            auditionCaption.setBounds (capRow.removeFromLeft (138));
            for (auto* b : auditionButtons)
            {
                if (capRow.getWidth() < 80) { b->setBounds ({}); continue; }
                const int w = juce::jmin (240, juce::jmax (150, capRow.getWidth() / juce::jmax (1, auditionButtons.size())));
                b->setBounds (capRow.removeFromLeft (w).reduced (2, 1));
            }
        }
    }
    outputsViewport.setBounds (r.removeFromTop (outH));

    auto fill = [] (juce::Viewport& vp, juce::Component& holder, auto& rowList, int stripW, int stripH)
    {
        constexpr int gap = 2;
        const int w = juce::jmax (1, (int) rowList.size()) * (stripW + gap);
        holder.setSize (juce::jmax (w, vp.getMaximumVisibleWidth()), stripH);
        int x = 0;
        for (auto* row : rowList) { row->setBounds (x, 0, stripW, stripH); x += stripW + gap; }
    };
    fill (inputsViewport, inputsHolder, inputRows, InputRow::kStripW, stripH);
    fill (outputsViewport, outputsHolder, outputRows, OutputRow::kStripW, OutputRow::kStripH);
    repaint();
}

void MainComponent::rebuildRows()
{
    inputRows.clear();
    inputsHolder.removeAllChildren();
    for (int i = 0; i < (int) app.project.inputs.size(); ++i) { inputRows.add (new InputRow (app, i)); inputsHolder.addAndMakeVisible (inputRows.getLast()); }
    outputRows.clear();
    outputsHolder.removeAllChildren();
    for (int i = 0; i < (int) app.project.outputs.size(); ++i) { outputRows.add (new OutputRow (app, i)); outputsHolder.addAndMakeVisible (outputRows.getLast()); }
    resized();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* src)
{
    stagePanel.rebuildIfNeeded(); resized();
    if (src == &app.project.structure) { rebuildRows(); rebuildAuditionBar(); }
    else { for (auto* r : inputRows) r->refreshFromModel(); for (auto* r : outputRows) r->refreshFromModel(); }   // a second name or a preamp setting changed
}

void MainComponent::peaksUpdated()
{
    for (auto* r : inputRows)  r->tick (app.inPeaks);
    for (auto* r : outputRows) r->tick (app.outPeaks);
}

void MainComponent::timerCallback()
{
    if (startPanel != nullptr && app.hasProject()) startPanel.reset();           // a project was chosen
    else if (startPanel == nullptr && ! app.hasProject()) showStartPanel();      // the project was closed (Project > Close)
    // The menu bar (with the Pre-Rec / C-R / P-B squares) is always there, in front of the other Fermata windows (not other programs), and goes with the main window when it is minimised.
    if (menuStrip != nullptr && getPeer() != nullptr && ! getPeer()->isMinimised() && ! menuStrip->isVisible()) menuStrip->setVisible (true);
    flagPre.set (app.sessionMode()); flagCr.set (app.crMicOpen()); flagPb.set (app.tbPlaybackOn());
    {
        const auto kind = app.activeKind ? app.activeKind() : juce::String();
        auto light = [&] (const char* k) { return app.windowLight ? app.windowLight (k) : (kind == k ? 2 : 0); };
        flagT.set (light ("take")); flagE.set (light ("edit")); flagM.set (light ("mixer"));
    }
    {
        const auto st = app.engine.ltcInStatus();
        ltcInLamp.set (app.project.ltcInput >= 0 && st.present, fpsName (st.fps, st.drop) + " fps");
        ltcOutLamp.set (app.project.ltcOutOn && app.engine.ltcOutActive(), LtcGenerator::formatName (app.project.ltcOutFormat));
    }
    if (sessionButton.getToggleState() != app.sessionMode())
    {
        sessionButton.setToggleState (app.sessionMode(), juce::dontSendNotification);
        sessionButton.setButtonText (app.sessionMode() ? "PRE-REC ON" : "Pre-Rec");
        repaint();
    }
    for (int i = 0; i < auditionButtons.size() && i + 1 < app.project.cueEnd(); ++i)
    {
        auto& m = *app.project.mixers[(size_t) i + 1];
        const bool on = app.isAuditioning (m.id);
        if (auditionButtons[i]->getToggleState() != on) { auditionButtons[i]->setToggleState (on, juce::dontSendNotification); auditionButtons[i]->setButtonText ((on ? "AUDITIONING: " : "") + m.name + " Audition"); }
    }
    const bool rec = app.engine.isRecording();
    const auto now2 = juce::Time::getMillisecondCounter();
    { int n = 1; const bool tr = app.takeBoxInfo (n); takeBox.set (tr, n); }
    {   // the displays in the top row
        const auto pn = app.project.name.toUpperCase().replace (" - ", juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")));
        const int before = projectBox.wantedWidth();
        projectBox.setText (pn.isEmpty() ? juce::String ("NO PROJECT") : pn);
        projectBox.setTooltip (app.project.projectFile.getFullPathName());
        auto* dev = app.devices.getCurrentAudioDevice();
        rateBox.setText (dev != nullptr ? juce::String (dev->getCurrentSampleRate() / 1000.0, (std::fmod (dev->getCurrentSampleRate(), 1000.0) == 0.0 ? 0 : 1)) + " KHZ" : juce::String ("-"));
        depthBox.setText (dev != nullptr ? juce::String (dev->getCurrentBitDepth()) + " BIT" : juce::String ("-"));
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastDiskCheck > 2000 || lastDiskCheck == 0)              // the free space on the project's drive (asked every 2 seconds)
        {
            lastDiskCheck = now;
            auto folder = app.project.projectFile.getParentDirectory();
            if (rec && app.engine.recordingFolder() != juce::File()) folder = app.engine.recordingFolder();     // (while recording: the drive the take goes to)
            diskAlarm.clear();
            if (app.hasProject() && folder.isDirectory())
            {
                const auto freeBytes = folder.getBytesFreeOnVolume();
                const double gb = (double) freeBytes / (1024.0 * 1024.0 * 1024.0);
                juce::String txt = gb >= 1000.0 ? juce::String (gb / 1024.0, 1) + " TB" : juce::String (juce::roundToInt (gb)) + " GB";
                juce::String tip = "Free space on the drive of the project (" + folder.getFullPathName() + ")";
                bool warn = gb < 10.0;
                if (const double bps = app.engine.recordingBytesPerSecond(); rec && bps > 0.0)
                {   // while recording: how long the drive can go on at this rate
                    const double left = (double) freeBytes / bps;
                    const int mins = (int) (left / 60.0);
                    txt << (mins >= 60 ? "  " + juce::String (mins / 60) + " h " + juce::String (mins % 60).paddedLeft ('0', 2) + " min" : "  " + juce::String (mins) + " min") << " left";
                    tip << " - about " << juce::String (mins) << " minutes of recording left at this rate";
                    warn = warn || left < 15.0 * 60.0;
                    if (left < 5.0 * 60.0) diskAlarm = "DISK NEARLY FULL - about " + juce::String (mins) + " min of recording left";
                }
                diskBox.setText (txt, warn ? juce::Colour (0xffff5a4a) : juce::Colour());
                diskBox.setTooltip (tip + (warn ? ": nearly full!" : ""));
            }
            else diskBox.setText ("-");
            // the second recording drive: say so if a copy could not be made
            const auto problems = app.engine.takeBackupProblems();
            for (auto& pr : problems) juce::Logger::writeToLog ("Second recording drive: " + pr);
            if (! problems.isEmpty())
                app.setNotice ("Second drive: " + problems[0] + (problems.size() > 1 ? "  (and " + juce::String (problems.size() - 1) + " more files of this take)" : juce::String())
                               + ". The main recording is not affected.");
        }
        if (projectBox.wantedWidth() != before || diskBox.wantedWidth() != lastDiskW) { lastDiskW = diskBox.wantedWidth(); resized(); }
    }

    juce::String s;
    if (auto* dev = app.devices.getCurrentAudioDevice())
    {
        s = dev->getTypeName() + ": " + dev->getName() + "   buffer " + juce::String (dev->getCurrentBufferSizeSamples())
            + "   in " + juce::String (dev->getActiveInputChannels().countNumberOfSetBits())
            + " / out " + juce::String (dev->getActiveOutputChannels().countNumberOfSetBits());
        if (app.engine.getBackupFolder() != juce::File()) s += "   2nd drive on";
        if (dev->getTypeName() != "ASIO") s += "  (not ASIO)";
        if (rec && app.engine.overruns() > 0) s = "DISK OVERRUN - " + lostText (app.engine.samplesLost(), app.engine.getSampleRate()) + " of audio lost (silence in its place)   " + s;
        if (rec && diskAlarm.isNotEmpty()) s = diskAlarm + "   " + s;
        if (const auto problem = app.engine.recordingProblem(); rec && problem.isNotEmpty())
        {
            s = "RECORDING HAS STOPPED REACHING THE DISK - " + problem + "   " + s;
            if (! problemNoticed) { problemNoticed = true; app.setNotice ("RECORDING PROBLEM: " + problem + ". Stop, free space or change drive, then record again."); juce::Logger::writeToLog ("Recording: " + problem); }
        }
        else if (! rec) problemNoticed = false;
        if (auto* x = dynamic_cast<juce::AudioIODevice*> (dev); x != nullptr && dev->getXRunCount() > 0)
            s += "   xruns: " + juce::String (dev->getXRunCount());
        if (app.lastLoad.blocks > 0)                                    // the load meter: how much of each block's time the audio thread needs
            s += "   DSP " + juce::String (juce::roundToInt (app.lastLoad.average)) + "% (peak " + juce::String (juce::roundToInt (app.lastLoad.peak)) + "%)"
                 + (app.lastLoad.late > 0 ? "  LATE BLOCKS: " + juce::String (app.lastLoad.late) : juce::String());
    }
    else s = "No audio device - open Audio settings";
    if (rec && ! s.startsWith ("DISK") && ! s.startsWith ("RECORDING HAS")) s = "RECORDING  -  " + s;
    if (app.crMicOpen() || app.tbPlaybackOn())
        s = juce::String (app.crMicOpen() ? "CR MIC OPEN  " : "") + (app.tbPlaybackOn() ? "TB PLAYBACK ON  " : "") + "-  " + s;
    else if (! app.talkbackReady() && (app.crWanted || app.tbWanted))
        s = "Talkback key pressed, but no CR input / TB outputs are chosen (CR button on an input, TB button on an output)  -  " + s;
    const bool noticeOn = juce::Time::getMillisecondCounter() < app.noticeUntil;
    if (noticeOn) s = app.notice;
    statusLabel.setText (s, juce::dontSendNotification);
    if (const int w = statusWidthFor (s); w != lastStatusTextW) { lastStatusTextW = w; resized(); }                                   // DRIVER INPUTS follows the end of the driver line
    if (noticeOn) statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff3ee0c0)); else
    statusLabel.setColour (juce::Label::textColourId, rec ? juce::Colour (0xffff5555) : s.startsWith ("DISK") || s.startsWith ("No audio") ? theme::warn : desk::pal().cream.withAlpha (0.8f));
    if (rec && (s.startsWith ("DISK") || s.startsWith ("RECORDING HAS"))) statusLabel.setColour (juce::Label::textColourId, ((now2 / 500) & 1) ? juce::Colour (0xffff5555) : juce::Colours::white);     // (flashes)
}

void MainComponent::showProjectMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "New project...");
    m.addItem (2, "Open project...");
    m.addItem (3, "Save");
    m.addItem (6, "Save as...");
    m.addItem (7, "Create copy of entire project...");
    m.addItem (10, "Close", app.hasProject());
    m.addSeparator();
    m.addItem (4, "Show project folder");
    m.addItem (5, "Rename project...");
    m.addSeparator();
    const auto workKey = app.lastWorkWindowKey ? app.lastWorkWindowKey() : juce::String();
    m.addItem (9, "Import AAF as edit windows...");
    m.addItem (8, "Re-draw all waveforms in current window", workKey.isNotEmpty());
    m.addSeparator();
    {
        const auto bf = app.engine.getBackupFolder();
        m.addItem (12, bf == juce::File() ? juce::String ("Second recording drive...") : "Second recording drive: " + bf.getFullPathName() + "   (change...)");
        if (bf != juce::File()) m.addItem (13, "Stop recording to the second drive");
    }
    m.addSeparator();
    auto picked = std::make_shared<int> (-1);
    m.addCustomItem (11, std::make_unique<desk::SwatchRow> (desk::themeIndex(), picked), nullptr, "Colour theme");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&projectButton), [this, workKey, picked] (int r)
    {
        if (r == 11) { if (*picked >= 0) pickColour (*picked); return; }
        if (r == 12) { chooseBackupFolder(); return; }
        if (r == 13)
        {
            app.engine.setBackupFolder ({});
            if (auto* st = app.props.getUserSettings()) { st->setValue ("backupFolder", juce::String()); app.props.saveIfNeeded(); }
            app.setNotice ("Takes are recorded to the main drive only.");
            return;
        }
        if (r == 8) { app.redrawWaveforms (app.audioFilesOfWindow (workKey)); return; }
        if (r == 9) { fixtools::importAaf (app, this); return; }
        if (r == 10) { closeProjectDialog(); return; }
        if (r == 1) newProjectDialog();
        else if (r == 2) openProjectDialog();
        else if (r == 3) app.saveNow();
        else if (r == 6) fixtools::copyProject (app, this, true);
        else if (r == 7) fixtools::copyProject (app, this, false);
        else if (r == 4) app.project.projectFile.getParentDirectory().revealToUser();
        else if (r == 5)
        {
            auto* aw = new juce::AlertWindow ("Rename project", "Project name:", juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor ("n", app.project.name);
            aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int res)
            {
                if (res == 1 && aw->getTextEditorContents ("n").isNotEmpty()) { app.project.name = aw->getTextEditorContents ("n"); app.project.changed(); }
                delete aw;
            }));
        }
    });
}

void MainComponent::chooseBackupFolder()
{
    const auto current = app.engine.getBackupFolder();
    chooser = std::make_unique<juce::FileChooser> ("Choose a folder on the second drive (every take is copied there while it records, in a folder named after the project)",
                                                   current != juce::File() ? current : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir == juce::File() || ! dir.isDirectory()) return;
        const auto main = app.project.projectFile.getParentDirectory();
        if (app.hasProject() && (dir == main || dir.isAChildOf (main)))
        {
            showError ("Second recording drive", "Choose a folder outside the project's own folder, ideally on another drive: the copy is there in case the main drive fails.");
            return;
        }
        app.engine.setBackupFolder (dir);
        if (auto* st = app.props.getUserSettings()) { st->setValue ("backupFolder", dir.getFullPathName()); app.props.saveIfNeeded(); }
        app.setNotice ("Every take is now also copied to " + dir.getFullPathName() + " while it records.");
    });
}

void MainComponent::pickColour (int index)
{
    applyDeskColour (index);                                         // every window takes the new colour
    if (auto* st = app.props.getUserSettings()) { st->setValue ("deskColour", desk::themeIndex()); app.props.saveIfNeeded(); }
    for (auto* r : inputRows) r->refreshFromModel();
    for (auto* r : outputRows) r->refreshFromModel();
    repaint();
    if (menuStrip != nullptr) menuStrip->repaint();
    for (auto* f : { &flagPre, &flagCr, &flagPb }) f->repaint();
    for (auto* f : { &flagT, &flagE, &flagM }) f->repaint();
    for (auto* b : { &projectBox, &rateBox, &depthBox, &diskBox }) b->repaint();
}

void MainComponent::closeProjectDialog()
{
    if (! app.hasProject()) return;
    if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Close project")
                                      .withMessage ("Do you want to save \"" + app.project.name + "\" before closing it?\n\n"
                                                    "(The project is also saved automatically as you work; \"Close without saving\" skips only this last save.) Fermata stays open, ready for a new or another project.")
                                      .withButton ("Save and close").withButton ("Close without saving").withButton ("Cancel"),
                                  [safe] (int r)
                                  {
                                      if (safe == nullptr) return;
                                      if (r == 1) safe->app.closeProject (true);
                                      else if (r == 2) safe->app.closeProject (false);
                                  });
}

void MainComponent::newProjectDialog()
{
    if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
    auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata");
    start.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Where do you want to save your project? Choose the place, then type a name: a folder with that name is created there and everything goes inside it", start.getChildFile ("New project.fermata"), "*.fermata;*.takedaw");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        const auto name = f.getFileNameWithoutExtension();
        auto folder = f.getParentDirectory().getChildFile (name);
        app.newProject (folder.getChildFile (name + ".fermata"));
    });
}

void MainComponent::openProjectDialog()
{
    if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
    auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata");
    chooser = std::make_unique<juce::FileChooser> ("Open a project", start, "*.fermata;*.takedaw");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f.existsAsFile()) app.openProject (f);
    });
}

void MainComponent::importWindowFileDialog (bool isEdit)
{
    if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
    auto start = app.project.projectFolder().getChildFile (isEdit ? "Edit Windows" : "Take Windows");
    if (! start.isDirectory()) start = app.project.projectFolder();
    chooser = std::make_unique<juce::FileChooser> (isEdit ? "Choose an edit file (.fmedit)" : "Choose a take window file (.fmtake)", start, isEdit ? "*.fmedit" : "*.fmtake");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, isEdit] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        juce::String msg; juce::Uuid id;
        if (! app.project.importWindowFile (f, msg, &id)) { showError ("Import", msg); return; }
        app.saveNow();
        if (app.project.missingAudioCount() == 0)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Imported", msg);
            if (isEdit) { if (app.showEdit) app.showEdit (id); } else if (app.showTakeWindow) app.showTakeWindow (id);
            return;
        }
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Imported - audio not found")
                                          .withMessage (msg + "\n\nDo you want to point Fermata at the folder that holds the audio files? It looks in that folder and all the folders inside it, by file name.")
                                          .withButton ("Find the audio files...").withButton ("Later"),
                                      [this, isEdit, id] (int r)
        {
            if (r != 1) { if (isEdit) { if (app.showEdit) app.showEdit (id); } else if (app.showTakeWindow) app.showTakeWindow (id); return; }
            relinkAudioDialog (isEdit, id);
        });
    });
}

void MainComponent::relinkAudioDialog (bool isEdit, const juce::Uuid& openAfter)
{
    chooser = std::make_unique<juce::FileChooser> ("Choose the folder that holds the audio files", app.project.projectFolder(), "*");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, isEdit, openAfter] (const juce::FileChooser& fc)
    {
        auto folder = fc.getResult();
        if (folder.isDirectory())
        {
            const int found = app.project.relinkAudio (folder);
            app.saveNow();
            const int left = app.project.missingAudioCount();
            juce::AlertWindow::showMessageBoxAsync (left == 0 ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon, "Audio files",
                                                    juce::String (found) + " audio file(s) found and connected." + (left > 0 ? "\n" + juce::String (left) + " still could not be found. You can choose another folder from the same menu (\"Find missing audio files...\")." : juce::String()));
        }
        if (isEdit) { if (app.showEdit) app.showEdit (openAfter); } else if (app.showTakeWindow) app.showTakeWindow (openAfter);
    });
}

void MainComponent::showTakesMenu()
{
    juce::PopupMenu m, del;
    int i = 1;
    for (auto& w : app.project.takeWindows) { m.addItem (i, "Open: " + w->name); del.addItem (2000 + i, w->name); ++i; }
    m.addSeparator();
    m.addItem (1000, "New take window...");
    m.addItem (1001, "Import a take window file (.fmtake)...");
    m.addItem (1002, "Find missing audio files...", app.project.missingAudioCount() > 0);
    if (i > 1) { m.addSeparator(); m.addSubMenu ("Delete a take window", del); }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&takesButton), [this] (int r)
    {
        if (r == 1000)
        {
            auto& w = app.project.addTakeWindow ("Piece " + juce::String ((int) app.project.takeWindows.size() + 1));
            app.recordTarget = w.id;
            if (app.showTakeWindow) app.showTakeWindow (w.id);
        }
        else if (r == 1001) importWindowFileDialog (false);
        else if (r == 1002) relinkAudioDialog (false, juce::Uuid::null());
        else if (r > 2000 && (size_t) (r - 2000) <= app.project.takeWindows.size())
        {
            const auto id = app.project.takeWindows[(size_t) r - 2001]->id; const auto nm = app.project.takeWindows[(size_t) r - 2001]->name;
            confirmAsync ("Delete take window",
                          "Do you really want to delete the take window \"" + nm + "\" and its file?\n\nThe take window is removed from this project and its .fmtake file goes to the recycle bin. "
                          "Your audio recordings are NOT deleted, and edits already made from it keep working.\n\nThis cannot be undone with Ctrl+Z.",
                          "Yes, I do want to delete it", [this, id] { app.deleteWindowAndFile (false, id); });
        }
        else if (r >= 1 && (size_t) r <= app.project.takeWindows.size() && app.showTakeWindow)
            app.showTakeWindow (app.project.takeWindows[(size_t) r - 1]->id);
    });
}

void MainComponent::showEditsMenu()
{
    juce::PopupMenu m, del;
    int i = 1;
    for (auto& e : app.project.edits) { m.addItem (i, "Open: " + e->name); del.addItem (2000 + i, e->name); ++i; }
    m.addSeparator();
    m.addItem (1000, "New empty edit");
    m.addItem (1001, "Import an edit file (.fmedit)...");
    m.addItem (1002, "Find missing audio files...", app.project.missingAudioCount() > 0);
    if (i > 1) { m.addSeparator(); m.addSubMenu ("Delete an edit", del); }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&editsButton), [this] (int r)
    {
        if (r == 1000)
        {
            auto& e = app.makeEdit ("Edit " + juce::String ((int) app.project.edits.size() + 1));
            if (app.showEdit) app.showEdit (e.id);
        }
        else if (r == 1001) importWindowFileDialog (true);
        else if (r == 1002) relinkAudioDialog (true, juce::Uuid::null());
        else if (r > 2000 && (size_t) (r - 2000) <= app.project.edits.size())
        {
            const auto id = app.project.edits[(size_t) r - 2001]->id; const auto nm = app.project.edits[(size_t) r - 2001]->name;
            bool inMastering = false;
            for (auto& it : app.project.mastering.items) if (it.editId == id) inMastering = true;
            confirmAsync ("Delete edit",
                          "Do you really want to delete the edit \"" + nm + "\" and its file?\n\nThe edit is removed from this project" + (inMastering ? ", and from the Mastering list," : "") +
                          " and its .fmedit file goes to the recycle bin. Your audio recordings are NOT deleted.\n\nThis cannot be undone with Ctrl+Z.",
                          "Yes, I do want to delete it", [this, id] { app.deleteWindowAndFile (true, id); });
        }
        else if (r >= 1 && (size_t) r <= app.project.edits.size() && app.showEdit)
            app.showEdit (app.project.edits[(size_t) r - 1]->id);
    });
}

void MainComponent::rebuildAuditionBar()
{
    auditionButtons.clear();
    const auto& mx = app.project.mixers;
    for (size_t i = 1; i < (size_t) app.project.cueEnd(); ++i)
    {
        auto* b = auditionButtons.add (new juce::TextButton());
        const auto id = mx[i]->id;
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd9822b));
        b->setTooltip ("Listen to this mixer on your own outputs, and edit it, until you press this again");
        b->onClick = [this, id] { app.setAudition (id); if (app.isAuditioning (id) && app.showMixer) app.showMixer (id); };
        addAndMakeVisible (b);
    }
    auditionCaption.setText (auditionButtons.isEmpty() ? juce::String() : "Audition a cue mixer:", juce::dontSendNotification);
    addAndMakeVisible (auditionCaption);
    for (int i = 0; i < auditionButtons.size(); ++i)
    {
        auto& m = *mx[(size_t) i + 1];
        const bool on = app.isAuditioning (m.id);
        auditionButtons[i]->setButtonText ((on ? "AUDITIONING: " : "") + m.name + " Audition");
        auditionButtons[i]->setToggleState (on, juce::dontSendNotification);
    }
    resized();
}

void MainComponent::showMixersMenu()
{
    juce::PopupMenu m;
    const int ce = app.project.cueEnd();
    for (int i = 0; i < ce; ++i) m.addItem (i + 1, "Open: " + app.project.mixers[(size_t) i]->name + (i == 0 ? "   (processing mixer, key M)" : "   (cue mixer)"));
    if (ce < (int) app.project.mixers.size())
    {
        m.addSeparator();
        m.addSectionHeader ("Edit mixers (each Edit plays through its own)");
        for (int i = ce; i < (int) app.project.mixers.size(); ++i) m.addItem (i + 1, "Open: " + app.project.mixers[(size_t) i]->name + "   (edit mixer)");
    }
    m.addSeparator();
    m.addItem (1000, "New cue mixer");
    m.addItem (1001, "Import a mixer file (.fmmix) as a new mixer...");
    if (ce > 1)
    {
        juce::PopupMenu del;
        for (int k = 1; k < ce; ++k) del.addItem (2000 + k, app.project.mixers[(size_t) k]->name);
        m.addSubMenu ("Delete a cue mixer", del);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&mixersButton), [this] (int r)
    {
        if (r > 2000 && (size_t) (r - 2000) < app.project.mixers.size())
        {
            const auto id = app.project.mixers[(size_t) (r - 2000)]->id;
            confirmAsync ("Delete cue mixer", "Delete the cue mixer '" + app.project.mixers[(size_t) (r - 2000)]->name + "'?\n\nIts levels, sends and plug-ins are lost. Nothing else is touched.", "Delete", [this, id] { app.deleteMixer (id); });
            return;
        }
        if (r == 1001)
        {
            if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
            auto start = app.project.projectFolder().getChildFile ("Mixer Windows");
            if (! start.isDirectory()) start = app.project.projectFolder();
            chooser = std::make_unique<juce::FileChooser> ("Choose a mixer file (.fmmix)", start, "*.fmmix");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (! f.existsAsFile()) return;
                juce::String msg;
                if (! app.project.importMixerFile (f, msg)) { showError ("Import", msg); return; }
                app.saveNow();
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Imported", msg);
            });
            return;
        }
        if (r == 1000)
        {
            auto& mx = app.project.addMixer ("Cue mixer " + juce::String (app.project.cueEnd()));
            if (app.showMixer) app.showMixer (mx.id);
        }
        else if (r >= 1 && (size_t) r <= app.project.mixers.size() && app.showMixer)
            app.showMixer (app.project.mixers[(size_t) r - 1]->id);
    });
}

// ---------------------------------------------------------------------------------------------------------------- LTC menus
void MainComponent::showLtcInMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("LTC input (timecode as audio)");
    m.addItem (1, "None", true, app.project.ltcInput < 0);
    const int n = (int) app.project.inputs.size();
    for (int i = 0; i < n; ++i) m.addItem (100 + i, "Input " + juce::String (i + 1) + "  -  " + app.project.inputLabel (i), true, app.project.ltcInput == i);
    m.addSeparator();
    m.addItem (2, "It is recorded with every take, on the track named LTC (show it with the LTC triangle in the take and edit windows)", false);
    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&ltcInLamp).withMinimumWidth (320), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1) safe->app.setLtcInput (-1);
        else if (r >= 100) safe->app.setLtcInput (r - 100);
    });
}

static bool parseTimecodeText (const juce::String& text, double fps, double& seconds)
{
    juce::StringArray p; p.addTokens (text.trim(), ":;.", "");
    p.removeEmptyStrings();
    if (p.size() != 4) return false;
    const int h = p[0].getIntValue(), mi = p[1].getIntValue(), s = p[2].getIntValue(), f = p[3].getIntValue();
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59 || f < 0 || f > 59) return false;
    seconds = h * 3600.0 + mi * 60.0 + s + f / fps;
    return true;
}

void MainComponent::showLtcOutMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("LTC output");
    m.addItem (1, "Send LTC", app.project.ltcOutput >= 0, app.project.ltcOutOn);
    juce::PopupMenu outs;
    outs.addItem (50, "None", true, app.project.ltcOutput < 0);
    for (int i = 0; i < (int) app.project.outputs.size(); ++i) outs.addItem (200 + i, "Output " + juce::String (i + 1) + "  -  " + app.project.outputLabel (i), true, app.project.ltcOutput == i);
    m.addSubMenu ("Output channel (route it in Aneman to the Hapi TC output)", outs);
    juce::PopupMenu fm;
    for (int c : { 1, 0, 3, 2, 4, 5 }) fm.addItem (10 + c, LtcGenerator::formatName (c) + (c == 2 ? "  (drop-frame)" : ""), true, app.project.ltcOutFormat == c);
    m.addSubMenu ("Frame rate", fm);
    juce::PopupMenu sm;
    sm.addItem (20, "The time of day on this computer", true, app.project.ltcOutStartMode == 0);
    sm.addItem (21, "00:00:00:00", true, app.project.ltcOutStartMode == 1 && app.project.ltcOutStartSeconds < 0.5);
    sm.addItem (22, "01:00:00:00", true, app.project.ltcOutStartMode == 1 && std::abs (app.project.ltcOutStartSeconds - 3600.0) < 0.5);
    sm.addItem (23, "Type a start time...");
    m.addSubMenu ("Starts at", sm);
    m.addItem (3, "Restart now", app.project.ltcOutOn);
    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&ltcOutLamp).withMinimumWidth (260), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& a = safe->app; auto& p = a.project;
        if (r == 1) p.ltcOutOn = ! p.ltcOutOn;
        else if (r == 50) { p.ltcOutput = -1; p.ltcOutOn = false; }
        else if (r >= 200) { p.ltcOutput = r - 200; p.ltcOutOn = true; }
        else if (r >= 10 && r < 20) p.ltcOutFormat = r - 10;
        else if (r == 20) p.ltcOutStartMode = 0;
        else if (r == 21 || r == 22) { p.ltcOutStartMode = 1; p.ltcOutStartSeconds = r == 21 ? 0.0 : 3600.0; }
        else if (r == 23)
        {
            auto* aw = new juce::AlertWindow ("LTC start time", "Type the timecode the LTC starts at (hours:minutes:seconds:frames):", juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor ("tc", "01:00:00:00", "Start");
            aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int res)
            {
                if (res == 1 && safe != nullptr)
                {
                    double sec = 0.0; double fps = 25.0; int n = 0; bool d = false; LtcGenerator::formatInfo (safe->app.project.ltcOutFormat, fps, n, d);
                    if (parseTimecodeText (aw->getTextEditorContents ("tc"), fps, sec))
                    { safe->app.project.ltcOutStartMode = 1; safe->app.project.ltcOutStartSeconds = sec; safe->app.project.markDirty(); safe->app.applyLtc (true); }
                    else safe->app.setNotice ("That is not a timecode. Type it like 01:00:00:00");
                }
            }), true);
            return;
        }
        p.markDirty();
        a.applyLtc (r == 3 || r == 20 || r == 21 || r == 22);
    });
}
} // namespace td
