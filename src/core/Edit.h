#pragma once
#include "Common.h"
#include "Automation.h"

namespace td
{
/** The shapes of a crossfade. The out-fade is always the mirror image of the in-fade. */
enum class FadeCurve { EqualPower = 0, Linear = 1, Cosine = 2, SCurve = 3, Logarithmic = 4, Exponential = 5, Fast = 6, Slow = 7, Count };

inline const char* fadeCurveName (FadeCurve c) noexcept
{
    switch (c)
    {
        case FadeCurve::EqualPower:  return "Equal power (sine)";
        case FadeCurve::Linear:      return "Linear";
        case FadeCurve::Cosine:      return "Cosine (S-curve, equal gain)";
        case FadeCurve::SCurve:      return "Smooth S (cubic)";
        case FadeCurve::Logarithmic: return "Logarithmic (fast start)";
        case FadeCurve::Exponential: return "Exponential (slow start)";
        case FadeCurve::Fast:        return "Fast (cubic, quick rise)";
        case FadeCurve::Slow:        return "Slow (cubic, late rise)";
        default:                     return "Equal power (sine)";
    }
}

/** The gain of a fade-IN at x = 0..1 through the fade (the fade-out uses 1 - x). */
inline float fadeInShape (FadeCurve c, float x) noexcept
{
    constexpr float halfPi = juce::MathConstants<float>::halfPi, pi = juce::MathConstants<float>::pi;
    switch (c)
    {
        case FadeCurve::Linear:      return x;
        case FadeCurve::Cosine:      return 0.5f - 0.5f * std::cos (pi * x);
        case FadeCurve::SCurve:      return x * x * (3.0f - 2.0f * x);
        case FadeCurve::Logarithmic: return std::log10 (1.0f + 9.0f * x);              // quick at first, then eases in
        case FadeCurve::Exponential: return (std::pow (10.0f, x) - 1.0f) / 9.0f;       // slow at first, then speeds up
        case FadeCurve::Fast:        return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
        case FadeCurve::Slow:        return x * x * x;
        default:                     return std::sin (halfPi * x);
    }
}


/** One audio file of an edit region (one track's audio). Edits keep their own copy of the file list, so a take can be
    removed from its take window without touching the edit: both just point at the same files on disk. */
struct RegionFile
{
    juce::Uuid   trackId;
    juce::String trackName;
    juce::File   file;
    int          numChannels = 1;
    juce::int64  fileStart = 0;        // the take's sample that sits at sample 0 of this file (0 for a whole take; a repaired / pitch-corrected piece is a short file that starts later)
};

/** A volume change inside a piece of an edit: from 'at' (samples after the piece's start) each file moves to its own level 'db[i]'
    (aligned with the piece's files), instantly if 'ramp' is 0, otherwise gliding over 'ramp' seconds. */
struct GainChange
{
    juce::int64        at = 0;
    double             ramp = 0.0;
    std::vector<float> db;
};
inline float dbToLinear (float db) noexcept { return db <= -99.0f ? 0.0f : std::pow (10.0f, db / 20.0f); }

/** A piece of one take (all of the take's tracks together) placed on the edit timeline.

    Audio at timeline sample t comes from file sample  t + (srcIn - startSample).
    Each region has its own fade-in and fade-out, completely independent of its neighbours:
      fade-in : from  start + inStart   to  start + inEnd     (seconds relative to the region's START)
      fade-out: from  end   + outStart  to  end   + outEnd    (seconds relative to the region's END)
    A fade may begin before the region start / finish after its end: the audio comes from the 'handles'
    (the parts of the take outside srcIn..srcOut). A fade with start == end is a hard cut at that position. */
/** A part that was sent out of Fermata to be processed in other software ("Export for Processing"). The audio stays as it was until the corrected files come back. */
struct WaitingPiece
{
    juce::String name;                 // what the user called it ("bar 4", "click removal")
    juce::int64  from = 0, to = 0;     // the part, in samples of the take (or of the piece's files): [from, to)
    std::vector<juce::File> files;     // the exported files, in the order of the piece's / take's files; the other program overwrites them
    std::vector<juce::Uuid> tracks;    // the track of each exported file (empty = all the files of the piece / take, in order)
    bool active() const noexcept { return ! files.empty() && to > from; }
};

struct EditRegion
{
    WaitingPiece waiting;              // set while the piece waits for corrected audio (from/to = srcIn/srcOut at the time of the export)
    juce::Uuid   id;
    juce::Uuid   windowId, takeId;     // where it came from (informational only: the edit does not depend on them)
    juce::String takeName;             // display name, e.g. "002 - Take"
    std::vector<RegionFile> files;
    juce::int64  sourceLength = 0;     // length of the take's files in samples (limits the handles); 0 = unknown
    juce::int64  srcIn = 0, srcOut = 0;           // sample range used inside the files (srcOut is exclusive)
    double       sampleRate = 48000.0;
    juce::int64  startSample = 0;      // position of srcIn on the edit timeline
    int          barIn = 0, barOut = 0;   // the bars of the take this piece came from (copied from the take; 0 = none entered)

    double       inStart = 0.0,  inEnd = 0.0;      // fade-in,  seconds relative to the region start
    double       outStart = 0.0, outEnd = 0.0;     // fade-out, seconds relative to the region end
    FadeCurve    curve = FadeCurve::EqualPower;
    bool         fixIn = false, fixOut = false;    // the join at its start / end was made by an offline fix (pitch, repair, de-click, export for processing), not by an edit: it is not counted as an edit point
    std::vector<GainChange> gains;     // volume changes inside this piece, sorted by 'at'

    juce::int64 length()    const noexcept { return srcOut - srcIn; }

    // ---- volume changes ----
    /** The level (linear gain) of file 'fileIndex' at 'rel' samples after the start of the piece, using only the first 'count' changes. */
    float levelAfter (size_t fileIndex, size_t count, juce::int64 rel) const noexcept
    {
        if (count == 0) return 1.0f;
        const auto& g = gains[count - 1];
        if (rel < g.at) return levelAfter (fileIndex, count - 1, rel);
        const float to = fileIndex < g.db.size() ? dbToLinear (g.db[fileIndex]) : 1.0f;
        const auto rl = (juce::int64) std::llround (g.ramp * sampleRate);
        if (rl > 0 && rel < g.at + rl)
        {
            const float from = levelAfter (fileIndex, count - 1, g.at);
            return from + (to - from) * (float) (rel - g.at) / (float) rl;
        }
        return to;
    }
    float levelAt (size_t fileIndex, juce::int64 rel) const noexcept { return levelAfter (fileIndex, gains.size(), rel); }
    float levelDbAt (size_t fileIndex, juce::int64 rel) const noexcept { return juce::Decibels::gainToDecibels (levelAt (fileIndex, rel), -100.0f); }

    /** Adds a change (or replaces the one at the same place, within 1 ms). */
    void setGainChange (juce::int64 at, double ramp, std::vector<float> db)
    {
        db.resize (files.size(), 0.0f);
        const auto tol = (juce::int64) (0.001 * sampleRate);
        for (auto& g : gains)
            if (std::abs (g.at - at) <= tol) { g.at = at; g.ramp = ramp; g.db = db; return; }
        GainChange c; c.at = at; c.ramp = ramp; c.db = db;
        gains.insert (std::upper_bound (gains.begin(), gains.end(), c, [] (const GainChange& a, const GainChange& b) { return a.at < b.at; }), c);
    }
    /** Removes the change at that place (within 1 ms). Returns true if there was one. */
    bool removeGainChangeAt (juce::int64 at)
    {
        const auto tol = (juce::int64) (0.001 * sampleRate);
        for (size_t i = 0; i < gains.size(); ++i)
            if (std::abs (gains[i].at - at) <= tol) { gains.erase (gains.begin() + (long) i); return true; }
        return false;
    }
    juce::int64 endSample() const noexcept { return startSample + length(); }
    juce::int64 samples (double seconds) const noexcept { return (juce::int64) std::llround (seconds * sampleRate); }

    // fades on the edit timeline (samples)
    juce::int64 fadeInBegin()  const noexcept { return startSample + samples (inStart); }
    juce::int64 fadeInFinish() const noexcept { return startSample + samples (inEnd); }
    juce::int64 fadeOutBegin() const noexcept { return endSample() + samples (outStart); }
    juce::int64 fadeOutFinish()const noexcept { return endSample() + samples (outEnd); }

    /** The gain this region's fades give at timeline sample t (0 outside the audible span). */
    float gainAt (juce::int64 t) const noexcept
    {
        const auto ib = fadeInBegin(), ie = fadeInFinish(), ob = fadeOutBegin(), oe = fadeOutFinish();
        float g = 1.0f;
        if (ie > ib) g *= t < ib ? 0.0f : t >= ie ? 1.0f : fadeGainFor (curve, (float) (t - ib) / (float) (ie - ib), true);
        else         g *= t < ib ? 0.0f : 1.0f;
        if (oe > ob) g *= t < ob ? 1.0f : t >= oe ? 0.0f : fadeGainFor (curve, (float) (t - ob) / (float) (oe - ob), false);
        else         g *= t < ob ? 1.0f : 0.0f;
        return g;
    }
    static float fadeGainFor (FadeCurve c, float x, bool fadeIn) noexcept
    {
        x = juce::jlimit (0.0f, 1.0f, x);
        return fadeInShape (c, fadeIn ? x : 1.0f - x);
    }
};

/** The edit: regions placed on a timeline. Normally butted end to end; slipping can open gaps / overlaps. */
struct EditDef
{
    juce::Uuid   id;
    juce::String name { "Edit" };
    juce::Uuid   windowId;                 // the take window this edit was first made from (informational)
    double       sampleRate = 0.0;         // set by the first region
    std::vector<EditRegion> regions;
    std::vector<EditRegion> overdubs;      // pieces laid OVER the edit (they play at the same time as whatever is under them); free to move, never ripple
    std::vector<juce::Uuid> trackIds;      // this edit's own track rows, top to bottom. Empty = every project track in the project's order (the default)
    double       playheadSeconds = 0.0;    // the edit window's playhead (where key 4 in a take window places a piece)
    int          insertIndex = -1;         // where the next region goes: -1 = at the end, else before regions[insertIndex]
    double       markIn = -1.0, markOut = -1.0;   // I / O points on the edit timeline, seconds (-1 = not set); used by Bounce Out
    double       fixIn = -1.0, fixOut = -1.0;     // keys 1 / 2: the part to pitch-correct or repair (seconds on the edit timeline, -1 = not set)
    std::vector<juce::Uuid> fixTracks;            // Alt + drag: only these tracks are marked (empty = every track)
    bool fixUses (const juce::Uuid& track) const { return fixTracks.empty() || std::find (fixTracks.begin(), fixTracks.end(), track) != fixTracks.end(); }

    // ---- automation (drawn in the edit window; only plays from the edit) ----
    bool                 automationOn = false;
    std::vector<AutoLane> lanes;

    AutoLane* findLane (const juce::Uuid& laneId) { for (auto& l : lanes) if (l.id == laneId) return &l; return nullptr; }
    AutoLane* findLane (const juce::Uuid& trackId, const juce::String& param, const juce::Uuid& mixerId)
    {
        for (auto& l : lanes) if (l.trackId == trackId && l.param == param && l.mixerId == mixerId) return &l;
        return nullptr;
    }
    AutoLane& addLane (const juce::Uuid& trackId, const juce::String& param, const juce::Uuid& mixerId)
    {
        if (auto* l = findLane (trackId, param, mixerId)) return *l;
        AutoLane l; l.trackId = trackId; l.param = param; l.mixerId = mixerId;
        lanes.push_back (std::move (l));
        return lanes.back();
    }

    /** Which piece of audio is at timeline sample t (the last one that starts at or before t and still covers it); -1 = none (a gap or past the end). */
    int regionAt (juce::int64 t) const
    {
        int found = -1;
        for (int i = 0; i < (int) regions.size(); ++i)
            if (t >= regions[(size_t) i].startSample && t < regions[(size_t) i].endSample()) found = i;
        return found;
    }

    /** Pins a point to the audio under timeline sample t (or to the plain time when there is no audio there). */
    void anchorPoint (AutoPoint& p, juce::int64 t) const
    {
        p.time = t; p.floating = false;
        const int i = regionAt (t);
        if (i < 0) { p.region = juce::Uuid::null(); p.take = juce::Uuid::null(); p.srcPos = 0; return; }
        const auto& r = regions[(size_t) i];
        p.region = r.id; p.take = r.takeId; p.srcPos = r.srcIn + (t - r.startSample);
    }

    /** Adds a point at timeline sample t (or moves a lane's point if 'replaceIndex' >= 0). Returns the point's index after sorting. */
    int addPoint (AutoLane& lane, juce::int64 t, float value)
    {
        AutoPoint p; p.id = juce::Uuid(); p.value = lane.clampValue (value);
        anchorPoint (p, juce::jmax ((juce::int64) 0, t));
        const auto id = p.id;
        lane.pts.push_back (p); lane.sortPoints();
        for (int i = 0; i < (int) lane.pts.size(); ++i) if (lane.pts[(size_t) i].id == id) return i;
        return (int) lane.pts.size() - 1;
    }

    /** The user moved a point: it attaches to whatever audio is now under it. Returns its new index. */
    int movePoint (AutoLane& lane, int index, juce::int64 t, float value)
    {
        if (! juce::isPositiveAndBelow (index, (int) lane.pts.size())) return index;
        auto p = lane.pts[(size_t) index];
        p.value = lane.clampValue (value);
        anchorPoint (p, juce::jmax ((juce::int64) 0, t));
        lane.pts[(size_t) index] = p; lane.sortPoints();
        for (int i = 0; i < (int) lane.pts.size(); ++i) if (lane.pts[(size_t) i].id == p.id) return i;
        return index;
    }

    /** Called after ANY change to the edit's pieces. Every point goes to where its audio is now. A point whose audio has gone is put where the
        edit's change suggests (it moves like its nearest surviving neighbours, in proportion) and marked 'floating' so it can be checked. */
    void resolveAutomation()
    {
        for (auto& lane : lanes)
        {
            const size_t n = lane.pts.size();
            std::vector<juce::int64> newTime (n, 0);
            std::vector<char> lost (n, 0);
            for (size_t k = 0; k < n; ++k)
            {
                auto& p = lane.pts[k];
                newTime[k] = p.time;
                if (p.region.isNull()) continue;                                  // fixed in time
                const int ri = [&] { for (int i = 0; i < (int) regions.size(); ++i) if (regions[(size_t) i].id == p.region) return i; return -1; }();
                if (ri >= 0 && p.srcPos >= regions[(size_t) ri].srcIn && p.srcPos <= regions[(size_t) ri].srcOut)
                {
                    newTime[k] = regions[(size_t) ri].startSample + (p.srcPos - regions[(size_t) ri].srcIn);
                    continue;
                }
                // its piece was split, trimmed or replaced: is the same audio of the same take in another piece? (the nearest one to where it was)
                int best = -1; juce::int64 bestDist = 0, bestTime = 0;
                if (! p.take.isNull())
                    for (int i = 0; i < (int) regions.size(); ++i)
                    {
                        const auto& r = regions[(size_t) i];
                        if (r.takeId != p.take || p.srcPos < r.srcIn || p.srcPos > r.srcOut) continue;
                        const auto t = r.startSample + (p.srcPos - r.srcIn);
                        const auto d = t > p.time ? t - p.time : p.time - t;
                        if (best < 0 || d < bestDist) { best = i; bestDist = d; bestTime = t; }
                    }
                if (best >= 0) { newTime[k] = bestTime; p.region = regions[(size_t) best].id; }
                else lost[k] = 1;
            }
            for (size_t k = 0; k < n; ++k)
            {
                if (! lost[k]) continue;
                const auto t0 = lane.pts[k].time;
                int prev = -1, next = -1;
                for (int j = (int) k - 1; j >= 0; --j) if (! lost[(size_t) j]) { prev = j; break; }
                for (size_t j = k + 1; j < n; ++j) if (! lost[j]) { next = (int) j; break; }
                double shift = 0.0;
                const double sp = prev >= 0 ? (double) (newTime[(size_t) prev] - lane.pts[(size_t) prev].time) : 0.0;
                const double sn = next >= 0 ? (double) (newTime[(size_t) next] - lane.pts[(size_t) next].time) : 0.0;
                if (prev >= 0 && next >= 0)
                {
                    const double span = (double) (lane.pts[(size_t) next].time - lane.pts[(size_t) prev].time);
                    const double f = span > 0.0 ? (double) (t0 - lane.pts[(size_t) prev].time) / span : 0.5;
                    shift = sp + (sn - sp) * juce::jlimit (0.0, 1.0, f);
                }
                else if (prev >= 0) shift = sp;
                else if (next >= 0) shift = sn;
                newTime[k] = juce::jmax ((juce::int64) 0, t0 + (juce::int64) std::llround (shift));
            }
            for (size_t k = 0; k < n; ++k) { lane.pts[k].time = newTime[k]; lane.pts[k].floating = lost[k] != 0; }
            lane.sortPoints();
        }
    }

    static constexpr double kDefaultHalfFade = 0.015;     // default crossfade: 30 ms centred on the join

    juce::int64 lengthSamples() const
    {
        juce::int64 e = 0;
        for (auto& r : regions) e = juce::jmax (e, r.endSample());
        for (auto& r : overdubs) e = juce::jmax (e, r.endSample());
        return e;
    }
    juce::int64 firstSample() const
    {
        juce::int64 f = std::numeric_limits<juce::int64>::max();
        for (auto& r : regions) f = juce::jmin (f, r.startSample);
        for (auto& r : overdubs) f = juce::jmin (f, r.startSample);
        return f == std::numeric_limits<juce::int64>::max() ? 0 : f;
    }
    bool isEmpty() const { return regions.empty() && overdubs.empty(); }
    double lengthSeconds() const { return sampleRate > 0 ? (double) lengthSamples() / sampleRate : 0.0; }

    /** Keep one region's fades legal: inside the audio that exists, in order, and not crossing each other. */
    void clampFades (int i)
    {
        if (! juce::isPositiveAndBelow (i, (int) regions.size())) return;
        auto& r = regions[(size_t) i];
        const double rate = juce::jmax (1.0, r.sampleRate);
        const double len = (double) r.length() / rate;
        r.inStart = juce::jmax (r.inStart, -(double) r.srcIn / rate);
        r.inEnd   = juce::jmax (r.inEnd, r.inStart);
        if (r.sourceLength > 0) r.outEnd = juce::jmin (r.outEnd, (double) (r.sourceLength - r.srcOut) / rate);
        r.outStart = juce::jmin (r.outStart, r.outEnd);
        r.outStart = juce::jmax (r.outStart, r.inEnd - len);        // the fade-out may not begin before the fade-in has finished
        r.outEnd   = juce::jmax (r.outEnd, r.outStart);
    }

    void clampAll() { for (int i = 0; i < (int) regions.size(); ++i) clampFades (i); if (insertIndex > (int) regions.size()) insertIndex = -1; }

    /** Default 30 ms centred crossfade for the join before region i. */
    void setDefaultJoinFades (int i)
    {
        if (i <= 0 || i >= (int) regions.size()) return;
        auto& a = regions[(size_t) i - 1]; auto& b = regions[(size_t) i];
        a.outStart = -kDefaultHalfFade; a.outEnd = kDefaultHalfFade;
        b.inStart  = -kDefaultHalfFade; b.inEnd  = kDefaultHalfFade;
        clampFades (i - 1); clampFades (i);
    }

    /** The two ends of the edit get a short fade (25 ms by default: no click, and the file is untouched). A fade the user already made
        there (starting at the piece's own start / end) is kept; a hard cut, or the crossfade left over from when the piece was in the middle, is replaced. */
    void hardenEnds()
    {
        if (regions.empty()) return;
        auto& f = regions.front(); auto& b = regions.back();
        if (f.inStart != 0.0 || f.inEnd <= f.inStart || f.inEnd < 0.0011) { f.inStart = 0.0; f.inEnd = kDefaultEdgeFade; }
        if (b.outEnd != 0.0 || b.outEnd <= b.outStart || b.outStart > -0.0011) { b.outStart = -kDefaultEdgeFade; b.outEnd = 0.0; }
    }

    /** Insert a region at the insert point (default: after the last one), rippling later regions. Returns its index. */
    int insertRegion (EditRegion r)
    {
        if (isEmpty()) sampleRate = r.sampleRate;
        const int n = (int) regions.size();
        const int at = insertIndex < 0 || insertIndex > n ? n : insertIndex;
        r.startSample = at >= n ? (n == 0 ? 0 : regions.back().endSample()) : regions[(size_t) at].startSample;
        r.inStart = r.inEnd = r.outStart = r.outEnd = 0.0;
        const auto len = r.length();
        for (int j = at; j < n; ++j) regions[(size_t) j].startSample += len;
        regions.insert (regions.begin() + at, std::move (r));
        insertIndex = -1;
        setDefaultJoinFades (at);
        setDefaultJoinFades (at + 1);
        hardenEnds();
        clampAll();
        return at;
    }

    /** Remove a region; later regions move up to close the gap when 'ripple' is true. */
    void removeRegion (int i, bool ripple = true)
    {
        if (! juce::isPositiveAndBelow (i, (int) regions.size())) return;
        const auto len = regions[(size_t) i].length();
        regions.erase (regions.begin() + i);
        if (ripple) for (int j = i; j < (int) regions.size(); ++j) regions[(size_t) j].startSample -= len;
        if (isEmpty()) sampleRate = 0.0;
        setDefaultJoinFades (i);
        hardenEnds();
        clampAll();
    }

    /** Re-order: moves a region to a new place in the list and re-packs the regions end to end. */
    void moveRegion (int from, int to)
    {
        const int n = (int) regions.size();
        if (! juce::isPositiveAndBelow (from, n) || ! juce::isPositiveAndBelow (to, n) || from == to) return;
        juce::int64 pos = regions.front().startSample;
        auto r = std::move (regions[(size_t) from]);
        regions.erase (regions.begin() + from);
        regions.insert (regions.begin() + to, std::move (r));
        for (auto& x : regions) { x.startSample = pos; pos += x.length(); }
        for (int j = juce::jmax (1, juce::jmin (from, to) - 1); j <= juce::jmin (n - 1, juce::jmax (from, to) + 1); ++j) setDefaultJoinFades (j);
        hardenEnds();
        clampAll();
    }

    /** True if edit point k (k = the join before regions[k]) was made by an offline fix: such joins are not counted or numbered, and "next / previous fade" skips them. */
    bool isFixJoin (int k) const noexcept { return k > 0 && k < (int) regions.size() && (regions[(size_t) k].fixIn || regions[(size_t) k - 1].fixOut); }
    EditRegion* find (const juce::Uuid& rid) { for (auto& r : regions) if (r.id == rid) return &r; return nullptr; }
    /** A piece of the main track OR an overdub. */
    EditRegion* findAny (const juce::Uuid& rid) { if (auto* r = find (rid)) return r; for (auto& o : overdubs) if (o.id == rid) return &o; return nullptr; }
    bool isOverdub (const juce::Uuid& rid) const { for (auto& o : overdubs) if (o.id == rid) return true; return false; }
    bool removeOverdub (const juce::Uuid& rid)
    {
        for (size_t i = 0; i < overdubs.size(); ++i) if (overdubs[i].id == rid) { overdubs.erase (overdubs.begin() + (long) i); if (isEmpty()) sampleRate = 0.0; return true; }
        return false;
    }

    /** Cuts region i in two at 'offset' samples after its start (the audio carries on unbroken, with a short crossfade over the cut).
        The volume changes are shared out between the two halves. Returns the index of the right half. */
    int splitRegion (int i, juce::int64 offset)
    {
        if (! juce::isPositiveAndBelow (i, (int) regions.size())) return i;
        auto left = regions[(size_t) i];
        if (offset <= 0 || offset >= left.length()) return i;
        EditRegion right = left;
        left.waiting = WaitingPiece(); right.waiting = WaitingPiece();            // a cut piece no longer matches the exported files
        right.id = juce::Uuid();
        right.fixIn = false; left.fixOut = false;                            // (the new join is an edit; the caller of a fix marks it as a fix join)
        right.srcIn += offset; right.startSample += offset;
        right.gains.clear();
        for (auto& g : left.gains) if (g.at >= offset) { auto c = g; c.at -= offset; right.gains.push_back (c); }
        if (! left.gains.empty())                                             // the right half starts at the level the left half had reached
        {
            std::vector<float> start;
            for (size_t f = 0; f < left.files.size(); ++f) start.push_back (left.levelDbAt (f, offset));
            if (right.gains.empty() || right.gains.front().at > 0) { GainChange c; c.at = 0; c.ramp = 0.0; c.db = start; right.gains.insert (right.gains.begin(), c); }
        }
        left.gains.erase (std::remove_if (left.gains.begin(), left.gains.end(), [offset] (const GainChange& g) { return g.at >= offset; }), left.gains.end());
        left.srcOut = left.srcIn + offset;
        left.outStart = left.outEnd = 0.0;
        right.inStart = right.inEnd = 0.0;
        regions[(size_t) i] = std::move (left);
        regions.insert (regions.begin() + i + 1, std::move (right));
        setDefaultJoinFades (i + 1);
        clampAll();
        return i + 1;
    }

    /** Puts a piece on the main track AT 'sample': a piece that is playing there is cut in two to make room, and everything after moves later. */
    int insertRegionAt (EditRegion r, juce::int64 sample)
    {
        if (isEmpty()) sampleRate = r.sampleRate;
        sample = juce::jmax ((juce::int64) 0, sample);
        int at = (int) regions.size();
        for (int i = 0; i < (int) regions.size(); ++i)
        {
            auto& x = regions[(size_t) i];
            if (sample > x.startSample && sample < x.endSample()) { at = splitRegion (i, sample - x.startSample); break; }
            if (x.startSample >= sample) { at = i; break; }
        }
        const int n = (int) regions.size();
        r.startSample = sample;
        r.inStart = r.inEnd = r.outStart = r.outEnd = 0.0;
        const auto len = r.length();
        for (int j = at; j < n; ++j) regions[(size_t) j].startSample += len;
        regions.insert (regions.begin() + at, std::move (r));
        insertIndex = -1;
        setDefaultJoinFades (at);
        setDefaultJoinFades (at + 1);
        hardenEnds();
        clampAll();
        return at;
    }

    /** Lays a piece OVER the edit at 'sample'. It plays together with everything else; short fades only stop clicks. */
    void addOverdub (EditRegion r, juce::int64 sample)
    {
        if (isEmpty()) sampleRate = r.sampleRate;
        r.startSample = juce::jmax ((juce::int64) 0, sample);
        r.inStart = 0.0; r.inEnd = 0.005; r.outStart = -0.005; r.outEnd = 0.0;      // an overdub: 5 ms
        overdubs.push_back (std::move (r));
    }
    int indexOf (const juce::Uuid& rid) const { for (size_t i = 0; i < regions.size(); ++i) if (regions[i].id == rid) return (int) i; return -1; }

    // ------------------------------------------------------------------ moving things (edit window and trim window)

    /** Slide region i (just the region, nothing else changes about its audio) by d samples.
        slipLeft: every region before it moves with it.  slipRight: every region after it moves with it.
        Returns the distance actually moved (the move stops where regions would change order or pass the start). */
    juce::int64 slideRegion (int i, juce::int64 d, bool slipLeft, bool slipRight)
    {
        const int n = (int) regions.size();
        if (! juce::isPositiveAndBelow (i, n) || d == 0) return 0;
        auto moves = [&] (int j) { return j == i || (j < i && slipLeft) || (j > i && slipRight); };

        juce::int64 lo = std::numeric_limits<juce::int64>::min(), hi = std::numeric_limits<juce::int64>::max();
        for (int j = 0; j < n; ++j)
        {
            if (moves (j)) lo = juce::jmax (lo, -regions[(size_t) j].startSample);                     // never before time zero
            if (j + 1 < n)
            {
                const auto sj = regions[(size_t) j].startSample, sk = regions[(size_t) j + 1].startSample;
                if (moves (j) && ! moves (j + 1)) hi = juce::jmin (hi, sk - sj);                        // may not pass the next region's start
                if (! moves (j) && moves (j + 1)) lo = juce::jmax (lo, sj - sk);                        // may not pass the previous region's start
            }
        }
        d = juce::jlimit (lo, juce::jmax (lo, hi), d);
        for (int j = 0; j < n; ++j) if (moves (j)) regions[(size_t) j].startSample += d;
        return d;
    }

    /** Trim window: slide the OUT audio (region k-1) under the join. The join does not move; the audio to its
        left moves later by d (earlier if d is negative). slipLeft: the regions before it move too, otherwise they stay put. */
    juce::int64 slipOut (int k, juce::int64 d, bool slipLeft)
    {
        const int n = (int) regions.size();
        if (k < 1 || k >= n || d == 0) return 0;
        auto& a = regions[(size_t) k - 1];
        const auto minLen = (juce::int64) (0.01 * a.sampleRate);
        juce::int64 lo = std::numeric_limits<juce::int64>::min() / 4, hi = std::numeric_limits<juce::int64>::max() / 4;
        hi = juce::jmin (hi, a.length() - minLen);                                      // the region must keep some length
        if (a.sourceLength > 0) lo = juce::jmax (lo, a.srcOut - a.sourceLength);        // srcOut - d <= sourceLength
        for (int j = 0; j <= k - 1; ++j)
        {
            const bool moved = j == k - 1 || slipLeft;
            if (moved) lo = juce::jmax (lo, -regions[(size_t) j].startSample);          // not before time zero
        }
        if (! slipLeft && k - 2 >= 0) lo = juce::jmax (lo, regions[(size_t) k - 2].startSample - a.startSample);   // keep order
        d = juce::jlimit (lo, juce::jmax (lo, hi), d);
        a.startSample += d; a.srcOut -= d;
        if (slipLeft) for (int j = 0; j < k - 1; ++j) regions[(size_t) j].startSample += d;
        clampFades (k - 1);
        return d;
    }

    /** Trim window: slide the IN audio (region k) under the join. The join does not move; the audio to its right
        moves later by d. slipRight: the regions after it move too (by the amount the region grew), otherwise they stay put. */
    juce::int64 slipIn (int k, juce::int64 d, bool slipRight)
    {
        const int n = (int) regions.size();
        if (k < 1 || k >= n || d == 0) return 0;
        auto& b = regions[(size_t) k];
        const auto minLen = (juce::int64) (0.01 * b.sampleRate);
        const juce::int64 lo = minLen - b.length();     // the region keeps some length
        const juce::int64 hi = b.srcIn;                 // srcIn - d >= 0
        d = juce::jlimit (lo, juce::jmax (lo, hi), d);
        b.srcIn -= d;
        if (slipRight) for (int j = k + 1; j < n; ++j) regions[(size_t) j].startSample += d;
        clampFades (k);
        return d;
    }

    /** Trim window: move the join itself. Both audios stay where they are in time; the cut moves. */
    juce::int64 moveJoin (int k, juce::int64 d)
    {
        const int n = (int) regions.size();
        if (k < 1 || k >= n || d == 0) return 0;
        auto& a = regions[(size_t) k - 1]; auto& b = regions[(size_t) k];
        const auto minA = (juce::int64) (0.01 * a.sampleRate), minB = (juce::int64) (0.01 * b.sampleRate);
        juce::int64 lo = juce::jmax (-(a.length() - minA), -b.srcIn);                   // earlier: A shorter, B gains from before its srcIn
        juce::int64 hi = b.length() - minB;                                             // later: B shorter
        if (a.sourceLength > 0) hi = juce::jmin (hi, a.sourceLength - a.srcOut);        // A needs source after its srcOut
        d = juce::jlimit (lo, juce::jmax (lo, hi), d);
        a.srcOut += d;
        b.srcIn += d; b.startSample += d;
        clampFades (k - 1); clampFades (k);
        return d;
    }

    /** 'A' key: copy the fade-out of the region before the join onto the fade-in of the region after it. */
    void copyOutToIn (int k)
    {
        if (k < 1 || k >= (int) regions.size()) return;
        auto& a = regions[(size_t) k - 1]; auto& b = regions[(size_t) k];
        b.inStart = a.outStart; b.inEnd = a.outEnd; b.curve = a.curve;
        clampFades (k);
    }
    /** 'Z' key: the other way round. */
    void copyInToOut (int k)
    {
        if (k < 1 || k >= (int) regions.size()) return;
        auto& a = regions[(size_t) k - 1]; auto& b = regions[(size_t) k];
        a.outStart = b.inStart; a.outEnd = b.inEnd; a.curve = b.curve;
        clampFades (k - 1);
    }
};

inline float fadeGain (FadeCurve c, float x, bool fadeIn) noexcept   // x = 0..1 through the fade
{
    x = juce::jlimit (0.0f, 1.0f, x);
    return fadeInShape (c, fadeIn ? x : 1.0f - x);
}
} // namespace td
