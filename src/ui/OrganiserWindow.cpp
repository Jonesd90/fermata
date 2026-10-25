#include "OrganiserWindow.h"

namespace td
{
// ======================================================================================================================== ShapeButton
void ShapeButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = getToggleState();
    g.setColour (on ? theme::accent.withAlpha (0.28f) : (over || down) ? theme::panel.brighter (0.12f) : theme::panel);
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (on ? theme::accent : theme::border); g.drawRoundedRectangle (b, 5.0f, on ? 1.5f : 1.0f);
    auto area = b.reduced (8.0f, 6.0f); area.removeFromBottom (14.0f);
    auto screen = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), area.getHeight() * 1.6f), juce::jmin (area.getHeight(), area.getWidth() / 1.6f));
    const auto cells = ScreenDivision::shape (shape).cells();
    for (auto& c : cells)
    {
        juce::Rectangle<float> r (screen.getX() + c.x * screen.getWidth(), screen.getY() + c.y * screen.getHeight(), c.w * screen.getWidth(), c.h * screen.getHeight());
        g.setColour (on ? theme::text : theme::text.withAlpha (0.62f)); g.fillRoundedRectangle (r.reduced (1.2f), 1.5f);
    }
    g.setColour (on ? theme::text : theme::dimText); g.setFont (juce::FontOptions (10.5f, on ? juce::Font::bold : juce::Font::plain));
    g.drawText (getName(), b.removeFromBottom (16.0f).toNearestInt(), juce::Justification::centred, false);
}

// ======================================================================================================================== ScreenPicture
ScreenPicture::ScreenPicture() { setDivision (ScreenDivision::shape (ScreenDivision::AboveBelow2), {}); }

void ScreenPicture::setDivision (const ScreenDivision& d, const std::vector<WindowRef>& r)
{
    div = d; cellRefs = r; cellRefs.resize ((size_t) div.cellCount);
    hoverLine = hoverCell = dragLine = dragCell = dropCell = -1; dragging = false;
    repaint();
}

juce::Colour ScreenPicture::kindColour (WindowRef::Kind k)
{
    switch (k)
    {
        case WindowRef::Kind::Main:  return juce::Colour (0xff8a9bb0);
        case WindowRef::Kind::Take:  return juce::Colour (0xff3fae6a);
        case WindowRef::Kind::Edit:  return juce::Colour (0xff3d9ad6);
        case WindowRef::Kind::Mixer: return juce::Colour (0xffe08a3c);
        case WindowRef::Kind::Other: return juce::Colour (0xff9b7fd4);
        default:                     return theme::border;
    }
}

juce::Rectangle<float> ScreenPicture::screenArea() const
{
    auto b = getLocalBounds().toFloat().reduced (6.0f);
    float w = b.getWidth(), h = w / aspect;
    if (h > b.getHeight()) { h = b.getHeight(); w = h * aspect; }
    return b.withSizeKeepingCentre (w, h);
}
juce::Rectangle<float> ScreenPicture::cellArea (int cell) const
{
    const auto cells = div.cells();
    if (! juce::isPositiveAndBelow (cell, (int) cells.size())) return {};
    const auto s = screenArea(); const auto& c = cells[(size_t) cell];
    return { s.getX() + c.x * s.getWidth(), s.getY() + c.y * s.getHeight(), c.w * s.getWidth(), c.h * s.getHeight() };
}
int ScreenPicture::lineAt (juce::Point<float> p) const
{
    const auto s = screenArea();
    for (auto& l : div.lines())
    {
        if (l.vertical)
        {
            const float x = s.getX() + l.at.x * s.getWidth();
            if (std::abs (p.x - x) <= 5.0f && p.y >= s.getY() + l.at.y * s.getHeight() && p.y <= s.getY() + (l.at.y + l.at.h) * s.getHeight()) return l.node;
        }
        else
        {
            const float y = s.getY() + l.at.y * s.getHeight();
            if (std::abs (p.y - y) <= 5.0f && p.x >= s.getX() + l.at.x * s.getWidth() && p.x <= s.getX() + (l.at.x + l.at.w) * s.getWidth()) return l.node;
        }
    }
    return -1;
}
int ScreenPicture::cellAt (juce::Point<float> p) const
{
    for (int c = 0; c < div.cellCount; ++c) if (cellArea (c).contains (p)) return c;
    return -1;
}

void ScreenPicture::describe (const WindowRef& r, juce::String& title, juce::String& sub, bool& missing) const
{
    missing = false;
    if (r.kind == WindowRef::Kind::None) { title = "Click to choose a window"; sub = "(or leave this part empty)"; return; }
    if (r.any() && r.kind == WindowRef::Kind::Main) { title = "Main window"; sub = "Fermata's main window"; return; }
    if (r.any())
    {
        title = juce::String ("Any ") + (r.kind == WindowRef::Kind::Main ? "main window" : r.kind == WindowRef::Kind::Take ? "take window" : r.kind == WindowRef::Kind::Edit ? "edit window" : r.kind == WindowRef::Kind::Mixer ? "mixer" : "window");
        juce::String best; int rec = -2;
        for (auto& e : entries) if (e.kind == WindowRef::kindCode (r.kind) && e.recency > rec) { rec = e.recency; best = e.title; }
        sub = best.isNotEmpty() ? "the one used last: " + best : "none in this project";
        missing = best.isEmpty();
        return;
    }
    for (auto& e : entries)
        if (e.key == r.key)
        {
            title = e.title;
            sub = e.kind == "main" ? "Main window" : e.kind == "take" ? (e.open ? "Take window" : "Take window (closed: it will be opened)") : e.kind == "edit" ? (e.open ? "Edit window" : "Edit window (closed: it will be opened)") : e.kind == "mixer" ? (e.open ? "Mixer" : "Mixer (closed: it will be opened)") : "Window";
            return;
        }
    title = "Window not in this project"; sub = "it will be left out"; missing = true;
}

void ScreenPicture::paint (juce::Graphics& g)
{
    const auto s = screenArea();
    // the screen: a dark glass with a thin bezel
    g.setColour (theme::field); g.fillRoundedRectangle (s.expanded (4.0f), 6.0f);
    g.setColour (theme::border.brighter (0.25f)); g.drawRoundedRectangle (s.expanded (4.0f), 6.0f, 1.0f);
    for (int c = 0; c < div.cellCount; ++c)
    {
        auto r = cellArea (c).reduced (4.0f);
        const auto& ref = cellRefs[(size_t) c];
        juce::String title, sub; bool missing = false; describe (ref, title, sub, missing);
        const bool empty = ref.kind == WindowRef::Kind::None;
        const bool hot = c == hoverCell && ! dragging, drop = dragging && dragCell >= 0 && c == dropCell && c != dragCell;
        auto fill = empty ? theme::row.withAlpha (0.6f) : theme::panel.interpolatedWith (kindColour (ref.kind), 0.16f);
        if (hot || drop) fill = fill.brighter (0.12f);
        g.setColour (fill); g.fillRoundedRectangle (r, 5.0f);
        g.setColour (drop ? theme::accent : empty ? theme::border : kindColour (ref.kind).withAlpha (0.7f)); g.drawRoundedRectangle (r, 5.0f, drop ? 2.0f : 1.0f);
        if (! empty) { g.setColour (kindColour (ref.kind)); g.fillRoundedRectangle (r.getX() + 6.0f, r.getY() + 6.0f, r.getWidth() - 12.0f, 3.0f, 1.5f); }
        if (dragging && c == dragCell) g.setColour (theme::dimText.withAlpha (0.35f)); else g.setColour (empty ? theme::dimText : missing ? theme::warn : theme::text);
        auto inner = r.reduced (10.0f, 12.0f);
        const bool compact = inner.getHeight() < 48.0f || inner.getWidth() < 120.0f;
        g.setFont (juce::FontOptions (compact ? 12.5f : 15.0f, juce::Font::bold));
        auto titleArea = inner.removeFromTop (compact ? 16.0f : 20.0f);
        g.drawFittedText (title, titleArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        g.setFont (juce::FontOptions (compact ? 10.5f : 12.0f));
        if (! (dragging && c == dragCell)) g.setColour (missing ? theme::warn.withAlpha (0.85f) : theme::dimText);
        g.drawFittedText (sub, inner.removeFromTop (compact ? 14.0f : 17.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        // the part's name, bottom right, small
        g.setColour (theme::dimText.withAlpha (0.55f)); g.setFont (juce::FontOptions (10.0f));
        g.drawText (ScreenDivision::cellName (shapeOf(), c), r.reduced (8.0f, 5.0f).toNearestInt(), juce::Justification::bottomRight, false);
    }
    // the lines between the cells
    for (auto& l : div.lines())
    {
        const bool hot = l.node == hoverLine || l.node == dragLine;
        g.setColour (hot ? theme::accent : theme::border.brighter (0.35f));
        if (l.vertical) { const float x = s.getX() + l.at.x * s.getWidth(); g.fillRect (x - (hot ? 1.5f : 1.0f), s.getY() + l.at.y * s.getHeight() + 3.0f, hot ? 3.0f : 2.0f, l.at.h * s.getHeight() - 6.0f); }
        else            { const float y = s.getY() + l.at.y * s.getHeight(); g.fillRect (s.getX() + l.at.x * s.getWidth() + 3.0f, y - (hot ? 1.5f : 1.0f), l.at.w * s.getWidth() - 6.0f, hot ? 3.0f : 2.0f); }
        if (hot)                                                                  // a grip in the middle of the line
        {
            const float cx = s.getX() + (l.vertical ? l.at.x : l.at.x + 0.5f * l.at.w) * s.getWidth(), cy = s.getY() + (l.vertical ? l.at.y + 0.5f * l.at.h : l.at.y) * s.getHeight();
            g.fillRoundedRectangle (l.vertical ? juce::Rectangle<float> (cx - 3.0f, cy - 14.0f, 6.0f, 28.0f) : juce::Rectangle<float> (cx - 14.0f, cy - 3.0f, 28.0f, 6.0f), 3.0f);
        }
    }
    // the cell being dragged: its name follows the mouse
    if (dragging && dragCell >= 0)
    {
        juce::String title, sub; bool missing = false; describe (cellRefs[(size_t) dragCell], title, sub, missing);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title) + 24.0f;
        juce::Rectangle<float> tag (dragPos.x - w * 0.5f, dragPos.y - 14.0f, w, 28.0f);
        g.setColour (theme::accent.withAlpha (0.92f)); g.fillRoundedRectangle (tag, 6.0f);
        g.setColour (juce::Colours::white); g.drawText (title, tag.toNearestInt(), juce::Justification::centred, false);
    }
}

void ScreenPicture::resized() {}

void ScreenPicture::mouseMove (const juce::MouseEvent& e)
{
    const int l = lineAt (e.position), c = l < 0 ? cellAt (e.position) : -1;
    if (l != hoverLine || c != hoverCell) { hoverLine = l; hoverCell = c; repaint(); }
    if (l >= 0) setMouseCursor (div.nodes[(size_t) l].split == ScreenDivision::Split::SideBySide ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
    else setMouseCursor (c >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    setTooltip (l >= 0 ? "Drag the line to change the split" : c >= 0 ? "Click to choose the window for this part; drag it onto another part to swap them" : juce::String());
}
void ScreenPicture::mouseExit (const juce::MouseEvent&) { hoverLine = hoverCell = -1; repaint(); }
void ScreenPicture::mouseDown (const juce::MouseEvent& e)
{
    dragLine = lineAt (e.position); dragCell = dragLine < 0 ? cellAt (e.position) : -1; dragging = false; dropCell = -1;
}
void ScreenPicture::mouseDrag (const juce::MouseEvent& e)
{
    if (dragLine >= 0)
    {
        const auto s = screenArea(); const auto a = div.areaOf (dragLine);
        const bool v = div.nodes[(size_t) dragLine].split == ScreenDivision::Split::SideBySide;
        const float frac = v ? ((e.position.x - s.getX()) / s.getWidth() - a.x) / a.w : ((e.position.y - s.getY()) / s.getHeight() - a.y) / a.h;
        div.setFrac (dragLine, frac);
        if (onChanged) onChanged();
        repaint();
        return;
    }
    if (dragCell >= 0 && (dragging || e.getDistanceFromDragStart() > 6))
    {
        dragging = true; dragPos = e.position; dropCell = cellAt (e.position);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        repaint();
    }
}
void ScreenPicture::mouseUp (const juce::MouseEvent& e)
{
    if (dragLine >= 0) { dragLine = -1; repaint(); return; }
    if (dragCell >= 0)
    {
        if (dragging)
        {
            const int to = cellAt (e.position);
            if (to >= 0 && to != dragCell) { std::swap (cellRefs[(size_t) dragCell], cellRefs[(size_t) to]); if (onChanged) onChanged(); }
        }
        else if (onChooseCell && e.mouseWasClicked()) onChooseCell (dragCell, e.getScreenPosition());
    }
    dragging = false; dragCell = dropCell = -1;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

// ======================================================================================================================== OrganiserComponent
OrganiserComponent::OrganiserComponent (AppContext& a) : app (a)
{
    for (int s = 0; s < ScreenDivision::NumShapes; ++s)
    {
        auto* b = shapes.add (new ShapeButton (s));
        b->onClick = [this, s] { chooseShape (s, true); };
        addAndMakeVisible (b);
    }
    picture.onChooseCell = [this] (int cell, juce::Point<int> p) { chooseWindowForCell (cell, p); };
    picture.onChanged = [this] { setStatus ({}); };
    addAndMakeVisible (picture);

    layoutList.setModel (this);
    layoutList.setRowHeight (26);
    layoutList.setColour (juce::ListBox::backgroundColourId, theme::row);
    layoutList.setColour (juce::ListBox::outlineColourId, theme::border);
    layoutList.setOutlineThickness (1);
    addAndMakeVisible (layoutList);

    arrangeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    arrangeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    arrangeButton.setTooltip ("Puts the windows where the picture shows them (Enter). Nothing moves until you press it.");
    arrangeButton.onClick = [this] { arrange(); };
    saveButton.setTooltip ("Saves the picture as a layout of this project, under a name");
    saveButton.onClick = [this] { saveAs(); };
    snapshotButton.setTooltip ("Reads where the windows are on the screen right now into the picture (up to four of them), so you can save that as a layout");
    snapshotButton.onClick = [this] { saveFromScreen(); };
    deleteButton.setTooltip ("Removes the chosen layout from the project (the program's own layouts cannot be removed)");
    deleteButton.onClick = [this] { deleteLayout(); };
    for (auto* b : { &arrangeButton, &saveButton, &snapshotButton, &deleteButton }) b->setWantsKeyboardFocus (false);   // (a clicked button kept the keyboard, so Enter pressed it again instead of Arranging)
    for (auto* b : { &arrangeButton, &saveButton, &snapshotButton, &deleteButton }) addAndMakeVisible (b);

    status.setFont (juce::FontOptions (12.5f)); status.setColour (juce::Label::textColourId, theme::dimText);
    status.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (status);
    info.setText ("The Display Organiser arranges Fermata's windows on the screen the main window is on. Take Displays on other screens are left where they are.\n\n"
                  "1. Pick a shape along the top.\n"
                  "2. Click each part of the picture and choose the window for it; drag a part onto another to swap them; drag the lines between the parts to change the split. A part left empty leaves that bit of the screen alone.\n"
                  "3. Press Arrange (or Enter). Nothing moves until you do. A closed take window, edit or mixer is opened; a minimised one comes back.\n\n"
                  "The layouts on the right: the program's own (in capitals) name a kind of window and take the one you used last; Save as... keeps the picture in this project under a name. "
                  "Save how the windows are now reads the screen into the picture, so a layout you made by hand can be kept.");
    addAndMakeVisible (info);

    refreshEntries();
    refreshAspect();
    chooseShape (ScreenDivision::AboveBelow2, false);
    loadLayout (builtIns.front());
    rebuildRows();
    app.project.addChangeListener (this);
    startTimer (700);
    setWantsKeyboardFocus (true);
    setSize (900, 580);
}

OrganiserComponent::~OrganiserComponent() { app.project.removeChangeListener (this); }

void OrganiserComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    auto caption = [&g] (juce::Rectangle<int> r, const juce::String& t) { g.setColour (theme::text); g.setFont (juce::FontOptions (11.5f, juce::Font::bold)); g.drawText (t, r, juce::Justification::centredLeft, false); };
    caption (captionShape, "SHAPE");
    caption (captionScreen, "THE SCREEN  -  click a part to choose its window, drag the lines");
    caption (captionLayouts, "LAYOUTS");
}

void OrganiserComponent::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    info.placeTopRight (*this, 6);
    captionShape = r.removeFromTop (18).withTrimmedRight (30);
    auto row = r.removeFromTop (66);
    const int bw = juce::jmin (96, (row.getWidth() - 270) / shapes.size());
    for (auto* b : shapes) b->setBounds (row.removeFromLeft (bw).reduced (2, 0));
    r.removeFromTop (8);
    auto bottom = r.removeFromBottom (36);
    arrangeButton.setBounds (bottom.removeFromRight (150).reduced (0, 3));
    bottom.removeFromRight (10);
    status.setBounds (bottom);
    r.removeFromBottom (8);
    auto right = r.removeFromRight (250);
    r.removeFromRight (14);
    captionScreen = r.removeFromTop (18);
    picture.setBounds (r);
    captionLayouts = right.removeFromTop (18);
    auto buttons = right.removeFromBottom (3 * 28 + 6);
    snapshotButton.setBounds (buttons.removeFromTop (28).reduced (0, 2));
    buttons.removeFromTop (3);
    auto line = buttons.removeFromTop (28).reduced (0, 2);
    saveButton.setBounds (line.removeFromLeft (line.getWidth() / 2 - 2));
    deleteButton.setBounds (line.withTrimmedLeft (4));
    right.removeFromBottom (6);
    layoutList.setBounds (right);
}

bool OrganiserComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::returnKey) { arrange(); return true; }
    return false;
}

void OrganiserComponent::refreshAspect()
{
    const auto main = app.mainWindowArea ? app.mainWindowArea() : juce::Rectangle<int>();
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto area = (main.isEmpty() ? displays.getPrimaryDisplay() : displays.getDisplayForRect (main))->userArea;
    if (area.getHeight() > 0) picture.setAspect ((float) area.getWidth() / (float) area.getHeight());
}

void OrganiserComponent::refreshEntries()
{
    entries = app.listWindows ? app.listWindows() : std::vector<AppContext::WindowEntry>();
    int h = 17; for (auto& e : entries) h = h * 31 + e.key.hashCode() * 7 + e.title.hashCode() + (e.open ? 1 : 0) + e.recency;
    if (h != lastEntriesHash) { lastEntriesHash = h; picture.setEntries (entries); }
}

void OrganiserComponent::timerCallback() { refreshEntries(); refreshAspect(); }
void OrganiserComponent::changeListenerCallback (juce::ChangeBroadcaster*) { refreshEntries(); rebuildRows(); }

void OrganiserComponent::chooseShape (int s, bool keepWindows)
{
    shape = s;
    for (int i = 0; i < shapes.size(); ++i) shapes[i]->setToggleState (i == s, juce::dontSendNotification);
    std::vector<WindowRef> refs = keepWindows ? picture.refs() : std::vector<WindowRef>();
    picture.setDivision (ScreenDivision::shape (s), refs);
    if (keepWindows) { loadedName.clear(); loadedBuiltIn = false; layoutList.deselectAllRows(); }
    setStatus ({});
}

void OrganiserComponent::loadLayout (const LayoutDef& l)
{
    shape = l.shape;
    for (int i = 0; i < shapes.size(); ++i) shapes[i]->setToggleState (i == shape, juce::dontSendNotification);
    picture.setDivision (l.division(), l.refs);
    loadedName = l.name; loadedBuiltIn = l.builtIn;
    setStatus ({});
}

LayoutDef OrganiserComponent::currentLayout (const juce::String& name) const
{
    LayoutDef l; l.name = name; l.shape = shape; l.fracs = picture.division().fracs(); l.refs = const_cast<ScreenPicture&> (picture).refs();
    return l;
}

juce::String OrganiserComponent::resolve (const WindowRef& r) const
{
    if (r.kind == WindowRef::Kind::None) return {};
    if (! r.any()) for (auto& e : entries) if (e.key == r.key) return e.key;
    juce::String best; int rec = -2;                                              // a named window that is gone, or "any": the kind's most recent (an open one first)
    for (auto& e : entries) if (e.kind == WindowRef::kindCode (r.kind) && e.recency > rec) { rec = e.recency; best = e.key; }
    return best;
}

void OrganiserComponent::chooseWindowForCell (int cell, juce::Point<int> screenPos)
{
    refreshEntries();
    juce::PopupMenu m;
    std::vector<WindowRef> choices;
    auto addRef = [&] (juce::PopupMenu& menu, const WindowRef& r, const juce::String& text, bool ticked)
    {
        choices.push_back (r); menu.addItem ((int) choices.size(), text, true, ticked);
    };
    const auto& current = picture.refs()[(size_t) cell];
    auto section = [&] (const char* kindCode, WindowRef::Kind kind, const juce::String& heading, const juce::String& anyText)
    {
        bool anyOfKind = false; for (auto& e : entries) anyOfKind = anyOfKind || e.kind == kindCode;
        if (! anyOfKind) return;
        m.addSectionHeader (heading);
        if (kind != WindowRef::Kind::Main && kind != WindowRef::Kind::Other) addRef (m, WindowRef::anyOf (kind), anyText, current.any() && current.kind == kind);
        for (auto& e : entries) if (e.kind == kindCode) addRef (m, WindowRef::of (e.key), e.title + (e.open || kind == WindowRef::Kind::Main || kind == WindowRef::Kind::Other ? juce::String() : juce::String ("   (closed)")), ! current.any() && current.key == e.key);
    };
    section ("main", WindowRef::Kind::Main, "Main window", {});
    section ("take", WindowRef::Kind::Take, "Take windows", "Any take window  (the one used last)");
    section ("edit", WindowRef::Kind::Edit, "Edit windows", "Any edit window  (the one used last)");
    section ("mixer", WindowRef::Kind::Mixer, "Mixers", "Any mixer  (the one used last)");
    section ("other", WindowRef::Kind::Other, "Other windows open now", {});
    m.addSeparator();
    addRef (m, WindowRef(), "Leave this part empty", current.kind == WindowRef::Kind::None);
    juce::Component::SafePointer<OrganiserComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }).withMinimumWidth (240), [safe, cell, choices] (int r)
    {
        if (safe == nullptr || r <= 0 || r > (int) choices.size()) return;
        auto& refs = safe->picture.refs();
        if (cell >= 0 && cell < (int) refs.size()) refs[(size_t) cell] = choices[(size_t) r - 1];
        safe->loadedName.clear(); safe->loadedBuiltIn = false; safe->layoutList.deselectAllRows();
        safe->picture.repaint(); safe->setStatus ({});
    });
}

void OrganiserComponent::arrange()
{
    refreshEntries();
    const auto cells = picture.division().cells();
    std::vector<std::pair<juce::String, juce::Rectangle<float>>> placements;
    juce::StringArray used, leftOut;
    for (int c = 0; c < (int) cells.size(); ++c)
    {
        const auto& ref = picture.refs()[(size_t) c];
        if (ref.kind == WindowRef::Kind::None) continue;
        const auto key = resolve (ref);
        if (key.isEmpty()) { leftOut.add (ScreenDivision::cellName (shape, c)); continue; }
        if (used.contains (key))
        {
            juce::String title = key; for (auto& e : entries) if (e.key == key) title = e.title;
            setStatus ("\"" + title + "\" is in two parts of the picture: choose a different window for one of them.", true); return;
        }
        used.add (key);
        placements.push_back ({ key, { cells[(size_t) c].x, cells[(size_t) c].y, cells[(size_t) c].w, cells[(size_t) c].h } });
    }
    if (placements.empty()) { setStatus ("Click a part of the picture and choose a window for it first.", true); return; }
    if (app.placeWindows) app.placeWindows (placements);
    juce::String msg = juce::String (placements.size()) + (placements.size() == 1 ? " window arranged." : " windows arranged.");
    if (! leftOut.isEmpty()) msg += "  Nothing for: " + leftOut.joinIntoString (", ") + " (no such window in this project).";
    setStatus (msg, ! leftOut.isEmpty());
}

void OrganiserComponent::saveAs()
{
    auto* aw = new juce::AlertWindow ("Save layout", "A name for this layout (saved with the project):", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("n", loadedBuiltIn || loadedName.isEmpty() ? juce::String ("My layout") : loadedName);
    aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<OrganiserComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int res)
    {
        const auto name = aw->getTextEditorContents ("n").trim();
        delete aw;
        if (safe == nullptr || res != 1 || name.isEmpty()) return;
        auto l = safe->currentLayout (name);
        auto& list = safe->app.project.layouts;
        bool replaced = false;
        for (auto& x : list) if (x.name.equalsIgnoreCase (name)) { x = l; replaced = true; }
        if (! replaced) list.push_back (l);
        safe->app.project.markDirty();
        safe->loadedName = name; safe->loadedBuiltIn = false;
        safe->rebuildRows();
        safe->setStatus (replaced ? "Layout \"" + name + "\" updated." : "Layout \"" + name + "\" saved with the project.");
    }));
}

void OrganiserComponent::saveFromScreen()
{
    if (! app.windowPlacesNow) return;
    const auto now = app.windowPlacesNow();
    if (now.windows.empty() || now.area.isEmpty()) { setStatus ("No windows are open on the main window's screen.", true); return; }
    std::vector<std::pair<juce::String, FracRect>> wins;
    for (auto& [key, b] : now.windows)
    {
        const auto r = b.getIntersection (now.area);
        wins.push_back ({ key, { (float) (r.getX() - now.area.getX()) / (float) now.area.getWidth(), (float) (r.getY() - now.area.getY()) / (float) now.area.getHeight(),
                                 (float) r.getWidth() / (float) now.area.getWidth(), (float) r.getHeight() / (float) now.area.getHeight() } });
    }
    auto l = layoutFromWindows (wins);
    l.name.clear();
    loadLayout (l);
    layoutList.deselectAllRows();
    setStatus (juce::String (juce::jmin ((int) wins.size(), 4)) + " window" + (wins.size() == 1 ? "" : "s") + " read from the screen" + (wins.size() > 4 ? " (the four biggest)" : "") + ": press Save as... to keep this layout.");
}

void OrganiserComponent::deleteLayout()
{
    if (loadedName.isEmpty() || loadedBuiltIn) { setStatus (loadedBuiltIn ? "The program's own layouts cannot be removed (save a changed copy under your own name)." : "Choose a layout in the list first.", true); return; }
    auto& list = app.project.layouts;
    const auto name = loadedName;
    list.erase (std::remove_if (list.begin(), list.end(), [&name] (const LayoutDef& l) { return l.name == name; }), list.end());
    app.project.markDirty();
    loadedName.clear();
    rebuildRows();
    setStatus ("Layout \"" + name + "\" removed.");
}

void OrganiserComponent::setStatus (const juce::String& t, bool warning)
{
    status.setText (t, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, warning ? theme::warn : theme::dimText);
}

// ---- the list
void OrganiserComponent::rebuildRows()
{
    rows.clear();
    rows.push_back ({ true, true, -1, "THE PROGRAM'S OWN" });
    for (int i = 0; i < (int) builtIns.size(); ++i) rows.push_back ({ false, true, i, builtIns[(size_t) i].name });
    rows.push_back ({ true, false, -1, "SAVED WITH THIS PROJECT" });
    const auto& pl = app.project.layouts;
    if (pl.empty()) rows.push_back ({ true, false, -2, "(none yet: Save as... keeps the picture)" });
    for (int i = 0; i < (int) pl.size(); ++i) rows.push_back ({ false, false, i, pl[(size_t) i].name });
    layoutList.updateContent();
    int sel = -1;
    for (int r = 0; r < (int) rows.size(); ++r) if (! rows[(size_t) r].header && rows[(size_t) r].builtIn == loadedBuiltIn && rows[(size_t) r].text == loadedName) sel = r;
    if (sel >= 0) layoutList.selectRow (sel, true, true); else layoutList.deselectAllRows();
    layoutList.repaint();
}

void OrganiserComponent::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size())) return;
    const auto& r = rows[(size_t) row];
    if (r.header)
    {
        g.setColour (r.index == -2 ? theme::dimText.withAlpha (0.7f) : theme::dimText);
        g.setFont (juce::FontOptions (r.index == -2 ? 11.0f : 10.5f, r.index == -2 ? juce::Font::italic : juce::Font::bold));
        g.drawText (r.text, 8, 0, w - 12, h, juce::Justification::centredLeft, true);
        if (r.index != -2) { g.setColour (theme::border); g.fillRect (6, h - 1, w - 12, 1); }
        return;
    }
    if (selected) { g.setColour (theme::accent.withAlpha (0.35f)); g.fillRect (2, 1, w - 4, h - 2); }
    else if ((row & 1) != 0) { g.setColour (theme::text.withAlpha (0.03f)); g.fillRect (0, 0, w, h); }
    const auto& def = r.builtIn ? builtIns[(size_t) r.index] : app.project.layouts[(size_t) r.index];
    // a tiny picture of the shape, then the name
    auto pic = juce::Rectangle<float> (8.0f, 4.0f, (float) (h - 8) * 1.6f, (float) (h - 8));
    g.setColour (theme::field); g.fillRoundedRectangle (pic, 2.0f);
    const auto cells = def.division().cells();
    for (size_t i = 0; i < cells.size(); ++i)
    {
        const auto& c = cells[i];
        juce::Rectangle<float> cr (pic.getX() + c.x * pic.getWidth(), pic.getY() + c.y * pic.getHeight(), c.w * pic.getWidth(), c.h * pic.getHeight());
        g.setColour (i < def.refs.size() ? ScreenPicture::kindColour (def.refs[i].kind).withAlpha (0.9f) : theme::border); g.fillRect (cr.reduced (1.0f));
    }
    g.setColour (theme::text); g.setFont (juce::FontOptions (13.0f, selected ? juce::Font::bold : juce::Font::plain));
    g.drawText (r.text, (int) pic.getRight() + 8, 0, w - (int) pic.getRight() - 12, h, juce::Justification::centredLeft, true);
}

void OrganiserComponent::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()) || rows[(size_t) row].header) { layoutList.deselectAllRows(); return; }
    const auto& r = rows[(size_t) row];
    const auto& def = r.builtIn ? builtIns[(size_t) r.index] : app.project.layouts[(size_t) r.index];
    loadLayout (def);
    setStatus (r.builtIn ? "The picture shows \"" + def.name + "\". Press Arrange to use it, or change it and Save as..." : "The picture shows \"" + def.name + "\". Press Arrange to use it.");
}
void OrganiserComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) { listBoxItemClicked (row, e); arrange(); }
void OrganiserComponent::selectedRowsChanged (int) {}
} // namespace td
