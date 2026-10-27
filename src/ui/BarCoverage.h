#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
/** The bar-coverage map (3.4.0): a strip along the piece's bars, under the take window's head, showing how many takes cover each bar - dark
    where no take has it, brighter the more takes do, duds not counted - so the producer sees at a glance which passages still need
    another take. It is drawn from the bars each take says it starts and ends in (the bars and notes box: right-click a take); a take
    with no bars entered is not on the map. The playhead's take is outlined, the bar being searched for (the magnifying glass) marked.
    Click a bar: the takes that have it are framed and the playhead goes to it (where it is in the take is estimated from the take's
    bar range: evenly spread; the take matching will place it exactly once a take's bars have been found by matching). Shown only when
    at least one take has bars. */
class BarCoverageStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kHeight = 22;
    explicit BarCoverageStrip (AppContext& a) : app (a) { setWantsKeyboardFocus (false); }

    std::function<void (int bar)> onBarClicked;
    juce::Uuid windowId;
    int searchBar = 0;                         // the bar the magnifying glass is looking for (0 = none)

    /** True when the window has at least one take with bars entered: the strip is worth showing. */
    bool hasBars() const
    {
        if (auto* w = app.project.findTakeWindow (windowId))
            for (auto& g : w->groups) if (g.barIn > 0) return true;
        return false;
    }
    /** The bars the map runs over. */
    void range (int& first, int& last) const
    {
        first = 0; last = 0;
        if (auto* w = app.project.findTakeWindow (windowId))
            for (auto& g : w->groups)
                if (g.barIn > 0)
                {
                    const int out = juce::jmax (g.barIn, g.barOut);
                    if (first == 0 || g.barIn < first) first = g.barIn;
                    last = juce::jmax (last, out);
                }
    }
    int barAt (int x) const
    {
        int first, last; range (first, last);
        if (first == 0) return 0;
        const float cell = cellWidth (first, last);
        return juce::jlimit (first, last, first + (int) ((float) (x - kLeft) / cell));
    }

    void paint (juce::Graphics& g) override
    {
        auto* w = app.project.findTakeWindow (windowId);
        int first, last; range (first, last);
        g.fillAll (theme::panel);
        if (w == nullptr || first == 0) return;
        const float cell = cellWidth (first, last);
        // how many takes have each bar (duds not counted), and the playhead's take
        const int n = last - first + 1;
        std::vector<int> count ((size_t) n, 0);
        int most = 0;
        for (auto& g2 : w->groups)
            if (g2.barIn > 0 && ! g2.dud)
                for (int b = g2.barIn; b <= juce::jmax (g2.barIn, g2.barOut); ++b) if (b >= first && b <= last) most = juce::jmax (most, ++count[(size_t) (b - first)]);
        const TakeGroup* cur = w->takeAt (w->playheadSeconds);
        g.setFont (juce::FontOptions (9.5f));
        g.setColour (theme::dimText);
        g.drawText ("BARS", juce::Rectangle<int> (2, 0, kLeft - 4, getHeight()), juce::Justification::centredLeft, false);
        const auto area = juce::Rectangle<float> ((float) kLeft, 2.0f, (float) (getWidth() - kLeft - 2), (float) (getHeight() - 4));
        for (int b = first; b <= last; ++b)
        {
            const auto r = juce::Rectangle<float> (area.getX() + (float) (b - first) * cell, area.getY(), cell, area.getHeight());
            const int c = count[(size_t) (b - first)];
            // dark = nobody has it; the teal of the bar search, deeper the more takes have it
            const auto fill = c == 0 ? theme::field : juce::Colour (0xff19a08a).withAlpha (juce::jlimit (0.35f, 1.0f, 0.35f + 0.25f * (float) c));
            g.setColour (fill); g.fillRect (r.reduced (cell > 4.0f ? 0.5f : 0.0f, 0.0f));
            if (b == searchBar) { g.setColour (juce::Colours::white); g.drawRect (r.expanded (0.0f, 1.0f), 1.5f); }
        }
        // the playhead's take: its bars outlined
        if (cur != nullptr && cur->barIn > 0)
        {
            const int out = juce::jmax (cur->barIn, cur->barOut);
            const auto r = juce::Rectangle<float> (area.getX() + (float) (cur->barIn - first) * cell, area.getY(), (float) (out - cur->barIn + 1) * cell, area.getHeight());
            g.setColour (theme::playhead); g.drawRect (r, 1.5f);
        }
        // bar numbers: every few bars, as the room allows
        const int step = cell >= 28.0f ? 1 : cell >= 14.0f ? 2 : cell >= 6.0f ? 5 : cell >= 3.0f ? 10 : 20;
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        for (int b = first; b <= last; ++b)
            if (b == first || b % step == 0)
            {
                const auto r = juce::Rectangle<float> (area.getX() + (float) (b - first) * cell, area.getY(), juce::jmax (cell, 24.0f), area.getHeight());
                g.drawText (juce::String (b), r.toNearestInt(), juce::Justification::centredLeft, false);
            }
        g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int bar = barAt (e.x);
        if (bar <= 0) { setTooltip ({}); return; }
        int n = 0, duds = 0;
        if (auto* w = app.project.findTakeWindow (windowId))
            for (auto& g : w->groups) if (barsContain (g.barIn, g.barOut, bar)) { if (g.dud) ++duds; else ++n; }
        setTooltip ("Bar " + juce::String (bar) + ": " + juce::String (n) + (n == 1 ? " take" : " takes") + (duds > 0 ? " (and " + juce::String (duds) + (duds == 1 ? " dud)" : " duds)") : juce::String())
                    + "\nClick: frame the takes that have it and go there.");
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked()) return;
        const int bar = barAt (e.x);
        if (bar > 0 && onBarClicked) onBarClicked (bar);
    }
private:
    static constexpr int kLeft = 36;
    float cellWidth (int first, int last) const { return (float) juce::jmax (1, getWidth() - kLeft - 2) / (float) juce::jmax (1, last - first + 1); }
    AppContext& app;
};
} // namespace td
