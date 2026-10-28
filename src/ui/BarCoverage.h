#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
/** The no-take pinstripe (3.12.0): thin diagonal lines across a crimson area, like the stripe in a suit, so a bar with no take never looks like the
    bright red of a bar with one. The lines are placed by the component's own coordinates, so neighbouring cells and runs join up into one pattern. */
inline void drawNoTakeStripes (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour line = juce::Colours::white.withAlpha (0.55f),
                               float spacing = 7.0f, float thickness = 0.8f)
{
    if (r.isEmpty()) return;
    juce::Graphics::ScopedSaveState keep (g);
    juce::Path clip; clip.addRectangle (r);
    g.reduceClipRegion (clip);
    g.setColour (line);
    const float h = r.getHeight();
    for (float x = std::floor ((r.getX() - h) / spacing) * spacing; x < r.getRight(); x += spacing)
        g.drawLine (x, r.getBottom(), x + h, r.getY(), thickness);
}

/** The bar strip (3.4.0, always there since 3.8.0): a strip along the piece's bars, under the take window's head.
    Until the piece's bars are entered it is red: right-click it and type how many bars the piece has. Then it runs from bar 1 to the piece's last bar
    (or further, when a take's bars go beyond it) and shows how many takes cover each bar in the coverage colours (3.11.0: crimson no take - with a thin
    diagonal pinstripe since 3.12.0 - bright red one, red-orange two, darkish yellow three, green from four - light - to ten - dark), duds not counted - so the producer sees at a glance which passages still need another take. It is drawn from the bars each take says it starts
    and ends in (the bars and notes box: Ctrl+B, or double-click a take); a take with no bars entered is not on it. The playhead's take is outlined,
    the bar being searched for (the magnifying glass) marked. Click a bar: the takes that have it are framed and the playhead goes to it (where it is
    in the take is estimated from the take's bar range: evenly spread). The BARS label at its left end is a button: it drops the take chart down
    from that corner (see TakeChart.h). */
class BarCoverageStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kHeight = 22;
    static constexpr int kLeft = 56;           // the BARS button
    explicit BarCoverageStrip (AppContext& a) : app (a) { setWantsKeyboardFocus (false); }

    std::function<void (int bar)> onBarClicked;
    std::function<void()> onLabelClicked;      // BARS: the take chart down / up
    std::function<void()> onAskBars;           // right-click: how many bars the piece has
    juce::Uuid windowId;
    int searchBar = 0;                         // the bar the magnifying glass is looking for (0 = none)
    bool chartOpen = false;                    // the take chart is down (BARS drawn pressed)

    /** True once the piece's bars are entered (the strip shows the bars; before that it is red). */
    bool hasBars() const
    {
        auto* w = app.project.findTakeWindow (windowId);
        return w != nullptr && w->pieceBars > 0;
    }
    /** The bars the strip runs over: 1 to the piece's last bar (or the last bar any take reaches, when that is further). */
    void range (int& first, int& last) const
    {
        first = 0; last = 0;
        if (auto* w = app.project.findTakeWindow (windowId); w != nullptr && w->pieceBars > 0) { first = 1; last = coverage::lastBar (*w); }
    }
    int barAt (int x) const
    {
        int first, last; range (first, last);
        if (first == 0 || x < kLeft) return 0;
        const float cell = cellWidth (first, last);
        return juce::jlimit (first, last, first + (int) ((float) (x - kLeft) / cell));
    }

    void paint (juce::Graphics& g) override
    {
        auto* w = app.project.findTakeWindow (windowId);
        g.fillAll (theme::panel);
        // the BARS button: drops the take chart down from here
        {
            const auto b = juce::Rectangle<float> (1.0f, 1.0f, (float) kLeft - 4.0f, (float) getHeight() - 2.0f);
            g.setColour (chartOpen ? juce::Colour (0xff19a08a) : theme::field.brighter (0.15f));
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (theme::border.brighter (0.2f)); g.drawRoundedRectangle (b, 3.0f, 1.0f);
            g.setColour (chartOpen ? juce::Colours::black : theme::text);
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawText ("BARS", b.withTrimmedRight (12.0f).toNearestInt(), juce::Justification::centred, false);
            juce::Path tri;                                                    // a small triangle: it drops down (pointing up while it is down)
            const float tx = b.getRight() - 10.0f, ty = b.getCentreY();
            if (chartOpen) tri.addTriangle (tx, ty + 2.5f, tx + 6.0f, ty + 2.5f, tx + 3.0f, ty - 2.5f);
            else           tri.addTriangle (tx, ty - 2.5f, tx + 6.0f, ty - 2.5f, tx + 3.0f, ty + 2.5f);
            g.fillPath (tri);
        }
        const auto area = juce::Rectangle<float> ((float) kLeft, 2.0f, (float) (getWidth() - kLeft - 2), (float) (getHeight() - 4));
        int first, last; range (first, last);
        if (w == nullptr || first == 0)
        {   // the piece's bars are not known yet: a red strip that says what to do
            g.setColour (juce::Colour (0xffb3261e)); g.fillRect (area);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawText ("Right-click here to enter how many bars the piece has", area.reduced (8.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
            g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
            return;
        }
        const float cell = cellWidth (first, last);
        // how many takes have each bar (duds not counted), and the playhead's take
        const auto count = coverage::counts (*w);
        const TakeGroup* cur = w->takeAt (w->playheadSeconds);
        for (int b = first; b <= last; ++b)
        {
            const auto r = juce::Rectangle<float> (area.getX() + (float) (b - first) * cell, area.getY(), cell, area.getHeight());
            const int c = b - 1 < (int) count.size() ? count[(size_t) (b - 1)] : 0;
            // the coverage colours (3.11.0): crimson no take, bright red one, red-orange two, darkish yellow three, light to dark green from four
            const auto rgb = coverage::colourFor (c);
            g.setColour (juce::Colour ((juce::uint8) rgb.r, (juce::uint8) rgb.g, (juce::uint8) rgb.b)); g.fillRect (r.reduced (cell > 4.0f ? 0.5f : 0.0f, 0.0f));
        }
        // the bars with no take: the pinstripe over each run of them (3.12.0), then the searched bar's frame on top
        for (int b = first; b <= last;)
        {
            const auto cnt = [&] (int bar) { return bar - 1 < (int) count.size() ? count[(size_t) (bar - 1)] : 0; };
            if (cnt (b) > 0) { ++b; continue; }
            int e = b; while (e + 1 <= last && cnt (e + 1) == 0) ++e;
            drawNoTakeStripes (g, juce::Rectangle<float> (area.getX() + (float) (b - first) * cell, area.getY(), (float) (e - b + 1) * cell, area.getHeight()));
            b = e + 1;
        }
        if (searchBar >= first && searchBar <= last)
        {
            const auto r = juce::Rectangle<float> (area.getX() + (float) (searchBar - first) * cell, area.getY(), cell, area.getHeight());
            g.setColour (juce::Colours::white); g.drawRect (r.expanded (0.0f, 1.0f), 1.5f);
        }
        // the playhead's take: its bars outlined
        if (cur != nullptr && cur->barIn > 0)
        {
            const int out = juce::jmax (cur->barIn, cur->barOut);
            const auto r = juce::Rectangle<float> (area.getX() + (float) (cur->barIn - first) * cell, area.getY(), (float) (out - cur->barIn + 1) * cell, area.getHeight());
            g.setColour (theme::playhead); g.drawRect (r, 1.5f);
        }
        // bar numbers: every few bars, as the room allows
        const int step = cell >= 28.0f ? 1 : cell >= 14.0f ? 2 : cell >= 6.0f ? 5 : cell >= 3.0f ? 10 : cell >= 1.5f ? 20 : 50;
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        for (int b = first; b <= last; ++b)
            if (b == first || b % step == 0)
            {
                const auto r = juce::Rectangle<float> (area.getX() + (float) (b - first) * cell, area.getY(), juce::jmax (cell, 24.0f), area.getHeight());
                const int c = b - 1 < (int) count.size() ? count[(size_t) (b - 1)] : 0;
                g.setColour (coverage::isLight (coverage::colourFor (c)) ? juce::Colours::black.withAlpha (0.8f) : juce::Colours::white.withAlpha (0.9f));     // (dark numbers on the light greens)
                g.drawText (juce::String (b), r.toNearestInt(), juce::Justification::centredLeft, false);
            }
        g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (e.x < kLeft) { setTooltip (juce::String (chartOpen ? "Put the take chart away" : "The take chart: every take as a line along the piece's bars, and the bars with no take, only one, two or more")
                                       + "\nRight-click: how many bars the piece has."); return; }
        if (! hasBars()) { setTooltip ("Right-click (or click) to enter how many bars the piece has: this strip then shows which bars the takes cover."); return; }
        const int bar = barAt (e.x);
        if (bar <= 0) { setTooltip ({}); return; }
        int n = 0, duds = 0;
        if (auto* w = app.project.findTakeWindow (windowId))
            for (auto& g : w->groups) if (barsContain (g.barIn, g.barOut, bar)) { if (g.dud) ++duds; else ++n; }
        setTooltip ("Bar " + juce::String (bar) + ": " + juce::String (n) + (n == 1 ? " take" : " takes") + (duds > 0 ? " (and " + juce::String (duds) + (duds == 1 ? " dud)" : " duds)") : juce::String())
                    + "\nClick: frame the takes that have it and go there. Right-click: how many bars the piece has.");
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked()) return;
        if (e.mods.isPopupMenu()) { if (onAskBars) onAskBars(); return; }
        if (e.x < kLeft) { if (onLabelClicked) onLabelClicked(); return; }
        if (! hasBars()) { if (onAskBars) onAskBars(); return; }
        const int bar = barAt (e.x);
        if (bar > 0 && onBarClicked) onBarClicked (bar);
    }
private:
    float cellWidth (int first, int last) const { return (float) juce::jmax (1, getWidth() - kLeft - 2) / (float) juce::jmax (1, last - first + 1); }
    AppContext& app;
};
} // namespace td
