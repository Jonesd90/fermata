#pragma once
#include "AppContext.h"

namespace td
{
class InputRow;
class OutputRow;

/** The Fermata mark and name, top left of the main window. */
class LogoBadge : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        drawFermataLogo (g, b.removeFromLeft (64.0f).reduced (2.0f, 4.0f), theme::text, theme::accent);
        g.setColour (theme::text);
        g.setFont (juce::Font (juce::FontOptions ("Georgia", 32.0f, juce::Font::bold)));
        g.drawText ("fermata", b.toNearestInt().withTrimmedLeft (4), juce::Justification::centredLeft, false);
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
    void paint (juce::Graphics& g) override { g.setColour (theme::panel); g.fillRect (getLocalBounds()); }
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
            b->setColour (juce::TextButton::buttonColourId, theme::row);
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
        g.fillAll (theme::panel);
        g.setColour (theme::border); g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }
};

/** The main window: transport, the Driver Inputs (live meters, name, preamp controls), ARM and monitoring for each input, and the menus that open the other windows. */
/** One entry of the menu bar along the top of the main window: flat text that lights up under the mouse (and stays lit when it is a switch that is on). */
class MenuBarButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f, 3.0f);
        if (getToggleState())  { g.setColour (findColour (buttonOnColourId)); g.fillRoundedRectangle (b, 3.0f); }
        else if (down)         { g.setColour (theme::accent.withAlpha (0.55f)); g.fillRoundedRectangle (b, 3.0f); }
        else if (over)         { g.setColour (theme::button); g.fillRoundedRectangle (b, 3.0f); }
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (14.0f, getToggleState() ? juce::Font::bold : juce::Font::plain));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }
    /** The width this entry needs for the given text. */
    static int widthFor (const juce::String& text)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (juce::FontOptions (14.0f, juce::Font::bold), text, 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 28;
    }
};

/** One of the status squares at the right end of the menu bar: a small labelled box that is dark when its state is off and lit in its own colour when on. */
class StatusFlag : public juce::Component, public juce::SettableTooltipClient
{
public:
    StatusFlag (juce::String text, juce::Colour colour, juce::String tip) : label (std::move (text)), lit (colour) { setTooltip (tip); setInterceptsMouseClicks (true, false); }
    void set (bool on) { if (on != isOn) { isOn = on; repaint(); } }
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
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (label, getLocalBounds(), juce::Justification::centred, false);
    }
private:
    juce::String label; juce::Colour lit; bool isOn = false;
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
    void showMixersMenu();
    void rebuildAuditionBar();
    void showEditsMenu();
    void newProjectDialog();
    void openProjectDialog();
    void showStartPanel();
    std::unique_ptr<juce::Component> startPanel;           // the "where do you want to save your project?" screen, until a project is chosen

    AppContext& app;
    MenuBarButton projectButton { "Project" }, audioButton { "Audio settings" }, preampsButton { "Preamps" }, designButton { "Project Designer" },
                  takesButton { "Take Window" }, editsButton { "Edit Window" }, mixersButton { "Mixer" }, bridgeButton { "Meter bridge" }, mediaButton { "Media" }, masteringButton { "Mastering" },
                  sessionButton { "Pre-Rec" }, takeDisplayButton { "Take Display" }, organiserButton { "Display Organiser" };
    int menuBarH = 34;
    std::unique_ptr<MenuBarStrip> menuStrip;                  // declared after the buttons: it goes first when this window is destroyed
    std::vector<MenuBarButton*> menuItems() { return { &projectButton, &audioButton, &preampsButton, &designButton, &takesButton, &sessionButton, &editsButton, &mixersButton, &bridgeButton, &takeDisplayButton, &organiserButton, &mediaButton, &masteringButton }; }
    LogoBadge logo;
    static constexpr int kTakeBoxW = 40;                       // the take counter at the left of the menu bar
    NextTakeBox takeBox;
    // always-visible status squares at the right end of the menu bar (they replaced the coloured borders)
    static constexpr int kFlagsW = 186;
    StatusFlag flagPre { "Pre-Rec", juce::Colour (0xffe53935), "Pre-Rec (red): session mode is ON - the inputs are kept for the last 6 seconds, so a take starts 6 seconds before you press Record" },
               flagCr  { "C-R", juce::Colour (0xffa83cff), "C-R (purple): the control-room mic is OPEN - you are talking to the talkback speakers" },
               flagPb  { "P-B", juce::Colour (0xffffd21f), "P-B (yellow): playback - recorded material only is being played to the talkback output" };
    DockBar dock;
    juce::Label projectName, timeLabel, recStatus, statusLabel, inputsCaption, outputsCaption;
    // Driver inputs (left) and outputs (right) are always shown; the divider between them can be dragged
    juce::Viewport inputsViewport, outputsViewport;
    juce::Component inputsHolder, outputsHolder;
    juce::OwnedArray<InputRow> inputRows;
    juce::OwnedArray<OutputRow> outputRows;
    Divider ioDivider { true };
    float ioSplit = 0.62f;                         // share of the width given to the inputs
    juce::OwnedArray<juce::TextButton> auditionButtons;
    juce::Label auditionCaption;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace td
