#pragma once
#include "AppContext.h"
#include <limits>

namespace td
{
/** The little mixer for changing the volume inside a grouped piece (like Pyramix's and SADiE's clip volume): one fader per file of the piece.
    The change starts at the point you right-clicked (or at the start of the piece), and can glide over 0.1 - 1.0 s instead of jumping. */
class VolumeChangePanel : public juce::Component
{
public:
    VolumeChangePanel (AppContext& a, const juce::Uuid& edit, const juce::Uuid& region, juce::int64 relSample, std::function<void()> close)
        : app (a), editId (edit), regionId (region), rel (relSample), closeFn (std::move (close))
    {
        auto* e = app.project.findEdit (editId);
        auto* r = e ? e->findAny (regionId) : nullptr;
        if (r == nullptr) { setSize (10, 10); return; }
        const double rate = juce::jmax (1.0, r->sampleRate);
        existing = false;
        const GainChange* ex = nullptr;
        for (auto& g : r->gains) if (std::abs (g.at - rel) <= (juce::int64) (0.001 * rate)) { existing = true; ex = &g; rel = g.at; initialRamp = g.ramp; }

        const int n = (int) r->files.size();
        for (int i = 0; i < n; ++i)
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow);
            setupFader (*s);
            s->setValue (juce::jlimit (-100.0f, 12.0f, ex != nullptr && (size_t) i < ex->db.size() ? ex->db[(size_t) i] : r->levelDbAt ((size_t) i, rel)), juce::dontSendNotification);
            addAndMakeVisible (*s);
            auto l = std::make_unique<juce::Label>();
            l->setText (r->files[(size_t) i].trackName, juce::dontSendNotification);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, chan::of (app.project, r->files[(size_t) i].trackId));
            l->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            addAndMakeVisible (*l);
            faders.push_back (std::move (s)); labels.push_back (std::move (l));
        }
        allFader.reset (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow));
        setupFader (*allFader);
        allFader->setRange (-24.0, 24.0, 0.1); allFader->setSkewFactor (1.0);
        allFader->setValue (0.0, juce::dontSendNotification);
        allFader->setTooltip ("Moves every fader up or down together, keeping the balance between them");
        allFader->onValueChange = [this]
        {
            const double d = allFader->getValue() - lastAll; lastAll = allFader->getValue();
            for (auto& f : faders) f->setValue (juce::jlimit (-100.0, 12.0, f->getValue() + d), juce::dontSendNotification);
        };
        addAndMakeVisible (*allFader);
        allLabel.setText ("All", juce::dontSendNotification); allLabel.setJustificationType (juce::Justification::centred);
        allLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold)); addAndMakeVisible (allLabel);

        title.setText ("Volume change at " + formatTimeShort ((double) (r->startSample + rel) / rate, 2) + "  -  " + r->takeName, juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold)); addAndMakeVisible (title);

        fadeBox.setButtonText ("Fade the change");
        fadeBox.setTooltip ("Off: the level jumps at once. On: it glides over the time on the right (a short fade sounds more natural)");
        fadeBox.setToggleState (existing ? initialRamp > 0.0 : true, juce::dontSendNotification);
        addAndMakeVisible (fadeBox);
        for (int i = 1; i <= 10; ++i) fadeLen.addItem (juce::String (i / 10.0, 1) + " s", i);
        fadeLen.setSelectedId (juce::jlimit (1, 10, (int) std::lround ((existing && initialRamp > 0.0 ? initialRamp : 0.3) * 10.0)), juce::dontSendNotification);
        addAndMakeVisible (fadeLen);
        wholeBox.setButtonText ("Between fades: the whole take (from its start)");
        wholeBox.setTooltip ("Set the level for the whole piece, instead of starting the change at the point you clicked");
        wholeBox.setToggleState (false, juce::dontSendNotification);
        wholeBox.onClick = [this] { fadeBox.setEnabled (! wholeBox.getToggleState()); fadeLen.setEnabled (! wholeBox.getToggleState()); };
        if (rel <= 0) { wholeBox.setToggleState (true, juce::dontSendNotification); wholeBox.onClick(); }
        addAndMakeVisible (wholeBox);

        apply.setButtonText ("Apply"); cancel.setButtonText ("Cancel"); remove.setButtonText ("Remove this change");
        remove.setEnabled (existing);
        apply.onClick  = [this] { doApply(); };
        cancel.onClick = [this] { if (closeFn) closeFn(); };
        remove.onClick = [this] { doRemove(); };
        for (auto* b : { &apply, &cancel, &remove }) addAndMakeVisible (b);
        apply.setColour (juce::TextButton::buttonColourId, theme::accent);

        setSize (juce::jmax (400, 70 * (n + 1) + 30), 420);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (20));
        auto bottom = r.removeFromBottom (32);
        const int bw = bottom.getWidth() / 3;
        apply.setBounds (bottom.removeFromLeft (bw).reduced (2)); cancel.setBounds (bottom.removeFromLeft (bw).reduced (2)); remove.setBounds (bottom.reduced (2));
        r.removeFromBottom (6);
        auto opts = r.removeFromBottom (54);
        auto row1 = opts.removeFromTop (26);
        fadeLen.setBounds (row1.removeFromRight (80).reduced (0, 2));
        fadeBox.setBounds (row1);
        wholeBox.setBounds (opts);
        r.removeFromBottom (4);
        const int n = (int) faders.size() + 1;
        const int w = r.getWidth() / juce::jmax (1, n);
        for (size_t i = 0; i < faders.size(); ++i)
        {
            auto col = r.removeFromLeft (w);
            labels[i]->setBounds (col.removeFromTop (18)); faders[i]->setBounds (col.reduced (6, 0));
        }
        allLabel.setBounds (r.removeFromTop (18)); allFader->setBounds (r.reduced (6, 0));
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

private:
    static void setupFader (juce::Slider& s)
    {
        s.setRange (-100.0, 12.0, 0.1);
        s.setSkewFactorFromMidPoint (-20.0);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 20);
        s.setTextValueSuffix (" dB");
        s.textFromValueFunction = [] (double v) { return v <= -99.9 ? juce::String ("-inf") : juce::String (v, 1); };
        s.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -100.0 : t.getDoubleValue(); };
    }

    void doApply()
    {
        auto* e = app.project.findEdit (editId);
        auto* r = e ? e->findAny (regionId) : nullptr;
        if (r == nullptr) { if (closeFn) closeFn(); return; }
        std::vector<float> db;
        for (auto& f : faders) db.push_back ((float) f->getValue());
        const bool whole = wholeBox.getToggleState();
        if (whole)                                                     // the whole piece: one level from its start, earlier changes in it are dropped
        {
            r->gains.clear();
            r->setGainChange (0, 0.0, db);
        }
        else r->setGainChange (rel, fadeBox.getToggleState() ? fadeLen.getSelectedId() / 10.0 : 0.0, db);
        app.project.changed();
        if (closeFn) closeFn();
    }

    void doRemove()
    {
        auto* e = app.project.findEdit (editId);
        auto* r = e ? e->findAny (regionId) : nullptr;
        if (r != nullptr && r->removeGainChangeAt (rel)) app.project.changed();
        if (closeFn) closeFn();
    }

    AppContext& app; juce::Uuid editId, regionId; juce::int64 rel; std::function<void()> closeFn;
    bool existing = false; double initialRamp = 0.0, lastAll = 0.0;
    std::vector<std::unique_ptr<juce::Slider>> faders; std::vector<std::unique_ptr<juce::Label>> labels;
    std::unique_ptr<juce::Slider> allFader; juce::Label allLabel, title;
    juce::ToggleButton fadeBox, wholeBox; juce::ComboBox fadeLen;
    juce::TextButton apply, cancel, remove;
};

/** Opens the volume mixer for a piece in its own small window. */
inline void showVolumeChange (AppContext& app, const juce::Uuid& editId, const juce::Uuid& regionId, juce::int64 relSample)
{
    struct Holder { juce::DialogWindow* w = nullptr; };
    auto holder = std::make_shared<Holder>();
    auto panel = std::make_unique<VolumeChangePanel> (app, editId, regionId, relSample, [holder]
    {
        if (holder->w) { holder->w->exitModalState (0); }
    });
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Change volume";
    o.content.setOwned (panel.release());
    o.componentToCentreAround = nullptr;
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = false; o.useBottomRightCornerResizer = false;
    holder->w = o.launchAsync();
}

/** A volume change over a stretch of time (the area between marks 1 and 2) for the tracks that are marked (all of them, or just those chosen with Ctrl + Alt + drag).
    Each track has a fader from +30 dB (top) through 0 (no change, middle) down to -inf (bottom). The level glides to the new value over the fade-in time at the start of
    the area and glides back to where it was over the fade-out time at the end of it.
    3.18.5: the change is heard at once (Play in the box plays it), and it can be opened again later (double-click or right-click its teal line in the edit window) to
    change the amounts and the fade times: its points share a group (GainChange::group / role). While the box is open the undo history waits, so Done is one
    undo step and Cancel leaves no trace. */
class RangeVolumePanel : public juce::Component, private juce::Timer
{
public:
    /** A new change over [from, to) of the edit's timeline. */
    RangeVolumePanel (AppContext& a, const juce::Uuid& edit, juce::int64 from, juce::int64 to, std::function<void()> close)
        : app (a), editId (edit), t0 (from), t1 (to), closeFn (std::move (close))
    {
        auto* e = app.project.findEdit (editId);
        if (e == nullptr) { setSize (10, 10); return; }
        begin (*e);
        for (auto& t : app.project.tracks)                       // the tracks that have audio in the area and are marked
        {
            bool has = false;
            if (e->fixUses (t.id))
                for (auto& r : e->regions) if (r.endSample() > t0 && r.startSample < t1) for (auto& f : r.files) if (f.trackId == t.id) has = true;
            if (has) { tracksUsed.push_back (t.id); startDb.push_back (0.0); }
        }
        build (6, 6);
        info.setText ("0 = no change. The level glides to the new value over the fade-in time, and back over the fade-out time. You hear each change at once: "
                      "press Play to listen. Earlier volume changes inside this area are replaced.", juce::dontSendNotification);
    }

    /** 3.18.5: a change made before, opened again by its group. */
    RangeVolumePanel (AppContext& a, const juce::Uuid& edit, const juce::String& groupId, std::function<void()> close)
        : app (a), editId (edit), group (groupId), closeFn (std::move (close))
    {
        auto* e = app.project.findEdit (editId);
        if (e == nullptr) { setSize (10, 10); return; }
        begin (*e);
        double fin = 0.3, fout = 0.3; bool haveStart = false, haveEnd = false;
        t0 = std::numeric_limits<juce::int64>::max(); t1 = 0;
        for (auto& r : e->regions)
            for (size_t i = 0; i < r.gains.size(); ++i)
            {
                const auto& g = r.gains[i];
                if (g.group != group) continue;
                const auto rampS = (juce::int64) std::llround (g.ramp * rate);
                if (g.role == 3) { t1 = juce::jmax (t1, r.startSample + g.at + rampS); if (! haveEnd) { fout = g.ramp; haveEnd = true; } continue; }
                t0 = juce::jmin (t0, r.startSample + g.at);
                if (g.role == 1 && ! haveStart) { fin = g.ramp; haveStart = true; }
                if (! haveEnd) t1 = juce::jmax (t1, r.endSample());
                for (size_t k = 0; k < r.files.size() && k < g.db.size(); ++k)          // what this change does to each track: its level less the one before it
                {
                    const auto tid = r.files[k].trackId;
                    if (std::find (tracksUsed.begin(), tracksUsed.end(), tid) != tracksUsed.end()) continue;
                    const float before = juce::Decibels::gainToDecibels (r.levelAfter (k, i, g.at), -100.0f);
                    const double d = g.db[k] <= -99.9f ? -100.0 : (double) (g.db[k] - before);
                    if (std::abs (d) > 0.05) { tracksUsed.push_back (tid); startDb.push_back (d); }
                }
            }
        if (t0 == std::numeric_limits<juce::int64>::max()) t0 = 0;
        const auto toId = [] (double sec) { return juce::jlimit (1, 20, (int) std::lround (sec * 20.0)); };
        build (toId (fin), toId (fout));
        info.setText ("Change the amounts or the fade times: you hear each change at once (press Play to listen). Done keeps it; Cancel puts it back as it was.",
                      juce::dontSendNotification);
        remove.setButtonText ("Remove this change"); addAndMakeVisible (remove);
        remove.onClick = [this] { removeAll(); };
        resized();                                               // (the Remove button has a place now)
    }

    ~RangeVolumePanel() override
    {
        stopTimer();
        if (begun)
        {
            if (! done)                                            // Cancel, Escape or the window's close button: as it was before the box opened
                if (auto* e = app.project.findEdit (editId)) { e->regions = before; app.project.changed(); }
            if (playing) app.stopPlayback();
            app.undoHold -= 1;
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (20));
        info.setBounds (r.removeFromTop (34));
        auto bottom = r.removeFromBottom (32);
        done_.setBounds (bottom.removeFromRight (100).reduced (2)); cancel.setBounds (bottom.removeFromRight (100).reduced (2));
        if (remove.isVisible()) remove.setBounds (bottom.removeFromRight (150).reduced (2));
        play.setBounds (bottom.removeFromLeft (90).reduced (2));
        r.removeFromBottom (4);
        auto fades = r.removeFromBottom (28);
        fadeInCap.setBounds (fades.removeFromLeft (60)); fadeIn.setBounds (fades.removeFromLeft (90).reduced (0, 3));
        fades.removeFromLeft (16);
        fadeOutCap.setBounds (fades.removeFromLeft (66)); fadeOut.setBounds (fades.removeFromLeft (90).reduced (0, 3));
        r.removeFromBottom (6);
        auto allCol = r.removeFromRight (kColW);
        allLabel.setBounds (allCol.removeFromTop (18)); allFader->setBounds (allCol.reduced (6, 0).withTrimmedBottom (14));
        viewport.setBounds (r);
        const int h = juce::jmax (100, viewport.getMaximumVisibleHeight());
        content.setSize (juce::jmax (r.getWidth(), (int) faders.size() * kColW), h);
        for (size_t i = 0; i < faders.size(); ++i)
        {
            auto col = juce::Rectangle<int> ((int) i * kColW, 0, kColW, h);
            labels[i]->setBounds (col.removeFromTop (18)); faders[i]->setBounds (col.reduced (6, 0));
        }
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

private:
    static constexpr int kColW = 74;
    static void setupFader (juce::Slider& s)
    {
        s.setRange (-100.0, 30.0, 0.1);
        s.setSkewFactorFromMidPoint (0.0);                      // 0 dB (no change) is in the middle of the fader: +30 at the top, -inf at the bottom
        s.setDoubleClickReturnValue (true, 0.0);
        s.setValue (0.0, juce::dontSendNotification);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 20);
        s.setTextValueSuffix (" dB");
        s.textFromValueFunction = [] (double v) { return v <= -99.9 ? juce::String ("-inf") : (v > 0 ? "+" : "") + juce::String (v, 1); };
        s.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -100.0 : t.getDoubleValue(); };
    }

    void begin (EditDef& e)
    {
        rate = juce::jmax (1.0, e.sampleRate);
        before = e.regions;                                      // (Cancel puts this back)
        app.undoHold += 1;                                       // (one undo step for whatever is done in the box)
        begun = true;
    }

    void build (int fadeInId, int fadeOutId)
    {
        const int n = (int) tracksUsed.size();
        for (int i = 0; i < n; ++i)
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow);
            setupFader (*s);
            s->setValue (startDb[(size_t) i], juce::dontSendNotification);
            s->onValueChange = [this] { pending = true; };
            content.addAndMakeVisible (*s);
            auto l = std::make_unique<juce::Label>();
            l->setText (app.project.nodeName (tracksUsed[(size_t) i]), juce::dontSendNotification);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, chan::of (app.project, tracksUsed[(size_t) i]));
            l->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            content.addAndMakeVisible (*l);
            faders.push_back (std::move (s)); labels.push_back (std::move (l));
        }
        allFader.reset (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow));
        setupFader (*allFader);
        allFader->setTooltip ("Moves every fader together");
        allFader->onValueChange = [this]
        {
            const double d = allFader->getValue() - lastAll; lastAll = allFader->getValue();
            for (auto& f : faders) f->setValue (juce::jlimit (-100.0, 30.0, f->getValue() <= -99.9 && d > 0 ? d : f->getValue() + d), juce::dontSendNotification);
            pending = true;
        };
        addAndMakeVisible (*allFader);
        allLabel.setText ("All", juce::dontSendNotification); allLabel.setJustificationType (juce::Justification::centred);
        allLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold)); addAndMakeVisible (allLabel);
        viewport.setViewedComponent (&content, false); viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);

        title.setText ("Volume change from " + formatTimeShort ((double) t0 / rate, 2) + " to " + formatTimeShort ((double) t1 / rate, 2)
                           + "  -  " + juce::String (n) + " track(s)", juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold)); addAndMakeVisible (title);
        info.setFont (juce::FontOptions (11.5f)); info.setColour (juce::Label::textColourId, theme::dimText); addAndMakeVisible (info);
        fadeInCap.setText ("Fade in", juce::dontSendNotification); addAndMakeVisible (fadeInCap);
        fadeOutCap.setText ("Fade out", juce::dontSendNotification); addAndMakeVisible (fadeOutCap);
        for (auto* c : { &fadeIn, &fadeOut })
        {
            for (int i = 1; i <= 20; ++i) c->addItem (juce::String (i / 20.0, 2) + " s", i);
            c->onChange = [this] { pending = true; };
            addAndMakeVisible (*c);
        }
        fadeIn.setSelectedId (fadeInId, juce::dontSendNotification); fadeOut.setSelectedId (fadeOutId, juce::dontSendNotification);
        play.setButtonText ("Play"); play.setTooltip ("Plays the change, from 2 seconds before it to 2 seconds after it (press again to stop)");
        play.onClick = [this] { togglePlay(); };
        done_.setButtonText ("Done"); cancel.setButtonText ("Cancel");
        done_.onClick = [this] { applyNow(); done = true; if (closeFn) closeFn(); };
        cancel.onClick = [this] { if (closeFn) closeFn(); };
        done_.setColour (juce::TextButton::buttonColourId, theme::accent);
        for (auto* b : { &play, &done_, &cancel }) addAndMakeVisible (b);
        remove.setVisible (false);
        content.setSize (juce::jmax (1, n) * kColW, 330);
        setSize (juce::jlimit (520, 1000, (n + 1) * kColW + 30), 500);
        startTimerHz (10);                                       // (a fader being dragged is heard as it moves, ten times a second)
    }

    void timerCallback() override
    {
        if (pending) applyNow();
        if (playing && ! app.isPlaying()) { playing = false; play.setButtonText ("Play"); }      // (it got to the end, or was stopped elsewhere)
    }

    void togglePlay()
    {
        if (app.isPlaying()) { app.stopPlayback(); playing = false; play.setButtonText ("Play"); return; }
        applyNow();
        const auto err = app.playEdit (editId, juce::jmax (0.0, (double) t0 / rate - 2.0), (double) t1 / rate + 2.0);
        if (err.isNotEmpty()) { showError ("Play", err); return; }
        playing = true; play.setButtonText ("Stop");
    }

    double deltaFor (const juce::Uuid& tid, bool& used) const
    {
        const auto it = std::find (tracksUsed.begin(), tracksUsed.end(), tid);
        used = it != tracksUsed.end();
        return used ? faders[(size_t) (it - tracksUsed.begin())]->getValue() : 0.0;
    }

    /** Makes the change (the first time) or brings its points up to date with the faders and the fade times. */
    void applyNow()
    {
        pending = false;
        auto* e = app.project.findEdit (editId);
        if (e == nullptr) return;
        const double fin = fadeIn.getSelectedId() / 20.0, fout = fadeOut.getSelectedId() / 20.0;
        if (group.isEmpty()) create (*e, fin, fout);
        else
            for (auto& r : e->regions)
            {
                int iStart = -1, iEnd = -1;
                for (size_t i = 0; i < r.gains.size(); ++i)
                    if (r.gains[i].group == group) { if (r.gains[i].role == 3) iEnd = (int) i; else if (iStart < 0) iStart = (int) i; }
                if (iStart < 0 && iEnd < 0) continue;
                const juce::int64 aRel = iStart >= 0 ? r.gains[(size_t) iStart].at : 0;
                const juce::int64 bEnd = iEnd >= 0 ? r.gains[(size_t) iEnd].at + (juce::int64) std::llround (r.gains[(size_t) iEnd].ramp * rate) : r.length();
                const juce::int64 half = juce::jmax ((juce::int64) 0, (bEnd - aRel) / 2);
                if (iStart >= 0)
                {
                    auto& g = r.gains[(size_t) iStart];
                    for (size_t k = 0; k < r.files.size() && k < g.db.size(); ++k)
                    {
                        bool used = false; const double d = deltaFor (r.files[k].trackId, used);
                        if (! used) continue;
                        const float base = juce::Decibels::gainToDecibels (r.levelAfter (k, (size_t) iStart, g.at), -100.0f);
                        g.db[k] = d <= -99.9 ? -100.0f : juce::jlimit (-100.0f, 40.0f, base + (float) d);
                    }
                    if (g.role == 1) g.ramp = (double) juce::jmin ((juce::int64) std::llround (fin * rate), half) / rate;
                }
                if (iEnd >= 0)
                {
                    auto& g = r.gains[(size_t) iEnd];
                    const auto rampS = juce::jmin ((juce::int64) std::llround (fout * rate), half);
                    g.at = bEnd - rampS; g.ramp = (double) rampS / rate;           // (it still arrives exactly at the end of the area)
                }
                std::stable_sort (r.gains.begin(), r.gains.end(), [] (const GainChange& x, const GainChange& y) { return x.at < y.at; });
            }
        app.project.changed();
    }

    void create (EditDef& e, double fin, double fout)
    {
        group = juce::Uuid().toString();
        for (auto& r : e.regions)
        {
            if (r.endSample() <= t0 || r.startSample >= t1) continue;
            const bool startsHere = t0 > r.startSample, endsHere = t1 < r.endSample();
            const juce::int64 aRel = startsHere ? t0 - r.startSample : 0, bRel = endsHere ? t1 - r.startSample : r.length();
            const juce::int64 half = (bRel - aRel) / 2;
            const juce::int64 rampIn = juce::jmin ((juce::int64) std::llround (fin * rate), half), rampOut = juce::jmin ((juce::int64) std::llround (fout * rate), half);
            // the level of every file now, at the start and at the end of the area (before anything is changed)
            std::vector<float> atA, atB;
            for (size_t k = 0; k < r.files.size(); ++k) { atA.push_back (r.levelDbAt (k, aRel)); atB.push_back (r.levelDbAt (k, juce::jmin (bRel, r.length() - 1))); }
            std::vector<float> inDb = atA;
            for (size_t k = 0; k < r.files.size(); ++k)
            {
                bool used = false; const double d = deltaFor (r.files[k].trackId, used);
                if (used) inDb[k] = d <= -99.9 ? -100.0f : juce::jlimit (-100.0f, 40.0f, atA[k] + (float) d);
            }
            // earlier changes inside the area are replaced
            r.gains.erase (std::remove_if (r.gains.begin(), r.gains.end(), [&] (const GainChange& g) { return g.at > aRel && g.at < bRel; }), r.gains.end());
            auto& s = r.setGainChange (aRel, startsHere ? (double) rampIn / rate : 0.0, inDb);
            s.group = group; s.role = startsHere ? 1 : 2;
            if (endsHere)                                        // glides back to where it was, arriving exactly at the end of the area
            {
                auto& b = r.setGainChange (bRel - rampOut, (double) rampOut / rate, atB);
                b.group = group; b.role = 3;
            }
        }
    }

    void removeAll()
    {
        if (auto* e = app.project.findEdit (editId))
        {
            for (auto& r : e->regions)
                r.gains.erase (std::remove_if (r.gains.begin(), r.gains.end(), [this] (const GainChange& g) { return g.group == group; }), r.gains.end());
            app.project.changed();
        }
        done = true;
        if (closeFn) closeFn();
    }

    AppContext& app; juce::Uuid editId; juce::String group; juce::int64 t0 = 0, t1 = 0; double rate = 48000.0; std::function<void()> closeFn;
    std::vector<EditRegion> before; bool begun = false, done = false, pending = false, playing = false;
    std::vector<juce::Uuid> tracksUsed; std::vector<double> startDb;
    std::vector<std::unique_ptr<juce::Slider>> faders; std::vector<std::unique_ptr<juce::Label>> labels;
    std::unique_ptr<juce::Slider> allFader; juce::Label allLabel, title, info, fadeInCap, fadeOutCap; double lastAll = 0.0;
    juce::Component content; juce::Viewport viewport;
    juce::ComboBox fadeIn, fadeOut; juce::TextButton play, done_, cancel, remove;
};

inline void launchRangeVolume (std::unique_ptr<RangeVolumePanel> panel, const juce::String& title)
{
    struct Holder { juce::DialogWindow* w = nullptr; };
    auto holder = std::make_shared<Holder>();
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = title;
    o.content.setOwned (panel.release());
    o.componentToCentreAround = nullptr;
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = false; o.useBottomRightCornerResizer = false;
    holder->w = o.launchAsync();
}

inline void showRangeVolumeChange (AppContext& app, const juce::Uuid& editId, juce::int64 from, juce::int64 to)
{
    auto closer = std::make_shared<juce::Component::SafePointer<juce::Component>>();
    auto panel = std::make_unique<RangeVolumePanel> (app, editId, from, to, [closer] { if (auto* c = closer->getComponent()) if (auto* w = c->findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); });
    *closer = panel.get();
    launchRangeVolume (std::move (panel), "Change volume between the 1/2 marks");
}

/** 3.18.5: a "between the 1/2 marks" change made before, opened again by its group (double-click or right-click its teal line). */
inline void showRangeVolumeChange (AppContext& app, const juce::Uuid& editId, const juce::String& group)
{
    auto closer = std::make_shared<juce::Component::SafePointer<juce::Component>>();
    auto panel = std::make_unique<RangeVolumePanel> (app, editId, group, [closer] { if (auto* c = closer->getComponent()) if (auto* w = c->findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); });
    *closer = panel.get();
    launchRangeVolume (std::move (panel), "Change the volume change");
}
} // namespace td
