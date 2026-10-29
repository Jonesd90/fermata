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
    sound at each join), so the new joins land on the same beat as the old ones.
    3.7.0, tuned on real takes: the marked part is searched for WITH the music either side of it (8 s in all, from its own take), which tells apart
    music that sounds alike for a few seconds - the opening of one verse and the next, whose inner voices differ; the part on its own then says
    exactly where it starts and ends in each version found. A false start is shorter than that, so the part on its own is also looked for in what is
    left of each take: those are listed as POSSIBLE (a false start, or the same notes somewhere else - check by ear).
    3.16.0, the bars: when the marked part's take has its bars entered, only the takes that have at least one of the marked bars are searched (a take
    with no bars entered is left out), and each only around where those bars are in it (the take's bars spread over its music: takematch::BarMap,
    with room either side) - not from end to end, where music that sounds alike elsewhere used to come up. */
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
        cap (what, "Every place in the takes of this take window where the marked music was played, in recording order (\"possible\": only the marked part itself matches there - a false start, "
                   "or the same notes elsewhere; check by ear). Audition puts a version into the edit in place of the marked part - everything after it moves to fit - and plays it from a little before "
                   "to a little after; it comes out again when playback stops. Use this one leaves it in (Ctrl+Z takes it out).", 12.5f);
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
        bool possible = false;                     // only the marked part on its own matches here (a false start, or the same notes elsewhere)
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
            row = r; text = owner.textOf (data); dim = data.pending || data.dud || data.possible; playing = playingThis;
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
        if (r.possible) t += "   possible - check by ear";                // (a false start, or the same notes elsewhere: the window's text says so)
        else if (r.passes > 1) t += "   (" + juce::String (r.pass) + (r.pass == 1 ? "st" : r.pass == 2 ? "nd" : r.pass == 3 ? "rd" : "th") + " of " + juce::String (r.passes) + ")";
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

    /** The marked part's fingerprint and the part with the music either side: from its own take's fingerprint (the take window's, made in the
        background), kContextSeconds in all. False while that is still being made. When its take is no longer in the window, the part on its own,
        read from the piece's files. */
    bool makePassage()
    {
        if (! passage.empty()) return true;
        auto* w = app.project.findTakeWindow (windowId);
        const TakeGroup* src = w != nullptr ? w->findGroup (piece.takeId) : nullptr;
        if (src != nullptr)
        {
            auto fp = app.takeMatch.fingerprintFor (piece.takeId);
            if (fp == nullptr)
            {
                const auto st = app.takeMatch.statusOf (piece.takeId).state;
                if (st != TakeMatchService::State::NoAudio && st != TakeMatchService::State::Failed) return false;      // (being made: the timer asks again)
            }
            else
            {
                const double fps = fp->fps;
                const int p0 = juce::jlimit (0, fp->frames(), (int) std::lround ((double) passageFrom / rate * fps)), p1 = juce::jlimit (p0, fp->frames(), (int) std::lround ((double) passageTo / rate * fps));
                const int need = (int) std::lround (takematch::kContextSeconds * fps);
                int c0 = p0, c1 = p1;
                if (c1 - c0 < need)
                {
                    const int extra = (need - (c1 - c0)) / 2;
                    c0 = juce::jmax (0, c0 - extra); c1 = juce::jmin (fp->frames(), c1 + extra);
                    if (c1 - c0 < need) { c0 = juce::jmax (0, c1 - need); c1 = juce::jmin (fp->frames(), c0 + need); }
                }
                part = fp->slice (p0, p1); passage = fp->slice (c0, c1); innerFrom = p0 - c0; innerTo = p1 - c0; withContext = true;
                return ! part.empty();
            }
        }
        part = takematch::analyseRange (formats, sourcesOf (piece.files), passageFrom, passageTo);
        passage = part; innerFrom = 0; innerTo = -1; withContext = false;
        return true;
    }
    /** The bars the marked part is in, from its take's bars - spread evenly over the take's MUSIC, from where it begins to where it ends (3.16.0;
        before, over the whole recording): 0 when the take has none entered. */
    void passageBars (int& lo, int& hi) const
    {
        lo = hi = 0;
        auto* w = app.project.findTakeWindow (windowId);
        const TakeGroup* src = w != nullptr ? w->findGroup (piece.takeId) : nullptr;
        if (src == nullptr || src->barIn <= 0 || src->lengthSeconds() <= 0.0) return;
        auto fp = app.takeMatch.fingerprintFor (src->id);
        const auto map = takematch::barMapOf (fp != nullptr ? *fp : takematch::Fingerprint(), src->barIn, src->barOut, src->lengthSeconds());
        if (! map.valid()) return;
        lo = map.barAt ((double) passageFrom / rate);
        hi = juce::jmax (lo, map.barAt ((double) juce::jmax (passageFrom, passageTo - 1) / rate));
    }
    /** Where in a take with these bars bars lo..hi should be (seconds), with room either side for the bars not being evenly spread: two bars, plus a
        twentieth of the take's music, plus two seconds. */
    static void searchSpan (const takematch::BarMap& m, int lo, int hi, double& from, double& to)
    {
        const double margin = 2.0 * m.barLength() + 0.05 * (m.to - m.from) + 2.0;
        from = m.timeOf (juce::jmax (lo, m.barIn)) - margin;
        to = m.timeOf (juce::jmin (hi, m.barOut) + 1) + margin;
    }

    /** Every take of the window: its versions, its possible ones, or a pending row. Called again by the timer while takes are still being analysed. */
    void search()
    {
        auto* w = app.project.findTakeWindow (windowId);
        if (w == nullptr) { status.setText ("The take window this piece came from is not in the project any more.", juce::dontSendNotification); return; }
        if (! makePassage())
        {
            status.setText ("Analysing the take the marked part comes from...", juce::dontSendNotification);
            stillWaiting = true;
            return;
        }
        if (part.empty()) { status.setText ("The marked part's audio could not be read.", juce::dontSendNotification); stillWaiting = false; return; }
        if (takematch::mostlySilent (part, 0, part.frames()))
        {
            status.setText ("The marked part is mostly silence: there is no music in it to look for. Mark the music to replace.", juce::dontSendNotification);
            stillWaiting = false; return;
        }
        int barLo = 0, barHi = 0; passageBars (barLo, barHi);
        rows.clear();
        int waiting = 0, leftOut = 0, noBars = 0;
        const double own = (double) passageFrom / rate;
        for (auto& g : w->groups)
        {
            // 3.16.0: when the marked part's bars are known, only the takes that have at least one of them are searched - a take with no bars entered
            // is left out too - and each only around where those bars are in it
            if (barLo > 0 && g.id != piece.takeId)
            {
                if (g.barIn <= 0) { ++noBars; continue; }
                if (! takematch::BarMap { g.barIn, juce::jmax (g.barIn, g.barOut), 0.0, 1.0 }.hasAny (barLo, barHi)) { ++leftOut; continue; }
            }
            const auto name = w->displayName (g);
            auto full = app.takeMatch.fingerprintFor (g.id);
            if (full == nullptr)
            {
                const auto st = app.takeMatch.statusOf (g.id);
                if (st.state == TakeMatchService::State::NoAudio || st.state == TakeMatchService::State::Failed) continue;
                Row r; r.takeId = g.id; r.takeName = name; r.dud = g.dud; r.pending = true; rows.push_back (r); ++waiting;
                continue;
            }
            // the stretch of this take to search: around the marked bars (3.16.0), else all of it
            const takematch::Fingerprint* fp = full.get();
            takematch::Fingerprint window;
            double offset = 0.0;
            if (barLo > 0 && g.barIn > 0)
            {
                const auto map = takematch::barMapOf (*full, g.barIn, g.barOut, full->lengthSeconds());
                double a = 0.0, b = 0.0; searchSpan (map, barLo, barHi, a, b);
                if (const double need = passage.lengthSeconds() + 4.0; b - a < need) { a -= (need - (b - a)) * 0.5; b = a + need; }    // (room for the music either side)
                const int f0 = juce::jlimit (0, full->frames(), (int) std::floor (a * full->fps)), f1 = juce::jlimit (f0, full->frames(), (int) std::ceil (b * full->fps));
                if (f0 > 0 || f1 < full->frames()) { window = full->slice (f0, f1); fp = &window; offset = (double) f0 / full->fps; }
            }
            auto shifted = [offset] (takematch::Version v) { v.start += offset; v.end += offset; return v; };
            auto notOwn = [&] (const takematch::Version& v) { return g.id != piece.takeId || std::abs (v.start - own) >= 1.0; };
            std::vector<takematch::Version> sure, maybe;
            for (auto v : takematch::findVersions (passage, *fp, takematch::kVersionCost, innerFrom, innerTo))
            {
                v = shifted (v);
                if (withContext) v = takematch::placePart (part, *full, v);        // (the context says where; the part on its own says exactly where)
                if (notOwn (v)) sure.push_back (v);
            }
            if (withContext)                                                      // what the context cannot find: a false start is shorter than it
                for (auto v : takematch::findVersions (part, *fp, takematch::kPossibleCost))
                {
                    v = shifted (v);
                    bool inSure = false;
                    for (auto& s2 : sure) if (v.start < s2.end + 0.5 && v.end > s2.start - 0.5) inSure = true;
                    if (! inSure && notOwn (v)) maybe.push_back (v);
                }
            auto byStart = [] (const takematch::Version& a, const takematch::Version& b) { return a.start < b.start; };
            std::sort (sure.begin(), sure.end(), byStart);
            int k = 0;
            std::vector<Row> mine;
            for (auto& v : sure) { Row r; r.takeId = g.id; r.takeName = name; r.dud = g.dud; r.v = v; r.pass = ++k; r.passes = (int) sure.size(); mine.push_back (r); }
            for (auto& v : maybe) { Row r; r.takeId = g.id; r.takeName = name; r.dud = g.dud; r.v = v; r.possible = true; mine.push_back (r); }
            std::sort (mine.begin(), mine.end(), [] (const Row& a, const Row& b) { return a.v.start < b.v.start; });
            for (auto& r : mine) rows.push_back (r);
        }
        list.updateContent();
        int found = 0, possible = 0; for (auto& r : rows) { if (r.pending) continue; if (r.possible) ++possible; else ++found; }
        juce::String s = found == 0 ? "No other version found" : found == 1 ? "1 version found" : juce::String (found) + " versions found";
        if (possible > 0) s += ", and " + juce::String (possible) + " possible (check by ear)";
        if (waiting > 0) s += "; " + juce::String (waiting) + (waiting == 1 ? " take is" : " takes are") + " still being analysed";
        if (barLo > 0) s += "; searched only around bar" + (barHi > barLo ? "s " + juce::String (barLo) + "-" + juce::String (barHi) : " " + juce::String (barLo));
        if (leftOut > 0) s += "; " + juce::String (leftOut) + (leftOut == 1 ? " take does" : " takes do") + " not have those bars";
        if (noBars > 0) s += "; " + juce::String (noBars) + (noBars == 1 ? " take has" : " takes have") + " no bars entered (left out)";
        if (barLo == 0) s += " (enter the bars of this piece's take - B in the take window - to search only the takes with those bars)";
        if (! withContext) s += " (the marked part's take is no longer in the window: searched without the music either side - check by ear)";
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
    takematch::Fingerprint passage, part;                                    // the marked part with the music either side, and on its own
    int innerFrom = 0, innerTo = -1;                                         // where the marked part is in 'passage' (frames)
    bool withContext = false;
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
