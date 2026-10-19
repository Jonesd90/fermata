#pragma once
#include "AppContext.h"
#include "StageMixer.h"
#include "DeskLook.h"

namespace td
{
class InputRow;
class OutputRow;

/** The Fermata mark and name, in one line at the left of the main window's top row. */
class LogoBadge : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        drawFermataLogo (g, b.removeFromLeft (b.getHeight() * 1.3f).reduced (1.0f, b.getHeight() * 0.12f), desk::pal().cream, desk::pal().cyan);
        g.setColour (desk::pal().cream);
        g.setFont (juce::Font (juce::FontOptions ("Georgia", juce::jmin (24.0f, b.getHeight() * 0.72f), juce::Font::bold)));
        g.drawText ("fermata", b.toNearestInt().withTrimmedLeft (5), juce::Justification::centredLeft, false);
    }
};

/** A thin bar you drag to resize the two areas next to it. */
class Divider : public juce::Component
{
public:
    explicit Divider (bool isVertical) : vertical (isVertical) { setMouseCursor (vertical ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor); }
    std::function<void (juce::Point<int>)> onMove;      // pointer position in the parent's coordinates
    void mouseDrag (const juce::MouseEvent& e) override { if (onMove && getParentComponent() != nullptr) onMove (e.getEventRelativeTo (getParentComponent()).getPosition()); }
    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::white.withAlpha (isMouseOver() ? 0.45f : 0.22f));
        auto b = getLocalBounds().toFloat();
        if (vertical) g.fillRoundedRectangle (b.withSizeKeepingCentre (3.0f, juce::jmin (60.0f, b.getHeight())), 1.5f);
        else          g.fillRoundedRectangle (b.withSizeKeepingCentre (juce::jmin (60.0f, b.getWidth()), 3.0f), 1.5f);
    }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
private:
    bool vertical;
};

/** Minimised windows: each one is a title bar along the bottom of the main window; a click brings the window back. */
class DockBar : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    DockBar() { DockHub::get().changes.addChangeListener (this); startTimerHz (4); }
    ~DockBar() override { DockHub::get().changes.removeChangeListener (this); }
    std::function<void()> onChange;
    int wantedHeight() const { return wins.empty() ? 0 : 30; }
    void resized() override
    {
        auto r = getLocalBounds().reduced (0, 2);
        const int w = juce::jlimit (120, 260, wins.empty() ? 200 : r.getWidth() / (int) wins.size() - 4);
        for (auto* b : buttons) b->setBounds (r.removeFromLeft (w).reduced (1, 0)), r.removeFromLeft (3);
    }
    void paint (juce::Graphics&) override {}                         // it sits on the wooden rail
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuild(); }
    void timerCallback() override { for (size_t i = 0; i < wins.size() && (int) i < buttons.size(); ++i) if (wins[i] != nullptr) buttons[(int) i]->setButtonText (wins[i]->getName()); }
    void rebuild()
    {
        buttons.clear();
        wins.clear();
        for (auto* w : DockHub::get().windows())
        {
            wins.push_back (juce::Component::SafePointer<juce::DocumentWindow> (w));
            auto* b = buttons.add (new juce::TextButton (w->getName()));
            b->setTooltip ("Minimised: click to bring this window back");
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xffefe4c8));
            b->setColour (juce::TextButton::textColourOffId, juce::Colour (0xff2a2418));
            b->setWantsKeyboardFocus (false);
            juce::Component::SafePointer<juce::DocumentWindow> sp (w);
            b->onClick = [sp] { if (sp != nullptr) DockHub::get().show (sp.getComponent()); };
            addAndMakeVisible (b);
        }
        resized();
        if (onChange) onChange();
    }
    juce::OwnedArray<juce::TextButton> buttons;
    std::vector<juce::Component::SafePointer<juce::DocumentWindow>> wins;
};

/** How big the writing in the menu bar is: the bar always stays on one line, so on a narrow screen the writing and the space round it get smaller. */
struct MenuStyle { float font = 14.0f; int pad = 14; };
inline MenuStyle& menuStyle() { static MenuStyle s; return s; }
inline int menuTextW (const juce::String& t, float size, bool bold)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain), t, 0.0f, 0.0f);
    return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
}

/** The menu bar of the main window lives in its own borderless window that always stays on top of the other Fermata windows (not other programs), laid exactly over the
    top strip of the main window and following it when it moves. Only this strip floats: the rest of the main window behaves as a normal window. */
class MenuBarStrip : public juce::Component
{
public:
    MenuBarStrip()
    {
        setOpaque (true);
        addToDesktop (juce::ComponentPeer::windowIsTemporary);          // no title bar, no taskbar button. It is OWNED by the main window (see MainComponent::syncMenuStrip): above it and the other Fermata windows, but not above other programs
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (desk::pal().menu);
        g.setColour (desk::pal().menuEdge); g.fillRect (0, getHeight() - 1, getWidth(), 1);
        g.setColour (desk::pal().menuEdge.brighter (0.35f));
        for (int x : separators) g.fillRect (x, 7, 1, getHeight() - 14);       // thin lines between the entries
    }
    std::vector<int> separators;
};

/** The main window: transport, the Driver Inputs (live meters, name, preamp controls), ARM and monitoring for each input, and the menus that open the other windows. */
/** One entry of the menu bar along the top of the main window: flat text that lights up under the mouse (and stays lit when it is a switch that is on). */
class MenuBarButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f, 4.0f);
        if (getToggleState())  { g.setColour (findColour (buttonOnColourId)); g.fillRoundedRectangle (b, 3.0f); }
        else if (down)         { g.setColour (theme::accent.withAlpha (0.55f)); g.fillRoundedRectangle (b, 3.0f); }
        else if (over)         { g.setColour (desk::pal().menuEdge.brighter (0.25f)); g.fillRoundedRectangle (b, 3.0f); }
        g.setColour (juce::Colour (0xffeef2f6));
        g.setFont (juce::FontOptions (menuStyle().font, getToggleState() ? juce::Font::bold : juce::Font::plain));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }
    /** The width this entry needs for the given text (at the menu bar's present size). */
    static int widthFor (const juce::String& text) { return menuTextW (text, menuStyle().font, true) + 2 * menuStyle().pad; }
};

/** One of the status squares at the right end of the menu bar: a small labelled box that is dark when its state is off and lit in its own colour when on. */
class StatusFlag : public juce::Component, public juce::SettableTooltipClient
{
public:
    StatusFlag (juce::String text, juce::Colour colour, juce::String tip) : label (std::move (text)), lit (colour) { setTooltip (tip); setInterceptsMouseClicks (true, false); }
    void set (bool on) { if (on != isOn) { isOn = on; repaint(); } }
    std::function<void()> onClick;                                  // the square is also a button
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    juce::MouseCursor getMouseCursor() override { return onClick ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor; }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f, 4.0f);
        if (isOn)
        {
            g.setColour (lit); g.fillRoundedRectangle (b, 4.0f);
            g.setColour (lit.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white);
        }
        else
        {
            g.setColour (theme::field); g.fillRoundedRectangle (b, 4.0f);
            g.setColour (theme::border); g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (theme::dimText.withAlpha (0.6f));
        }
        g.setFont (juce::FontOptions (menuStyle().font - 1.0f, juce::Font::bold));
        g.drawText (label, getLocalBounds(), juce::Justification::centred, false);
    }
    int wantedWidth() const { return label.length() <= 1 ? 0 : menuTextW (label, menuStyle().font - 1.0f, true) + juce::jmax (10, menuStyle().pad + 4); }
private:
    juce::String label; juce::Colour lit; bool isOn = false;
};

/** The small light just after Take Window, Edit Window and Mixer in the menu bar: green = a window of that kind is the one you are working in, orange = one is
    open, red = open but minimised, unlit = none is open. Click it to bring back the one you used last. */
class WindowLamp : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit WindowLamp (const juce::String& tip) { setTooltip (tip); }
    void set (int s) { if (s != state) { state = s; repaint(); } }        // 0 none open, 1 open, 2 in use, 3 minimised
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    juce::MouseCursor getMouseCursor() override { return juce::MouseCursor::PointingHandCursor; }
    void paint (juce::Graphics& g) override
    {
        const float d = 9.0f;
        auto c = juce::Rectangle<float> (d, d).withCentre (getLocalBounds().toFloat().getCentre());
        const juce::Colour col (state == 2 ? 0xff35e07a : state == 1 ? 0xffff9a2e : state == 3 ? 0xffe5484d : 0xff2a3644);
        if (state != 0) { g.setColour (col.withAlpha (0.3f)); g.fillEllipse (c.expanded (2.5f)); }
        g.setColour (col); g.fillEllipse (c);
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.drawEllipse (c, 1.0f);
    }
private:
    int state = 0;
};

/** One of the small black displays in the main window's top row (project, rate, depth, disk free), with its caption to the left. */
class InfoBox : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit InfoBox (juce::String cap = {}) : caption (std::move (cap)) { setInterceptsMouseClicks (false, false); }
    void setText (const juce::String& t, juce::Colour c = {}) { if (t != text || c != colour) { text = t; colour = c; repaint(); } }
    void setFontHeight (float h) { fontH = h; repaint(); }
    int wantedWidth() const { return captionWidth() + textWidth() + 16; }
    void paint (juce::Graphics& g) override
    {
        const auto& p = desk::pal();
        auto r = getLocalBounds();
        if (caption.isNotEmpty())
        {
            g.setColour (p.cream); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (caption, r.removeFromLeft (captionWidth()), juce::Justification::centredLeft, false);
        }
        g.setColour (p.disp); g.fillRect (r);
        g.setColour (p.dispEdge); g.drawRect (r, 1);
        g.setColour (colour.isTransparent() ? p.cyan : colour);
        g.setFont (font());
        g.drawFittedText (text, r.reduced (8, 0), juce::Justification::centredLeft, 1, 0.75f);
    }
private:
    juce::Font font() const { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), fontH, juce::Font::bold)); }
    int captionWidth() const { return caption.isEmpty() ? 0 : juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)), caption)) + 8; }
    int textWidth() const { return juce::roundToInt (juce::GlyphArrangement::getStringWidth (font(), text)); }
    juce::String caption, text;
    juce::Colour colour;
    float fontH = 13.0f;
};

/** A lamp with a label in the menu bar: red when nothing is happening, green when LTC is arriving / being sent, with the frame rate beside it. Click for its menu. */
class LtcLamp : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LtcLamp (juce::String name, juce::String tip) : title (std::move (name)), baseTip (std::move (tip)) { setTooltip (baseTip); setInterceptsMouseClicks (true, false); }
    void set (bool on, const juce::String& rateText)
    {
        if (on != isOn || rateText != rate)
        {
            isOn = on; rate = rateText; repaint();
            setTooltip (baseTip + "\n\nNow: " + (isOn ? rate : juce::String ("no signal")));      // (the lamp itself only shows the light and its name)
        }
    }
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    juce::MouseCursor getMouseCursor() override { return juce::MouseCursor::PointingHandCursor; }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f, 4.0f);
        g.setColour (theme::field); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (theme::border); g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
        const auto lamp = isOn ? juce::Colour (0xff2ecc40) : juce::Colour (0xffe53935);
        const float d = 11.0f;
        auto c = juce::Rectangle<float> (d, d).withCentre ({ b.getX() + 12.0f, b.getCentreY() });
        if (isOn) { g.setColour (lamp.withAlpha (0.30f)); g.fillEllipse (c.expanded (3.0f)); }
        g.setColour (lamp); g.fillEllipse (c);
        g.setColour (juce::Colours::black.withAlpha (0.45f)); g.drawEllipse (c, 1.0f);
        const float fs = menuStyle().font - 1.5f;
        g.setColour (juce::Colour (0xffeef2f6)); g.setFont (juce::FontOptions (fs, juce::Font::bold));
        g.drawText (title, getLocalBounds().withTrimmedLeft (24).reduced (2, 0), juce::Justification::centredLeft, false);
    }
    int wantedWidth() const { return 24 + menuTextW (title, menuStyle().font - 1.5f, true) + 10; }
private:
    juce::String title, baseTip, rate; bool isOn = false;
};

class MainComponent : public juce::Component, private juce::ChangeListener, private juce::Timer, private PeakListener, private juce::ComponentListener
{
public:
    explicit MainComponent (AppContext&);
    ~MainComponent() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    /** Width the menu bar needs to show every entry on one line. */
    int menuBarWidth() const;
    /** The height the main window needs to show everything at full size. */
    int fullHeight() const;
    /** How wide the driver line (status) is with this text: DRIVER INPUTS sits just after it. */
    int statusWidthFor (const juce::String& t) const { return juce::jlimit (60, 520, juce::GlyphArrangement::getStringWidthInt (statusLabel.getFont(), t) + 10); }
    int statusWidth() const { return statusWidthFor (statusLabel.getText()); }
    int lastStatusTextW = -1;
    /** Where the bottom edge of the menu bar is on the screen: the Display Organiser keeps everything else below it. */
    int menuBarBottomOnScreen() const;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void parentHierarchyChanged() override;
    void componentMovedOrResized (juce::Component&, bool, bool) override { syncMenuStrip(); }
    void syncMenuStrip();                       // lays the floating menu bar exactly over the top strip of this window
    juce::Component::SafePointer<juce::Component> watchedTop;
    void timerCallback() override;
    void peaksUpdated() override;
    void rebuildRows();
    void showProjectMenu();
    void showTakesMenu();
    void importWindowFileDialog (bool isEdit);
    void relinkAudioDialog (bool isEdit, const juce::Uuid& openAfter);
    void showMixersMenu();
    void rebuildAuditionBar();
    void showEditsMenu();
    void newProjectDialog();
    void closeProjectDialog();
    void openProjectDialog();
    void showStartPanel();
    std::unique_ptr<juce::Component> startPanel;           // the "where do you want to save your project?" screen, until a project is chosen
    ClickAwayEndsTyping clickAway { *this };               // a click outside an input / output name box ends the typing in it

    AppContext& app;
    MenuBarButton projectButton { "Project" }, audioButton { "Audio settings" }, preampsButton { "Preamps" }, designButton { "Project Designer" },
                  takesButton { "Take Window" }, editsButton { "Edit Window" }, mixersButton { "Mixer" }, bridgeButton { "Meter bridge" }, mediaButton { "Media" }, masteringButton { "Mastering" },
                  sessionButton { "Pre-Rec" }, takeDisplayButton { "Take Display" }, organiserButton { "Display Organiser" };
    int menuBarH = 34;
    std::unique_ptr<MenuBarStrip> menuStrip;                  // declared after the buttons: it goes first when this window is destroyed
    std::vector<MenuBarButton*> menuItems() { return { &projectButton, &audioButton, &preampsButton, &designButton, &takesButton, &editsButton, &mixersButton, &bridgeButton, &takeDisplayButton, &organiserButton, &mediaButton, &masteringButton }; }
    LogoBadge logo;
    static constexpr int kTakeBoxW = 36;                       // the take counter at the left of the menu bar
    void layoutMenuBar();                                      // everything in the bar on one line, as large as the width allows
    NextTakeBox takeBox;
    // always-visible status squares at the right end of the menu bar (they replaced the coloured borders)
    LtcLamp ltcInLamp { "LTC IN", "LTC in: red = no timecode arriving, green = LTC is arriving (with its frame rate). Click to choose the audio input it comes in on (a Hapi TC input routed in Aneman arrives as an ordinary input channel). It is recorded with every take on the track named LTC." },
            ltcOutLamp { "LTC OUT", "LTC out: green = Fermata is sending LTC. Click to choose the output channel (route it in Aneman to the Hapi TC output), the frame rate and the start time." };
    void showLtcInMenu();
    void showLtcOutMenu();
    StatusFlag flagPre { "Pre-Rec", juce::Colour (0xffe53935), "Pre-Rec (red): session mode is ON - the inputs are kept for the last 6 seconds, so a take starts 6 seconds before you press Record. Click it to switch Pre-Rec on or off." },
               flagCr  { "Talkback", juce::Colour (0xffa83cff), "Talkback (purple): the talkback mic is OPEN - you are talking to the talkback speakers. Click it (or press numpad +) to open or close the mic (it stays as you leave it)." },
               flagPb  { "Playback", juce::Colour (0xffffd21f), "Playback (yellow): recorded material only is being played to the talkback output. Click it (or press numpad -) to switch it on or off." };
    // little squares to the right of Take Window / Edit Window / Mixer: bring back the one you used last (lit while that kind of window is the one you are working in)
    WindowLamp flagT { "Take windows - green: the one you are working in, orange: open, red: minimised, unlit: none open. Click to bring back the take window you used last (it opens if it was closed)." },
               flagE { "Edit windows - green: the one you are working in, orange: open, red: minimised, unlit: none open. Click to bring back the edit window you used last (it opens if it was closed)." },
               flagM { "Mixers - green: the one you are working in, orange: open, red: minimised, unlit: none open. Click to bring back the mixer you used last (it opens if it was closed)." };
    WindowLamp* flagFor (const juce::Component* b) { return b == &takesButton ? &flagT : b == &editsButton ? &flagE : b == &mixersButton ? &flagM : nullptr; }
    InfoBox projectBox, rateBox { "RATE" }, depthBox { "DEPTH" }, diskBox { "DISK FREE" };
    int headerLineY = -1;                              // the line under the top row
    juce::uint32 lastDiskCheck = 0;
    int lastDiskW = 0;
    juce::String diskAlarm;                    // while recording: "DISK NEARLY FULL ..." when the drive has only minutes left
    bool problemNoticed = false;               // the notice about a file that stopped taking data was shown for this recording
    DockBar dock;
    juce::Label projectName, timeLabel, recStatus, statusLabel, inputsCaption, outputsCaption;
    // Driver inputs (left) and outputs (right) are always shown; the divider between them can be dragged
    juce::Viewport inputsViewport, outputsViewport;
    juce::Component inputsHolder, outputsHolder;
    juce::OwnedArray<InputRow> inputRows;
    juce::OwnedArray<OutputRow> outputRows;
    desk::PictureBadge auditionIcon { desk::Ic::Phones };
    static constexpr int kCheekW = wood::kCheekW, kRailFull = 44, kRailCompact = 30;   // the wooden side panels and the wooden rail along the bottom
    int railH = kRailFull;                             // thinner when the screen is short (see resized)
    float squeeze = 0.0f;                              // how much the input strips are shrunk to fit the height (0 = not at all)
    int laidOutStageW = -2;                            // the stage speaker panel's width at the last layout (-1 = not shown): a change means a new layout
    std::vector<int> headerSeps;                       // x of the thin lines between the parts of the top row
    juce::Rectangle<int> headerLine;                   // the one line of the top row
    void pickColour (int index);
    void chooseBackupFolder();                         // Project > Second recording drive                       // the colour of the desk (chosen in the Project menu)
    StageSpeakerPanel stagePanel { app };
    juce::OwnedArray<juce::TextButton> auditionButtons;
    juce::Label auditionCaption;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace td
