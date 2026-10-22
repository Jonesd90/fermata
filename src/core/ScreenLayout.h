#pragma once
#include "Common.h"
#include <vector>
#include <algorithm>

namespace td
{
/** A rectangle in fractions of the screen (the core has no juce_graphics, so no juce::Rectangle). */
struct FracRect
{
    float x = 0, y = 0, w = 0, h = 0;
    float right() const noexcept { return x + w; }
    float bottom() const noexcept { return y + h; }
    float cx() const noexcept { return x + 0.5f * w; }
    float cy() const noexcept { return y + 0.5f * h; }
    float area() const noexcept { return w * h; }
    FracRect intersection (const FracRect& o) const noexcept
    {
        const float l = std::max (x, o.x), t = std::max (y, o.y), r = std::min (right(), o.right()), b = std::min (bottom(), o.bottom());
        return r > l && b > t ? FracRect { l, t, r - l, b - t } : FracRect {};
    }
    bool operator== (const FracRect& o) const noexcept { return x == o.x && y == o.y && w == o.w && h == o.h; }
    float distanceOfCentres (const FracRect& o) const noexcept { return std::hypot (cx() - o.cx(), cy() - o.cy()); }
};

/** How the Display Organiser divides the screen: a tree of splits. Every split is a line across its part of the screen (a vertical line for
    "side by side", a horizontal one for "above / below") at a fraction of that part; the leaves are the cells the windows go in, numbered in
    reading order. The lines can be dragged, so each split keeps its fraction; a shape (the eight pictures in the Organiser) is a tree with the
    fractions at their even starting places. Pure: no windows, no screen - fractions of whatever area it is put on. */
struct ScreenDivision
{
    enum class Split { None, SideBySide, AboveBelow };          // None: a leaf (a cell)
    struct Node
    {
        Split split = Split::None;
        float frac = 0.5f;                                     // where the line is, as a fraction of this node's width (SideBySide) or height (AboveBelow)
        int a = -1, b = -1;                                    // the two halves (left / right, or top / bottom)
        int cell = -1;                                         // the cell number (leaves only)
        int linked = -1;                                       // another node whose line moves with this one (the four-window grid's two inner lines)
    };
    std::vector<Node> nodes;
    int root = -1;
    int cellCount = 0;
    int shapeIndex = 0;                                        // which of the eight shapes it was made from

    static constexpr float kMinFrac = 0.12f, kMaxFrac = 0.88f;

    // ---- the eight shapes
    enum Shape : int { One = 0, AboveBelow2, SideBySide2, Rows3, Columns3, OneAboveTwo, OneBesideTwo, Grid4, NumShapes };
    static const char* shapeName (int s)
    {
        static const char* n[] = { "One", "Above / below", "Side by side", "3 rows", "3 columns", "1 above 2", "1 beside 2", "Four" };
        return juce::isPositiveAndBelow (s, (int) NumShapes) ? n[s] : "";
    }
    /** The cells' names in a shape ("Top", "Bottom left" ...). */
    static juce::String cellName (int shape, int cell)
    {
        static const char* names[NumShapes][4] = { { "Window", "", "", "" }, { "Top", "Bottom", "", "" }, { "Left", "Right", "", "" }, { "Top", "Middle", "Bottom", "" },
                                                   { "Left", "Middle", "Right", "" }, { "Top", "Bottom left", "Bottom right", "" }, { "Left", "Top right", "Bottom right", "" },
                                                   { "Top left", "Top right", "Bottom left", "Bottom right" } };
        return juce::isPositiveAndBelow (shape, (int) NumShapes) && juce::isPositiveAndBelow (cell, 4) ? names[shape][cell] : juce::String();
    }
    static int cellsOf (int shape) { static const int n[] = { 1, 2, 2, 3, 3, 3, 3, 4 }; return juce::isPositiveAndBelow (shape, (int) NumShapes) ? n[shape] : 0; }

    static ScreenDivision shape (int s)
    {
        ScreenDivision d; d.shapeIndex = juce::jlimit (0, (int) NumShapes - 1, s);
        auto leaf  = [&d] { Node n; n.cell = d.cellCount++; d.nodes.push_back (n); return (int) d.nodes.size() - 1; };
        auto split = [&d] (Split sp, float f, int a, int b) { Node n; n.split = sp; n.frac = f; n.a = a; n.b = b; d.nodes.push_back (n); return (int) d.nodes.size() - 1; };
        switch (s)
        {
            case AboveBelow2:  { int a = leaf(), b = leaf(); d.root = split (Split::AboveBelow, 0.5f, a, b); break; }
            case SideBySide2:  { int a = leaf(), b = leaf(); d.root = split (Split::SideBySide, 0.5f, a, b); break; }
            case Rows3:        { int a = leaf(), b = leaf(), c = leaf(); int bc = split (Split::AboveBelow, 0.5f, b, c); d.root = split (Split::AboveBelow, 1.0f / 3.0f, a, bc); break; }
            case Columns3:     { int a = leaf(), b = leaf(), c = leaf(); int bc = split (Split::SideBySide, 0.5f, b, c); d.root = split (Split::SideBySide, 1.0f / 3.0f, a, bc); break; }
            case OneAboveTwo:  { int a = leaf(), b = leaf(), c = leaf(); int bc = split (Split::SideBySide, 0.5f, b, c); d.root = split (Split::AboveBelow, 0.5f, a, bc); break; }
            case OneBesideTwo: { int a = leaf(), b = leaf(), c = leaf(); int bc = split (Split::AboveBelow, 0.5f, b, c); d.root = split (Split::SideBySide, 0.5f, a, bc); break; }
            case Grid4:        { int a = leaf(), b = leaf(), c = leaf(), e = leaf(); int ab = split (Split::SideBySide, 0.5f, a, b), ce = split (Split::SideBySide, 0.5f, c, e);
                                 d.nodes[(size_t) ab].linked = ce; d.nodes[(size_t) ce].linked = ab; d.root = split (Split::AboveBelow, 0.5f, ab, ce); break; }
            default:           { d.root = leaf(); break; }
        }
        return d;
    }

    // ---- what it comes to
    /** The cells as fractions of the whole area, in cell order. */
    std::vector<FracRect> cells() const
    {
        std::vector<FracRect> out ((size_t) cellCount);
        walk (root, FracRect { 0.0f, 0.0f, 1.0f, 1.0f }, [&out] (const Node& n, FracRect r) { if (n.cell >= 0 && n.cell < (int) out.size()) out[(size_t) n.cell] = r; }, nullptr);
        return out;
    }
    /** The dividing lines: which node, and where the line lies (a zero-width / zero-height rectangle in fractions of the whole area). */
    struct Line { int node; bool vertical; FracRect at; };
    std::vector<Line> lines() const
    {
        std::vector<Line> out;
        walk (root, FracRect { 0.0f, 0.0f, 1.0f, 1.0f }, nullptr, [&out, this] (int idx, const Node& n, FracRect r)
        {
            if (n.split == Split::SideBySide) out.push_back ({ idx, true,  { r.x + n.frac * r.w, r.y, 0.0f, r.h } });
            else                              out.push_back ({ idx, false, { r.x, r.y + n.frac * r.h, r.w, 0.0f } });
        });
        return out;
    }
    /** Moves a line to 'frac' of its own node (kept within the limits); a linked line moves with it. */
    void setFrac (int node, float frac)
    {
        if (! juce::isPositiveAndBelow (node, (int) nodes.size())) return;
        frac = juce::jlimit (kMinFrac, kMaxFrac, frac);
        nodes[(size_t) node].frac = frac;
        if (const int l = nodes[(size_t) node].linked; juce::isPositiveAndBelow (l, (int) nodes.size())) nodes[(size_t) l].frac = frac;
    }
    /** The node's own area (fractions of the whole), for turning a mouse position into a fraction. */
    FracRect areaOf (int node) const
    {
        FracRect found;
        walk (root, FracRect { 0.0f, 0.0f, 1.0f, 1.0f }, nullptr, [&found, node] (int idx, const Node&, FracRect r) { if (idx == node) found = r; });
        return found;
    }
    /** The fractions of every split, the outermost line first (saved with a layout); and putting them back. */
    std::vector<float> fracs() const
    {
        std::vector<float> f;
        walk (root, FracRect { 0.0f, 0.0f, 1.0f, 1.0f }, nullptr, [&f] (int, const Node& n, FracRect) { f.push_back (n.frac); });
        return f;
    }
    void setFracs (const std::vector<float>& f)
    {
        size_t k = 0;
        walk (root, FracRect { 0.0f, 0.0f, 1.0f, 1.0f }, nullptr, [&] (int idx, const Node&, FracRect) { if (k < f.size()) nodes[(size_t) idx].frac = juce::jlimit (kMinFrac, kMaxFrac, f[k++]); });
    }

    /** Finds the fractions that make the cells sit closest to these rectangles (fractions of the area, one per cell, in cell order): "save how the
        windows are now". Every line is put at the mean of the window edges that meet it. */
    void fitTo (const std::vector<FracRect>& wanted)
    {
        if ((int) wanted.size() != cellCount) return;
        for (int pass = 0; pass < 3; ++pass)                                  // the outer lines depend on the inner ones' areas and the other way round: a few passes settle it
            walk (root, FracRect { 0.0f, 0.0f, 1.0f, 1.0f }, nullptr, [&] (int idx, const Node& n, FracRect r)
            {
                std::vector<int> left, right; cellsUnder (n.a, left); cellsUnder (n.b, right);
                double sum = 0.0; int count = 0;
                for (int c : left)  { const auto& w = wanted[(size_t) c]; sum += n.split == Split::SideBySide ? w.right() : w.bottom(); ++count; }
                for (int c : right) { const auto& w = wanted[(size_t) c]; sum += n.split == Split::SideBySide ? w.x : w.y; ++count; }
                if (count == 0) return;
                const float edge = (float) (sum / count);
                const float frac = n.split == Split::SideBySide ? (r.w > 0.0f ? (edge - r.x) / r.w : 0.5f) : (r.h > 0.0f ? (edge - r.y) / r.h : 0.5f);
                nodes[(size_t) idx].frac = juce::jlimit (kMinFrac, kMaxFrac, frac);
            });
        for (size_t i = 0; i < nodes.size(); ++i)                              // linked lines (the grid's inner ones) settle on their mean
            if (const int l = nodes[i].linked; l > (int) i && l < (int) nodes.size()) nodes[(size_t) l].frac = nodes[i].frac = 0.5f * (nodes[i].frac + nodes[(size_t) l].frac);
    }
    /** How well these rectangles match the cells (1 = exactly, 0 = not at all): the mean overlap of each window with its cell. */
    float matchScore (const std::vector<FracRect>& wanted) const
    {
        if ((int) wanted.size() != cellCount || cellCount == 0) return 0.0f;
        const auto c = cells();
        float s = 0.0f;
        for (int i = 0; i < cellCount; ++i)
        {
            const auto& w = wanted[(size_t) i]; const auto& k = c[(size_t) i];
            const float inter = k.intersection (w).area();
            const float uni = k.area() + w.area() - inter;
            s += uni > 0.0f ? inter / uni : 0.0f;
        }
        return s / (float) cellCount;
    }

private:
    void cellsUnder (int idx, std::vector<int>& out) const
    {
        if (! juce::isPositiveAndBelow (idx, (int) nodes.size())) return;
        const auto& n = nodes[(size_t) idx];
        if (n.split == Split::None) { out.push_back (n.cell); return; }
        cellsUnder (n.a, out); cellsUnder (n.b, out);
    }
    template <typename Leaf, typename Branch>
    void walk (int idx, FracRect r, Leaf leaf, Branch branch) const
    {
        if (! juce::isPositiveAndBelow (idx, (int) nodes.size())) return;
        const auto& n = nodes[(size_t) idx];
        if (n.split == Split::None) { if constexpr (! std::is_same_v<Leaf, std::nullptr_t>) leaf (n, r); return; }
        if constexpr (! std::is_same_v<Branch, std::nullptr_t>) branch (idx, n, r);
        if (n.split == Split::SideBySide) { const float wa = n.frac * r.w; walk (n.a, FracRect { r.x, r.y, wa, r.h }, leaf, branch); walk (n.b, FracRect { r.x + wa, r.y, r.w - wa, r.h }, leaf, branch); }
        else                              { const float ha = n.frac * r.h; walk (n.a, FracRect { r.x, r.y, r.w, ha }, leaf, branch); walk (n.b, FracRect { r.x, r.y + ha, r.w, r.h - ha }, leaf, branch); }
    }
};

/** Which window goes in a cell: a particular one (its key: "main", "take:<id>", "edit:<id>", "mixer:<id>", or another window's key), or any
    window of a kind (the one used last). An empty cell is left alone when the windows are arranged. */
struct WindowRef
{
    enum class Kind { None, Main, Take, Edit, Mixer, Other };
    Kind kind = Kind::None;
    juce::String key;                                          // the particular window ("" = any of the kind)
    bool any() const noexcept { return key.isEmpty(); }
    static Kind kindOf (const juce::String& k)
    {
        if (k == "main") return Kind::Main;
        if (k.startsWith ("take:")) return Kind::Take;
        if (k.startsWith ("edit:")) return Kind::Edit;
        if (k.startsWith ("mixer:")) return Kind::Mixer;
        return k.isEmpty() ? Kind::None : Kind::Other;
    }
    static WindowRef of (const juce::String& k) { WindowRef r; r.kind = kindOf (k); r.key = k; return r; }
    static WindowRef anyOf (Kind k) { WindowRef r; r.kind = k; return r; }
    static const char* kindName (Kind k)
    {
        switch (k) { case Kind::Main: return "Main window"; case Kind::Take: return "Take window"; case Kind::Edit: return "Edit window"; case Kind::Mixer: return "Mixer"; case Kind::Other: return "Window"; default: return ""; }
    }
    static const char* kindCode (Kind k)
    {
        switch (k) { case Kind::Main: return "main"; case Kind::Take: return "take"; case Kind::Edit: return "edit"; case Kind::Mixer: return "mixer"; case Kind::Other: return "other"; default: return "none"; }
    }
    static Kind kindFromCode (const juce::String& c)
    {
        return c == "main" ? Kind::Main : c == "take" ? Kind::Take : c == "edit" ? Kind::Edit : c == "mixer" ? Kind::Mixer : c == "other" ? Kind::Other : Kind::None;
    }
};

/** A saved layout: a shape with its lines where they were dragged, and the window for every cell. The program's own layouts name only the kind
    of window ("a take window"); the ones saved in a project name the windows themselves. */
struct LayoutDef
{
    juce::String name;
    int shape = ScreenDivision::AboveBelow2;
    std::vector<float> fracs;
    std::vector<WindowRef> refs;
    bool builtIn = false;

    ScreenDivision division() const { auto d = ScreenDivision::shape (shape); d.setFracs (fracs); return d; }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", name); o->setProperty ("shape", shape);
        juce::Array<juce::var> f; for (float x : fracs) f.add (x); o->setProperty ("fracs", f);
        juce::Array<juce::var> r;
        for (auto& w : refs) { auto* ro = new juce::DynamicObject(); ro->setProperty ("kind", WindowRef::kindCode (w.kind)); ro->setProperty ("key", w.key); r.add (juce::var (ro)); }
        o->setProperty ("cells", r);
        return juce::var (o);
    }
    static LayoutDef fromVar (const juce::var& v)
    {
        LayoutDef l;
        l.name = v.getProperty ("name", "").toString();
        l.shape = juce::jlimit (0, (int) ScreenDivision::NumShapes - 1, (int) v.getProperty ("shape", 1));
        if (auto* f = v["fracs"].getArray()) for (auto& x : *f) l.fracs.push_back ((float) (double) x);
        if (auto* r = v["cells"].getArray()) for (auto& c : *r) { WindowRef w; w.kind = WindowRef::kindFromCode (c.getProperty ("kind", "").toString()); w.key = c.getProperty ("key", "").toString(); l.refs.push_back (w); }
        l.refs.resize ((size_t) ScreenDivision::cellsOf (l.shape));
        return l;
    }

    /** The program's own layouts. */
    static std::vector<LayoutDef> builtIns()
    {
        using K = WindowRef::Kind;
        auto make = [] (const char* name, int shape, std::initializer_list<K> kinds, std::initializer_list<float> fr = {})
        {
            LayoutDef l; l.name = name; l.shape = shape; l.builtIn = true;
            for (K k : kinds) l.refs.push_back (WindowRef::anyOf (k));
            l.refs.resize ((size_t) ScreenDivision::cellsOf (shape));
            l.fracs = fr;
            return l;
        };
        return {
            make ("Recording",            ScreenDivision::AboveBelow2,  { K::Main, K::Take },         { 0.5f }),
            make ("Editing",              ScreenDivision::AboveBelow2,  { K::Edit, K::Mixer },        { 0.62f }),
            make ("Take and edit",        ScreenDivision::SideBySide2,  { K::Take, K::Edit }),
            make ("Take above edit",      ScreenDivision::AboveBelow2,  { K::Take, K::Edit }),
            make ("Edit beside mixer",    ScreenDivision::SideBySide2,  { K::Edit, K::Mixer },        { 0.6f }),
            make ("Take, edit and mixer", ScreenDivision::OneAboveTwo,  { K::Take, K::Edit, K::Mixer }, { 0.45f, 0.5f }),
            make ("Mixing",               ScreenDivision::One,          { K::Mixer }),
        };
    }
};

/** "Save how the windows are now": the shape and lines that best match these windows (rectangles as fractions of the screen, with their keys).
    Up to four windows; the biggest are kept when there are more. The cells are filled with the windows in reading order of the cells. */
inline LayoutDef layoutFromWindows (std::vector<std::pair<juce::String, FracRect>> wins)
{
    LayoutDef best;
    std::sort (wins.begin(), wins.end(), [] (auto& a, auto& b) { return a.second.area() > b.second.area(); });
    if (wins.size() > 4) wins.resize (4);
    if (wins.empty()) { best.shape = ScreenDivision::One; best.refs.resize (1); return best; }
    float bestScore = -1.0f;
    for (int s = 0; s < ScreenDivision::NumShapes; ++s)
    {
        if (ScreenDivision::cellsOf (s) != (int) wins.size()) continue;
        auto d = ScreenDivision::shape (s);
        // the windows in the order of the cells: each cell takes the unplaced window whose centre is nearest its own
        const auto cells = d.cells();
        std::vector<int> order; std::vector<bool> used (wins.size(), false);
        for (auto& c : cells)
        {
            int pick = -1; float bestDist = 1.0e9f;
            for (size_t w = 0; w < wins.size(); ++w)
            {
                if (used[w]) continue;
                const float dist = c.distanceOfCentres (wins[w].second);
                if (dist < bestDist) { bestDist = dist; pick = (int) w; }
            }
            used[(size_t) pick] = true; order.push_back (pick);
        }
        std::vector<FracRect> wanted;
        for (int i : order) wanted.push_back (wins[(size_t) i].second);
        d.fitTo (wanted);
        const float score = d.matchScore (wanted);
        if (score > bestScore)
        {
            bestScore = score;
            best = LayoutDef(); best.shape = s; best.fracs = d.fracs();
            for (int i : order) best.refs.push_back (WindowRef::of (wins[(size_t) i].first));
        }
    }
    return best;
}
} // namespace td
