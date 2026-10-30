#pragma once
#include "AppContext.h"
#include "RecordMonitor.h"

namespace td
{
/** 3.18.0: what the take matching is doing, in words - for the MATCHING box in the main window, the lamp in each take window and the panel.
    The take matching (TakeMatchService) analyses takes so that J and Find other versions can find the same music in other takes. It runs only for
    the take windows switched on, never while Rec mode is on, and not while paused. */
namespace takematchui
{
    inline juce::String timeLeftText (double seconds)
    {
        if (seconds < 0.0) return "working out the time left";
        if (seconds < 60.0) return "under a minute left";
        const int mins = juce::roundToInt (seconds / 60.0);
        if (mins < 60) return "about " + juce::String (mins) + " min left";
        return "about " + juce::String (mins / 60) + " h " + juce::String (mins % 60).paddedLeft ('0', 2) + " min left";
    }
    /** The take being analysed now: "Ave Verum, 012 - Take", or empty. */
    inline juce::String nowTakeName (AppContext& app, const juce::Uuid& takeId)
    {
        if (takeId.isNull()) return {};
        for (auto& w : app.project.takeWindows)
            if (auto* g = w->findGroup (takeId)) return w->name + ", " + w->displayName (*g);
        return {};
    }
    enum class Look { Off, Working, Held, Ready };
    inline juce::Colour colourOf (Look l)
    {
        switch (l)
        {
            case Look::Working: return juce::Colour (0xff2a9cc0);
            case Look::Held:    return juce::Colour (0xffe0a040);
            case Look::Ready:   return juce::Colour (0xff2e9e4f);
            case Look::Off:     break;
        }
        return theme::dimText;
    }
    /** The MATCHING box in the main window: "OFF", "12 / 26", "PAUSED 12 / 26", "HELD - REC 12 / 26", "READY 26". */
    inline juce::String boxText (AppContext& app, Look& look)
    {
        const auto pr = app.takeMatch.progress();
        if (pr.windowsOn == 0) { look = Look::Off; return "OFF"; }
        const juce::String counts = juce::String (pr.ready + pr.cannot) + " / " + juce::String (pr.total);
        if (pr.left() == 0) { look = Look::Ready; return "READY " + juce::String (pr.total); }
        if (app.recMode()) { look = Look::Held; return "HELD - REC " + counts; }
        if (app.takeMatch.isPaused()) { look = Look::Held; return "PAUSED " + counts; }
        look = Look::Working; return counts;
    }
}

/** The panel the MATCHING box opens: Pause / Resume, where it has got and roughly how long is left, and a switch for every take window. */
class TakeMatchPanel : public juce::Component, private juce::Timer
{
public:
    explicit TakeMatchPanel (AppContext& a) : app (a)
    {
        title.setText ("Take matching", juce::dontSendNotification);
        title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        status.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        about.setFont (juce::FontOptions (12.0f));
        about.setText ("Take matching analyses every take of a piece so that J and Find other versions can find the same music in its other takes. "
                       "It runs only for the take windows switched on below - switch it on for the pieces you are editing - and never while Rec is on. "
                       "What it has done is kept in the project's Analysis folder.", juce::dontSendNotification);
        about.setJustificationType (juce::Justification::topLeft);
        for (auto* l : { &title, &status, &about }) { l->setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); }
        addAndMakeVisible (pause);
        pause.onClick = [this] { app.takeMatch.setPaused (! app.takeMatch.isPaused()); refresh(); };
        pause.setTooltip ("Stops the analysis at once (the take under way starts again first when you resume). A restart of Fermata resumes it.");
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&list, false);
        viewport.setScrollBarsShown (true, false);
        rebuild();
        setSize (540, 150 + juce::jmin (12, (int) rows.size()) * kRowH + (rows.empty() ? kRowH : 0));
        startTimerHz (2);
        refresh();
    }
    ~TakeMatchPanel() override { viewport.setViewedComponent (nullptr, false); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto top = r.removeFromTop (26);
        pause.setBounds (top.removeFromRight (110));
        title.setBounds (top);
        status.setBounds (r.removeFromTop (24));
        about.setBounds (r.removeFromTop (52));
        r.removeFromTop (6);
        viewport.setBounds (r);
        list.setSize (juce::jmax (100, r.getWidth() - (rows.size() > 12 ? viewport.getScrollBarThickness() : 0)), juce::jmax (kRowH, (int) rows.size() * kRowH));
        int y = 0;
        for (auto& row : rows)
        {
            auto line = juce::Rectangle<int> (0, y, list.getWidth(), kRowH).reduced (0, 2);
            row->toggle.setBounds (line.removeFromLeft (64));
            line.removeFromLeft (8);
            row->counts.setBounds (line.removeFromRight (190));
            row->name.setBounds (line);
            y += kRowH;
        }
        none.setBounds (0, 0, list.getWidth(), kRowH);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

private:
    static constexpr int kRowH = 28;
    struct Row
    {
        juce::Uuid id;
        juce::TextButton toggle;
        juce::Label name, counts;
    };
    void timerCallback() override { refresh(); }
    /** The rows, one per take window (made again when windows come or go). */
    void rebuild()
    {
        rows.clear();
        list.removeAllChildren();
        shownIds.clear();
        for (auto& w : app.project.takeWindows)
        {
            auto row = std::make_unique<Row>();
            row->id = w->id;
            shownIds.push_back (w->id);
            row->toggle.setClickingTogglesState (false);
            row->toggle.setColour (juce::TextButton::buttonOnColourId, takematchui::colourOf (takematchui::Look::Working));
            row->toggle.setTooltip ("Switch take matching on or off for this take window");
            const auto id = w->id;
            row->toggle.onClick = [this, id]
            {
                if (auto* tw = app.project.findTakeWindow (id)) app.setTakeMatching (id, ! tw->takeMatching);
                refresh();
            };
            for (auto* l : { &row->name, &row->counts }) { l->setColour (juce::Label::textColourId, theme::text); l->setFont (juce::FontOptions (13.0f)); }
            row->counts.setJustificationType (juce::Justification::centredRight);
            for (auto* c : std::initializer_list<juce::Component*> { &row->toggle, &row->name, &row->counts }) list.addAndMakeVisible (c);
            rows.push_back (std::move (row));
        }
        none.setText ("This project has no take windows yet.", juce::dontSendNotification);
        none.setColour (juce::Label::textColourId, theme::dimText);
        if (rows.empty()) list.addAndMakeVisible (none);
        resized();
    }
    void refresh()
    {
        {   // a take window made or deleted since: the rows again
            std::vector<juce::Uuid> ids;
            for (auto& w : app.project.takeWindows) ids.push_back (w->id);
            if (ids != shownIds) rebuild();
        }
        auto& tm = app.takeMatch;
        const auto pr = tm.progress();
        juce::String s;
        juce::Colour c = theme::text;
        if (pr.windowsOn == 0) s = "Off: no take window has it switched on.";
        else if (pr.left() == 0) { s = "Ready: every take of the windows switched on is analysed (" + juce::String (pr.ready) + ")."; c = takematchui::colourOf (takematchui::Look::Ready); }
        else if (app.recMode()) { s = "Held while Rec is on: " + juce::String (pr.left()) + " takes still to do. It carries on by itself when Rec is switched off."; c = theme::warn; }
        else if (tm.isPaused()) { s = "Paused: " + juce::String (pr.ready + pr.cannot) + " of " + juce::String (pr.total) + " takes done, " + juce::String (pr.left()) + " to do."; c = theme::warn; }
        else
        {
            const auto now = takematchui::nowTakeName (app, pr.nowTake);
            s = juce::String (pr.ready + pr.cannot) + " of " + juce::String (pr.total) + " takes done - " + takematchui::timeLeftText (pr.secondsLeft)
              + (now.isNotEmpty() ? "  (now: " + now + ", " + juce::String (juce::roundToInt (pr.nowProgress * 100.0f)) + " %)" : juce::String());
        }
        status.setText (s, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, c);
        pause.setButtonText (tm.isPaused() ? "Resume" : "Pause");
        pause.setEnabled (pr.windowsOn > 0 || tm.isPaused());
        for (auto& row : rows)
        {
            auto* w = app.project.findTakeWindow (row->id);
            if (w == nullptr) continue;
            row->toggle.setToggleState (w->takeMatching, juce::dontSendNotification);
            row->toggle.setButtonText (w->takeMatching ? "ON" : "OFF");
            row->name.setText (w->name, juce::dontSendNotification);
            const auto wp = tm.progress (w);
            juce::String t;
            if (wp.total == 0) t = "no takes yet";
            else
            {
                t = juce::String (wp.ready) + " / " + juce::String (wp.total) + " analysed";
                if (wp.cannot > 0) t << " (" << wp.cannot << " cannot be)";
            }
            row->counts.setText (t, juce::dontSendNotification);
            row->counts.setColour (juce::Label::textColourId, w->takeMatching ? theme::text : theme::dimText);
            row->name.setColour (juce::Label::textColourId, w->takeMatching ? theme::text : theme::dimText);
        }
    }

    AppContext& app;
    juce::Label title, status, about, none;
    juce::TextButton pause { "Pause" };
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<juce::Uuid> shownIds;
};

inline void showTakeMatchPanel (AppContext& app, juce::Component& anchor)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<TakeMatchPanel> (app), anchor.getScreenBounds(), nullptr);
}

/** The take matching lamp in a take window's marks row, beside the recording lamps: MATCHING over OFF / 12 / 26 / 45 % / PAUSED / HELD / READY.
    A click switches take matching on or off for this window; a right-click opens the panel. */
class TakeMatchLamp : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    TakeMatchLamp (AppContext& a, const juce::Uuid& windowId) : app (a), id (windowId)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
        setTooltip ("Take matching for this take window (needed by J and Find other versions): click to switch it on or off. It analyses this window's takes "
                    "in the background - never while Rec is on. Right-click: how far it has got, Pause, and every window's switch.");
        startTimerHz (2);
        timerCallback();
    }
    static int idealWidth (int height) { return juce::jmax (78, height * 3); }
    void paint (juce::Graphics& g) override
    {
        auto box = getLocalBounds().toFloat();
        const auto fill = look == takematchui::Look::Off ? theme::field : takematchui::colourOf (look);
        g.setColour (fill); g.fillRoundedRectangle (box, 3.0f);
        g.setColour (theme::border); g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (look == takematchui::Look::Off ? theme::dimText : look == takematchui::Look::Held ? juce::Colour (0xff1a1200) : juce::Colours::white);
        auto inner = box.reduced (2.0f, 1.0f);
        if (box.getHeight() >= 19.0f) paintLampWords (g, box, "MATCHING", word);       // (the same writing as MAIN / 2ND / DEVICE beside it)
        else
        {
            g.setFont (juce::FontOptions (juce::jmin (11.0f, box.getHeight() * 0.5f), juce::Font::bold));
            g.drawFittedText ("MATCH  " + word, inner.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showTakeMatchPanel (app, *this); return; }
        if (auto* w = app.project.findTakeWindow (id)) app.setTakeMatching (id, ! w->takeMatching);
        timerCallback();
    }
private:
    void timerCallback() override
    {
        auto* w = app.project.findTakeWindow (id);
        juce::String wd; auto lk = takematchui::Look::Off;
        if (w != nullptr && w->takeMatching)
        {
            const auto pr = app.takeMatch.progress (w);
            if (pr.left() == 0) { lk = takematchui::Look::Ready; wd = "READY"; }
            else if (app.recMode()) { lk = takematchui::Look::Held; wd = "HELD (REC)"; }
            else if (app.takeMatch.isPaused()) { lk = takematchui::Look::Held; wd = "PAUSED"; }
            else
            {
                lk = takematchui::Look::Working;
                wd = juce::String (pr.ready + pr.cannot) + " / " + juce::String (pr.total);
                if (! pr.nowTake.isNull()) wd << "  " << juce::roundToInt (pr.nowProgress * 100.0f) << "%";
            }
        }
        else wd = "OFF";
        if (wd != word || lk != look) { word = wd; look = lk; repaint(); }
    }
    AppContext& app;
    juce::Uuid id;
    juce::String word { "OFF" };
    takematchui::Look look = takematchui::Look::Off;
};
} // namespace td
