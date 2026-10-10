#pragma once
/** Re-HarmoniSer core engine (plain C++17, no JUCE, no file or UI code).

    It works on float buffers in memory: one buffer per channel, all the same length and sample rate. Times are in seconds from the start of the
    buffers you give it. Every correction is applied to ALL channels in the same way (so channels never end up with different pitches).
    The functions are thread-safe (no shared state) and may run on a background thread; the long ones take Hooks for progress and cancelling. */
#include "Dsp.h"

namespace rehar
{
/** The settings of one note correction (the defaults are the prototype's). */
struct Params
{
    double strength = 1.0, smooth = 0.25, ease = 0.10, hold = 0.25, fadeOut = 0.35, bw = 45.0;
    int nharm = 10;
    bool match = true;
    double a4 = 440.0, move = 0.0, snap = 0.0, keep = 0.0;
};

/** Planar float audio: block[channel][frame]. */
using Block = std::vector<std::vector<float>>;
inline std::vector<Span> spansOf (const Block& b) { std::vector<Span> s; for (auto& c : b) s.push_back ({ c.data(), c.size() }); return s; }

// ---------------------------------------------------------------------------------------------------- measuring a note
/** The channels to measure a note with (the loudest few in the note's pitch range after weighting) and their weights. visGainDb: the "see" gain per channel (empty = all 0 dB). */
struct Tracking { std::vector<int> channels; Vec weights; };
Tracking trackingInputs (const std::vector<Span>& seg, int sr, double fLo, double fHi, const Vec& visGainDb = {}, int kmax = 6);

struct Track { Vec ts, f0; };
/** Harmonic-comb f0 track inside [t0, t1] for a fundamental in [fLo, fHi]. chans: the chosen channels (full rate), ws: their weights. tSeg0: time of sample 0 of chans. Throws std::runtime_error with a message for the user. */
Track trackNote (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, double hopS = 0.02);
/** Keeps the demodulated partials centred (refines the f0 track). */
Vec refineTrack (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0, double bw = 30, int nh = 4, int iters = 4, double smoothS = 0.12);
/** Does the measured line look like an overtone? Returns k (2..4) when it probably is the k-th partial, else 1. */
int detectPartial (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0);

// ---------------------------------------------------------------------------------------------------- the correction
struct Plan
{
    int refMidi = 0; double refHz = 0; std::string refLabel;
    Vec c, wob, off, planned;                 // cents: as sung, its wobble, the correction, as planned
    std::string warning;
};
/** Works out the correction for one note. pull: cents per time step added by hand (empty or the size of ts). */
Plan planCurve (const Vec& ts, const Vec& f0, const Params& p, int midi, const Vec& pull = {});

/** Retunes the note in 'seg' (all channels alike). f0 in Hz and off in cents on the grid ts (seconds, from the start of seg + tSeg0). Returns the corrected copy. */
Block retuneSegment (const Block& seg, int sr, double tSeg0, const Vec& ts, const Vec& f0, const Vec& off, const Params& p, const Hooks& hooks = {});
/** Keeps only the picked note (fundamental and overtones, following fPts) between tA and tB; everything else there is removed (for the Isolate audition). */
Block soloSegment (const Block& seg, int sr, double tSeg0, const Vec& ts, const Vec& fPts, const Params& p, double tA, double tB, double fade = 0.06);

struct LevelReport { double rise = 0, riseT = 0, dip = 0, dipT = 0, stdev = 0; };
LevelReport levelReport (const Block& before, const Block& after, int sr, double tSeg0, double tA, double tB, double bandLo, double bandHi);

// ---------------------------------------------------------------------------------------------------- Erase ReBrush
struct Brush
{
    std::vector<std::pair<double, double>> pts;     // (time s, pitch Hz) along the stroke
    double rt = 0.2, rc = 60.0, amount = 1.0;       // radius in time (s) and in cents; how much is taken out (0..1)
    bool harm = true;                               // also the overtones above
};
/** The stretch of time (seconds) a brush stroke touches. */
std::pair<double, double> brushRegion (const Brush&);
Block brushSegment (const Block& seg, int sr, double tSeg0, const Brush&, int nharm = 10, const Hooks& hooks = {});

// ---------------------------------------------------------------------------------------------------- Section ReFinement (several voices on one note)
struct VoicePeak { double cents, amp; int partial; };
struct VoiceScan { Vec ts; std::vector<std::vector<VoicePeak>> peaks; Vec grid, hist, centres; };
/** The channels (decimated to <= 24 kHz, as double) used to follow voices, with their weights and rate. */
struct VoiceInputs { std::vector<Vec> chans; Vec ws; int sr = 0; };
VoiceInputs voiceInputs (const std::vector<Span>& seg, int sr, double fT, const Vec& visGainDb = {});
VoiceScan findVoices (const VoiceInputs& in, double tSeg0, double t0, double t1, double fT, int H = 8, double span = 60.0, double win = 0.6, double hop = 0.15);
/** C[voice][time]: each voice's cents from the written note over the grid ts. */
std::vector<Vec> trackVoices (const Vec& ts, const std::vector<std::vector<VoicePeak>>& peaks, const Vec& centres, double minsep = 4.0);
struct VoiceSet
{
    double t0 = 0, t1 = 0, a4 = 440.0, fT = 0; int midi = 0;
    Vec ts; std::vector<Vec> C; Vec strength;
    Vec means() const;
};
/** What the Find voices button does. Throws std::runtime_error (words for the user) when nothing is found. */
VoiceSet detectVoices (const std::vector<Span>& seg, int sr, double tSeg0, double t0, double t1, int midi, double a4, const Vec& visGainDb = {}, const Hooks& hooks = {});
/** Moves each voice j by offs[j] cents (0 = leave alone), in every channel the same way. */
Block retuneVoices (const Block& seg, int sr, double tSeg0, double fT, const Vec& ts, const std::vector<Vec>& C, const Vec& offs, double tA, double tB, const VoiceInputs& trk,
                    const Hooks& hooks = {}, int H = 12, double bwMax = 45.0, double fade = 0.15, double bwLow = 0.8);

// ---------------------------------------------------------------------------------------------------- Ensemble ReCentre (the choir's own A4)
struct RefPoints { Vec T, D, W; };
/** Steady spectral peaks of the loudest channels: time, cents from the nearest A=440 semitone (-50..50) and weight. segStart = time of sample 0. Throws when under 2 s. */
RefPoints refPoints (const std::vector<Span>& seg, int sr, double segStart, const Vec& visGainDb = {}, int kmax = 4, const Hooks& hooks = {});
struct RefStats { bool ok = false; double cents = 0, a4 = 440.0, spread = 0; int n = 0; };
RefStats refStats (const Vec& D, const Vec& W, int minN = 20);
struct RefSeriesPoint { double t, cents, spread; int n; };
std::vector<RefSeriesPoint> refSeries (const RefPoints&);
} // namespace rehar
