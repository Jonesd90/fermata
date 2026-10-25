#pragma once
#include "Edit.h"
#include <functional>

namespace td { namespace retime
{
/** The ReTimEr (2.103): slows down or speeds up a part of an edit - every track together, without changing the pitch - and moves everything after
    it along. Two ways in:
      - a WHOLE PIECE (the piece that was clicked, from the edit point before it to the edit point after it): evenly, its own audio stretched together with
        20 s of its take on either side, so its edit points can still be trimmed afterwards;
      - MARKS 1 - 2: only the music between the marks, with a glide into the new speed after mark 1 (ramp in) and back to the normal speed before
        mark 2 (ramp out), 0 = at once.
    The audio is made again by Signalsmith Stretch (third_party): one stretcher for all the channels of a piece, so the time differences between the
    microphones stay exactly as they were. Edit points inside the part keep working: each take is stretched on its own (with handles) and its
    crossfades stay where the music is. */

// ============================================================================================== the speed plan
/** How the ReTimEr moves time: the original timeline (samples) onto the new one. Speed 1 (nothing changes) before t0. From t0 the music stretches by
    S (new length / old length; above 1 is slower) in the held part, gliding in from speed 1 over rIn samples after t0 and back out to speed 1 over rOut
    samples before t1 (raised-cosine glides: no corner at either end); after t1 the speed is 1 again and everything is later by delta().
    'uniform': S everywhere (a whole piece's own audio, its handles included). */
struct TimeMap
{
    double t0 = 0.0, t1 = 0.0, S = 1.0, rIn = 0.0, rOut = 0.0;
    bool uniform = false;

    /** New samples per original sample at original time t. */
    double rate (double t) const noexcept
    {
        if (uniform) return S;
        if (t < t0 || t >= t1) return 1.0;
        if (rIn > 0.0 && t < t0 + rIn) return 1.0 + (S - 1.0) * w ((t - t0) / rIn);
        if (rOut > 0.0 && t >= t1 - rOut) return 1.0 + (S - 1.0) * w ((t1 - t) / rOut);
        return S;
    }
    /** New time of original time t. */
    double map (double t) const noexcept
    {
        if (uniform) return t0 + (t - t0) * S;
        if (t <= t0) return t;
        const double a = t0 + rIn, b = t1 - rOut;
        if (t < a) return t0 + (t - t0) + (S - 1.0) * rIn * F ((t - t0) / rIn);
        const double mapA = a + (S - 1.0) * rIn * 0.5;
        if (t < b) return mapA + (t - a) * S;
        const double mapB = mapA + (b - a) * S;
        if (t < t1) { const double y = (t - b) / rOut; return mapB + rOut * (y + (S - 1.0) * (0.5 - F (1.0 - y))); }
        return mapB + rOut * (1.0 + (S - 1.0) * 0.5) + (t - t1);
    }
    /** Original time of new time u (the inverse of map). */
    double unmap (double u) const noexcept
    {
        if (uniform) return t0 + (u - t0) / S;
        if (u <= t0) return u;
        const double a = t0 + rIn, b = t1 - rOut, mapA = map (a), mapB = map (b), mapT1 = map (t1);
        if (u >= mapT1) return u - (mapT1 - t1);
        if (u >= mapA && u < mapB) return a + (u - mapA) / S;
        double lo = u < mapA ? t0 : b, hi = u < mapA ? a : t1;                  // a glide: Newton, kept inside the bracket (map only ever rises)
        double t = lo + (hi - lo) * (u - map (lo)) / juce::jmax (1.0e-9, map (hi) - map (lo));
        for (int i = 0; i < 60; ++i)
        {
            const double f = map (t) - u;
            if (std::abs (f) < 1.0e-7) break;
            if (f > 0.0) hi = t; else lo = t;
            const double r = rate (t);
            double next = t - f / juce::jmax (1.0e-9, r);
            if (! (next > lo && next < hi)) next = 0.5 * (lo + hi);
            t = next;
        }
        return t;
    }
    /** How much later (in samples) everything after t1 is. */
    double delta() const noexcept { return uniform ? (t1 - t0) * (S - 1.0) : map (t1) - t1; }
    double newLength() const noexcept { return map (t1) - t0; }
    /** The held stretch S that makes a part of 'length' samples last 'target' samples with glides of rIn and rOut samples (rIn + rOut <= length).
        At or below 0 when it cannot be done. */
    static double solve (double length, double target, double rIn, double rOut) noexcept
    {
        const double held = length - 0.5 * (rIn + rOut);
        return held > 0.0 ? 1.0 + (target - length) / held : 0.0;
    }
    static double w (double x) noexcept { return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * juce::jlimit (0.0, 1.0, x)); }
    static double F (double x) noexcept { x = juce::jlimit (0.0, 1.0, x); return 0.5 * x - std::sin (juce::MathConstants<double>::pi * x) / juce::MathConstants<double>::twoPi; }
};

// ============================================================================================== what the window asks for, and the plan
constexpr double kStretchMin = 0.5, kStretchMax = 2.0;        // the stretch (new length / old) of the held part: the library is clean within this
constexpr double kFixFade = 0.030;                            // the crossfade where the retimed audio starts inside a take (the two are the same sound there)
constexpr double kOutFade = 0.060;                            // ... and where it hands back to the take after mark 2 (equal power: see makePlan)
constexpr double kGuard = 0.060;                              // a mark this close to an edit point's crossfade goes onto the edit point
constexpr double kHandleWhole = 20.0, kHandleInner = 5.0;     // extra audio stretched beyond the edit points of a whole piece / inside marks 1 - 2
constexpr double kRunOn = 30.0;                               // after mark 2 the retimed take runs on to its next edit point when that is this close

struct Request
{
    bool wholePiece = false;
    juce::int64 from = 0, to = 0;          // marks 1 - 2 (timeline samples)
    juce::Uuid piece;                      // whole piece: the piece that was clicked
    double S = 1.0;                        // the stretch of the held part (new / old)
    double rampIn = 0.0, rampOut = 0.0;    // seconds of the original (marks only)
    juce::String label;                    // what the new pieces and files are called after, e.g. "105.0%"
};

/** One file a stream reads: one piece's audio on one track. Virtual time t reads take sample srcIn + (t - start). A piece joined to its neighbour
    by a fix join (an offline fix) is read together with it, through that join's crossfade; the stream's own two ends are left open (handles). */
struct Source
{
    juce::File file; juce::int64 fileStart = 0;
    juce::int64 start = 0, srcIn = 0;
    bool fadeIn = false, fadeOut = false;                   // a fix join inside the stream at this end
    juce::int64 inB = 0, inE = 0, outB = 0, outE = 0;
    FadeCurve inCurve = FadeCurve::Linear, outCurve = FadeCurve::Linear;
    float gainAt (juce::int64 t) const noexcept
    {
        float g = 1.0f;
        if (fadeIn)  g *= t < inB ? 0.0f : t >= inE ? 1.0f : EditRegion::fadeGainFor (inCurve, (float) (t - inB) / (float) juce::jmax ((juce::int64) 1, inE - inB), true);
        if (fadeOut) g *= t < outB ? 1.0f : t >= outE ? 0.0f : EditRegion::fadeGainFor (outCurve, (float) (t - outB) / (float) juce::jmax ((juce::int64) 1, outE - outB), false);
        return g;
    }
    /** The virtual times this source can be heard at (its open ends reach as far as its file). */
    juce::int64 from() const noexcept { return fadeIn ? inB : (std::numeric_limits<juce::int64>::min)(); }
    juce::int64 to() const noexcept   { return fadeOut ? outE : (std::numeric_limits<juce::int64>::max)(); }
};
struct StreamTrack { juce::Uuid trackId; juce::String trackName; int numChannels = 1; std::vector<Source> sources; juce::File dest; };
/** Audio that is made again in one go, through one stretcher: a take's piece (with any pieces fix-joined to it), or an overdub. */
struct Stream
{
    juce::int64 a = 0, b = 0;              // what is read (original timeline)
    juce::int64 outA = 0, outB = 0;        // what is made (new timeline): sample 0 of the new files is outA
    TimeMap audio;                         // how its audio is stretched
    std::vector<StreamTrack> tracks;
    juce::String what;                     // its name, for messages
    int channels() const { int n = 0; for (auto& t : tracks) n += t.numChannels; return n; }
};

struct Plan
{
    juce::String error, note;              // why it cannot be done; what was adjusted (a mark moved onto an edit point ...)
    double sampleRate = 48000.0;
    bool wholePiece = false;
    juce::int64 t0 = 0, t1 = 0, delta = 0; // the part (original timeline) and how much later everything after it is
    TimeMap map;                           // where every moment of the edit goes
    std::vector<EditRegion> regions, overdubs;      // the edit afterwards (the retimed pieces point at their new files)
    std::vector<Stream> streams;
    std::vector<juce::Uuid> retimed;       // the new pieces
    /** Where timeline sample t goes. */
    juce::int64 newSampleAt (juce::int64 t) const noexcept
    {
        if (t <= t0) return t;
        if (t >= t1) return t + delta;
        return (juce::int64) std::llround (map.map ((double) t));
    }
    juce::int64 framesToMake() const { juce::int64 n = 0; for (auto& s : streams) n += (s.outB - s.outA) * (juce::int64) juce::jmax (1, s.channels()); return n; }
    bool ok() const noexcept { return error.isEmpty(); }
};

/** Marks 1 - 2 that fall on (or within kGuard of) a crossfade at an edit point, or the fade at the start / end of a piece, go onto that edit point / edge, so
    that the take before it stays as it is and the one after it is retimed from its start. Joins made by offline fixes are not edit points: a mark there stays
    (the retimed part then takes the whole fix join in, see makePlan). 'note' gets a sentence for each mark that moved. */
std::pair<juce::int64, juce::int64> snapSpan (const EditDef& e, juce::int64 from, juce::int64 to, juce::String* note = nullptr);

/** The pieces of a whole-piece retime: from the edit point before the piece to the one after it (fix joins and snips are inside). {-1,-1} if not found. */
std::pair<int, int> wholeRun (const EditDef& e, const juce::Uuid& piece);

using FileLength = std::function<juce::int64 (const juce::File&)>;           // samples in a file, -1 if it cannot be read
using IsLtc = std::function<bool (const juce::Uuid& trackId)>;               // the timecode track (never stretched: a retimed piece has no LTC)
/** The quietest moment (timeline sample) of piece 'r' (all its tracks) within [from, to): where the retimed audio hands back to the take after mark 2. */
using Quietest = std::function<juce::int64 (const EditRegion& r, juce::int64 from, juce::int64 to)>;

/** Works out everything (any thread): what is cut where, what is made again, where everything goes. The new files are named in 'folder' (not made). */
Plan makePlan (const EditDef& e, const Request& q, const juce::File& folder, const FileLength& length, const IsLtc& isLtc, const Quietest& quietest = {});
/** Puts a plan (made from this same edit) into the edit: its pieces and overdubs, the automation points (each stays on its music), the insert point (by the
    piece it was before). Marks 1 - 2 are not moved (they stay what the person marked, also after an Undo). */
void applyPlan (EditDef& e, const Plan& p);

/** Makes the new files of all the streams (job thread), up to 'threads' streams at a time. Returns "" or why not; on failure or cancel nothing is left behind. */
juce::String renderStreams (const Plan&, const std::function<bool()>& cancelled, const std::function<void (float)>& progress, int threads = 2);

/** A Quietest for makePlan, from the piece's files (any thread): the start of the quietest kOutFade-long stretch of [from, to), all tracks summed. */
juce::int64 quietestMoment (juce::AudioFormatManager&, const EditRegion& r, juce::int64 from, juce::int64 to, const IsLtc& isLtc);

// ---- lower level (and the tests)
/** Fills 'out' (numChannels vectors, already n long and zeroed) with the audio to be stretched over [start, start + n) of the original timeline. */
using ReadFn = std::function<void (juce::int64 start, int n, std::vector<std::vector<float>>& out)>;
/** Receives the stretched audio in order: 'block' (numChannels vectors) holds n samples from new-timeline sample 'at'. False stops the stretch. */
using WriteFn = std::function<bool (const std::vector<std::vector<float>>& block, int n, juce::int64 at)>;
/** Stretches the audio through 'map' and delivers exactly [outA, outB) of the new timeline (every channel through one stretcher). */
juce::String stretch (int numChannels, double sampleRate, const TimeMap& map, juce::int64 outA, juce::int64 outB, const ReadFn&, const WriteFn&,
                      const std::function<bool()>& cancelled = {}, const std::function<void (double)>& progress = {});
}} // namespace td::retime
