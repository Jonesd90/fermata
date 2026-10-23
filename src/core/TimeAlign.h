#pragma once
#include "Common.h"
#include "Denoise.h"
#include <complex>
#include <functional>
#include <limits>
#include <vector>

namespace td
{
/** Spot-mic time alignment. A spot mic is nearer its instrument than the main pair, so it hears every note a few milliseconds earlier; mixed in
    as it is, the instrument is smeared (heard twice, close together) and coloured (comb filtering where the two overlap). Delaying the spot by
    exactly its lead puts it back in time with the main pair.

    No distances are needed: the lead is MEASURED from a recording (the first run-through for levels is ideal), which also catches what a tape
    measure cannot - the real sound path, a device with more latency than another, the room's temperature.

    How: every track has a role - Main (one or two main pairs, used together), Spot, Distance (room / hall / ambience) or Not used. The take is cut
    into overlapping Hann frames (up to a few hundred, spread over the take). For every spot and distance track only the frames where it is
    playing are used (its louder half; for a spot, of those the half where it is loudest against the main pair, i.e. where its own instrument
    stands out), so nobody has to mark a passage per instrument. For each of those tracks and each MAIN channel the cross-spectrum is summed over
    the frames and turned into a GCC-PHAT correlation (every frequency weighted alike: only the timing counts, so the room's colour and the loudest
    notes do not pull the answer; 100 Hz - 8 kHz), whose peak is the time between the two mics (refined between samples). A peak pointing down
    means the polarity is reversed.

    Bleed: a spot also hears the other instruments, and an instrument nearer the main pair than the spot (soloists in front of an organ spot) is
    heard so well by the main pair that its peak can be the bigger one - on the other side, since that instrument reaches the main pair first. So a
    spot's own peak is looked for on the "main pair later" side only, the main channels of each side are made to agree on one peak (the one most of
    them found, by clarity; the others are looked for again near it), and only when nearly everything the spot hears reaches the main pair first by
    a wide margin is it called farther away than the main pair (a distance mic marked Spot).

    Pan: a spot panned left is heard mostly with the left main mic, so it is lined up with the LEFT main channels' arrival; one in the centre with
    the middle; anything between, weighted by the pan's power (with spaced omnis the two sides can be several ms apart). The left and right
    arrivals are kept, so the delay can follow a new pan without measuring again. Fermata also suggests a pan that puts the spot where its
    instrument sits in the main pair's image: from the level difference between the main channels and their time difference (1 ms counted as
    12 dB, as loudspeaker listening trades them).

    Distance mics are measured but never delayed (they are meant to be late); anything drastic is flagged: one that hears the music before the
    main pair, a reversed polarity, the two channels of a pair far apart in time (a patching mistake), no clear result. Two main pairs are checked
    against each other (they should be in line). */
namespace timealign
{
enum Role : int { NotUsed = 0, Main, Spot, Distance, NumRoles };
inline const char* roleName (int r)
{
    static const char* n[] = { "Not used", "Main", "Spot", "Distance" };
    return juce::isPositiveAndBelow (r, (int) NumRoles) ? n[r] : "";
}
/** A first guess at a track's role from its name (the user corrects it). */
inline int guessRole (const juce::String& trackName)
{
    const auto n = " " + trackName.toLowerCase().replaceCharacters ("-_/.()", "      ") + " ";
    auto has = [&n] (const char* w) { return n.contains (" " + juce::String (w)) ; };
    for (auto* w : { "ltc", "talkback", "talk back", "tb ", "cr mic", "click", "timecode", "conductor" }) if (has (w)) return NotUsed;
    for (auto* w : { "main", "ortf", "omni", "spaced", "decca", "tree", "nos", "din", "xy", "x y", "ab ", "a b ", "outrigger", "flank", "pair " }) if (has (w)) return Main;
    for (auto* w : { "hall", "room", "amb", "ambience", "ambient", "far", "distant", "distance", "surround", "rear", "back", "height", "audience", "gallery", "balcony" }) if (has (w)) return Distance;
    return Spot;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline bool known (double v) noexcept { return v == v; }

/** One track going in. */
struct TrackIn
{
    int role = NotUsed;
    int numChannels = 1;
    /** Its pan in the processing mixer: a mono track's pan in pan[0]; a stereo track's left and right channel positions in pan[0], pan[1] (-1 .. +1).
        For a Main track these are also where its channels sit in the image. */
    float pan[2] = { 0.0f, 0.0f };
    /** Where a Main track's channel c sits (-1 left .. +1 right): its pans for the first two channels; a surround main: L R C Ls Rs ... */
    float sideOf (int c) const
    {
        if (numChannels == 1) return pan[0];
        if (c < 2) return pan[c];
        static const float s[] = { -1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f, -1.0f, 1.0f };
        return s[c & 7];
    }
};

/** One track's result. Times are milliseconds. */
struct TrackOut
{
    bool   measured = false;
    int    severity = 0;                  // 0 fine, 1 a note, 2 a warning (something drastic)
    juce::String message;
    // Spot
    double leftMs = kNaN, rightMs = kNaN; // how much earlier it hears the music than the left / right main channels (the centre ones count for both)
    double delayMs = 0.0;                 // the delay to give it, for its present pan (>= 0)
    float  suggestedPan = 0.0f;           // where its instrument sits in the main pair's image (a stereo pair: where its LEFT channel's sound sits)
    float  suggestedPanR = 0.0f;          // a stereo pair: where its right channel's sound sits
    bool   suggestedStereo = false;       // the two above are a stereo pair's left and right pans
    // Distance
    double arrivalMs = kNaN;              // how much LATER than the main pair it hears the music (positive: later, as it should be)
    double spreadMs = 0.0;                // a stereo distance track: how far apart its two channels are
    // both
    bool   inverted = false;
    float  clarity = 0.0f;                // the correlation peak over its RMS: about 7+ clear, 4 - 7 fair, below 4 unclear
};

/** Two main tracks on the same side (or two stereo main pairs) compared: they should be in line (the same time) and the same polarity. */
struct MainCheck
{
    int track = -1, against = -1;         // indices in the TrackIn list
    double ms = 0.0;                      // how much later 'track' hears the music than 'against'
    bool inverted = false;
    float clarity = 0.0f;
    bool drastic() const noexcept { return std::abs (ms) > 2.0 || inverted; }
};

struct Analysis
{
    bool ok = false;
    juce::String error;                   // why nothing was measured
    std::vector<MainCheck> mainChecks;
    std::vector<TrackOut> tracks;         // one per TrackIn
    int framesUsed = 0;
};

inline const char* clarityName (float c) { return c >= 7.0f ? "clear" : c >= 4.0f ? "fair" : "unclear"; }

/** The power each side of the image gets from a pan (constant power, -3 dB in the centre): left, right. */
inline void panPower (float p, double& wl, double& wr)
{
    const double a = (juce::jlimit (-1.0, 1.0, (double) p) + 1.0) * juce::MathConstants<double>::pi * 0.25;
    wl = std::cos (a) * std::cos (a); wr = std::sin (a) * std::sin (a);
}
/** The delay for a spot at its present pan, from its measured left and right leads. */
inline double delayForPan (const TrackOut& o, const TrackIn& t)
{
    const bool l = known (o.leftMs), r = known (o.rightMs);
    if (! l && ! r) return 0.0;
    if (! l) return juce::jmax (0.0, o.rightMs);
    if (! r) return juce::jmax (0.0, o.leftMs);
    double wl = 0.0, wr = 0.0;
    if (t.numChannels >= 2) { double a, b, c, d; panPower (t.pan[0], a, b); panPower (t.pan[1], c, d); wl = 0.5 * (a + c); wr = 0.5 * (b + d); }
    else panPower (t.pan[0], wl, wr);
    const double s = wl + wr;
    return juce::jmax (0.0, s > 0.0 ? (wl * o.leftMs + wr * o.rightMs) / s : 0.5 * (o.leftMs + o.rightMs));
}

inline int nextPow2 (int v) { int p = 1; while (p < v && p < (1 << 26)) p <<= 1; return p; }

/** Reads n samples of a track's channel from 'start' (samples from the start of the analysed stretch) into dst; false if it cannot. */
using Reader = std::function<bool (int track, int channel, juce::int64 start, int n, float* dst)>;

namespace detail
{
struct Peak { double lag = 0.0; double value = 0.0; double sharpness = 0.0; double level = 0.0; bool valid = false; };

/** The GCC-PHAT correlation of a summed cross-spectrum (R . conj S, bins 0 .. N/2): every frequency (100 Hz - 8 kHz) weighted alike. */
struct Curve
{
    std::vector<double> c;
    double level = 0.0;                   // the summed magnitude (how much the two have in common)
    int N = 0;
    double at (int lag) const { return c[(size_t) (lag >= 0 ? lag : N + lag)]; }
};
inline Curve gccCurve (const std::vector<std::complex<double>>& G, int N, double sr)
{
    Curve cv; cv.N = N;
    const int bins = N / 2 + 1;
    std::vector<std::complex<double>> g ((size_t) bins);
    const double fLo = 100.0, fHi = juce::jmin (8000.0, 0.45 * sr);
    for (int k = 0; k < bins; ++k)
    {
        const double f = (double) k * sr / (double) N;
        double w = 1.0;
        if (f < fLo) w = f < fLo * 0.7071 ? 0.0 : 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (f - fLo * 0.7071) / (fLo * (1.0 - 0.7071)));
        else if (f > fHi) w = f > fHi * 1.4142 ? 0.0 : 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * (f - fHi) / (fHi * 0.4142));
        const double mag = std::abs (G[(size_t) k]);
        cv.level += w * mag;
        g[(size_t) k] = mag > 1.0e-30 ? G[(size_t) k] / mag * w : std::complex<double> (0.0, 0.0);
    }
    cv.c.assign ((size_t) N, 0.0);
    denoise::detail::realInverse (g.data(), N, cv.c.data());
    return cv;
}
/** The biggest peak (either way up) between lagMin and lagMax, refined between samples. Its sharpness is its height over the RMS of the rest of
    the whole correlation (+/- maxLag), so peaks found in different windows compare. lag > 0: R hears it later. */
inline Peak peakIn (const Curve& cv, int lagMin, int lagMax, int maxLag)
{
    Peak pk; pk.level = cv.level;
    lagMin = juce::jmax (lagMin, -maxLag); lagMax = juce::jmin (lagMax, maxLag);
    if (lagMax < lagMin) return pk;
    int best = lagMin; double bestAbs = -1.0;
    for (int lag = lagMin; lag <= lagMax; ++lag) { const double a = std::abs (cv.at (lag)); if (a > bestAbs) { bestAbs = a; best = lag; } }
    double ss = 0.0; int cnt = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) if (std::abs (lag - best) > 3) { ss += cv.at (lag) * cv.at (lag); ++cnt; }
    const double rms = cnt > 0 ? std::sqrt (ss / cnt) : 1.0e-12;
    double frac = 0.0;
    if (best > -maxLag && best < maxLag)
    {
        const double ym = std::abs (cv.at (best - 1)), y0 = std::abs (cv.at (best)), yp = std::abs (cv.at (best + 1));
        const double den = ym - 2.0 * y0 + yp;
        if (std::abs (den) > 1.0e-12) frac = juce::jlimit (-0.5, 0.5, 0.5 * (ym - yp) / den);
    }
    pk.lag = (double) best + frac; pk.value = cv.at (best); pk.sharpness = bestAbs / juce::jmax (1.0e-12, rms); pk.valid = bestAbs > 0.0;
    return pk;
}
/** The correlation's peak within +/- maxLag. */
inline Peak gccPeak (const std::vector<std::complex<double>>& G, int N, double sr, int maxLag) { return peakIn (gccCurve (G, N, sr), -maxLag, maxLag, maxLag); }
} // namespace detail

/** Measures every Spot and Distance track against the Main tracks over 'length' samples (read through 'read'). progress (0 .. 1) may return false
    to stop. maxLagMs: the farthest a mic can be from the main pair (80 ms = 27 m); maxFrames: how many frames are used at most (spread evenly). */
inline Analysis analyse (const std::vector<TrackIn>& tracks, juce::int64 length, double sr, const Reader& read,
                         const std::function<bool (float)>& progress = {}, double maxLagMs = 80.0, int maxFrames = 360)
{
    Analysis an;
    an.tracks.resize (tracks.size());
    std::vector<int> mains, others;
    for (int t = 0; t < (int) tracks.size(); ++t)
    {
        if (tracks[(size_t) t].role == Main) mains.push_back (t);
        else if (tracks[(size_t) t].role == Spot || tracks[(size_t) t].role == Distance) others.push_back (t);
    }
    if (mains.empty()) { an.error = "Choose the main pair first: set at least one track to Main."; return an; }
    if (others.empty() && mains.size() < 2) { an.error = "Set the spot mics to Spot (and any room or hall mics to Distance)."; return an; }
    const int maxLag = juce::jmax (8, (int) std::ceil (maxLagMs * sr / 1000.0));
    if (length < (juce::int64) (0.5 * sr) || length < 3 * maxLag) { an.error = "The take (or the marked part) is too short: at least a few seconds of music are needed."; return an; }
    const int N = nextPow2 (juce::jmax (8192, 4 * maxLag));
    const int hop = N / 2;
    const int bins = N / 2 + 1;

    // the frames: every hop, or a few hundred spread evenly over a long take
    std::vector<juce::int64> starts;
    {
        const juce::int64 possible = length >= N ? 1 + (length - N) / hop : 1;
        const juce::int64 count = juce::jmin<juce::int64> (possible, (juce::int64) juce::jmax (1, maxFrames));
        for (juce::int64 i = 0; i < count; ++i)
            starts.push_back (count <= 1 ? 0 : (length >= N ? (juce::int64) std::llround ((double) i * (double) (length - N) / (double) (count - 1)) : 0));
    }
    const int F = (int) starts.size();
    std::vector<double> win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) N);
    std::vector<float> buf ((size_t) N);
    std::vector<double> frame ((size_t) N);
    auto readMono = [&] (int t, juce::int64 start, bool windowed, std::vector<double>& out) -> bool    // the track's channels summed
    {
        const auto& tr = tracks[(size_t) t];
        std::fill (out.begin(), out.end(), 0.0);
        const int n = (int) juce::jmin<juce::int64> (N, length - start);
        for (int c = 0; c < tr.numChannels; ++c)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            if (n > 0 && ! read (t, c, start, n, buf.data())) return false;
            for (int i = 0; i < N; ++i) out[(size_t) i] += (double) buf[(size_t) i];
        }
        if (windowed) for (int i = 0; i < N; ++i) out[(size_t) i] *= win[(size_t) i];
        return true;
    };
    auto readChannel = [&] (int t, int c, juce::int64 start, std::vector<double>& out) -> bool
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        const int n = (int) juce::jmin<juce::int64> (N, length - start);
        if (n > 0 && ! read (t, c, start, n, buf.data())) return false;
        for (int i = 0; i < N; ++i) out[(size_t) i] = (double) buf[(size_t) i] * win[(size_t) i];
        return true;
    };

    // ---- pass 1: how loud each spot / distance track is in each frame (each channel of a stereo pair on its own: the two can hear different
    // instruments), and the main pair
    struct Unit { int track = -1, ch = -1; std::vector<double> energy; std::vector<char> use; int frames = 0; };     // ch -1: the track's channels summed
    std::vector<Unit> units;
    for (int t : others)
    {
        if (tracks[(size_t) t].numChannels == 2) for (int c = 0; c < 2; ++c) units.push_back ({ t, c, std::vector<double> ((size_t) F, 0.0), std::vector<char> ((size_t) F, 0), 0 });
        else units.push_back ({ t, -1, std::vector<double> ((size_t) F, 0.0), std::vector<char> ((size_t) F, 0), 0 });
    }
    auto readRaw = [&] (int t, int c, juce::int64 start, std::vector<double>& out) -> bool    // one channel, or (c -1) all of them summed; no window
    {
        if (c < 0) return readMono (t, start, false, out);
        std::fill (buf.begin(), buf.end(), 0.0f);
        const int n = (int) juce::jmin<juce::int64> (N, length - start);
        if (n > 0 && ! read (t, c, start, n, buf.data())) return false;
        for (int i = 0; i < N; ++i) out[(size_t) i] = (double) buf[(size_t) i];
        return true;
    };
    std::vector<double> mainEnergy ((size_t) F, 0.0);
    for (int f = 0; f < F; ++f)
    {
        for (auto& u : units)
        {
            if (! readRaw (u.track, u.ch, starts[(size_t) f], frame)) { an.error = "The audio could not be read."; return an; }
            double s = 0.0; for (double v : frame) s += v * v;
            u.energy[(size_t) f] = s;
        }
        for (int t : mains)
        {
            if (! readMono (t, starts[(size_t) f], false, frame)) { an.error = "The audio could not be read."; return an; }
            double s = 0.0; for (double v : frame) s += v * v;
            mainEnergy[(size_t) f] += s;
        }
        if (progress && ! progress (0.25f * (float) (f + 1) / (float) F)) { an.error = "Stopped."; return an; }
    }
    // the frames each one is measured in: its louder half (where it is playing); for a spot, of those the half where it stands out most against
    // the main pair - where its own instrument, not what it hears of the others, is what it picks up
    auto median = [] (std::vector<double> v) { if (v.empty()) return 0.0; std::nth_element (v.begin(), v.begin() + (long) (v.size() / 2), v.end()); return v[v.size() / 2]; };
    for (auto& u : units)
    {
        const auto& e = u.energy;
        double total = 0.0; for (double v : e) total += v;
        if (total <= 1.0e-12 * N) continue;                                     // silent (said below)
        const double thr = median (e);
        for (int f = 0; f < F; ++f) if (e[(size_t) f] >= thr && e[(size_t) f] > 0.0) { u.use[(size_t) f] = 1; ++u.frames; }
        if (tracks[(size_t) u.track].role == Spot && u.frames >= 8)
        {
            std::vector<double> ratio;
            for (int f = 0; f < F; ++f) if (u.use[(size_t) f]) ratio.push_back (e[(size_t) f] / juce::jmax (1.0e-30, mainEnergy[(size_t) f]));
            const double rthr = median (ratio);
            for (int f = 0; f < F; ++f)
                if (u.use[(size_t) f] && e[(size_t) f] / juce::jmax (1.0e-30, mainEnergy[(size_t) f]) < rthr) { u.use[(size_t) f] = 0; --u.frames; }
        }
    }
    std::vector<char> deadChannel (tracks.size(), 0);
    for (int t : others)
    {
        int live = 0, all = 0;
        for (auto& u : units) if (u.track == t) { ++all; live += u.frames > 0 ? 1 : 0; }
        if (live == 0) { an.tracks[(size_t) t].message = "Silent in this take."; an.tracks[(size_t) t].severity = 2; }
        else if (live < all) deadChannel[(size_t) t] = 1;                     // (measured on the other; said at the end)
    }

    // ---- pass 2: the cross-spectra (each spot / distance track - or each channel of a distance track - against every main channel)
    struct MainCh { int track, ch; float side; };
    std::vector<MainCh> mainChs;
    for (int t : mains) for (int c = 0; c < tracks[(size_t) t].numChannels; ++c) mainChs.push_back ({ t, c, tracks[(size_t) t].sideOf (c) });
    struct Pair { int track = -1, ch = -1; int main = -1; std::vector<std::complex<double>> G; };     // ch -1: the track's channels summed
    std::vector<Pair> pairs;
    for (auto& u : units)
        if (u.frames > 0)
            for (int m = 0; m < (int) mainChs.size(); ++m) pairs.push_back ({ u.track, u.ch, m, std::vector<std::complex<double>> ((size_t) bins) });
    // each main track against the first main track on the same side (left, right, or a stereo / centre one): are two main pairs in line?
    auto sideClass = [&] (int t) { const auto& tr = tracks[(size_t) t]; float s = 0.0f; for (int c = 0; c < tr.numChannels; ++c) s += tr.sideOf (c); s /= (float) juce::jmax (1, tr.numChannels); return s < -0.3f ? -1 : s > 0.3f ? 1 : 0; };
    std::vector<int> mainRef (mains.size(), -1);
    for (size_t i = 1; i < mains.size(); ++i)
        for (size_t j = 0; j < i; ++j) if (sideClass (mains[j]) == sideClass (mains[i])) { mainRef[i] = (int) j; break; }
    std::vector<std::vector<std::complex<double>>> mainVsFirst (mains.size(), std::vector<std::complex<double>> ((size_t) bins));
    std::vector<std::vector<std::complex<double>>> M (mainChs.size(), std::vector<std::complex<double>> ((size_t) bins));
    std::vector<std::complex<double>> S ((size_t) bins);
    std::vector<std::vector<std::complex<double>>> mainMono (mains.size(), std::vector<std::complex<double>> ((size_t) bins));
    std::vector<int> used (tracks.size(), 0);
    for (int f = 0; f < F; ++f)
    {
        const auto st = starts[(size_t) f];
        for (size_t m = 0; m < mainChs.size(); ++m)
        {
            if (! readChannel (mainChs[m].track, mainChs[m].ch, st, frame)) { an.error = "The audio could not be read."; return an; }
            denoise::detail::realForward (frame.data(), N, M[m].data());
        }
        if (mains.size() > 1)
            for (size_t i = 0; i < mains.size(); ++i)
            {
                std::fill (mainMono[i].begin(), mainMono[i].end(), std::complex<double> (0.0, 0.0));
                for (size_t m = 0; m < mainChs.size(); ++m) if (mainChs[m].track == mains[i]) for (int k = 0; k < bins; ++k) mainMono[i][(size_t) k] += M[m][(size_t) k];
            }
        if (mains.size() > 1)
            for (size_t i = 1; i < mains.size(); ++i)
                if (const int j = mainRef[i]; j >= 0)
                    for (int k = 0; k < bins; ++k) mainVsFirst[i][(size_t) k] += mainMono[i][(size_t) k] * std::conj (mainMono[(size_t) j][(size_t) k]);
        for (auto& u : units)
        {
            if (! u.use[(size_t) f]) continue;                                 // its instrument is not playing here (or is drowned by the others)
            ++used[(size_t) u.track];
            if (! (u.ch >= 0 ? readChannel (u.track, u.ch, st, frame) : readMono (u.track, st, true, frame))) { an.error = "The audio could not be read."; return an; }
            denoise::detail::realForward (frame.data(), N, S.data());
            for (auto& p : pairs) if (p.track == u.track && p.ch == u.ch) for (int k = 0; k < bins; ++k) p.G[(size_t) k] += M[(size_t) p.main][(size_t) k] * std::conj (S[(size_t) k]);
        }
        if (progress && ! progress (0.25f + 0.7f * (float) (f + 1) / (float) F)) { an.error = "Stopped."; return an; }
    }
    an.framesUsed = F;
    const double toMs = 1000.0 / sr;

    // ---- the results
    for (int t : others)
    {
        auto& o = an.tracks[(size_t) t];
        if (o.severity == 2) continue;                                          // (silent)
        const auto& tr = tracks[(size_t) t];
        struct Got { detail::Peak pk, all; float side; int ch; size_t curve; };
        std::vector<detail::Curve> curves;
        std::vector<Got> got;
        for (auto& p : pairs)
            if (p.track == t)
            {
                curves.push_back (detail::gccCurve (p.G, N, sr));
                Got g; g.side = mainChs[(size_t) p.main].side; g.ch = p.ch; g.curve = curves.size() - 1;
                g.all = detail::peakIn (curves.back(), -maxLag, maxLag, maxLag);
                g.pk = g.all;
                got.push_back (g);
            }
        if (got.empty() || used[(size_t) t] == 0) { o.message = "Not enough of this track to measure."; o.severity = 1; continue; }
        o.measured = true;
        auto weighted = [&] (const std::function<bool (const Got&)>& use_) -> double
        {
            double w = 0.0, s = 0.0;
            for (auto& g : got) if (use_ (g)) { w += g.pk.sharpness; s += g.pk.sharpness * g.pk.lag; }
            return w > 0.0 ? s / w * toMs : kNaN;
        };
        if (tr.role == Spot)
        {
            // A spot is nearer its instrument than the main pair, so its instrument's peak is on the "main pair later" side. What it hears of an
            // instrument nearer the main pair than itself (the soloists in front, say, in an organ or a timpani spot) peaks on the other side - and
            // can be the bigger peak, because the main pair hears that instrument so well. So the spot's own peak is looked for on its side only.
            const int minLead = juce::jmax (1, (int) std::lround (0.5e-3 * sr));
            for (auto& g : got) g.pk = detail::peakIn (curves[g.curve], -minLead, maxLag, maxLag);
            // the main channels of one side should agree (one may have found the instrument, another something else the spot hears): the time most
            // of them agree on wins (by clarity), and the others are looked for again near it
            const int win = juce::jmax (4, (int) std::lround (4.0e-3 * sr));
            for (int group : { -1, 0, 1 })
            {
                auto inGroup = [group] (const Got& g) { return group < 0 ? g.side < 0.0f : group > 0 ? g.side > 0.0f : g.side == 0.0f; };
                double bestScore = -1.0, tau = 0.0;
                for (auto& a : got)
                    if (inGroup (a))
                    {
                        double sc = 0.0;
                        for (auto& b : got) if (inGroup (b) && std::abs (b.pk.lag - a.pk.lag) <= win) sc += b.pk.sharpness;
                        if (sc > bestScore) { bestScore = sc; tau = a.pk.lag; }
                    }
                if (bestScore < 0.0) continue;
                for (auto& b : got)
                    if (inGroup (b) && std::abs (b.pk.lag - tau) > win)
                        b.pk = detail::peakIn (curves[b.curve], juce::jmax (-minLead, (int) std::floor (tau) - win), (int) std::ceil (tau) + win, maxLag);
            }
            // a "spot" that is really farther away than the main pair: on most main channels the music reaches the main pair first, by far the
            // biggest peak, and nothing of note on the spot's side
            int laterCount = 0; double laterSum = 0.0;
            for (auto& g : got) if (g.all.lag < -minLead && std::abs (g.all.value) >= 2.0 * std::abs (g.pk.value)) { ++laterCount; laterSum += -g.all.lag * toMs; }
            double sumSign = 0.0, best = 0.0;
            if (2 * laterCount > (int) got.size())
            {
                for (auto& g : got) g.pk = g.all;
                for (auto& g : got) { sumSign += g.pk.sharpness * (g.pk.value < 0.0 ? -1.0 : 1.0); best = juce::jmax (best, g.pk.sharpness); }
                o.clarity = (float) best; o.inverted = sumSign < 0.0;
                o.leftMs  = weighted ([] (const Got& g) { return g.side <= 0.0f; });
                o.rightMs = weighted ([] (const Got& g) { return g.side >= 0.0f; });
                o.delayMs = 0.0; o.severity = 2;
                o.message = "It hears the music " + juce::String (laterSum / laterCount, 2) + " ms AFTER the main pair: it is farther away, so a delay cannot line it up. Is it a distance mic?";
                continue;
            }
            for (auto& g : got) { sumSign += g.pk.sharpness * (g.pk.value < 0.0 ? -1.0 : 1.0); best = juce::jmax (best, g.pk.sharpness); }
            o.clarity = (float) best;
            o.inverted = sumSign < 0.0;
            o.leftMs  = weighted ([] (const Got& g) { return g.side <= 0.0f; });
            o.rightMs = weighted ([] (const Got& g) { return g.side >= 0.0f; });
            o.delayMs = delayForPan (o, tr);
            // where its instrument sits in the main pair: the main channels' level difference, plus their time difference (1 ms ~ 12 dB). A stereo
            // pair: each channel's own place (a pair over a choir hears the sopranos on one side and the basses on the other)
            auto placeOf = [&] (int ch, float& pan) -> bool
            {
                double aL = 0.0, aR = 0.0; int nL = 0, nR = 0;
                for (auto& g : got) if (g.ch == ch) { if (g.side < 0.0f) { aL += g.pk.level; ++nL; } else if (g.side > 0.0f) { aR += g.pk.level; ++nR; } }
                const double lMs = weighted ([ch] (const Got& g) { return g.ch == ch && g.side <= 0.0f; });
                const double rMs = weighted ([ch] (const Got& g) { return g.ch == ch && g.side >= 0.0f; });
                if (nL == 0 || nR == 0 || aL <= 0.0 || aR <= 0.0 || ! known (lMs) || ! known (rMs)) return false;
                const double levelDb = 20.0 * std::log10 ((aR / nR) / (aL / nL));
                const double d = juce::jlimit (-30.0, 30.0, levelDb + 12.0 * (lMs - rMs));
                pan = (float) juce::jlimit (-1.0, 1.0, 4.0 / juce::MathConstants<double>::pi * std::atan (std::pow (10.0, d / 20.0)) - 1.0);
                return true;
            };
            if (tr.numChannels == 2)
            {
                float a = 0.0f, b = 0.0f;
                if (placeOf (0, a) && placeOf (1, b)) { o.suggestedPan = a; o.suggestedPanR = b; o.suggestedStereo = true; }
            }
            else placeOf (-1, o.suggestedPan);
            if (o.inverted) { o.severity = 2; o.message = "Its polarity looks reversed (press its polarity button)."; }
            else if (o.clarity < 4.0f) { o.severity = 1; o.message = "No clear result: its instrument may hardly play in this take."; }
            else if (o.delayMs > 50.0) { o.severity = 1; o.message = "An unusually large delay: check it by ear."; }
            else if (known (o.leftMs) && known (o.rightMs) && std::abs (o.leftMs - o.rightMs) > 10.0)
            { o.severity = 1; o.message = "Its left and right measurements are " + juce::String (std::abs (o.leftMs - o.rightMs), 1) + " ms apart: check it by ear."; }
        }
        else    // Distance
        {
            double sumSign = 0.0, best = 0.0;
            for (auto& g : got) { sumSign += g.pk.sharpness * (g.pk.value < 0.0 ? -1.0 : 1.0); best = juce::jmax (best, g.pk.sharpness); }
            o.clarity = (float) best;
            o.inverted = sumSign < 0.0;
            o.arrivalMs = -weighted ([] (const Got&) { return true; });
            if (tr.numChannels >= 2)
            {
                double a[2] = { kNaN, kNaN };
                for (int c = 0; c < 2; ++c) a[c] = -weighted ([c] (const Got& g) { return g.ch == c; });
                if (known (a[0]) && known (a[1])) o.spreadMs = std::abs (a[0] - a[1]);
            }
            if (o.arrivalMs < -1.0) { o.severity = 2; o.message = "It hears the music " + juce::String (-o.arrivalMs, 2) + " ms BEFORE the main pair: is it really a distance mic (or a spot)?"; }
            else if (o.inverted) { o.severity = 2; o.message = "Its polarity looks reversed against the main pair."; }
            else if (o.spreadMs > 25.0) { o.severity = 2; o.message = "Its two channels arrive " + juce::String (o.spreadMs, 1) + " ms apart: check the patching."; }
            else if (o.clarity < 4.0f) { o.severity = 1; o.message = "No clear result (a very diffuse sound: usually fine for a distance mic)."; }
            else if (o.arrivalMs > 0.9 * maxLagMs) { o.severity = 1; o.message = "At the limit of what can be measured: it may be very far away."; }
        }
    }
    // two (or more) main pairs: are they in line, and the same way up?
    for (size_t i = 1; i < mains.size(); ++i)
    {
        if (mainRef[i] < 0) continue;
        const auto pk = detail::gccPeak (mainVsFirst[i], N, sr, maxLag);
        MainCheck mc;
        mc.track = mains[i]; mc.against = mains[(size_t) mainRef[i]];
        mc.ms = pk.lag * toMs;                                                 // (G = this . conj (first): lag > 0 when this one hears it later)
        mc.inverted = pk.value < 0.0;
        mc.clarity = (float) pk.sharpness;
        an.mainChecks.push_back (mc);
    }
    for (int t : others)
        if (deadChannel[(size_t) t])
        {
            auto& o = an.tracks[(size_t) t];
            o.message = "One of its two channels is silent in this take: check the patching." + (o.message.isNotEmpty() ? " " + o.message : juce::String());
            o.severity = 2;
        }
    if (progress) progress (1.0f);
    an.ok = true;
    return an;
}
} // namespace timealign
} // namespace td
