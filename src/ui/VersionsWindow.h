#pragma once
#include "AppContext.h"
#include "Uikit.h"
#include "FixTools.h"
#include "../core/TakeMatch.h"
#include "../core/AudioFormats.h"

namespace td
{
/** Find other versions (3.5.0). The producer marks the part of the edit that wants replacing (keys 1 and 2), right-clicks the piece and asks for
    the other versions: every place in every take of the same take window where that music was played - a take that has it twice (a repeat, a false
    start and the real thing, several passes) is listed twice. The list is in recording order. Audition puts a version into the edit in place of the
    marked part, with everything after it moved to fit (the ripple: markers, overdubs, the lot - Edit::replaceRangeRippled), plays from a little
    before to a little after (half a second each side, adjustable), and takes it out again when playback stops. Use this one leaves it in; Ctrl+Z
    takes it out. Where exactly the version starts and ends is found from the music (core/TakeMatch.h: the fingerprints, then the shape of the
    sound at each join), so the new joins land on the same beat as the old ones. */
class VersionsComponent : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    VersionsComponent (AppContext& a, const juce::Uuid& eid) : app (a), editId (eid), hold (a)
    {
        registerAudioFormats (formats);
        auto* e = app.project.findEdit (editId);
        jassert (e != nullptr && e->fixIn >= 0.0 && e->fixOut > e->fixIn);
        rate = e->sampleRate;
        from = (juce::int64) std::llround (e->fixIn * rate); to = (juce::int64) std::llround (e->fixOut * rate);
        base.regions = e->regions; base.overdubs = e->overdubs; base.lanes = e->lanes; base.fixIn = e->fixIn; base.fixOut = e->fixOut; base.insertIndex = e->insertIndex;
        base.markers = e->markers; base.feedback = e->feedback; base.guide = e->guide; base.playheadSeconds = e->playheadSeconds;
        // the piece the marked part lies in, and where that part is in its take
        for (auto& r : e->regions)
            if (from >= r.startSample && to <= r.endSample()) { piece = r; break; }
        windowId = piece.windowId;
        if (app.project.findTakeWindow (windowId) == nullptr)
            for (auto& w : app.project.takeWindows) if (w->findGroup (piece.takeId) != nullptr) { windowId = w->id; break; }
        passageFrom = piece.srcIn + (from - piece.startSample); passageTo = piece.srcIn + (to - piece.startSample);

        auto cap = [this] (juce::Label& l, const juce::String& t, float size, bool bold = false)
        {
            l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)); addAndMakeVisible (l);
        };
        cap (title, "Other versions of " + timeText ((double) from / rate) + " - " + timeText ((double) to / rate) + "  (" + piece.takeName + ")", 16.0f, true);
        cap (what, "Every place in the takes of this take window where the marked music was played, in recording order. Audition puts a version into the edit in place of the marked part - "
                   "everything after it moves to fit - and plays it from a little before to a little after; it comes out again when playback stops. Use this one leaves it in (Ctrl+Z takes it out).", 12.5f);
        what.setJustificationType (juce::Justification::topLeft); what.setMinimumHorizontalScale (1.0f);
        cap (rollCaption, "Play this much before and after:", 13.0f);
        addAndMakeVisible (roll);
        roll.setRange (0.1, 5.0, 0.05); roll.setValue (0.5, juce::dontSendNotification); roll.setTextValueSuffix (" s");
        roll.setSliderStyle (juce::Slider::LinearBar); roll.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, grid::btnH);
        roll.setTooltip ("How much of the edit plays before the version and after it, so the joins can be heard in their place.");
        addAndMakeVisible (list); list.setModel (this); list.setRowHeight (30); list.setColour (juce::ListBox::backgroundColourId, theme::field);
        addAndMakeVisible (stop); stop.onClick = [this] { stopAudition(); };
        addAndMakeVisible (close); close.onClick = [this] { closeMe(); };
        addAndMakeVisible (status); status.setJustificationType (juce::Justification::centredLeft); status.setMinimumHorizontalScale (1.0f); status.setFont (juce::FontOptions (12.5f));
        setSize (760, 460);
        search();
        startTimer (200);
    }
    ~VersionsComponent() override { if (hold) { app.stopPlayback(); takeOut(); } }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        what.setBounds (r.removeFromTop (54));
        r.removeFromTop (4);
        auto row = r.removeFromTop (grid::btnH);
        rollCaption.setBounds (row.removeFromLeft (200)); roll.setBounds (row.removeFromLeft (220));
        r.removeFromTop (8);
        auto bottom = r.removeFromBottom (grid::btnH);
        close.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (grid::gap); stop.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (12);
        status.setBounds (bottom);
        r.removeFromBottom (8);
        list.setBounds (r);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }

private:
    /** One version: a take and the place in it. 'pass' / 'passes' say which of the take's versions it is when it has more than one. */
    struct Row
    {
        juce::Uuid takeId; juce::String takeName; bool dud = false;
        takematch::Version v;
        int pass = 1, passes = 1;
        bool pending = false;                      // the take's fingerprint is not ready yet: this row stands for the whole take
        bool refined = false; juce::int64 srcIn = 0, srcOut = 0;        // the exact samples once the joins have been refined
    };
    /** A row of the list: the text, an Audition button and a Use button. */
    class RowComponent : public juce::Component
    {
    public:
        explicit RowComponent (VersionsComponent& o) : owner (o)
        {
            addAndMakeVisible (audition); addAndMakeVisible (use);
            audition.onClick = [this] { owner.audition (row); };
            use.onClick = [this] { owner.use (row); };
        }
        void set (int r, const Row& data, bool playingThis)
        {
            row = r; text = owner.textOf (data); dim = data.pending || data.dud; playing = playingThis;
            audition.setEnabled (! data.pending); use.setEnabled (! data.pending);
            audition.setButtonText (playingThis ? "Playing" : "Audition");
            repaint();
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (6, 3);
            use.setBounds (r.removeFromRight (96)); r.removeFromRight (grid::gap); audition.setBounds (r.removeFromRight (84));
        }
        void paint (juce::Graphics& g) override
        {
            if (playing) { g.setColour (theme::accent.withAlpha (0.18f)); g.fillRect (getLocalBounds()); }
            g.setColour (dim ? theme::dimText : theme::text); g.setFont (juce::FontOptions (13.0f));
            g.drawText (text, getLocalBounds().reduced (8, 0).withTrimmedRight (200), juce::Justification::centredLeft, true);
            g.setColour (theme::border.withAlpha (0.5f)); g.fillRect (0, getHeight() - 1, getWidth(), 1);
        }
    private:
        VersionsComponent& owner; int row = -1; juce::String text; bool dim = false, playing = false;
        juce::TextButton audition { "Audition" }, use { "Use this one" };
    };

    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int rowNumber, bool, juce::Component* existing) override
    {
        if (rowNumber < 0 || rowNumber >= (int) rows.size()) { delete existing; return nullptr; }
        auto* c = dynamic_cast<RowComponent*> (existing);
        if (c == nullptr) { delete existing; c = new RowComponent (*this); }
        c->set (rowNumber, rows[(size_t) rowNumber], hold && playingRow == rowNumber);
        return c;
    }

    static juce::String timeText (double seconds)
    {
        const int m = (int) (seconds / 60.0); const double s = seconds - 60.0 * m;
        return juce::String (m) + ":" + (s < 10.0 ? "0" : "") + juce::String (s, 1);
    }
    juce::String textOf (const Row& r) const
    {
        if (r.pending) return r.takeName + "   (analysing this take...)";
        juce::String t = r.takeName + "   at " + timeText (r.v.start);
        if (r.passes > 1) t += "   (" + juce::String (r.pass) + (r.pass == 1 ? "st" : r.pass == 2 ? "nd" : r.pass == 3 ? "rd" : "th") + " of " + juce::String (r.passes) + ")";
        t += "   match " + juce::String (juce::roundToInt (100.0 * juce::jlimit (0.0, 1.0, 1.0 - r.v.cost))) + " %";
        const int tempo = juce::roundToInt (100.0 * (1.0 / juce::jmax (0.01, r.v.stretch) - 1.0));
        if (tempo != 0) t += "   tempo " + juce::String (tempo > 0 ? "+" : "") + juce::String (tempo) + " %";
        if (r.dud) t += "   (marked dud)";
        return t;
    }

    std::vector<takematch::Source> sourcesOf (const std::vector<RegionFile>& files) const
    {
        std::vector<takematch::Source> out;
        for (auto& f : files) if (f.file != juce::File() && ! isLtc (f.trackId)) out.push_back ({ f.file, f.fileStart });
        return out;
    }
    std::vector<takematch::Source> sourcesOf (const TakeGroup& g) const
    {
        std::vector<takematch::Source> out;
        for (auto& f : g.files) if (f.file != juce::File() && ! isLtc (f.trackId)) out.push_back ({ f.file, 0 });
        return out;
    }
    bool isLtc (const juce::Uuid& trackId) const { for (auto& t : app.project.tracks) if (t.id == trackId) return t.isLtc; return false; }

    /** The passage's fingerprint (once), then every take of the window: its versions, or a pending row. Called again by the timer while takes are still being analysed. */
    void search()
    {
        auto* w = app.project.findTakeWindow (windowId);
        if (w == nullptr) { status.setText ("The take window this piece came from is not in the project any more.", juce::dontSendNotification); return; }
        if (passage.empty())
        {
            passage = takematch::analyseRange (formats, sourcesOf (piece.files), passageFrom, passageTo);
            if (passage.empty()) { status.setText ("The marked part's audio could not be read.", juce::dontSendNotification); return; }
        }
        rows.clear();
        int waiting = 0;
        for (auto& g : w->groups)
        {
            const auto name = w->displayName (g);
            auto fp = app.takeMatch.fingerprintFor (g.id);
            if (fp == nullptr)
            {
                const auto st = app.takeMatch.statusOf (g.id);
                if (st.state == TakeMatchService::State::NoAudio || st.state == TakeMatchService::State::Failed) continue;
                Row r; r.takeId = g.id; r.takeName = name; r.dud = g.dud; r.pending = true; rows.push_back (r); ++waiting;
                continue;
            }
            auto vs = takematch::findVersions (passage, *fp);
            // (the marked part itself, in its own take, is not another version)
            if (g.id == piece.takeId)
            {
                const double own = (double) passageFrom / rate;
                vs.erase (std::remove_if (vs.begin(), vs.end(), [own] (const takematch::Version& v) { return std::abs (v.start - own) < 1.0; }), vs.end());
            }
            std::sort (vs.begin(), vs.end(), [] (const takematch::Version& a, const takematch::Version& b) { return a.start < b.start; });
            int k = 0;
            for (auto& v : vs) { Row r; r.takeId = g.id; r.takeName = name; r.dud = g.dud; r.v = v; r.pass = ++k; r.passes = (int) vs.size(); rows.push_back (r); }
        }
        list.updateContent();
        const int found = (int) rows.size() - waiting;
        juce::String s = found == 0 ? "No other version found" : found == 1 ? "1 version found" : juce::String (found) + " versions found";
        if (waiting > 0) s += "; " + juce::String (waiting) + (waiting == 1 ? " take is" : " takes are") + " still being analysed";
        s += ".";
        status.setText (s, juce::dontSendNotification);
        stillWaiting = waiting > 0;
    }

    /** The exact samples of a version in its take: the fingerprint says where to the frame (50 ms); the joins are then refined against the marked part's own
        audio, so the new piece starts and ends on the same beats. The same track (microphone) is compared when the other take has it. */
    bool refine (Row& r)
    {
        if (r.refined) return true;
        auto* w = app.project.findTakeWindow (windowId);
        auto* g = w != nullptr ? w->findGroup (r.takeId) : nullptr;
        if (g == nullptr) return false;
        const auto mine = sourcesOf (piece.files), theirs = sourcesOf (*g);
        if (mine.empty() || theirs.empty()) return false;
        takematch::Source a = mine[0], b = theirs[0];
        for (auto& f : piece.files)
            if (f.file == a.file)
                for (auto& f2 : g->files) if (f2.trackId == f.trackId && f2.file != juce::File()) { b = { f2.file, 0 }; break; }
        const juce::int64 guessIn = (juce::int64) std::llround (r.v.start * rate), guessOut = (juce::int64) std::llround (r.v.end * rate);
        // the fingerprint places a join to its frame; the shape of the sound narrows it to the sample (searching 0.15 s either way: a frame and a half)
        r.srcIn = takematch::refineJoin (formats, a, passageFrom, b, guessIn, 0.2, 0.15);
        r.srcOut = takematch::refineJoin (formats, a, passageTo, b, guessOut, 0.2, 0.15);
        r.srcIn = juce::jlimit ((juce::int64) 0, g->lengthSamples, r.srcIn); r.srcOut = juce::jlimit (r.srcIn, g->lengthSamples, r.srcOut);
        r.refined = r.srcOut > r.srcIn + (juce::int64) (0.05 * rate);
        return r.refined;
    }
    /** The piece of the edit that a version becomes. */
    EditRegion regionOf (const Row& r) const
    {
        EditRegion x;
        auto* w = app.project.findTakeWindow (windowId);
        auto* g = w != nullptr ? w->findGroup (r.takeId) : nullptr;
        if (g == nullptr) return x;
        x.windowId = windowId; x.takeId = g->id; x.takeName = w->displayName (*g); x.sampleRate = g->sampleRate; x.barIn = g->barIn; x.barOut = g->barOut;
        x.srcIn = r.srcIn; x.srcOut = r.srcOut; x.sourceLength = g->lengthSamples;
        for (auto& f : g->files) x.files.push_back ({ f.trackId, f.trackName, f.file, f.numChannels });
        return x;
    }

    void putIn (int rowIndex)
    {
        auto* e = app.project.findEdit (editId);
        if (e == nullptr || rowIndex < 0 || rowIndex >= (int) rows.size()) return;
        auto& r = rows[(size_t) rowIndex];
        if (! refine (r)) { status.setText ("The version's audio could not be read.", juce::dontSendNotification); return; }
        restore (*e);
        e->replaceRangeRippled (regionOf (r), from, to);
        hold = true; playingRow = rowIndex;
        app.project.changed();
    }
    void takeOut()
    {
        if (! hold) return;
        if (auto* e = app.project.findEdit (editId)) { restore (*e); app.project.changed(); }
        hold = false; playingRow = -1;
        list.updateContent();
    }
    void restore (EditDef& e) const
    {
        e.regions = base.regions; e.overdubs = base.overdubs; e.lanes = base.lanes; e.fixIn = base.fixIn; e.fixOut = base.fixOut; e.insertIndex = base.insertIndex;
        e.markers = base.markers; e.feedback = base.feedback; e.guide = base.guide; e.playheadSeconds = base.playheadSeconds;
    }
    void audition (int rowIndex)
    {
        app.stopPlayback(); takeOut();
        putIn (rowIndex);
        if (! hold) return;
        const auto& r = rows[(size_t) rowIndex];
        const double pre = roll.getValue(), len = (double) (r.srcOut - r.srcIn) / rate;
        const auto err = app.playEdit (editId, juce::jmax (0.0, (double) from / rate - pre), (double) from / rate + len + pre);
        if (err.isNotEmpty()) { status.setText (err, juce::dontSendNotification); takeOut(); return; }
        list.updateContent();
        status.setText ("Playing " + r.takeName + "'s version in place (" + juce::String (len, 2) + " s; the marked part was " + juce::String ((double) (to - from) / rate, 2) + " s).", juce::dontSendNotification);
    }
    void stopAudition() { app.stopPlayback(); takeOut(); }
    void use (int rowIndex)
    {
        app.stopPlayback();
        if (! (hold && playingRow == rowIndex)) { takeOut(); putIn (rowIndex); }
        if (! hold) return;
        const auto& r = rows[(size_t) rowIndex];
        const double delta = (double) (r.srcOut - r.srcIn - (to - from)) / rate;
        hold = false; playingRow = -1;                                      // (it is the edit now: the automatic save may write it)
        app.armFixUndo ("Find other versions", [a = &app, id = editId, old = base]
        {
            if (auto* e = a->project.findEdit (id))
            {
                e->regions = old.regions; e->overdubs = old.overdubs; e->lanes = old.lanes; e->fixIn = old.fixIn; e->fixOut = old.fixOut; e->insertIndex = old.insertIndex;
                e->markers = old.markers; e->feedback = old.feedback; e->guide = old.guide; e->playheadSeconds = old.playheadSeconds;
                a->project.changed();
            }
        }, true, editId);
        app.project.changed();
        app.setNotice (r.takeName + "'s version is in" + (std::abs (delta) >= 0.005 ? "; everything after it moved " + juce::String (std::abs (delta), 2) + " s " + (delta > 0 ? "later" : "earlier") : juce::String()) + ". Ctrl+Z takes it out.");
        closeMe();
    }
    void closeMe()
    {
        if (hold) { app.stopPlayback(); takeOut(); }
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
    }
    void timerCallback() override
    {
        if (hold && ! app.isPlaying()) takeOut();                            // (it finished playing by itself)
        if (stillWaiting && ! hold && ++ticks % 10 == 0) search();           // every two seconds, while takes are still being analysed
    }

    struct Base
    {
        std::vector<EditRegion> regions, overdubs; std::vector<AutoLane> lanes; double fixIn = -1.0, fixOut = -1.0; int insertIndex = -1;
        std::vector<Marker> markers; std::vector<FeedbackNote> feedback; EditGuide guide; double playheadSeconds = 0.0;
    };

    AppContext& app;
    juce::Uuid editId, windowId;
    juce::AudioFormatManager formats;
    double rate = 48000.0;
    juce::int64 from = 0, to = 0, passageFrom = 0, passageTo = 0;
    EditRegion piece;
    takematch::Fingerprint passage;
    std::vector<Row> rows;
    Base base;
    fixtools::PreviewApplied hold;                                           // while a version is in the edit only to be listened to, the automatic save waits
    int playingRow = -1, ticks = 0;
    bool stillWaiting = false;
    juce::Label title, what, rollCaption, status;
    juce::Slider roll;
    juce::ListBox list;
    juce::TextButton stop { "Stop" }, close { "Close" };
};

/** Opens the window (from the edit window's right-click menu on a piece, with marks 1 and 2 set). */
inline void showVersions (AppContext& app, const juce::Uuid& editId, juce::Component* parent)
{
    auto* e = app.project.findEdit (editId);
    if (e == nullptr) return;
    if (! (e->fixIn >= 0.0 && e->fixOut > e->fixIn)) { fixtools::say ("Find other versions", "Mark the part to replace first: key 1 at its start and key 2 at its end."); return; }
    const juce::int64 from = (juce::int64) std::llround (e->fixIn * e->sampleRate), to = (juce::int64) std::llround (e->fixOut * e->sampleRate);
    const EditRegion* piece = nullptr;
    for (auto& r : e->regions) if (from >= r.startSample && to <= r.endSample()) { piece = &r; break; }
    if (piece == nullptr) { fixtools::say ("Find other versions", "The marked part must lie within one piece of the main track (it runs across an edit point, or over a gap). Mark the part inside one piece."); return; }
    if (to - from < (juce::int64) (0.5 * e->sampleRate)) { fixtools::say ("Find other versions", "The marked part is too short to search for (half a second at least)."); return; }
    if (to - from > (juce::int64) (120.0 * e->sampleRate)) { fixtools::say ("Find other versions", "The marked part is too long to search for (two minutes at most)."); return; }
    if (app.isPlaying()) app.stopPlayback();
    fixtools::launchDialog (std::make_unique<VersionsComponent> (app, editId), "Find other versions", parent);
}
} // namespace td
