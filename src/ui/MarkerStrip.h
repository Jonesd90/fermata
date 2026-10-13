#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../core/Markers.h"

namespace td
{
/** The quick-marker strip along the very top of a take window / edit window (above the ruler).
    Right-click the strip: Add Marker / Clear All Markers. Drag a marker to move it; right-click it to rename or delete it.
    Used by both windows: each gives it its list and the time <-> pixel maths. */
namespace markerui
{
constexpr int kStripH = 14;

inline juce::Font flagFont() { return juce::Font (juce::FontOptions (11.0f, juce::Font::bold)); }
inline int flagWidth (const juce::String& name) { return juce::jlimit (22, 180, juce::GlyphArrangement::getStringWidthInt (flagFont(), name) + 12); }
inline juce::Colour flagColour() { return juce::Colour (0xffe6a82c); }

/** The little name box that opens next to a new marker (or a renamed one). Enter keeps the name; clicking off keeps the old one. */
class NameBox : public juce::Component
{
public:
    NameBox (const juce::String& current, std::function<void (const juce::String&)> onName) : done (std::move (onName))
    {
        addAndMakeVisible (label); addAndMakeVisible (editor);
        label.setText ("Marker name", juce::dontSendNotification); label.setFont (juce::FontOptions (12.0f));
        editor.setText (current, false); editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [this] { commit(); };
        editor.onEscapeKey = [this] { close(); };
        setSize (230, 62);
    }
    void resized() override { label.setBounds (8, 4, getWidth() - 16, 18); editor.setBounds (8, 26, getWidth() - 16, 28); }
    void parentHierarchyChanged() override { if (isShowing()) editor.grabKeyboardFocus(); }
    void visibilityChanged() override { if (isShowing()) editor.grabKeyboardFocus(); }

private:
    void commit()
    {
        const auto t = editor.getText().trim();
        if (t.isNotEmpty() && done) done (t);
        close();
    }
    void close() { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); }
    juce::Label label; juce::TextEditor editor; std::function<void (const juce::String&)> done;
};

/** Draws the strip's flags (and a faint line down from each) on a timeline. 'xOf' gives a time's x position; nothing is drawn left of 'leftLimit'
    (the track-name column). 'skipLines' leaves out the lines down the window (the flags are always drawn). */
inline void paintFlags (juce::Graphics& g, const std::vector<Marker>& list, const std::function<int (double)>& xOf, int leftLimit, int totalHeight, const juce::Uuid& dragging)
{
    g.setFont (flagFont());
    for (auto& m : list)
    {
        const int x = xOf (m.seconds);
        if (x < leftLimit) continue;
        const bool held = m.id == dragging;
        g.setColour (flagColour().withAlpha (held ? 0.75f : 0.4f));
        g.fillRect (x, kStripH, 1, juce::jmax (0, totalHeight - kStripH));
        auto f = juce::Rectangle<int> (x, 1, flagWidth (m.name), kStripH - 2);
        g.setColour (held ? flagColour().brighter (0.3f) : flagColour());
        g.fillRect (f);
        g.setColour (juce::Colour (0xff1a1a1a));
        g.drawText (m.name, f.reduced (4, 0), juce::Justification::centredLeft, true);
    }
}

/** The mouse side. Lives inside the timeline component; the timeline calls it first in mouseDown / mouseDrag / mouseUp and stops if it returns true. */
class Controller
{
public:
    Controller (juce::Component& ownerComponent, int nameColumnWidth,
                std::function<std::vector<Marker>*()> getList,
                std::function<double (int)> xToSeconds, std::function<int (double)> secondsToX,
                std::function<void()> onChanged)
        : owner (ownerComponent), nameW (std::move (nameColumnWidth)), list (std::move (getList)), toSeconds (std::move (xToSeconds)), toX (std::move (secondsToX)), changed (std::move (onChanged)) {}

    /** The name column is resizable in the edit window: give the current width before each use. */
    void setNameWidth (int w) { nameW = w; }

    bool inStrip (const juce::MouseEvent& e) const { return e.y >= 0 && e.y < kStripH && e.x > nameW; }
    bool dragging() const { return dragId != juce::Uuid::null(); }
    const juce::Uuid& draggedId() const { return dragId; }

    bool mouseDown (const juce::MouseEvent& e)
    {
        if (! inStrip (e)) return false;
        auto* l = list(); if (l == nullptr) return true;
        const auto* hit = markerAt (*l, e.x);
        if (e.mods.isPopupMenu()) { showMenu (e, hit != nullptr ? hit->id : juce::Uuid::null()); return true; }
        if (hit != nullptr)
        {
            if (e.getNumberOfClicks() >= 2) { rename (hit->id); return true; }
            dragId = hit->id; grabDx = e.x - toX (hit->seconds); moved = false;
        }
        return true;                                                                         // (a click on the bare strip does nothing)
    }
    bool mouseDrag (const juce::MouseEvent& e)
    {
        if (! dragging()) return false;
        if (auto* l = list()) if (auto* m = findMarker (*l, dragId))
        {
            m->seconds = juce::jmax (0.0, toSeconds (e.x - grabDx)); moved = true;
            owner.repaint();
        }
        return true;
    }
    bool mouseUp (const juce::MouseEvent&)
    {
        if (! dragging()) return false;
        dragId = juce::Uuid::null();
        if (moved && changed) changed();
        moved = false; owner.repaint();
        return true;
    }

private:
    const Marker* markerAt (const std::vector<Marker>& l, int x) const
    {
        const Marker* hit = nullptr;
        for (auto& m : l)
        {
            const int mx = toX (m.seconds);
            if ((x >= mx && x < mx + flagWidth (m.name)) || std::abs (x - mx) <= 3) hit = &m;       // (the last one drawn is on top)
        }
        return hit;
    }

    void showMenu (const juce::MouseEvent& e, const juce::Uuid& onMarker)
    {
        auto* l = list(); if (l == nullptr) return;
        juce::PopupMenu m;
        const double at = toSeconds (e.x);
        if (onMarker != juce::Uuid::null()) { m.addItem (1, "Rename marker..."); m.addItem (2, "Delete marker"); }
        else { m.addItem (3, "Add Marker"); m.addItem (4, "Clear All Markers", ! l->empty()); }
        juce::Component::SafePointer<juce::Component> safe (&owner);
        m.showMenuAsync (juce::PopupMenu::Options(), [this, safe, onMarker, at] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto* l = list(); if (l == nullptr) return;
            if (r == 1) rename (onMarker);
            else if (r == 2) { removeMarker (*l, onMarker); if (changed) changed(); owner.repaint(); }
            else if (r == 3)
            {
                const auto id = addMarker (*l, at);                                           // it is there at once, called by its number; the box can give it a name
                if (changed) changed();
                owner.repaint();
                rename (id);
            }
            else if (r == 4) confirmClear();
        });
    }

    void rename (const juce::Uuid& id)
    {
        auto* l = list(); if (l == nullptr) return;
        auto* m = findMarker (*l, id); if (m == nullptr) return;
        juce::Component::SafePointer<juce::Component> safe (&owner);
        auto box = std::make_unique<NameBox> (m->name, [this, safe, id] (const juce::String& name)
        {
            if (safe == nullptr) return;
            if (auto* l2 = list()) if (auto* mm = findMarker (*l2, id)) { mm->name = name; if (changed) changed(); owner.repaint(); }
        });
        const auto area = owner.localAreaToGlobal (juce::Rectangle<int> (toX (m->seconds), 0, flagWidth (m->name), kStripH));
        juce::CallOutBox::launchAsynchronously (std::move (box), area, nullptr);
    }

    void confirmClear()
    {
        juce::Component::SafePointer<juce::Component> safe (&owner);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle ("Clear all markers").withMessage ("Take away every marker in this window?")
            .withButton ("Clear all").withButton ("Cancel").withAssociatedComponent (&owner),
            [this, safe] (int result)
            {
                if (safe == nullptr || result != 1) return;
                if (auto* l = list()) { l->clear(); if (changed) changed(); owner.repaint(); }
            });
    }

    juce::Component& owner;
    int nameW;
    std::function<std::vector<Marker>*()> list;
    std::function<double (int)> toSeconds;
    std::function<int (double)> toX;
    std::function<void()> changed;
    juce::Uuid dragId = juce::Uuid::null();
    int grabDx = 0; bool moved = false;
};
} // namespace markerui
} // namespace td
