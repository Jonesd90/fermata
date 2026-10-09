#include "MainWindow.h"
#include <set>
#include "WinOwner.h"
#include "FixTools.h"
#include "PreampSetup.h"

namespace td
{
/** One driver input: its number and fixed driver name, an optional second name, a live meter, and the preamp controls
    (they belong to the physical input, so there is exactly one set no matter how many mixers or tracks use it). */
class InputRow : public juce::Component
{
public:
    InputRow (AppContext& a, int index) : app (a), idx (index)
    {
        number.setText (juce::String (idx + 1), juce::dontSendNotification);
        number.setJustificationType (juce::Justification::centredRight);
        number.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        driver.setText (app.project.inputs[(size_t) idx].driverName, juce::dontSendNotification);
        driver.setColour (juce::Label::textColourId, theme::text);
        driver.setTooltip ("The name the audio driver gives this input (it cannot be changed)");
        alias.setText (app.project.inputs[(size_t) idx].name, juce::dontSendNotification);
        alias.setTextToShowWhenEmpty ("second name (e.g. ORTF L)", theme::dimText.withAlpha (0.6f));
        alias.setTooltip ("Your own name for this input. It is shown next to the driver name everywhere.");
        auto commit = [this]
        {
            if (idx >= (int) app.project.inputs.size()) return;
            app.project.setInputName (idx, alias.getText());
        };
        alias.onReturnKey = commit; alias.onFocusLost = commit;
        for (auto* c : std::initializer_list<juce::Component*> { &number, &driver, &alias, &meter }) addAndMakeVisible (c);

        gain.setSliderStyle (juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
        gain.setRange (0.0, 66.0, 0.5);
        gain.setScrollWheelEnabled (false);                   // the mouse wheel must never change a preamp gain by accident
        gain.setTextValueSuffix (" dB");
        gain.setDoubleClickReturnValue (true, 20.0);
        gain.setTooltip ("Preamp gain (double-click for 20 dB)");
        gain.onValueChange = [this] { push(); };
        addAndMakeVisible (gain);

        // the monitor mode applies to every track that is fed by this input (ARM lives in the Take window)
        static const char* tips[] = { "E - Editing: the live input is only heard while it is ARMed.",
                                      "S - Session: always heard, armed or not. While a take plays back it is muted and you hear the recording.",
                                      "S-L - Session Live: always heard, even while a take plays back.",
                                      "T - Talkback (conductor's microphone): always heard between takes, completely off while recording." };
        for (int m = 0; m < 4; ++m)
        {
            auto* b = monButtons.add (new juce::TextButton (monitorLabel ((Monitor) m)));
            b->setRadioGroupId (4700 + idx);
            b->setClickingTogglesState (true);
            b->setTooltip (tips[m]);
            b->setColour (juce::TextButton::buttonOnColourId, m == 3 ? juce::Colour (0xffc26500) : juce::Colour (0xff1f7a46));
            b->onClick = [this, m] { if (updating) return; for (auto* t : tracksOnInput()) t->setMonitor ((Monitor) m); app.project.markDirty(); };
            addAndMakeVisible (b);
        }

        crButton.setClickingTogglesState (true);
        crButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffa83cff));
        crButton.setTooltip ("CR: this input is part of the Control Room microphone. Switch CR on for as many inputs as you like: they are all heard together. Numpad + opens it to the TB (talkback) outputs: tap to latch, hold for as long as the key is held.");
        crButton.onClick = [this] { if (! updating) app.setCrMicInput (idx, crButton.getToggleState()); };
        addAndMakeVisible (crButton);
        mic = make ("MIC", "Mic / Line: lit = LINE input, off = MIC input", theme::accent);
        v48 = make ("48V", "Phantom power (+48 V)", juce::Colour (0xffc62828));
        pol = make (juce::String (juce::CharPointer_UTF8 ("\xc3\x98")), "Polarity invert", theme::accent);
        lc  = make ("LC", "Low cut filter", theme::accent);
        pad = make ("PAD", "Pad: the mic preamp's input attenuator", theme::accent);
        zhi = make ("Z HI", "High input impedance (the device's Z in switch)", theme::accent);
        boost = make ("BOOST", "Boost: the mic preamp's extra-gain setting (Hapi: Mic / Boost). Sent to the device as its 'lift' switch.", theme::accent);
        refreshFromModel();
    }

    void refreshFromModel()
    {
        if (idx >= (int) app.project.inputs.size()) return;
        auto& in = app.project.inputs[(size_t) idx];
        updating = true;
        if (driver.getText() != in.driverName) driver.setText (in.driverName, juce::dontSendNotification);
        if (! alias.hasKeyboardFocus (false) && alias.getText() != in.name) alias.setText (in.name, juce::dontSendNotification);
        if (std::abs (gain.getValue() - in.preamp.gainDb) > 0.01 && ! gain.isMouseButtonDown()) gain.setValue (in.preamp.gainDb, juce::dontSendNotification);
        const bool lo = in.preamp.lineOnly;                                         // a fixed line input (Anubis jack 3/4): only gain, phase, low cut and Line / Instrument
        if (lo)                                                                     // the first button is the Line / Instrument switch: LINE (plain) or INSTR (blue)
        {
            mic->setToggleState (in.preamp.zHigh, juce::dontSendNotification);
            mic->setButtonText (in.preamp.zHigh ? "INSTR" : "LINE");
            mic->setEnabled (in.preamp.hasInstrument);
            mic->setTooltip ("Line / Instrument: lit = INSTRUMENT input, plain = LINE input");
        }
        else
        {
            mic->setToggleState (in.preamp.line, juce::dontSendNotification);
            mic->setButtonText (in.preamp.line ? "LINE" : "MIC");
            mic->setEnabled (true);
            mic->setTooltip ("Mic / Line: lit = LINE input, off = MIC input");
        }
        for (auto* b : { v48, pad, boost, zhi }) b->setVisible (! lo);
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

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 3);
        number.setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        driver.setBounds (r.removeFromLeft (104));
        alias.setBounds (r.removeFromLeft (120).reduced (0, 1));
        r.removeFromLeft (6);
        meter.setBounds (r.removeFromLeft (84).reduced (0, 6));
        r.removeFromLeft (6);
        gain.setBounds (r.removeFromLeft (190));
        r.removeFromLeft (4);
        for (auto* b : { mic, v48, pol, lc, pad, zhi, boost }) b->setBounds (r.removeFromLeft (b == mic ? 48 : b == boost ? 58 : b == zhi ? 46 : 40).reduced (1, 0));
        r.removeFromLeft (10);
        for (auto* b : monButtons) b->setBounds (r.removeFromLeft (b->getButtonText().length() > 1 ? 38 : 30).reduced (1, 0));
        r.removeFromLeft (10);
        crButton.setBounds (r.removeFromLeft (36).reduced (1, 0));
    }
    void paint (juce::Graphics& g) override { g.setColour (theme::row.withAlpha (0.8f)); g.fillRect (getLocalBounds().reduced (1)); g.setColour (theme::border.withAlpha (0.6f)); g.drawRect (getLocalBounds().reduced (1), 1); }

    static constexpr int kRowHeight = 30, kMinWidth = 1100;
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
    juce::TextButton* make (const juce::String& text, const juce::String& tip, juce::Colour on)
    {
        auto* b = buttons.add (new juce::TextButton (text));
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, on);
        b->setTooltip (tip);
        b->onClick = [this] { push(); };
        addAndMakeVisible (b);
        return b;
    }
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

    AppContext& app; int idx; bool updating = false;
    juce::Label number, driver; juce::TextEditor alias; LevelMeter meter { false }; juce::Slider gain;
    juce::OwnedArray<juce::TextButton> buttons, monButtons;
    juce::TextButton crButton { "CR" };
    juce::TextButton *mic = nullptr, *v48 = nullptr, *pol = nullptr, *lc = nullptr, *boost = nullptr, *pad = nullptr, *zhi = nullptr;
};

/** One driver output: number, fixed driver name, an optional second name, a live meter and which mixer feeds it. */
class OutputRow : public juce::Component
{
public:
    OutputRow (AppContext& a, int index) : app (a), idx (index)
    {
        number.setText (juce::String (idx + 1), juce::dontSendNotification);
        number.setJustificationType (juce::Justification::centredRight);
        number.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        driver.setText (app.project.outputs[(size_t) idx].driverName, juce::dontSendNotification);
        driver.setColour (juce::Label::textColourId, theme::text);
        driver.setTooltip ("The name the audio driver gives this output (it cannot be changed)");
        alias.setText (app.project.outputs[(size_t) idx].name, juce::dontSendNotification);
        alias.setTextToShowWhenEmpty ("second name (e.g. Monitors L)", theme::dimText.withAlpha (0.6f));
        alias.setTooltip ("Your own name for this output.");
        auto commit = [this]
        {
            if (idx >= (int) app.project.outputs.size()) return;
            if (app.project.outputs[(size_t) idx].name != alias.getText()) { app.project.outputs[(size_t) idx].name = alias.getText(); app.project.changed(); }
        };
        alias.onReturnKey = commit; alias.onFocusLost = commit;
        fedBy.setColour (juce::Label::textColourId, theme::dimText);
        fedBy.setFont (juce::FontOptions (12.0f));
        fedBy.setMinimumHorizontalScale (0.8f);
        for (auto* c : std::initializer_list<juce::Component*> { &number, &driver, &alias, &meter, &fedBy }) addAndMakeVisible (c);
        tbButton.setClickingTogglesState (true);
        tbButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe0b800));
        tbButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        tbButton.setTooltip ("TB: make this output and the next one a stereo talkback pair (to the studio). Any outputs, any number of pairs; on the last output it is mono. Only the CR mic and recorded playback can ever reach it - never live microphones. Click again to turn the pair off.");
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
        updating = false;
        if (driver.getText() != o.driverName) driver.setText (o.driverName, juce::dontSendNotification);
        if (! alias.hasKeyboardFocus (false) && alias.getText() != o.name) alias.setText (o.name, juce::dontSendNotification);
        const auto fed = app.project.outputFedBy (idx);
        const auto t = fed.isEmpty() ? juce::String() : "<- " + fed;
        if (fedBy.getText() != t) fedBy.setText (t, juce::dontSendNotification);
    }
    void tick (const std::vector<float>& outPk) { meter.setLevel (juce::isPositiveAndBelow (idx, (int) outPk.size()) ? outPk[(size_t) idx] : 0.0f); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 3);
        number.setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        driver.setBounds (r.removeFromLeft (104));
        alias.setBounds (r.removeFromLeft (130).reduced (0, 1));
        r.removeFromLeft (6);
        meter.setBounds (r.removeFromLeft (84).reduced (0, 6));
        r.removeFromLeft (6);
        tbButton.setBounds (r.removeFromLeft (44).reduced (1, 0));
        r.removeFromLeft (6);
        fedBy.setBounds (r);
    }
    void paint (juce::Graphics& g) override { g.setColour (theme::row.withAlpha (0.8f)); g.fillRect (getLocalBounds().reduced (1)); g.setColour (theme::border.withAlpha (0.6f)); g.drawRect (getLocalBounds().reduced (1), 1); }
    static constexpr int kRowHeight = 30, kMinWidth = 510;
private:
    bool hasPair (int first) const { auto& v = app.project.tbOutputs; return std::find (v.begin(), v.end(), first) != v.end(); }
    bool isTbNow() const { return hasPair (idx) || hasPair (idx - 1); }
    int tbFirst() const { return hasPair (idx) ? idx : hasPair (idx - 1) ? idx - 1 : idx; }       // the right-hand output of a pair switches that pair off
    AppContext& app; int idx; bool updating = false;
    juce::TextButton tbButton { "TB" };
    juce::Label number, driver, fedBy; juce::TextEditor alias; LevelMeter meter { false };
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
    addAndMakeVisible (logo);
    addChildComponent (stagePanel);
    takeBox.setCompact (true);                                    // the take counter lives in the menu bar
    addAndMakeVisible (dock);
    dock.onChange = [this] { resized(); };
    menuStrip = std::make_unique<MenuBarStrip>();                  // the menu bar floats above every window (see MenuBarStrip)
    for (auto* b : menuItems()) menuStrip->addAndMakeVisible (b);
    menuStrip->addAndMakeVisible (takeBox);
    for (auto* f : { &flagPre, &flagCr, &flagPb }) menuStrip->addAndMakeVisible (f);
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
    sessionButton.onClick = [this] { app.setSessionMode (! app.sessionMode()); };
    if (auto* st = app.props.getUserSettings()) if (st->getBoolValue ("sessionMode", false)) app.engine.setSessionMode (true);
    mediaButton.setTooltip ("A folder tree of where every file of the project is saved");
    preampsButton.setTooltip ("Set up the Anubis / Hapi / MT48 devices whose preamps Fermata controls");
    bridgeButton.setTooltip ("Meter bridge: a meter for every driver input and output (key B)");
    mixersButton.setTooltip ("Mixers. Key M opens or closes the processing mixer.");

    projectName.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    addAndMakeVisible (projectName);
    statusLabel.setFont (juce::FontOptions (12.5f));
    addAndMakeVisible (statusLabel);
    inputsCaption.setText ("DRIVER INPUTS   MIC/LINE  48V  " + juce::String (juce::CharPointer_UTF8 ("\xc3\x98")) + "  LC  PAD  Z HI  BOOST   |   E / S / S-L / T = monitoring", juce::dontSendNotification);
    outputsCaption.setText ("DRIVER OUTPUTS", juce::dontSendNotification);
    for (auto* l : { &inputsCaption, &outputsCaption })
    {
        l->setColour (juce::Label::textColourId, theme::dimText);
        l->setFont (juce::FontOptions (12.5f, juce::Font::bold));
        addAndMakeVisible (l);
    }
    for (auto* v : { &inputsViewport, &outputsViewport }) { v->setScrollBarsShown (true, true); addAndMakeVisible (v); }
    inputsViewport.setViewedComponent (&inputsHolder, false);
    outputsViewport.setViewedComponent (&outputsHolder, false);
    ioDivider.onMove = [this] (juce::Point<int> p)
    {
        const int w = getWidth() - 20;
        ioSplit = juce::jlimit (0.2f, 0.85f, (float) (p.x - 10) / (float) juce::jmax (1, w));
        resized();
    };
    addAndMakeVisible (ioDivider);
    rebuildRows(); rebuildAuditionBar();
    app.project.structure.addChangeListener (this);
    app.project.addChangeListener (this);
    startTimerHz (30);
    app.peakListeners.add (this);
    if (! app.hasProject()) showStartPanel();                      // nothing can be done until the project's folder is chosen
    setSize (juce::jmax (1120, menuBarWidth() + 4), 840);
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

int MainComponent::menuBarWidth() const
{
    int w = 8 + kTakeBoxW + kFlagsW;
    for (auto* b : const_cast<MainComponent*> (this)->menuItems())
        w += MenuBarButton::widthFor (b == &sessionButton ? juce::String ("PRE-REC ON") : b->getButtonText());
    return w;
}

int MainComponent::menuBarBottomOnScreen() const { return localAreaToGlobal (juce::Rectangle<int> (0, 0, 1, menuBarH)).getBottom(); }

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    g.setColour (theme::panel); g.fillRect (0, 0, getWidth(), menuBarH);                 // the menu bar
    g.setColour (theme::border); g.fillRect (0, menuBarH - 1, getWidth(), 1);
    // a faint Fermata mark behind everything, in the middle
    const float w = (float) getWidth() * 0.45f;
    auto area = juce::Rectangle<float> (w, w * 0.5f).withCentre (getLocalBounds().toFloat().getCentre());
    drawFermataLogo (g, area, theme::text.withAlpha (0.07f), theme::accent.withAlpha (0.14f));
}

void MainComponent::resized()
{
    if (startPanel != nullptr) startPanel->setBounds (getLocalBounds());
    // ---- the menu bar: every entry on one line, along the top (it only wraps onto a second line if the window is made narrower than the entries)
    {
        int x = 4, y = 0, lineH = 30;
        menuBarH = lineH + 4;
        takeBox.setBounds (x + 2, y + 2, kTakeBoxW - 6, lineH);      // the take counter: a small box at the left end of the bar
        {   // the three status squares: right end of the first line
            int fx = getWidth() - kFlagsW;
            flagPre.setBounds (fx, y + 2, 72, lineH); fx += 72 + 4;
            flagCr.setBounds (fx, y + 2, 52, lineH);  fx += 52 + 4;
            flagPb.setBounds (fx, y + 2, 52, lineH);
        }
        x += kTakeBoxW;
        auto widthOf = [] (MenuBarButton* b) { return MenuBarButton::widthFor (b->getButtonText()); };
        const int rightW = widthOf (&mediaButton) + widthOf (&masteringButton);
        for (auto* b : menuItems())
        {
            if (b == &mediaButton || b == &masteringButton) continue;
            const int w = b == &sessionButton ? MenuBarButton::widthFor ("PRE-REC ON") : widthOf (b);
            if (x > 4 && x + w > getWidth() - 4 - (y == 0 ? kFlagsW + rightW : 0)) { x = 4; y += lineH; menuBarH += lineH; }
            b->setBounds (x, y + 2, w, lineH);
            x += w;
        }
        // Media, then Mastering, all the way to the right (just before the status squares)
        const int mw = widthOf (&mediaButton), gw = widthOf (&masteringButton);
        masteringButton.setBounds (getWidth() - kFlagsW - gw, 2, gw, lineH);
        mediaButton.setBounds (getWidth() - kFlagsW - gw - mw, 2, mw, lineH);
    }
    menuStrip->setSize (getWidth(), menuBarH);
    syncMenuStrip();
    auto r = getLocalBounds().withTrimmedTop (menuBarH).reduced (10);
    dock.setBounds (r.removeFromBottom (dock.wantedHeight()));
    // the stage speaker mixer lives top right, beside the logo, project line and audition buttons (it needs a CR input or a TB pair)
    const bool showStage = stagePanel.wanted();
    stagePanel.setVisible (showStage);
    const int topY = r.getY(), rightEdge = r.getRight();
    int leftLimit = rightEdge;
    if (showStage)
    {
        const int panelW = juce::jlimit (220, juce::jmax (220, rightEdge - (r.getX() + 560)), stagePanel.wantedWidth());
        stagePanel.setBounds (rightEdge - panelW, topY, panelW, StageSpeakerPanel::kHeight);       // (the height is set again below to what the left side leaves free)
        leftLimit = rightEdge - panelW - 10;
    }
    auto header = r.removeFromTop (60);
    logo.setBounds (header.removeFromLeft (200));
    r.removeFromTop (8);
    auto info = r.removeFromTop (22).withRight (leftLimit);  // one line: project, then the driver and recording state
    projectName.setBounds (info.removeFromLeft (juce::jmin (260, info.getWidth() / 3)));
    statusLabel.setBounds (info);
    if (! auditionButtons.isEmpty())
    {
        auto bar = r.removeFromTop (32).withRight (leftLimit);
        auditionCaption.setBounds (bar.removeFromLeft (150));
        for (auto* b : auditionButtons) b->setBounds (bar.removeFromLeft (juce::jmin (260, juce::jmax (150, bar.getWidth() / juce::jmax (1, auditionButtons.size())))).withSizeKeepingCentre (juce::jmin (260, juce::jmax (150, bar.getWidth() / juce::jmax (1, auditionButtons.size()))) - 4, grid::btnH));
    }
    if (showStage)      // the panel fills the space the left side leaves free, so the lists below never move down because of it (a little room is made only if the left side is very short)
    {
        const int minH = 100;
        if (r.getY() - topY < minH) r.removeFromTop (minH - (r.getY() - topY));
        stagePanel.setBounds (stagePanel.getBounds().withHeight (juce::jmin (StageSpeakerPanel::kHeight, r.getY() - topY)));
    }
    r.removeFromTop (6);
    r.removeFromTop (2);
    // driver inputs (with ARM, monitoring and the preamp controls) on the left, driver outputs on the right

    const int gap = 8;
    const int inW = juce::jlimit (150, juce::jmax (150, r.getWidth() - gap - 150), (int) ((r.getWidth() - gap) * ioSplit));
    auto left = r.removeFromLeft (inW);
    ioDivider.setBounds (r.removeFromLeft (gap));
    inputsCaption.setBounds (left.removeFromTop (22));
    inputsViewport.setBounds (left);
    ioDivider.setBounds (ioDivider.getBounds().withY (left.getY()).withHeight (2 * InputRow::kRowHeight));   // the handle sits on the first two rows, clear of the logo behind
    outputsCaption.setBounds (r.removeFromTop (22));
    outputsViewport.setBounds (r);

    auto fill = [] (juce::Viewport& vp, juce::Component& holder, auto& rowList, int rowH, int minW)
    {
        const int h = juce::jmax (1, (int) rowList.size()) * rowH;
        holder.setSize (juce::jmax (minW, vp.getMaximumVisibleWidth()), h);
        int y = 0;
        for (auto* row : rowList) { row->setBounds (0, y, holder.getWidth(), rowH); y += rowH; }
    };
    fill (inputsViewport, inputsHolder, inputRows, InputRow::kRowHeight, InputRow::kMinWidth);
    fill (outputsViewport, outputsHolder, outputRows, OutputRow::kRowHeight, OutputRow::kMinWidth);
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
    // The menu bar (with the Pre-Rec / C-R / P-B squares) is always there, in front of the other Fermata windows (not other programs), and goes with the main window when it is minimised.
    if (menuStrip != nullptr && getPeer() != nullptr && ! getPeer()->isMinimised() && ! menuStrip->isVisible()) menuStrip->setVisible (true);
    flagPre.set (app.sessionMode()); flagCr.set (app.crMicOpen()); flagPb.set (app.tbPlaybackOn());
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
    { int n = 1; const bool tr = app.takeBoxInfo (n); takeBox.set (tr, n); }
    projectName.setText (app.project.name, juce::dontSendNotification);
    projectName.setTooltip (app.project.projectFile.getFullPathName());

    juce::String s;
    if (auto* dev = app.devices.getCurrentAudioDevice())
    {
        s = dev->getTypeName() + ": " + dev->getName() + "  " + juce::String (dev->getCurrentSampleRate() / 1000.0, 1) + " kHz  "
            + juce::String (dev->getCurrentBitDepth()) + "-bit  buf " + juce::String (dev->getCurrentBufferSizeSamples())
            + "  in " + juce::String (dev->getActiveInputChannels().countNumberOfSetBits())
            + "/out " + juce::String (dev->getActiveOutputChannels().countNumberOfSetBits());
        if (dev->getTypeName() != "ASIO") s += "  (not ASIO)";
        if (rec && app.engine.overruns() > 0) s = "DISK OVERRUN - audio may have been lost!   " + s;
        if (auto* x = dynamic_cast<juce::AudioIODevice*> (dev); x != nullptr && dev->getXRunCount() > 0)
            s += "   xruns: " + juce::String (dev->getXRunCount());
    }
    else s = "No audio device - open Audio settings";
    if (rec && ! s.startsWith ("DISK")) s = "RECORDING  -  " + s;
    if (app.crMicOpen() || app.tbPlaybackOn())
        s = juce::String (app.crMicOpen() ? "CR MIC OPEN  " : "") + (app.tbPlaybackOn() ? "TB PLAYBACK ON  " : "") + "-  " + s;
    else if (! app.talkbackReady() && (app.crWanted || app.tbWanted))
        s = "Talkback key pressed, but no CR input / TB outputs are chosen (CR button on an input, TB button on an output)  -  " + s;
    const bool noticeOn = juce::Time::getMillisecondCounter() < app.noticeUntil;
    if (noticeOn) s = app.notice;
    statusLabel.setText (s, juce::dontSendNotification);
    if (noticeOn) statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff3ee0c0)); else
    statusLabel.setColour (juce::Label::textColourId, rec ? juce::Colour (0xffff5555) : s.startsWith ("DISK") || s.startsWith ("No audio") ? theme::warn : theme::dimText);
}

void MainComponent::showProjectMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "New project...");
    m.addItem (2, "Open project...");
    m.addItem (3, "Save");
    m.addItem (6, "Save as...");
    m.addItem (7, "Create copy of entire project...");
    m.addSeparator();
    m.addItem (4, "Show project folder");
    m.addItem (5, "Rename project...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&projectButton), [this] (int r)
    {
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

void MainComponent::showTakesMenu()
{
    juce::PopupMenu m;
    int i = 1;
    for (auto& w : app.project.takeWindows) m.addItem (i++, "Open: " + w->name);
    m.addSeparator();
    m.addItem (1000, "New take window...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&takesButton), [this] (int r)
    {
        if (r == 1000)
        {
            auto& w = app.project.addTakeWindow ("Piece " + juce::String ((int) app.project.takeWindows.size() + 1));
            app.recordTarget = w.id;
            if (app.showTakeWindow) app.showTakeWindow (w.id);
        }
        else if (r >= 1 && (size_t) r <= app.project.takeWindows.size() && app.showTakeWindow)
            app.showTakeWindow (app.project.takeWindows[(size_t) r - 1]->id);
    });
}

void MainComponent::showEditsMenu()
{
    juce::PopupMenu m;
    int i = 1;
    for (auto& e : app.project.edits) m.addItem (i++, "Open: " + e->name);
    m.addSeparator();
    m.addItem (1000, "New empty edit");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&editsButton), [this] (int r)
    {
        if (r == 1000)
        {
            auto& e = app.makeEdit ("Edit " + juce::String ((int) app.project.edits.size() + 1));
            if (app.showEdit) app.showEdit (e.id);
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
        if (r == 1000)
        {
            auto& mx = app.project.addMixer ("Cue mixer " + juce::String (app.project.cueEnd()));
            if (app.showMixer) app.showMixer (mx.id);
        }
        else if (r >= 1 && (size_t) r <= app.project.mixers.size() && app.showMixer)
            app.showMixer (app.project.mixers[(size_t) r - 1]->id);
    });
}
} // namespace td
