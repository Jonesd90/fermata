#include "TakeDisplay.h"
#include "TalkbackGlow.h"

namespace td
{
namespace
{
const juce::Colour kBack { 0xff0b0d10 }, kGreen { 0xff1f7a46 }, kRed { 0xffc0302b }, kDim { 0xff9ba5b4 }, kWhite { 0xfff4f6f8 };

juce::Font fontOf (float h, bool bold) { return juce::Font (bold ? juce::FontOptions (h, juce::Font::bold) : juce::FontOptions (h)); }

float widthOf (const juce::Font& f, const juce::String& s)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

/** One line of text, as large as fits in the box (never taller than its height). */
void drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, bool bold, juce::Colour c, juce::Justification j)
{
    if (text.isEmpty() || r.getWidth() < 4.0f || r.getHeight() < 4.0f) return;
    float h = r.getHeight();
    const float w = widthOf (fontOf (h, bold), text);
    if (w > r.getWidth() && w > 0.0f) h *= r.getWidth() / w;
    g.setColour (c); g.setFont (fontOf (h, bold));
    g.drawText (text, r, j, false);
}

/** A clock (digits and colons) drawn in fixed cells, so the numbers do not jiggle as the digits change. */
void drawClock (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, juce::Colour c, juce::Justification j)
{
    if (r.getWidth() < 4.0f || r.getHeight() < 4.0f) return;
    auto layoutWidth = [&] (float h)
    {
        const auto f = fontOf (h, true);
        float digit = 0.0f;
        for (int d = 0; d < 10; ++d) digit = juce::jmax (digit, widthOf (f, juce::String (d)));
        float total = 0.0f;
        for (auto ch : s) total += ch == ':' ? widthOf (f, ":") : ch == '-' ? widthOf (f, "-") : digit;
        return std::pair<float, float> (total, digit);
    };
    float h = r.getHeight();
    auto [total, digit] = layoutWidth (h);
    if (total > r.getWidth() && total > 0.0f) { h *= r.getWidth() / total; std::tie (total, digit) = layoutWidth (h); }
    const auto f = fontOf (h, true);
    float x = j.testFlags (juce::Justification::right) ? r.getRight() - total : j.testFlags (juce::Justification::horizontallyCentred) ? r.getCentreX() - total * 0.5f : r.getX();
    g.setColour (c); g.setFont (f);
    for (auto ch : s)
    {
        const float cw = ch == ':' ? widthOf (f, ":") : ch == '-' ? widthOf (f, "-") : digit;
        g.drawText (juce::String::charToString (ch), juce::Rectangle<float> (x, r.getY(), cw, r.getHeight()), juce::Justification::centred, false);
        x += cw;
    }
}
} // namespace

// ------------------------------------------------------------------ the picture

TakeDisplayView::TakeDisplayView (AppContext& a) : app (a)
{
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
    startTimerHz (10);
}

void TakeDisplayView::loadLogoIfNeeded()
{
    const auto& s = app.takeDisplay;
    const juce::String want = s.logoMode == 2 ? s.logoPath : juce::String();
    if (want == logoLoadedFor) return;
    logoLoadedFor = want;
    logo = want.isNotEmpty() ? juce::ImageFileFormat::loadFrom (juce::File (want)) : juce::Image();
}

void TakeDisplayView::paintOverChildren (juce::Graphics& g)
{
    if (showTalkback) paintTalkbackGlow (g, getLocalBounds().toFloat().reduced (0.5f), app.crMicOpen(), app.tbPlaybackOn());
}

void TakeDisplayView::paint (juce::Graphics& g)
{
    g.fillAll (kBack);
    const float W = (float) getWidth(), H = (float) getHeight();
    if (W < 20.0f || H < 20.0f) return;
    const auto& s = app.takeDisplay;
    loadLogoIfNeeded();

    // ---- which take
    int number = 1;
    const bool recording = app.takeBoxInfo (number);
    juce::String piece, takeName;
    {
        const TakeWindowDef* w = nullptr;
        const TakeGroup* grp = nullptr;
        if (recording) for (auto& t : app.project.takeWindows) if ((grp = t->findGroup (app.engine.currentTakeId())) != nullptr) { w = t.get(); break; }
        if (w == nullptr) { w = app.project.findTakeWindow (app.recordTarget); if (w == nullptr && ! app.project.takeWindows.empty()) w = app.project.takeWindows.front().get(); }
        if (w != nullptr) piece = w->name;
        if (grp != nullptr && grp->label.isNotEmpty()) takeName = grp->label;
    }

    const float m = H * 0.045f;
    auto area = juce::Rectangle<float> (m, m, W - 2 * m, H - 2 * m);

    // ---- top band: logo on the left, the project line (artist / album) beside it
    auto top = area.removeFromTop (H * 0.15f);
    if (s.logoMode != 1)
    {
        auto lr = top.removeFromLeft (top.getHeight() * (s.logoMode == 0 ? 1.7f : 1.4f));
        if (s.logoMode == 0) drawFermataLogo (g, lr, kWhite, juce::Colour (0xff2a8fb0));
        else if (logo.isValid()) g.drawImage (logo, lr, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
        top.removeFromLeft (m * 0.8f);
    }
    drawFitted (g, s.line, top, true, kWhite, juce::Justification::centredLeft);

    // ---- bottom band: time of day and time left
    auto bottom = area.removeFromBottom (H * 0.20f);
    area.removeFromBottom (m * 0.6f);
    const auto now = juce::Time::currentTimeMillis();
    juce::int64 left = 0;
    const bool timer = app.sessionSecondsLeft (left);
    auto clockBox = [&] (juce::Rectangle<float> r, const juce::String& caption, const juce::String& value, juce::Colour c, juce::Justification j)
    {
        drawFitted (g, caption, r.removeFromTop (r.getHeight() * 0.22f), false, kDim, j);
        drawClock (g, value, r, c, j);
    };
    if (timer)
    {
        auto l = bottom.removeFromLeft (bottom.getWidth() * 0.46f);
        bottom.removeFromLeft (bottom.getWidth() * 0.08f);
        clockBox (l, "TIME OF DAY", sessionclock::timeOfDay (now), kWhite, juce::Justification::centredLeft);
        clockBox (bottom, left < 0 ? "OVER TIME" : "SESSION TIME LEFT", sessionclock::formatClock (left), left < 0 ? juce::Colour (0xffff5a52) : kWhite, juce::Justification::centredRight);
    }
    else clockBox (bottom, "TIME OF DAY", sessionclock::timeOfDay (now), kWhite, juce::Justification::centred);

    // ---- middle: the piece, and the take number in a big green (next) or red (this) panel
    auto mid = area;
    auto title = mid.removeFromTop (mid.getHeight() * 0.16f);
    drawFitted (g, piece + (takeName.isNotEmpty() ? "   -   " + takeName : juce::String()), title, true, kWhite, juce::Justification::centred);
    mid.removeFromTop (mid.getHeight() * 0.03f);
    auto panel = mid.withSizeKeepingCentre (juce::jmin (mid.getWidth(), mid.getHeight() * 1.9f), mid.getHeight());
    g.setColour (recording ? kRed : kGreen);
    g.fillRoundedRectangle (panel, panel.getHeight() * 0.06f);
    auto inner = panel.reduced (panel.getHeight() * 0.05f);
    drawFitted (g, recording ? "THIS TAKE" : "NEXT TAKE", inner.removeFromTop (inner.getHeight() * 0.18f), true, juce::Colours::white.withAlpha (0.92f), juce::Justification::centred);
    drawFitted (g, juce::String (number), inner, true, juce::Colours::white, juce::Justification::centred);
}

// ------------------------------------------------------------------ one screen

TakeDisplayWindow::TakeDisplayWindow (AppContext& a, TakeDisplayHub& h, int displayIndex, juce::Rectangle<int> area)
    : hub (h), index (displayIndex), view (a)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::NoCursor);
    addAndMakeVisible (view);
    addToDesktop (0);                                    // no title bar, no border, no task bar button
    setBounds (area);
    setAlwaysOnTop (true);
    setVisible (true);
}

TakeDisplayWindow::~TakeDisplayWindow() {}

bool TakeDisplayWindow::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { closeSelf(); return true; }
    return false;
}

void TakeDisplayWindow::mouseDoubleClick (const juce::MouseEvent&) { closeSelf(); }

void TakeDisplayWindow::closeSelf()
{
    const int i = index; auto* h = &hub;
    juce::MessageManager::callAsync ([h, i] { h->setOpen (i, false); });
}

void TakeDisplayHub::setOpen (int idx, bool open)
{
    if (! open) { windows.erase (idx); changes.sendChangeMessage(); app.applyTalkback(); return; }
    const auto& ds = juce::Desktop::getInstance().getDisplays().displays;
    if (! juce::isPositiveAndBelow (idx, ds.size()) || windows.count (idx) > 0) return;
    windows[idx] = std::make_unique<TakeDisplayWindow> (app, *this, idx, ds.getReference (idx).totalArea);
    changes.sendChangeMessage();
    app.applyTalkback();
}

// ------------------------------------------------------------------ the control window

TakeDisplayControl::TakeDisplayControl (AppContext& a) : app (a), preview (a)
{
    preview.showTalkback = false;
    auto caption = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); l.setFont (juce::FontOptions (13.0f)); addAndMakeVisible (l); };
    caption (lineCaption, "Project line");
    caption (logoCaption, "Logo");
    caption (timerCaption, "Session timer");
    caption (screensCaption, "Show on screen");
    caption (previewCaption, "What the screens show (preview)");
    screensCaption.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    previewCaption.setColour (juce::Label::textColourId, theme::dimText);
    logoPathLabel.setColour (juce::Label::textColourId, theme::dimText); logoPathLabel.setFont (juce::FontOptions (12.0f)); addAndMakeVisible (logoPathLabel);
    timerStatus.setFont (juce::FontOptions (13.0f)); addAndMakeVisible (timerStatus);
    hint.setText ("On a screen: double-click or press Esc to close it. A screen can also be switched off here.", juce::dontSendNotification);
    hint.setColour (juce::Label::textColourId, theme::dimText); hint.setFont (juce::FontOptions (12.0f)); addAndMakeVisible (hint);

    lineEdit.setText (app.takeDisplay.line, juce::dontSendNotification);
    lineEdit.setTextToShowWhenEmpty ("an artist name or an album title", theme::dimText);
    lineEdit.setFont (juce::FontOptions (15.0f));
    lineEdit.onTextChange = [this] { app.takeDisplay.line = lineEdit.getText(); app.saveTakeDisplay(); };
    addAndMakeVisible (lineEdit);

    logoBox.addItem ("The Fermata mark", 1); logoBox.addItem ("No logo", 2); logoBox.addItem ("My own image", 3);
    logoBox.setSelectedId (app.takeDisplay.logoMode + 1, juce::dontSendNotification);
    logoBox.onChange = [this]
    {
        app.takeDisplay.logoMode = logoBox.getSelectedId() - 1;
        if (app.takeDisplay.logoMode == 2 && app.takeDisplay.logoPath.isEmpty()) chooseLogo.triggerClick();
        app.saveTakeDisplay(); timerCallback();
    };
    addAndMakeVisible (logoBox);
    chooseLogo.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose your logo (PNG, JPG or GIF)", juce::File (app.takeDisplay.logoPath).getParentDirectory(), "*.png;*.jpg;*.jpeg;*.gif");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile()) { app.takeDisplay.logoPath = f.getFullPathName(); app.takeDisplay.logoMode = 2; logoBox.setSelectedId (3, juce::dontSendNotification); app.saveTakeDisplay(); timerCallback(); }
        });
    };
    addAndMakeVisible (chooseLogo);

    timerMode.addItem ("Ends at (time of day)", 1); timerMode.addItem ("Length of the session", 2);
    timerMode.setSelectedId (app.takeDisplay.timerMode + 1, juce::dontSendNotification);
    timerMode.onChange = [this]
    {
        app.takeDisplay.timerMode = timerMode.getSelectedId() - 1;
        timerText.setText (app.takeDisplay.timerMode == 0 ? app.takeDisplay.endAtText : app.takeDisplay.lengthText, juce::dontSendNotification);
        app.saveTakeDisplay();
    };
    addAndMakeVisible (timerMode);
    timerText.setText (app.takeDisplay.timerMode == 0 ? app.takeDisplay.endAtText : app.takeDisplay.lengthText, juce::dontSendNotification);
    timerText.setInputRestrictions (5, "0123456789:");
    timerText.setJustification (juce::Justification::centred);
    timerText.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    timerText.onReturnKey = [this] { applyTimer(); };
    addAndMakeVisible (timerText);
    startTimer.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    startTimer.onClick = [this] { applyTimer(); };
    clearTimer.onClick = [this] { app.clearSessionTimer(); refreshTimerStatus(); };
    addAndMakeVisible (startTimer); addAndMakeVisible (clearTimer);
    addAndMakeVisible (preview);

    if (app.takeDisplayHub == nullptr) app.takeDisplayHub = std::make_unique<TakeDisplayHub> (app);
    app.takeDisplayHub->changes.addChangeListener (this);
    rebuildScreenButtons();
    refreshTimerStatus();
    startTimerHz (2);
    setSize (760, 640);
}

TakeDisplayControl::~TakeDisplayControl()
{
    if (app.takeDisplayHub != nullptr) app.takeDisplayHub->changes.removeChangeListener (this);
}

void TakeDisplayControl::applyTimer()
{
    const auto err = app.takeDisplay.timerMode == 0 ? app.setSessionEndAt (timerText.getText()) : app.setSessionLength (timerText.getText());
    if (err.isNotEmpty()) { timerStatus.setColour (juce::Label::textColourId, theme::warn); timerStatus.setText (err, juce::dontSendNotification); return; }
    timerText.setText (app.takeDisplay.timerMode == 0 ? app.takeDisplay.endAtText : app.takeDisplay.lengthText, juce::dontSendNotification);
    refreshTimerStatus();
}

void TakeDisplayControl::refreshTimerStatus()
{
    if (timerStatus.findColour (juce::Label::textColourId) == theme::warn && app.takeDisplay.endMs <= 0) return;      // keep an error message showing until something is set
    timerStatus.setColour (juce::Label::textColourId, theme::dimText);
    juce::int64 left = 0;
    if (app.sessionSecondsLeft (left))
        timerStatus.setText ("Ends at " + juce::Time (app.takeDisplay.endMs).formatted ("%H:%M") + "    -    " + (left < 0 ? "over time by " : "time left ") + sessionclock::formatClock (left < 0 ? -left : left), juce::dontSendNotification);
    else
        timerStatus.setText ("No countdown is showing.", juce::dontSendNotification);
}

void TakeDisplayControl::timerCallback()
{
    refreshTimerStatus();
    if ((int) juce::Desktop::getInstance().getDisplays().displays.size() != shownDisplays) rebuildScreenButtons();
    logoPathLabel.setText (app.takeDisplay.logoMode == 2 ? juce::File (app.takeDisplay.logoPath).getFileName() : juce::String(), juce::dontSendNotification);
}

void TakeDisplayControl::rebuildScreenButtons()
{
    screenButtons.clear();
    auto& ds = juce::Desktop::getInstance().getDisplays().displays;
    shownDisplays = (int) ds.size();
    for (int i = 0; i < ds.size(); ++i)
    {
        auto& d = ds.getReference (i);
        auto* b = screenButtons.add (new juce::TextButton ("Screen " + juce::String (i + 1) + (d.isMain ? " (main)" : "") + "  " + juce::String (d.totalArea.getWidth()) + " x " + juce::String (d.totalArea.getHeight())));
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff1f7a46));
        b->onClick = [this, i, b] { if (app.takeDisplayHub != nullptr) app.takeDisplayHub->setOpen (i, b->getToggleState()); };
        addAndMakeVisible (b);
    }
    syncScreens();
    resized();
}

void TakeDisplayControl::syncScreens()
{
    for (int i = 0; i < screenButtons.size(); ++i)
        screenButtons[i]->setToggleState (app.takeDisplayHub != nullptr && app.takeDisplayHub->isOpen (i), juce::dontSendNotification);
}

void TakeDisplayControl::paint (juce::Graphics& g) { g.fillAll (theme::window); }

void TakeDisplayControl::resized()
{
    auto r = getLocalBounds().reduced (14);
    const int labelW = 110;
    auto row = [&] (int h = 30) { auto x = r.removeFromTop (h); r.removeFromTop (6); return x; };

    auto a = row(); lineCaption.setBounds (a.removeFromLeft (labelW)); lineEdit.setBounds (a);

    auto b = row (); logoCaption.setBounds (b.removeFromLeft (labelW));
    logoBox.setBounds (b.removeFromLeft (190)); b.removeFromLeft (8);
    chooseLogo.setBounds (b.removeFromLeft (grid::btnW + 14).withSizeKeepingCentre (grid::btnW + 14, grid::btnH)); b.removeFromLeft (8);
    logoPathLabel.setBounds (b);

    auto c = row (); timerCaption.setBounds (c.removeFromLeft (labelW));
    timerMode.setBounds (c.removeFromLeft (190)); c.removeFromLeft (8);
    timerText.setBounds (c.removeFromLeft (80)); c.removeFromLeft (8);
    startTimer.setBounds (c.removeFromLeft (grid::btnW).withSizeKeepingCentre (grid::btnW, grid::btnH)); c.removeFromLeft (grid::gap);
    clearTimer.setBounds (c.removeFromLeft (grid::btnW).withSizeKeepingCentre (grid::btnW, grid::btnH));
    auto st = row (22); st.removeFromLeft (labelW); timerStatus.setBounds (st);

    r.removeFromTop (4);
    screensCaption.setBounds (row (22).removeFromLeft (labelW + 60));
    {
        auto line = r.removeFromTop (grid::btnH + 4);
        int x = line.getX();
        for (auto* sb : screenButtons)
        {
            const int w = 230;
            if (x + w > line.getRight()) { line = r.removeFromTop (grid::btnH + 4); x = line.getX(); }
            sb->setBounds (x, line.getY(), w, grid::btnH);
            x += w + grid::gap;
        }
    }
    r.removeFromTop (4);
    { auto pc = r.removeFromTop (22); hint.placeRightOf (pc); previewCaption.setBounds (pc); }
    auto box = r.reduced (0, 2);
    const int pw = juce::jmin (box.getWidth(), (int) (box.getHeight() * 16.0f / 9.0f));
    preview.setBounds (box.withSizeKeepingCentre (pw, (int) (pw * 9.0f / 16.0f)));
}
} // namespace td
