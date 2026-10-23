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
    double sepWindow = 0.20;                  // seconds: the window of the fit that keeps the note apart from the voices beside it
    // a voice on its own (no other voice found beside it): the band round each overtone is flat and as wide as the note's own wobble, so the room's
    // ring-out (spread round the OLD pitch) moves with the note instead of being left behind as a ghost; and the correction follows the ring-out after the note
    bool   lone = true;                       // off = the old narrow Gaussian bands (as 2.56), for comparison
    double loneMargin = 20.0;                 // cents added either side of the wobble the band must cover
    double loneMaxHalf = 75.0;                // cents: the band never reaches further than this from the line (a neighbouring semitone is 100 c away)
    double tailMax = 1.5;                     // seconds: how long after the box the room's ring-out is still corrected (it stops early when the ring-out has died, or when a new sound starts there)
    double loneTopHz = 8000.0;                // a voice on its own: its overtones are moved up to this frequency (not only the first nharm), so none is left at the old pitch
    // "One voice only" (the user says only one singer / instrument is on this note): nothing is looked for beside it and nothing found there is kept apart,
    // the band is never narrowed for a sound close by, and the whole note moves as a voice on its own
    bool   oneVoice = false;
    int nharm = 10;
    bool match = true;
    bool trendFit = true;                     // the note's pitch (what the wobble is measured from) as a fitted trend; false = the prototype's plain smoothing
    double a4 = 440.0, move = 0.0, snap = 0.0, keep = 0.0;
};

/** Planar float audio: block[channel][frame]. */
using Block = std::vector<std::vector<float>>;
inline std::vector<Span> spansOf (const Block& b) { std::vector<Span> s; for (auto& c : b) s.push_back ({ c.data(), c.size() }); return s; }

// ---------------------------------------------------------------------------------------------------- measuring a note
/** Is this channel a timecode (LTC) signal rather than music? It is a loud, very steady, buzzing signal (its level hardly moves and its peaks are
    barely above its average), which music never is. Such a channel must not be measured, and must not be changed by a correction. */
bool looksLikeTimecode (const Span& channel, int sampleRate);
/** The channels to measure a note with (the loudest few in the note's pitch range after weighting) and their weights. visGainDb: the "see" gain per channel (empty = all 0 dB). */
struct Tracking { std::vector<int> channels; Vec weights; };
Tracking trackingInputs (const std::vector<Span>& seg, int sr, double fLo, double fHi, const Vec& visGainDb = {}, int kmax = 6);

struct Track { Vec ts, f0; };
/** Harmonic-comb f0 track inside [t0, t1] for a fundamental in [fLo, fHi]. chans: the chosen channels (full rate), ws: their weights. tSeg0: time of sample 0 of chans. Throws std::runtime_error with a message for the user. */
Track trackNote (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, double hopS = 0.02);
/** Keeps the demodulated partials centred (refines the f0 track). */
Vec refineTrack (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0, double bw = 30, int nh = 4, int iters = 4, double smoothS = 0.12);
/** Like trackNote, but it follows the voice in the MIDDLE of the box (a prior centred on the box, and a continuous path through time instead of picking the
    loudest comb frame by frame), so a quieter / flatter voice that is boxed is not swapped for a louder neighbour. The returned track is a coarse first guess. */
Track trackNoteLocked (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, double hopS = 0.02, double prior = 20.0);
/** The refined line of ONE voice among others: coarse to fine, every overtone seen through a window that is the same width in pitch (so a neighbouring voice a
    few Hz away is shut out at every overtone) and the overtones that disagree with the rest (a neighbour beating in them) count for less. wide: the first windows are twice as
    wide, so a voice swinging far from the line (a big vibrato) is still caught; only for a voice with no other near it (or with the others taken out first). */
Vec refineTrackLocked (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0, double b0 = 20.0, bool wide = false);
/** refineTrackLocked with the other voices taken out first: nb[i] are the lines (Hz) of the neighbouring voices on the same time grid as ts. */
Vec refineTrackPeeled (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0, const std::vector<Vec>& nb, double b0 = 20.0, bool wide = false,
                       const std::vector<Vec>* nbActive = nullptr);
/** The other voices singing within +-span cents of the boxed one (on the same time grid as ts): the boxed voice is taken out, then the strongest steady line left near
    it is followed, refined and taken out too, up to maxVoices times; a line is only kept when it clearly stands above its surroundings. A voice has to be there all through
    the note. With 'active' given, a sound that is there for a part of it only (a piano note struck, or dying away, inside the box; a singer who comes in) is kept too: its line is
    held flat outside that part, and active gets for every line returned either an empty Vec (there all through the note) or when it sounds (0..1 on the grid ts). */
std::vector<Vec> findNeighbours (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0, double span = 110.0, int maxVoices = 2,
                                 std::vector<Vec>* active = nullptr);
/** Everything the editor measures about a boxed note: its line (Hz) on the grid ts, and the lines of the voices beside it (empty when it is alone). */
struct NoteMeasure
{
    Vec ts, f0; std::vector<Vec> nb;
    std::vector<Vec> nbActive;                 // per line of nb: empty = that voice is there all through the note; else 0..1 on the grid ts = when it sounds (a piano note struck inside the box, say)
};
/** lookForOthers = false ("One voice only"): no voices are looked for beside the note (nb stays empty). */
NoteMeasure measureNote (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, const Hooks& hooks = {}, double prior = 20.0,
                         bool lookForOthers = true);
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

/** Retunes the note in 'seg' (all channels alike). f0 in Hz and off in cents on the grid ts (seconds, from the start of seg + tSeg0). Returns the corrected copy.
    nb: the lines (Hz, on the grid ts) of other voices found beside the note: they are taken out of what the note is measured in and the band is kept narrow, so
    they are left as they were. Empty (or none found) = the band is p.bw * sqrt (overtone number), as it always was.
    nbActive: for the lines of nb that sound for a part of the note only (a piano note struck inside the box), when they do (0..1 on the grid ts; an empty entry, or none
    given, = all through the note). */
Block retuneSegment (const Block& seg, int sr, double tSeg0, const Vec& ts, const Vec& f0, const Vec& off, const Params& p, const Hooks& hooks = {}, const std::vector<Vec>& nb = {},
                     const std::vector<Vec>& nbActive = {});
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
                    const Hooks& hooks = {}, int H = 12, double bwMax = 45.0, double fade = 0.15, double bwLow = 0.8, bool jointFit = true);

// ---------------------------------------------------------------------------------------------------- Ensemble ReCentre (the choir's own A4)
struct RefPoints { Vec T, D, W; };
/** Steady spectral peaks of the loudest channels: time, cents from the nearest A=440 semitone (-50..50) and weight. segStart = time of sample 0. Throws when under 2 s. */
RefPoints refPoints (const std::vector<Span>& seg, int sr, double segStart, const Vec& visGainDb = {}, int kmax = 4, const Hooks& hooks = {});
struct RefStats { bool ok = false; double cents = 0, a4 = 440.0, spread = 0; int n = 0; };
RefStats refStats (const Vec& D, const Vec& W, int minN = 20);
struct RefSeriesPoint { double t, cents, spread; int n; };
std::vector<RefSeriesPoint> refSeries (const RefPoints&);
} // namespace rehar
