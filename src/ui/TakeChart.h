#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
/** The take chart (3.8.0): a panel that drops down from the BARS label at the left of a take window's bar strip (click BARS; click it again, Esc or
    Close to put it away) and shows at a glance which bars of the piece have been recorded. The piece's bars run across it (how many: right-click the
    bar strip, or the box at the top of the chart - 100 bars, and a take of bars 1 - 50 covers half the width); every take is a line of its own, in
    recording order, from its IN Bar to its OUT Bar (the bars and notes box: Ctrl+B, or double-click a take). Duds are drawn dimmed and never count. The
    ALL line at the bottom is brighter the more takes have a bar.
    The buttons No take / Only 1 / 2 takes / 3+ light those bars up across the chart, and the list underneath says where they are ("Bars 51 - 58: only
    take 3"); with no button on, the list has the whole piece, run by run. Click a take's line, the ALL line or a line of the list: the take window goes
    there (in that take, when a take's line was clicked). It never takes the keyboard (but for its own box): the take window's keys work while it is down. */
class TakeChartComponent : public juce::Component, private juce::ChangeListener, private juce::Timer, private juce::ListBoxModel
{
public:
    TakeChartComponent (AppContext& a, const juce::Uuid& wid) : app (a), windowId (wid), view (*this), corner (this, &limits)
    {
        addAndMakeVisible (barsCaption); addAndMakeVisible (barsBox); addAndMakeVisible (summary); addAndMakeVisible (view); addAndMakeVisible (list);
        addAndMakeVisible (closeButton); addAndMakeVisible (corner);
        closeButton.setWantsKeyboardFocus (false);
        closeButton.setTooltip ("Put the take chart away (or click BARS again, or Esc)");
        closeButton.onClick = [this] { if (onClose) onClose(); };
        limits.setMinimumSize (520, 300);
        barsCaption.setText ("Bars in the piece", juce::dontSendNotification);
        barsCaption.setColour (juce::Label::textColourId, theme::text);
        barsBox.setInputRestrictions (5, "0123456789");
        barsBox.setJustification (juce::Justification::centred);
        barsBox.setTooltip ("How many bars the piece has: the chart then runs from bar 1 to there, so the bars at the end that nobody has recorded show too. Empty: up to the last bar any take reaches.");
        barsBox.onReturnKey = [this] { applyBars(); if (onDoneTyping) onDoneTyping(); };
        barsBox.onFocusLost = [this] { applyBars(); };
        barsBox.onEscapeKey = [this] { showBarsBox(); if (onDoneTyping) onDoneTyping(); };
        const char* names[4] = { "No take", "Only 1", "2 takes", "3+" };
        const char* tips[4] = { "Light up the bars that no take has (duds do not count)", "Light up the bars that only one take has",
                                "Light up the bars that exactly two takes have", "Light up the bars that three or more takes have" };
        for (int i = 0; i < 4; ++i)
        {
            auto& b = shows[(size_t) i];
            b.setButtonText (names[i]); b.setTooltip (tips[i]);
            b.setClickingTogglesState (true);
            b.setColour (juce::TextButton::buttonOnColourId, colourFor (i).withAlpha (0.85f));
            b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            b.setWantsKeyboardFocus (false);
            b.onClick = [this] { refresh(); };
            addAndMakeVisible (b);
        }
        summary.setColour (juce::Label::textColourId, theme::dimText);
        summary.setFont (juce::FontOptions (12.5f));
        summary.setMinimumHorizontalScale (0.7f);
        list.setModel (this);
        list.setRowHeight (20);
        list.setColour (juce::ListBox::backgroundColourId, theme::field);
        list.setColour (juce::ListBox::outlineColourId, theme::border);
        list.setOutlineThickness (1);
        list.setWantsKeyboardFocus (false);
        view.setWantsKeyboardFocus (false);
        setWantsKeyboardFocus (false);
        app.project.addChangeListener (this);
        startTimer (500);                            // (a take still being recorded grows; its bars may be typed meanwhile)
        setSize (900, 520);
        showBarsBox();
        refresh();
    }
    ~TakeChartComponent() override { app.project.removeChangeListener (this); list.setModel (nullptr); }

    std::function<void (int bar, const juce::Uuid& take)> onGoTo;      // a click: the take window goes to that bar (in that take when it is not null)
    std::function<void()> onClose;                                      // Close pressed: put the chart away
    std::function<void()> onDoneTyping;                                 // Enter / Esc in its box: the keyboard goes back to the take window
    std::function<void()> onResized;                                    // the corner was dragged (the take window keeps the size)

    /** The four colours: none, one, two, three or more takes. */
    static juce::Colour colourFor (int category)
    {
        switch (category)
        {
            case 0:  return juce::Colour (0xffd94848);
            case 1:  return juce::Colour (0xffe8a33a);
            case 2:  return juce::Colour (0xffd9c94a);
            default: return juce::Colour (0xff3fae6a);
        }
    }
    static int categoryOf (int takes) { return juce::jlimit (0, 3, takes); }

    void resized() override
    {
        corner.setBounds (getWidth() - 16, getHeight() - 16, 16, 16);
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (26);
        closeButton.setBounds (top.removeFromRight (70).reduced (2, 0));
        barsCaption.setBounds (top.removeFromLeft (118));
        barsBox.setBounds (top.removeFromLeft (64).reduced (0, 1));
        top.removeFromLeft (16);
        for (auto& b : shows) { b.setBounds (top.removeFromLeft (78).reduced (2, 0)); }
        r.removeFromTop (4);
        summary.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        const int listH = juce::jlimit (60, 220, r.getHeight() / 3);
        list.setBounds (r.removeFromBottom (listH).withTrimmedRight (12));
        r.removeFromBottom (6);
        view.setBounds (r);
        if (onResized) onResized();
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::window);
        g.setColour (theme::border.brighter (0.3f)); g.drawRect (getLocalBounds(), 2);
    }

    /** Reads the take window again (its takes, their bars, the piece's bars) and redraws. */
    void refresh()
    {
        rows.clear(); runs.clear(); shown.clear(); noBars.clear();
        last = 0;
        if (auto* w = app.project.findTakeWindow (windowId))
        {
            last = coverage::lastBar (*w);
            for (auto& g : w->groups) { rows.push_back ({ g.id, g.number, g.barIn, juce::jmax (g.barIn, g.barOut), g.dud }); if (g.barIn <= 0) noBars.add (juce::String (g.number)); }
            std::stable_sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) { return a.number < b.number; });
            runs = coverage::runs (*w);
            counts = coverage::counts (*w);
        }
        else counts.clear();
        bool any = false;
        for (auto& b : shows) any = any || b.getToggleState();
        for (auto& rn : runs) if (! any || shows[(size_t) categoryOf (rn.count())].getToggleState()) shown.push_back (rn);
        // the summary: how many bars of each kind
        int k[4] = { 0, 0, 0, 0 };
        for (int c : counts) ++k[categoryOf (c)];
        juce::String s;
        if (last == 0) s = "No bars yet: type the piece's bars above, and each take's bars with Ctrl+B in the take window (or double-click a take).";
        else
        {
            s << last << " bars: " << k[0] << " with no take, " << k[1] << " with only one, " << k[2] << " with two, " << k[3] << " with three or more.";
            if (! noBars.isEmpty()) s << "  Takes with no bars entered: " << noBars.joinIntoString (", ") << ".";
        }
        summary.setText (s, juce::dontSendNotification);
        list.updateContent(); list.repaint();
        view.repaint();
    }

private:
    struct Row { juce::Uuid id; int number = 0, in = 0, out = 0; bool dud = false; };

    /** The chart itself: the bar ruler, a line per take, the ALL line. */
    struct View : public juce::Component, public juce::SettableTooltipClient
    {
        explicit View (TakeChartComponent& o) : owner (o) {}
        static constexpr int kLabelW = 84, kRulerH = 18, kAllH = 22;
        float cell() const { return (float) juce::jmax (1, getWidth() - kLabelW - 4) / (float) juce::jmax (1, owner.last); }
        float rowH() const { return juce::jlimit (6.0f, 24.0f, (float) (getHeight() - kRulerH - kAllH - 6) / (float) juce::jmax (1, (int) owner.rows.size())); }
        float xOf (int bar) const { return (float) kLabelW + (float) (bar - 1) * cell(); }
        int barAt (int x) const { return owner.last <= 0 ? 0 : juce::jlimit (1, owner.last, 1 + (int) ((float) (x - kLabelW) / cell())); }
        /** -1 the ruler, 0 .. rows-1 a take, -2 the ALL line, -3 nothing. */
        int rowAt (int y) const
        {
            if (y < kRulerH) return -1;
            const int allY = getHeight() - kAllH;
            if (y >= allY) return -2;
            const int i = (int) ((float) (y - kRulerH) / rowH());
            return juce::isPositiveAndBelow (i, (int) owner.rows.size()) ? i : -3;
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::field);
            g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
            const int last = owner.last;
            if (last <= 0)
            {
                g.setColour (theme::dimText); g.setFont (juce::FontOptions (14.0f));
                g.drawText ("No bars entered yet", getLocalBounds(), juce::Justification::centred, false);
                return;
            }
            const float cw = cell(), rh = rowH();
            const float right = xOf (last + 1);
            const float allY = (float) (getHeight() - kAllH);
            // the bars lit up by the buttons: bands from the ruler to the ALL line
            for (int b = 1; b <= last; ++b)
            {
                const int cat = categoryOf (owner.counts[(size_t) (b - 1)]);
                if (! owner.shows[(size_t) cat].getToggleState()) continue;
                g.setColour (colourFor (cat).withAlpha (0.30f));
                g.fillRect (juce::Rectangle<float> (xOf (b), (float) kRulerH, cw, allY - (float) kRulerH));
            }
            // the ruler: bar numbers as the room allows, and a faint line at each numbered bar
            const int step = cw >= 28.0f ? 1 : cw >= 14.0f ? 2 : cw >= 6.0f ? 5 : cw >= 3.0f ? 10 : cw >= 1.5f ? 20 : 50;
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            for (int b = 1; b <= last; ++b)
                if (b == 1 || b % step == 0)
                {
                    const float x = xOf (b);
                    g.setColour (theme::border.withAlpha (0.5f)); g.drawVerticalLine ((int) x, (float) kRulerH, allY);
                    if (x + 26.0f > (float) getWidth()) continue;                     // (a number that would be cut off at the right edge is left out)
                    g.setColour (theme::text.withAlpha (0.8f));
                    g.drawText (juce::String (b), juce::Rectangle<float> (x + 2.0f, 0.0f, 40.0f, (float) kRulerH), juce::Justification::centredLeft, false);
                }
            g.setColour (theme::border); g.drawVerticalLine ((int) right, (float) kRulerH, (float) getHeight());
            // the takes
            auto* w = owner.app.project.findTakeWindow (owner.windowId);
            const TakeGroup* cur = w != nullptr ? w->takeAt (w->playheadSeconds) : nullptr;
            const bool recording = owner.app.engine.isRecording();
            for (size_t i = 0; i < owner.rows.size(); ++i)
            {
                const auto& row = owner.rows[i];
                const float y = (float) kRulerH + (float) i * rh;
                const bool live = recording && row.id == owner.app.engine.currentTakeId();
                g.setColour (theme::text.withAlpha (row.dud ? 0.45f : 0.9f));
                g.setFont (juce::FontOptions (juce::jmin (12.5f, rh - 1.0f)));
                g.drawText ("Take " + juce::String (row.number) + (row.dud ? " (dud)" : live ? " REC" : ""), juce::Rectangle<float> (4.0f, y, (float) kLabelW - 6.0f, rh),
                            juce::Justification::centredLeft, false);
                if (row.in <= 0) continue;
                const auto r = juce::Rectangle<float> (xOf (row.in), y + 1.5f, (float) (row.out - row.in + 1) * cw, juce::jmax (2.0f, rh - 3.0f));
                g.setColour (row.dud ? juce::Colour (0xff6b7480).withAlpha (0.55f) : juce::Colour (0xff19a08a));
                g.fillRoundedRectangle (r, juce::jmin (3.0f, r.getHeight() * 0.3f));
                if (r.getWidth() > 60.0f && rh >= 12.0f)
                {
                    g.setColour (juce::Colours::white.withAlpha (row.dud ? 0.6f : 0.95f)); g.setFont (juce::FontOptions (juce::jmin (11.0f, rh - 3.0f), juce::Font::bold));
                    g.drawText (barsLabel (row.in, row.out), r.reduced (4.0f, 0.0f), juce::Justification::centredLeft, false);
                }
                if (live) { g.setColour (juce::Colour (0xffff5555)); g.drawRoundedRectangle (r, 3.0f, 1.5f); }
                else if (cur != nullptr && cur->id == row.id) { g.setColour (theme::playhead); g.drawRoundedRectangle (r.expanded (0.5f), 3.0f, 1.5f); }
            }
            // ALL: how many takes have each bar (dark: none; brighter the more)
            g.setColour (theme::text.withAlpha (0.9f)); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("ALL", juce::Rectangle<float> (4.0f, allY, (float) kLabelW - 6.0f, (float) kAllH), juce::Justification::centredLeft, false);
            for (int b = 1; b <= last; ++b)
            {
                const int c = owner.counts[(size_t) (b - 1)];
                const auto r = juce::Rectangle<float> (xOf (b), allY + 3.0f, cw, (float) kAllH - 6.0f);
                g.setColour (c == 0 ? colourFor (0).withAlpha (0.55f) : juce::Colour (0xff19a08a).withAlpha (juce::jlimit (0.35f, 1.0f, 0.35f + 0.22f * (float) c)));
                g.fillRect (r.reduced (cw > 4.0f ? 0.5f : 0.0f, 0.0f));
            }
        }
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int bar = barAt (e.x);
            if (bar <= 0 || e.x < kLabelW) { setTooltip ({}); return; }
            const int c = owner.counts[(size_t) (bar - 1)];
            juce::String t = "Bar " + juce::String (bar) + ": " + (c == 0 ? juce::String ("no take") : c == 1 ? juce::String ("one take") : juce::String (c) + " takes");
            for (auto& rn : owner.runs) if (bar >= rn.from && bar <= rn.to) { t = coverage::describe (rn); break; }
            setTooltip (t + "\nClick: go there in the take window.");
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mouseWasClicked() || e.x < kLabelW) return;
            const int bar = barAt (e.x);
            if (bar <= 0 || ! owner.onGoTo) return;
            const int row = rowAt (e.y);
            juce::Uuid take;
            if (row >= 0 && barsContain (owner.rows[(size_t) row].in, owner.rows[(size_t) row].out, bar)) take = owner.rows[(size_t) row].id;
            owner.onGoTo (bar, take);
        }
        TakeChartComponent& owner;
    };

    // the list
    int getNumRows() override { return (int) shown.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) shown.size())) return;
        const auto& rn = shown[(size_t) row];
        if (selected) g.fillAll (theme::accent.withAlpha (0.25f));
        g.setColour (colourFor (categoryOf (rn.count())));
        g.fillRoundedRectangle (6.0f, 4.0f, 12.0f, (float) h - 8.0f, 2.0f);
        g.setColour (theme::text); g.setFont (juce::FontOptions (13.0f));
        g.drawText (coverage::describe (rn), juce::Rectangle<int> (26, 0, w - 30, h), juce::Justification::centredLeft, true);
    }
    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow (row, (int) shown.size()) || ! onGoTo) return;
        const auto& rn = shown[(size_t) row];
        juce::Uuid take;
        if (rn.count() == 1) for (auto& r : rows) if (r.number == rn.takes.front() && ! r.dud) { take = r.id; break; }
        onGoTo (rn.from, take);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { if (! barsBox.hasKeyboardFocus (true)) showBarsBox(); refresh(); }
    void timerCallback() override { if (app.engine.isRecording()) view.repaint(); }
    void showBarsBox()
    {
        const auto* w = app.project.findTakeWindow (windowId);
        barsBox.setText (w != nullptr && w->pieceBars > 0 ? juce::String (w->pieceBars) : juce::String(), juce::dontSendNotification);
    }
    void applyBars()
    {
        auto* w = app.project.findTakeWindow (windowId);
        if (w == nullptr) return;
        const int n = juce::jlimit (0, 99999, barsBox.getText().getIntValue());
        if (n == w->pieceBars) return;
        w->pieceBars = n;
        app.project.changed();                                             // (the take window's strip follows; saved with the window; an undo step)
    }

    AppContext& app;
    juce::Uuid windowId;
    juce::Label barsCaption, summary;
    juce::TextEditor barsBox;
    std::array<juce::TextButton, 4> shows;
    juce::TextButton closeButton { "Close" };
    View view;
    juce::ComponentBoundsConstrainer limits;
    juce::ResizableCornerComponent corner;
    juce::ListBox list;
    std::vector<Row> rows;
    std::vector<coverage::Run> runs, shown;
    std::vector<int> counts;
    juce::StringArray noBars;
    int last = 0;
};
} // namespace td
