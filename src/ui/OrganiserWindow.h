#pragma once
#include "AppContext.h"
#include "../core/ScreenLayout.h"

namespace td
{
/** One of the eight shapes: a small picture of the screen divided, with its name under it. */
class ShapeButton : public juce::Button
{
public:
    explicit ShapeButton (int shapeIndex) : juce::Button (ScreenDivision::shapeName (shapeIndex)), shape (shapeIndex) { setClickingTogglesState (false); setWantsKeyboardFocus (false); setTooltip (juce::String (ScreenDivision::cellsOf (shape)) + (ScreenDivision::cellsOf (shape) == 1 ? " window: " : " windows: ") + ScreenDivision::shapeName (shape)); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    const int shape;
};

/** The picture of the screen: the cells with the windows chosen for them, and the lines between them. Click a cell to choose its window, drag a
    cell onto another to swap them, drag a line to move it. Nothing moves on the real screen until Arrange. */
class ScreenPicture : public juce::Component, public juce::SettableTooltipClient
{
public:
    ScreenPicture();
    void setDivision (const ScreenDivision&, const std::vector<WindowRef>&);
    const ScreenDivision& division() const noexcept { return div; }
    int shapeOf() const noexcept { return div.shapeIndex; }
    std::vector<WindowRef>& refs() noexcept { return cellRefs; }
    void setAspect (float widthOverHeight) { aspect = juce::jlimit (0.5f, 4.0f, widthOverHeight); resized(); repaint(); }
    /** The windows the picture can show (titles, kinds, open or not). */
    void setEntries (const std::vector<AppContext::WindowEntry>& e) { entries = e; repaint(); }
    std::function<void (int cell, juce::Point<int> screenPos)> onChooseCell;      // a click on a cell: the owner shows the menu
    std::function<void()> onChanged;                                              // a line moved or two cells swapped
    /** What the cell shows for its window: the title line and the smaller line under it. */
    void describe (const WindowRef&, juce::String& title, juce::String& sub, bool& missing) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    juce::Rectangle<float> screenArea() const;                                    // where the "screen" is drawn inside the component
    juce::Rectangle<float> cellArea (int cell) const;
    static juce::Colour kindColour (WindowRef::Kind);

private:
    int lineAt (juce::Point<float>) const;                                        // the node of the line under the mouse (-1: none)
    int cellAt (juce::Point<float>) const;

    ScreenDivision div;
    std::vector<WindowRef> cellRefs;
    std::vector<AppContext::WindowEntry> entries;
    float aspect = 16.0f / 9.0f;
    int hoverLine = -1, hoverCell = -1, dragLine = -1, dragCell = -1, dropCell = -1;
    bool dragging = false;
    juce::Point<float> dragPos;
};

/** The Display Organiser: choose a shape, put a window in each part (click a part), drag the lines, press Arrange. Layouts can be saved with the
    project; the program's own layouts (Recording, Editing ...) name a kind of window and take the one used last. */
class OrganiserComponent : public juce::Component, private juce::ListBoxModel, private juce::Timer, private juce::ChangeListener
{
public:
    explicit OrganiserComponent (AppContext&);
    ~OrganiserComponent() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    // what is shown
    void chooseShape (int shape, bool keepWindows);
    void loadLayout (const LayoutDef&);
    LayoutDef currentLayout (const juce::String& name) const;
    void refreshEntries();
    void refreshAspect();
    void chooseWindowForCell (int cell, juce::Point<int> screenPos);
    /** The window a reference comes to right now ("" = none): a named window if it still exists, otherwise the kind's most recent. */
    juce::String resolve (const WindowRef&) const;
    void arrange();
    void saveAs();
    void saveFromScreen();
    void deleteLayout();
    void setStatus (const juce::String&, bool warning = false);

    // the layouts list
    struct ListRow { bool header = false; bool builtIn = false; int index = -1; juce::String text; };
    std::vector<ListRow> rows;
    void rebuildRows();
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int) override { arrange(); }        // Enter with the keyboard in the list: Arrange (3.0.1: the list used to swallow it)
    void selectedRowsChanged (int) override;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    AppContext& app;
    int shape = ScreenDivision::AboveBelow2;
    juce::String loadedName; bool loadedBuiltIn = false;                        // the layout the picture was loaded from (for Delete, and the list's highlight)
    std::vector<AppContext::WindowEntry> entries;
    std::vector<LayoutDef> builtIns = LayoutDef::builtIns();
    juce::OwnedArray<ShapeButton> shapes;
    ScreenPicture picture;
    juce::ListBox layoutList;
    juce::TextButton arrangeButton { "Arrange" }, saveButton { "Save as..." }, snapshotButton { "Save how the windows are now" }, deleteButton { "Delete" };
    juce::Label status;
    InfoNote info;
    juce::Rectangle<int> captionShape, captionScreen, captionLayouts;
    int lastEntriesHash = 0;
};
} // namespace td
