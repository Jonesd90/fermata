#pragma once
#include "Surface.h"
#include "FermataLookAndFeel.h"

namespace td
{
/** The CONTROLLER box (the main window's top row, and the panel): which mixer the surface controls - Off, the mixer in front ("follow"), or one
    mixer pinned by name. It keeps itself up to date as mixers come and go. */
class SurfaceTargetBox : public juce::ComboBox, private juce::ChangeListener
{
public:
    explicit SurfaceTargetBox (AppContext& a) : app (a)
    {
        setTooltip ("Which mixer the control surface (the S1) controls: the mixer whose window is in front (or was in front last), or one mixer, whatever is in front. Off: the surface is left alone.");
        fill();
        onChange = [this] { apply(); };
        app.project.structure.addChangeListener (this);
        if (app.surface != nullptr) app.surface->addChangeListener (this);
    }
    ~SurfaceTargetBox() override
    {
        app.project.structure.removeChangeListener (this);
        if (app.surface != nullptr) app.surface->removeChangeListener (this);
    }
    void fill()
    {
        auto* s = app.surface;
        juce::String sig;
        for (auto& m : app.project.mixers) sig << m->id.toString() << m->name << "|";
        if (s != nullptr) sig << (int) s->target() << s->pinned().toString();
        if (sig == lastSig) return;
        lastSig = sig;
        filling = true;
        clear (juce::dontSendNotification);
        addItem ("Off", 1);
        addItem ("Follow Fermata focus", 2);
        int i = 10;
        for (auto& m : app.project.mixers) addItem ("Pinned: " + m->name, i++);
        int want = 2;
        if (s != nullptr)
        {
            if (s->target() == SurfaceController::Target::Off) want = 1;
            else if (s->target() == SurfaceController::Target::Pinned)
                for (size_t k = 0; k < app.project.mixers.size(); ++k) if (app.project.mixers[k]->id == s->pinned()) want = 10 + (int) k;
        }
        setSelectedId (want, juce::dontSendNotification);
        filling = false;
    }
private:
    void apply()
    {
        auto* s = app.surface;
        if (filling || s == nullptr) return;
        const int id = getSelectedId();
        if (id == 1) s->setTarget (SurfaceController::Target::Off);
        else if (id == 2) s->setTarget (SurfaceController::Target::Follow);
        else if (id >= 10 && (size_t) id - 10 < app.project.mixers.size()) s->setTarget (SurfaceController::Target::Pinned, app.project.mixers[(size_t) id - 10]->id);
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { fill(); }
    AppContext& app; bool filling = false; juce::String lastSig;
};

/** "Mixer controller": the MIDI ports the surface is on, which mixer it controls, what its keys do (the strip keys from lists, the other keys by
    pressing them: Learn), and a monitor of what arrives on the MIDI port - so that any key of the S1 can be made to do something without knowing
    beforehand what EuControl sends for it. */
class SurfacePanel : public juce::Component, private juce::ChangeListener, private juce::ListBoxModel, private juce::Timer
{
public:
    explicit SurfacePanel (AppContext& a) : app (a), targetBox (a)
    {
        auto* s = app.surface;
        intro.setText ("An Avid S1, or any Mackie Control surface. EuControl passes the S1 to Fermata as a Mackie Control surface on its virtual MIDI ports "
                       "(\"Euphonix EuMidi1\"): press Copy Fermata's path, then in EuControl (the icon by the clock) open MIDI Setup, and for ports 1 - 4 choose "
                       "Select Mackie Application, paste the path (Ctrl + V) into the file name box and Select; Apply and restart EuControl. Close Pyramix and other EUCON "
                       "programs: EuControl gives the S1 to the EUCON program you clicked last. Then the ports appear below and the S1 lamp goes green. "
                       "Eight strips of the chosen mixer at a time (Bank / Channel keys); dials = pan, after Inserts = preamp gain; Flip swaps dials and faders. "
                       "The eight keys along the bottom of the S1 are set up already; any key can be given another job: press Learn, then the key.", juce::dontSendNotification);
        intro.setFont (juce::FontOptions (12.0f)); intro.setColour (juce::Label::textColourId, theme::dimText); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro);

        auto caption = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (11.5f, juce::Font::bold)); l.setColour (juce::Label::textColourId, theme::text);
            l.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (l);
        };
        caption (inCap, "MIDI in"); caption (outCap, "MIDI out"); caption (targetCap, "Controls"); caption (stripCap, "THE STRIP KEYS"); caption (keysCap, "THE OTHER KEYS"); caption (monCap, "MIDI MONITOR");
        inBox.setTextWhenNothingSelected ("(none)"); outBox.setTextWhenNothingSelected ("(none)");
        inBox.onChange = [this] { applyPorts(); }; outBox.onChange = [this] { applyPorts(); };
        addAndMakeVisible (inBox); addAndMakeVisible (outBox);
        pathButton.setButtonText ("Copy Fermata's path");
        pathButton.setTooltip ("Puts the full path of Fermata.exe on the clipboard, to paste into EuControl's MIDI Setup (Select Mackie Application, ports 1 - 4)");
        pathButton.onClick = [this]
        {
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
            juce::SystemClipboard::copyTextToClipboard (exe);
            app.setNotice ("Copied: " + exe + "  -  paste it into EuControl > MIDI Setup > Select Mackie Application (ports 1 - 4)");
        };
        addAndMakeVisible (pathButton);
        lawBox.addItem ("Fader scale: the S1's own (EuControl)", 1);
        lawBox.addItem ("Fader scale: as Fermata's screen", 2);
        lawBox.setSelectedId (s != nullptr && s->faderLaw() == mcu::FaderLaw::Screen ? 2 : 1, juce::dontSendNotification);
        lawBox.setTooltip ("Where a level puts the fader. The S1's own: where EuControl's dB read-out (the S1's displays) has it, so the number the S1 shows is Fermata's level "
                           "(0 dB at 85 % of the travel; the top is +6 dB). Fermata's screen: the same curve as the faders on the screen (0 dB at 80 %, +12 at the top).");
        lawBox.onChange = [this] { if (app.surface != nullptr) app.surface->setFaderLaw (lawBox.getSelectedId() == 2 ? mcu::FaderLaw::Screen : mcu::FaderLaw::EuControl); };
        addAndMakeVisible (lawBox);
        refreshButton.setButtonText ("Refresh");
        refreshButton.setTooltip ("Look for MIDI ports again (after starting EuControl, or plugging a surface in)");
        refreshButton.onClick = [this] { fillPorts(); };
        addAndMakeVisible (refreshButton);
        status.setFont (juce::FontOptions (12.0f, juce::Font::bold)); status.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (status);
        addAndMakeVisible (targetBox);
        metersBox.setButtonText ("Send the levels to the surface's meters");
        metersBox.setToggleState (s != nullptr && s->metersOn(), juce::dontSendNotification);
        metersBox.onClick = [this] { if (app.surface != nullptr) app.surface->setMetersOn (metersBox.getToggleState()); };
        addAndMakeVisible (metersBox);

        for (int b = 0; b < surface::NumStripButtons; ++b)
        {
            auto* l = stripLabels.add (new juce::Label());
            l->setText (surface::stripButtonName (b) + " key:", juce::dontSendNotification); l->setFont (juce::FontOptions (12.0f)); l->setColour (juce::Label::textColourId, theme::text);
            addAndMakeVisible (l);
            auto* cb = stripBoxes.add (new juce::ComboBox());
            for (int a = 0; a < surface::NumStripActions; ++a) cb->addItem (surface::stripActionName (a), a + 1);
            cb->setSelectedId ((s != nullptr ? s->stripAssign (b) : 0) + 1, juce::dontSendNotification);
            cb->onChange = [this, b, cb] { if (app.surface != nullptr) app.surface->setStripAssign (b, cb->getSelectedId() - 1); };
            addAndMakeVisible (cb);
        }
        for (int a = 0; a < surface::NumActions; ++a)
        {
            auto* row = rows.add (new ActionRow (*this, a));
            addAndMakeVisible (row);
        }
        resetButton.setButtonText ("Back to the standard keys");
        resetButton.setTooltip ("The S1's eight bottom keys (Shift, Control, Opt, Alt, User 1 - 4) and a Mackie's F1 - F8 for the eight jobs; Plug-in = Inserts, Pan, Flip, Bank / Channel, the transport");
        resetButton.onClick = [this] { if (app.surface != nullptr) app.surface->resetMap(); };
        addAndMakeVisible (resetButton);

        monitor.setModel (this);
        monitor.setRowHeight (18);
        monitor.setColour (juce::ListBox::backgroundColourId, theme::field);
        monitor.setColour (juce::ListBox::outlineColourId, theme::border);
        monitor.setOutlineThickness (1);
        addAndMakeVisible (monitor);
        pauseButton.setButtonText ("Pause"); pauseButton.setClickingTogglesState (true);
        pauseButton.setToggleState (s != nullptr && s->monitorPaused, juce::dontSendNotification);
        pauseButton.onClick = [this] { if (app.surface != nullptr) app.surface->monitorPaused = pauseButton.getToggleState(); };
        clearButton.setButtonText ("Clear");
        clearButton.onClick = [this] { if (app.surface != nullptr) app.surface->clearMonitor(); };
        outToo.setButtonText ("Show what Fermata sends too");
        outToo.setToggleState (s != nullptr && s->monitorShowsOutput, juce::dontSendNotification);
        outToo.onClick = [this] { if (app.surface != nullptr) app.surface->monitorShowsOutput = outToo.getToggleState(); };
        addAndMakeVisible (pauseButton); addAndMakeVisible (clearButton); addAndMakeVisible (outToo);

        fillPorts();
        refresh();
        if (s != nullptr) s->addChangeListener (this);
        startTimer (500);
        setSize (780, 860);
    }
    ~SurfacePanel() override
    {
        if (app.surface != nullptr) { app.surface->cancelLearn(); app.surface->removeChangeListener (this); }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::text); g.setFont (FermataLookAndFeel::displayFont (17.0f));
        g.drawText ("Mixer controller  -  Avid S1 / Mackie Control", 16, 10, getWidth() - 32, 24, juce::Justification::centredLeft, false);
        g.setColour (theme::border);
        for (int y : rules) g.fillRect (16, y, getWidth() - 32, 1);
    }

    void resized() override
    {
        rules.clear();
        auto r = getLocalBounds().reduced (16, 10);
        r.removeFromTop (28);
        intro.setBounds (r.removeFromTop (96));
        r.removeFromTop (6);
        {
            auto row = r.removeFromTop (24);
            inCap.setBounds (row.removeFromLeft (54)); inBox.setBounds (row.removeFromLeft (250)); row.removeFromLeft (14);
            outCap.setBounds (row.removeFromLeft (62)); outBox.setBounds (row.removeFromLeft (250)); row.removeFromLeft (14);
            refreshButton.setBounds (row.removeFromLeft (80));
        }
        r.removeFromTop (6);
        {
            auto row = r.removeFromTop (24);
            pathButton.setBounds (row.removeFromLeft (160)); row.removeFromLeft (14);
            lawBox.setBounds (row.removeFromLeft (290));
        }
        r.removeFromTop (6);
        {
            auto row = r.removeFromTop (24);
            targetCap.setBounds (row.removeFromLeft (54)); targetBox.setBounds (row.removeFromLeft (250)); row.removeFromLeft (14);
            metersBox.setBounds (row.removeFromLeft (290)); row.removeFromLeft (10);
            status.setBounds (row);
        }
        r.removeFromTop (8); rules.push_back (r.getY()); r.removeFromTop (8);
        auto body = r.removeFromTop (surface::NumActions * 22 + 22);
        auto left = body.removeFromLeft (260); body.removeFromLeft (20);
        stripCap.setBounds (left.removeFromTop (18)); left.removeFromTop (4);
        for (int b = 0; b < stripBoxes.size(); ++b)
        {
            auto row = left.removeFromTop (24);
            stripLabels[b]->setBounds (row.removeFromLeft (76)); stripBoxes[b]->setBounds (row.reduced (0, 1));
            left.removeFromTop (2);
        }
        left.removeFromTop (10);
        resetButton.setBounds (left.removeFromTop (24));
        keysCap.setBounds (body.removeFromTop (18)); body.removeFromTop (4);
        for (auto* row : rows) row->setBounds (body.removeFromTop (22));
        r.removeFromTop (8); rules.push_back (r.getY()); r.removeFromTop (8);
        {
            auto row = r.removeFromTop (22);
            monCap.setBounds (row.removeFromLeft (110));
            pauseButton.setBounds (row.removeFromLeft (60)); row.removeFromLeft (6);
            clearButton.setBounds (row.removeFromLeft (60)); row.removeFromLeft (12);
            outToo.setBounds (row.removeFromLeft (240));
        }
        r.removeFromTop (4);
        monitor.setBounds (r);
    }

private:
    /** One of the other keys: what it does, which keys do it, Learn and Clear. */
    struct ActionRow : public juce::Component
    {
        ActionRow (SurfacePanel& p, int a) : panel (p), action (a)
        {
            name.setText (surface::actionName (a), juce::dontSendNotification); name.setFont (juce::FontOptions (12.0f)); name.setColour (juce::Label::textColourId, theme::text);
            keys.setFont (juce::FontOptions (11.5f, juce::Font::bold)); keys.setColour (juce::Label::textColourId, theme::accent);
            learn.setButtonText ("Learn"); learn.setClickingTogglesState (false);
            learn.setTooltip ("Press this, then the key on the surface that should do this");
            learn.onClick = [this] { if (auto* s = panel.app.surface) { if (s->learning() == action) s->cancelLearn(); else s->startLearn (action); } };
            clear.setButtonText ("x"); clear.setTooltip ("No key does this");
            clear.onClick = [this] { if (auto* s = panel.app.surface) s->unbind (action); };
            addAndMakeVisible (name); addAndMakeVisible (keys); addAndMakeVisible (learn); addAndMakeVisible (clear);
        }
        void refresh()
        {
            auto* s = panel.app.surface;
            const bool me = s != nullptr && s->learning() == action;
            keys.setText (s != nullptr ? (me ? "press the key now ..." : s->bindingsText (action)) : juce::String(), juce::dontSendNotification);
            keys.setColour (juce::Label::textColourId, me ? theme::warn : theme::accent);
            learn.setButtonText (me ? "Cancel" : "Learn");
            learn.setToggleState (me, juce::dontSendNotification);
            learn.setEnabled (s != nullptr && (s->learning() < 0 || me));
            clear.setEnabled (s != nullptr && s->bindingsText (action).isNotEmpty());
        }
        void resized() override
        {
            auto r = getLocalBounds();
            clear.setBounds (r.removeFromRight (22).reduced (0, 2)); r.removeFromRight (4);
            learn.setBounds (r.removeFromRight (62).reduced (0, 2)); r.removeFromRight (8);
            name.setBounds (r.removeFromLeft (214)); keys.setBounds (r);
        }
        SurfacePanel& panel; int action;
        juce::Label name, keys; juce::TextButton learn, clear;
    };

    void fillPorts()
    {
        auto* s = app.surface;
        const auto want = s != nullptr ? s->ports() : SurfaceController::Ports();
        // the port chosen, or (nothing chosen yet) the one the surface took because it looked like the S1
        const auto inWant = want.inId.isNotEmpty() ? want.inId : (s != nullptr ? s->openInId() : juce::String());         // ("none" matches no port: (none) stays shown)
        const auto outWant = want.outId.isNotEmpty() ? want.outId : (s != nullptr ? s->openOutId() : juce::String());
        filling = true;
        inIds.clear(); outIds.clear();
        inBox.clear (juce::dontSendNotification); outBox.clear (juce::dontSendNotification);
        inBox.addItem ("(none)", 1); outBox.addItem ("(none)", 1);
        int sel = 1, k = 2;
        for (auto& d : juce::MidiInput::getAvailableDevices()) { inIds.add (d.identifier); inBox.addItem (d.name, k); if (d.identifier == inWant) sel = k; ++k; }
        inBox.setSelectedId (sel, juce::dontSendNotification);
        sel = 1; k = 2;
        for (auto& d : juce::MidiOutput::getAvailableDevices()) { outIds.add (d.identifier); outBox.addItem (d.name, k); if (d.identifier == outWant) sel = k; ++k; }
        outBox.setSelectedId (sel, juce::dontSendNotification);
        filling = false;
    }
    void applyPorts()
    {
        if (filling || app.surface == nullptr) return;
        const int i = inBox.getSelectedId() - 2, o = outBox.getSelectedId() - 2;
        app.surface->setPorts (i >= 0 && i < inIds.size() ? inIds[i] : juce::String (SurfaceController::kNoPort),          // "(none)": really none (an empty choice would pick the S1 again)
                               o >= 0 && o < outIds.size() ? outIds[o] : juce::String (SurfaceController::kNoPort));
    }
    void refresh()
    {
        auto* s = app.surface;
        status.setText (s != nullptr ? s->statusText() : juce::String ("(no surface)"), juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, s != nullptr && s->connected() ? juce::Colour (0xff2ecc40) : theme::warn);
        for (int b = 0; b < stripBoxes.size(); ++b) if (s != nullptr && stripBoxes[b]->getSelectedId() != s->stripAssign (b) + 1) stripBoxes[b]->setSelectedId (s->stripAssign (b) + 1, juce::dontSendNotification);
        if (s != nullptr) { const int want = s->faderLaw() == mcu::FaderLaw::Screen ? 2 : 1; if (lawBox.getSelectedId() != want) lawBox.setSelectedId (want, juce::dontSendNotification); }
        for (auto* row : rows) row->refresh();
        const int n = s != nullptr ? (int) s->monitor().size() : 0;
        if (n != shownLines) { shownLines = n; monitor.updateContent(); if (s != nullptr && ! s->monitorPaused && n > 0) monitor.scrollToEnsureRowIsOnscreen (n - 1); }
        else monitor.repaint();
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { refresh(); }

    // ---- the monitor's rows
    int getNumRows() override { return app.surface != nullptr ? (int) app.surface->monitor().size() : 0; }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        auto* s = app.surface;
        if (s == nullptr || row < 0 || row >= (int) s->monitor().size()) return;
        const auto& l = s->monitor()[(size_t) row];
        if ((row & 1) != 0) { g.setColour (theme::text.withAlpha (0.04f)); g.fillRect (0, 0, w, h); }
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::plain)));
        g.setColour (theme::dimText); g.drawText (l.time, 6, 0, 66, h, juce::Justification::centredLeft, false);
        g.setColour (l.dir == "in" ? juce::Colour (0xff2ecc40) : theme::accent); g.drawText (l.dir, 74, 0, 30, h, juce::Justification::centredLeft, false);
        g.setColour (theme::dimText); g.drawText (l.hex, 104, 0, 160, h, juce::Justification::centredLeft, false);
        g.setColour (theme::text); g.drawText (l.text, 268, 0, w - 274, h, juce::Justification::centredLeft, true);
    }

    AppContext& app;
    juce::Label intro, inCap, outCap, targetCap, stripCap, keysCap, monCap, status;
    juce::ComboBox inBox, outBox; juce::StringArray inIds, outIds; bool filling = false;
    juce::TextButton refreshButton, resetButton, pauseButton, clearButton, pathButton;
    juce::ComboBox lawBox;
    SurfaceTargetBox targetBox;
    juce::ToggleButton metersBox, outToo;
    juce::OwnedArray<juce::Label> stripLabels; juce::OwnedArray<juce::ComboBox> stripBoxes;
    juce::OwnedArray<ActionRow> rows;
    juce::ListBox monitor; int shownLines = -1;
    std::vector<int> rules;
};
} // namespace td
