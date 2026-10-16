#pragma once
#include "LostSamples.h"
#include <cstdlib>
#include "Dsp.h"
#include <functional>
#include <numeric>
#include <juce_core/juce_core.h>

namespace td
{
/** Which clean audio fills a damaged spot. */
enum class RepairSide { Left = 0, Right = 1, Both = 2 };

/** The direction a spectral repair looks for its material: along time (the sound before and after the box), along frequency (the sound above and below it), or both. */
enum class RepairDirection { LeftRight = 0, UpDown = 1, Both = 2 };

/** What a spectral repair does to the box: Attenuate pulls down only what sticks out above the normal level of the sound around it (the sound underneath stays: best for a
    click, a cough or a dropped pencil over a held chord); Replace rebuilds the box from the sound around it. */
enum class RepairMode { Replace = 0, Attenuate = 1 };

/** The controls of a spectral repair (like iZotope RX's Spectral Repair). */
struct RepairParams
{
    RepairMode mode = RepairMode::Attenuate;
    bool   linkChannels = false;                        // Attenuate: the channels of one file (a stereo pair) get the same gain, so the stereo image is not changed
    int    bands = 1024;                                // Attenuate: the frequency bands of the analysis (the FFT is twice this)
    bool   multiRes = true;                             // Attenuate: a longer window for the bass, a shorter one for the top
    double strength = 1.0;                              // 0 .. 1: how much of the repair replaces the original inside the box
    RepairDirection direction = RepairDirection::LeftRight;
    double contextPct = 100.0;                          // how much surrounding sound is used, as a % of the box (its length in time, its height in frequency)
    double weighting = 0.5;                             // 0 = only the sound BEFORE the box, 1 = only the sound AFTER it, 0.5 = both equally (cross-fade)
    bool   consonant = false;                           // Attenuate: a STRAY CONSONANT (an early or late copy of a consonant next to the main one): the box is matched to the side
                                                        // away from the main consonant (found from all the tracks together while the before / after slider is in the middle)
    double consonantBefore = -1.0;                      // consonant: how much the sound BEFORE the box is matched (1 = only before, 0 = only after), found by the repair window on
    int    consonantEdge = 0;                           // all the tracks together, and the edge the main consonant is at (+1 after the box, -1 before it). -1: each call finds it
};

/** A spectrogram of several tracks merged into one picture (the loudest track at every point), in dB.
    Two layers: 'db' (a short window: sharp in time, 23 Hz between rows) for everything, and 'lowDb' (a window about 8 times longer, 3 Hz between rows) for the
    bass. level() blends them: the long one at the bottom, the short one at the top, a smooth cross-over in between. */
struct MergedSpectrogram
{
    int bins = 0, frames = 0;
    double sampleRate = 48000.0, hopSamples = 512.0, fftSize = 2048.0;
    std::vector<float> db;                       // frames * bins
    int lowBins = 0; double lowBinHz = 0.0, crossLo = 250.0, crossHi = 1500.0, lowGainDb = 0.0;
    std::vector<float> lowDb;                    // frames * lowBins (empty = no bass layer)
    float at (int frame, int bin) const { return db[(size_t) frame * (size_t) bins + (size_t) bin]; }
    double hzToBin (double hz) const { return hz * fftSize / sampleRate; }

    /** The level (dB) at a frame and a pitch, from both layers. */
    float level (int frame, double hz) const
    {
        const double bf = juce::jlimit (0.0, (double) bins - 1.0001, hzToBin (hz));
        const int b0 = (int) bf; const float fr = (float) (bf - b0);
        const float hi = at (frame, b0) * (1.0f - fr) + at (frame, b0 + 1) * fr;
        if (lowBins < 2 || hz >= crossHi) return hi;
        const double lf = juce::jlimit (0.0, (double) lowBins - 1.0001, hz / lowBinHz);
        const int l0 = (int) lf; const float lr = (float) (lf - l0);
        const float* row = lowDb.data() + (size_t) frame * (size_t) lowBins;
        const float lo = row[l0] * (1.0f - lr) + row[l0 + 1] * lr + (float) lowGainDb;
        double w = 1.0;                                                        // 1 = all long window, 0 = all short window
        if (hz > crossLo) { const double t = std::log (hz / crossLo) / std::log (crossHi / crossLo); w = 1.0 - t * t * (3.0 - 2.0 * t); }
        return (float) (w * lo + (1.0 - w) * hi);
    }
};

class SpectralRepair
{
public:
    /** The merged spectrogram of all the tracks (each a mono vector of the same length).
        bassMode: 0 = blended (a long window for the bass, a short one above: the default), 1 = short window only (sharpest for clicks), 2 = extra fine bass. */
    static MergedSpectrogram spectrogram (const std::vector<std::vector<float>>& tracks, double sr, int maxFrames = 1600, int bassMode = 0)
    {
        MergedSpectrogram s; s.sampleRate = sr;
        const int N = 2048;
        s.fftSize = N; s.bins = N / 2 + 1;
        const size_t L = tracks.empty() ? 0 : tracks[0].size();
        if (L < (size_t) N) { s.frames = 0; return s; }
        const double hop = juce::jmax (512.0, std::ceil ((double) L / (double) maxFrames));
        s.hopSamples = hop;
        s.frames = (int) ((double) (L - (size_t) N) / hop) + 1;
        s.db.assign ((size_t) s.frames * (size_t) s.bins, -120.0f);
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<std::complex<double>> buf ((size_t) N);
        // the bass layer: the sound is thinned out (D times fewer samples, with a low-pass first) and a window of the same N points then spans D times longer
        const int D = bassMode == 2 ? 16 : 8;
        const bool bass = bassMode != 1;
        std::vector<double> taps;
        if (bass)
        {
            const int nt = D * 8 + 1; const double fc = 0.45 / (double) D;
            for (int k = 0; k < nt; ++k)
            {
                const double x = (double) (k - nt / 2);
                const double sinc = x == 0.0 ? 2.0 * fc : std::sin (2.0 * dsp::kPi * fc * x) / (dsp::kPi * x);
                taps.push_back (sinc * (0.54 - 0.46 * std::cos (2.0 * dsp::kPi * (double) k / (double) (nt - 1))));
            }
            s.lowBinHz = (sr / (double) D) / (double) N;
            s.crossLo = bassMode == 2 ? 150.0 : 250.0; s.crossHi = bassMode == 2 ? 800.0 : 1500.0;
            s.lowBins = juce::jmin (N / 2, (int) std::ceil (s.crossHi * 1.15 / s.lowBinHz) + 2);
            s.lowGainDb = 10.0 * std::log10 ((double) D) * 0.5;               // noise reads lower in a longer window: half the difference is made up, so there is no visible step
            s.lowDb.assign ((size_t) s.frames * (size_t) s.lowBins, -120.0f);
        }
        std::vector<double> dec ((size_t) N);
        const long span = (long) N * D;
        for (auto& t : tracks)
            for (int f = 0; f < s.frames; ++f)
            {
                const size_t o = (size_t) std::llround ((double) f * hop);
                if (o + (size_t) N > t.size()) break;
                for (int i = 0; i < N; ++i) buf[(size_t) i] = (double) t[o + (size_t) i] * win[(size_t) i];
                fft.forward (buf);
                for (int b = 0; b < s.bins; ++b)
                {
                    const float d = (float) (20.0 * std::log10 (std::abs (buf[(size_t) b]) * 4.0 / (double) N + 1.0e-9));   // ~ dBFS of a sine
                    auto& dst = s.db[(size_t) f * (size_t) s.bins + (size_t) b];
                    if (d > dst) dst = d;
                }
                if (! bass) continue;
                // the long window is centred on the same moment as the short one
                const long centre = (long) o + N / 2, start = centre - span / 2, nt = (long) taps.size(), L2 = (long) t.size();
                for (int k = 0; k < N; ++k)
                {
                    const long c = start + (long) k * D; double a = 0.0;
                    for (long j = 0; j < nt; ++j) { const long q = c + j - nt / 2; if (q >= 0 && q < L2) a += taps[(size_t) j] * (double) t[(size_t) q]; }
                    dec[(size_t) k] = a;
                }
                for (int i = 0; i < N; ++i) buf[(size_t) i] = dec[(size_t) i] * win[(size_t) i];
                fft.forward (buf);
                for (int b = 0; b < s.lowBins; ++b)
                {
                    const float d = (float) (20.0 * std::log10 (std::abs (buf[(size_t) b]) * 4.0 / (double) N + 1.0e-9));
                    auto& dst = s.lowDb[(size_t) f * (size_t) s.lowBins + (size_t) b];
                    if (d > dst) dst = d;
                }
            }
        return s;
    }

    // ------------------------------------------------------------------------------------------------ the box patch
    /** Replaces the part of the sound inside [t0,t1) (samples of x) and between f0 and f1 Hz by a continuation of the clean sound next to it. */
    static void patch (std::vector<float>& x, double sr, long t0, long t1, double f0, double f1, RepairSide side, double contextPct = 0.0, double weighting = -1.0)
    {
        const long L = (long) x.size();
        t0 = juce::jlimit (0L, L, t0); t1 = juce::jlimit (0L, L, t1);
        if (t1 - t0 < 2) return;
        std::vector<double> d (x.begin(), x.end());
        const bool leftOk = t0 > 4096, rightOk = L - t1 > 4096;
        const long ctxS = contextPct > 0.0 ? (long) (contextPct / 100.0 * (double) (t1 - t0)) : 0L;
        if (side == RepairSide::Left && ! leftOk && rightOk) side = RepairSide::Right;
        else if (side == RepairSide::Right && ! rightOk && leftOk) side = RepairSide::Left;
        std::vector<double> a = d, b;
        if (side == RepairSide::Left || side == RepairSide::Both) if (leftOk || side == RepairSide::Left) fillFromLeft (a, sr, t0, t1, f0, f1, ctxS);
        if (side == RepairSide::Right || side == RepairSide::Both)
        {
            b = d; std::reverse (b.begin(), b.end());
            fillFromLeft (b, sr, L - t1, L - t0, f0, f1, ctxS);
            std::reverse (b.begin(), b.end());
        }
        const long xf = juce::jmax (1L, juce::jmin (96L, (t1 - t0) / 4));
        for (long n = t0; n < t1; ++n)
        {
            double y;
            if (side == RepairSide::Left) y = a[(size_t) n];
            else if (side == RepairSide::Right) y = b[(size_t) n];
            else { double w = (double) (n - t0 + 1) / (double) (t1 - t0 + 1);
                if (weighting >= 0.0) w = std::pow (w, std::log (juce::jlimit (0.02, 0.98, weighting)) / std::log (0.5));       // moves the middle of the cross-fade towards the before / after side
                y = (1.0 - w) * (leftOk ? a[(size_t) n] : b[(size_t) n]) + w * (rightOk ? b[(size_t) n] : a[(size_t) n]); }
            const double edge = juce::jmin (1.0, (double) (n - t0 + 1) / (double) xf, (double) (t1 - n) / (double) xf);
            x[(size_t) n] = (float) ((1.0 - edge) * (double) x[(size_t) n] + edge * y);
        }
    }


    // ------------------------------------------------------------------------------------------------ the spectral repair with RX-style controls
    /** Rebuilds the box [t0,t1) x [f0,f1 Hz) from the sound around it, as the parameters say. 'x' keeps its length. */
    static void repair (std::vector<float>& x, double sr, long t0, long t1, double f0, double f1, const RepairParams& p)
    {
        const long L = (long) x.size();
        t0 = juce::jlimit (0L, L, t0); t1 = juce::jlimit (0L, L, t1);
        if (t1 - t0 < 2) return;
        if (p.mode == RepairMode::Attenuate) { attenuate (x, sr, t0, t1, f0, f1, p); return; }
        const double ctxPct = juce::jlimit (5.0, 1000.0, p.contextPct);
        const bool doH = p.direction != RepairDirection::UpDown, doV = p.direction != RepairDirection::LeftRight;
        const std::vector<float> orig = x;
        std::vector<float> h, v;
        if (doH) { h = orig; patch (h, sr, t0, t1, f0, f1, RepairSide::Both, ctxPct, p.weighting); }
        if (doV) { v = orig; fillVertical (v, sr, t0, t1, f0, f1, ctxPct); }
        const std::vector<float>* rep = doH ? &h : &v;
        if (doH && doV) { combineGeometric (h, v, sr, t0, t1, f0, f1); rep = &h; }
        const double s = juce::jlimit (0.0, 1.0, p.strength);
        for (long n = t0; n < t1; ++n) x[(size_t) n] = (float) ((double) orig[(size_t) n] + s * ((double) (*rep)[(size_t) n] - (double) orig[(size_t) n]));
    }


    // ------------------------------------------------------------------------------------------------ attenuate
    /** Pulls down what sticks out of the box above the normal level of the sound around it (the sound underneath stays). Several analysis sizes when multiRes is on. */
    static void attenuate (std::vector<float>& x, double sr, long t0, long t1, double f0, double f1, const RepairParams& p)
    {
        std::vector<std::vector<float>*> xs { &x }; attenuate (xs, sr, t0, t1, f0, f1, p);
    }
    /** Attenuate on several channels at once: with p.linkChannels all of them get the same gain (a stereo pair keeps its timing and level difference). */
    static void attenuate (std::vector<std::vector<float>*>& xs, double sr, long t0, long t1, double f0, double f1, const RepairParams& p)
    {
        if (p.strength <= 0.0 || t1 <= t0 || xs.empty()) return;
        int baseN = 256; while (baseN < 2 * p.bands && baseN < 16384) baseN <<= 1;
        Sides sides;                                                            // consonant mode: the side to match and the edge the main consonant is at (the same for every channel)

        if (p.consonant)
        {
            double wl = 0.5; int edge = 0;
            if (std::abs (p.weighting - 0.5) >= 0.04)                           // the slider moved from the middle: the side it says
            {
                wl = juce::jlimit (0.0, 1.0, 1.0 - p.weighting); edge = wl > 0.65 ? 1 : (wl < 0.35 ? -1 : 0);
            }
            else if (p.consonantBefore >= 0.0) { wl = juce::jlimit (0.0, 1.0, p.consonantBefore); edge = p.consonantEdge; }     // found on all the tracks by the window
            else
            {
                std::vector<const std::vector<float>*> cs; for (auto* x : xs) cs.push_back (x);
                const auto d = consonantSides (cs, sr, t0, t1, f0, f1); wl = d.first; edge = d.second;
            }
            sides = { wl, edge };
        }
        const auto* sw = p.consonant ? &sides : nullptr;
        if (p.multiRes)
        {
            attenuatePass (xs, sr, baseN * 2, 0.0, 400.0, t0, t1, f0, f1, p, sw);
            attenuatePass (xs, sr, baseN, 400.0, 3000.0, t0, t1, f0, f1, p, sw);
            attenuatePass (xs, sr, juce::jmax (256, baseN / 4), 3000.0, 1.0e9, t0, t1, f0, f1, p, sw);
        }
        else attenuatePass (xs, sr, baseN, 0.0, 1.0e9, t0, t1, f0, f1, p, sw);
    }

    /** For a box round a stray consonant: which side of the box holds the MAIN consonant, judged on all the tracks together (the box is one choice for all of them:
        the main consonant is either just after it or just before it). Each track is measured in the top of the box's pitch range, where consonants are noise and
        sung vowels hardly reach: the 25 ms right next to each edge (a burst such as t or d shows there), and the quieter half of each side (a long s or sh fills its side).
        Returns { how much the sound BEFORE the box is to be matched (1 = only before: the main consonant is after the box, 0 = only after), the edge the main
        consonant is at (+1 after the box, -1 before it, 0 not clear: then both sides are blended) }. Silent tracks have no say. */
    struct Sides { double wLeft = 0.5; int fade = 0; };
    static std::pair<double, int> consonantSides (const std::vector<const std::vector<float>*>& tracks, double sr, long t0, long t1, double f0, double f1)
    {
        const int N = juce::jmax (256, (int) std::lround (sr * 0.0053 / 128.0) * 128);    // ~5 ms
        const int H = N / 4, bins = N / 2 + 1; const double hz = sr / (double) N;
        double lo = juce::jmax (f0, sideHz), hi = juce::jmin (f1, 16000.0);
        if (hi - lo < 1000.0) { lo = f0; hi = juce::jmin (f1, sr * 0.49); }
        const int c0 = juce::jlimit (1, bins - 1, (int) std::ceil (lo / hz)), c1 = juce::jlimit (c0, bins - 1, (int) std::floor (hi / hz));
        const long span = (long) (bgSeconds * sr), nearSpan = (long) (nearSeconds * sr);
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<std::complex<double>> buf ((size_t) N);
        auto frames = [&] (const std::vector<float>& x, long a, long b)              // the top-band power of every frame lying wholly in [a, b), in time order
        {
            std::vector<double> v;
            a = juce::jmax (0L, a); b = juce::jmin ((long) x.size(), b);
            for (long s = a; s + N <= b; s += H)
            {
                for (int i = 0; i < N; ++i) buf[(size_t) i] = (double) x[(size_t) (s + i)] * win[(size_t) i];
                fft.forward (buf);
                double e = 0.0; for (int k = c0; k <= c1; ++k) e += std::norm (buf[(size_t) k]);
                v.push_back (e);
            }
            return v;
        };
        auto measure = [&] (std::vector<double> v, bool before, double& quiet, double& near)  // the mean just next to the edge, and the mean of the quieter half
        {
            if (v.empty()) return false;
            const int nn = juce::jmax (1, (int) (nearSpan / H));
            double s = 0.0; int k = 0;
            for (int i = 0; i < juce::jmin (nn, (int) v.size()); ++i) { s += before ? v[v.size() - 1 - (size_t) i] : v[(size_t) i]; ++k; }
            near = s / juce::jmax (1, k);
            std::sort (v.begin(), v.end());
            const int use = juce::jmax (1, (int) std::ceil (bgPart * (double) v.size()));
            s = 0.0; for (int i = 0; i < use; ++i) s += v[(size_t) i];
            quiet = s / use;
            return true;
        };
        double vote = 0.0, dqSum = 0.0; int n = 0; bool onlyBefore = true, onlyAfter = true;
        for (auto* t : tracks)
        {
            if (t == nullptr || t->empty()) continue;
            double pk = 0.0;
            for (long i = juce::jmax (0L, t0 - span); i < juce::jmin ((long) t->size(), t1 + span); ++i) pk = juce::jmax (pk, (double) std::abs ((*t)[(size_t) i]));
            if (pk < 1.0e-6) continue;                                                  // (digital silence)
            double qL = 0, nL = 0, qR = 0, nR = 0;
            const bool hl = measure (frames (*t, t0 - span, t0), true, qL, nL), hr = measure (frames (*t, t1, t1 + span), false, qR, nR);
            onlyBefore = onlyBefore && hl && ! hr; onlyAfter = onlyAfter && hr && ! hl;
            if (! hl || ! hr) continue;
            const double dQ = 10.0 * std::log10 ((qR + 1.0e-30) / (qL + 1.0e-30)), dN = 10.0 * std::log10 ((nR + 1.0e-30) / (nL + 1.0e-30));   // > 0: the sound after the box is noisier up top
            double v;                                                                   // > 0: this track says the main consonant is after the box
            if ((dQ > 0.0) == (dN > 0.0)) v = (dQ > 0.0 ? 1.0 : -1.0) * juce::jmin (1.0, juce::jmax (std::abs (dQ), std::abs (dN)) / nearDb);
            else if (std::abs (dQ) > nearDb) v = dQ > 0.0 ? 1.0 : -1.0;             // a long consonant fills one side: it wins
            else v = (dN > 0.0 ? 1.0 : -1.0) * juce::jmin (1.0, std::abs (dN) / nearDb);
            vote += v; dqSum += dQ; ++n;
        }
        if (n == 0) return onlyBefore ? std::make_pair (1.0, 0) : (onlyAfter ? std::make_pair (0.0, 0) : std::make_pair (0.5, 0));     // only one side to go by
        const double V = vote / n;
        if (V >= 0.25) return { 1.0, 1 };
        if (V <= -0.25) return { 0.0, -1 };
        return { juce::jlimit (0.0, 1.0, 0.5 + (dqSum / n) / sideDb), 0 };
    }

private:
    static double smoothStepLog (double f, double edge)                      // 0 well below the edge, 1 well above it
    {
        if (f <= 0.0) return 0.0;
        const double t = juce::jlimit (0.0, 1.0, (std::log (f / edge) / std::log (1.3 / 0.7)) + 0.5);
        return t * t * (3.0 - 2.0 * t);
    }
    static double hash01 (unsigned a, unsigned b)
    {
        unsigned h = a * 73856093u ^ b * 19349663u ^ 0x9e3779b9u;
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
        return (double) (h & 0xffffff) / 16777216.0;
    }
    // consonant mode (tuned on choir recordings with early / late s, sh, t and d):
    static constexpr double bgSeconds = 0.15;       // how far each side is searched for the background (s)
    static constexpr double bgPart = 0.5;           // the quieter half of those frames is the background (a consonant next to the box is louder and is left out)
    static constexpr double bgMargin = 1.5;         // what stays within this much (power, +1.8 dB) of the background is left alone
    static constexpr double sideHz = 4500.0;        // the sides are compared above this pitch (consonants are noise up there, sung vowels hardly reach it)
    static constexpr double sideDb = 6.0;           // a difference of half this (dB) or more decides the side outright; less: the two sides are blended
    static constexpr double nearSeconds = 0.025, nearDb = 3.0;   // the main consonant's edge: the louder of the two 25 ms next to the box, by 3 dB or more
    static constexpr double edgeSeconds = 0.010;    // the level just outside each edge of the box is measured over this long
    static constexpr double decayPart = 0.4, decayMin = 0.012, decayMax = 0.08;     // at the start of the box the sound dies away over 40 % of it (12 to 80 ms) ...
    static constexpr double onsetPart = 0.25, onsetMin = 0.010, onsetMax = 0.04;    // ... and at its end the sound after it is met over 25 % (10 to 40 ms)
    static void attenuatePass (std::vector<std::vector<float>*>& xs, double sr, int N, double bandLo, double bandHi, long t0, long t1, double f0, double f1, const RepairParams& p,
                               const Sides* sides = nullptr)
    {
        const int nch = (int) xs.size(); if (nch < 1) return;
        const long L = (long) xs[0]->size();
        const int H = N / 4, bins = N / 2 + 1;
        const double hzPerBin = sr / (double) N;
        const double lo = juce::jmax (f0, bandLo * 0.7), hi = juce::jmin (f1, bandHi * 1.3);
        if (hi <= lo) return;
        const int b0 = juce::jlimit (0, bins - 1, (int) std::floor (lo / hzPerBin)), b1 = juce::jlimit (0, bins - 1, (int) std::ceil (hi / hzPerBin));
        if (b1 <= b0) return;
        const long gap = t1 - t0;
        long ctx = juce::jlimit ((long) 4 * N, (long) (10.0 * sr), (long) (juce::jlimit (5.0, 1000.0, p.contextPct) / 100.0 * (double) gap));
        if (p.consonant) ctx = juce::jmax (ctx, (long) (bgSeconds * sr) + (long) N);
        const long S = juce::jmax (0L, t0 - ctx - 2L * N), E = juce::jmin (L, t1 + ctx + 2L * N);
        if (E - S < 2L * N) return;
        const long F = (E - S - N) / H + 1;
        if (F < 4) return;
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<std::complex<double>> buf ((size_t) N);
        using Spec = std::vector<std::vector<std::complex<double>>>;
        std::vector<Spec> X ((size_t) nch, Spec ((size_t) F, std::vector<std::complex<double>> ((size_t) bins)));
        for (int c = 0; c < nch; ++c)
            for (long f = 0; f < F; ++f)
            {
                for (int i = 0; i < N; ++i) buf[(size_t) i] = (double) (*xs[(size_t) c])[(size_t) (S + f * H + i)] * win[(size_t) i];
                fft.forward (buf);
                for (int b = 0; b < bins; ++b) X[(size_t) c][(size_t) f][(size_t) b] = buf[(size_t) b];
            }
        // which frames are clean sound to learn from (wholly outside the box, within the surrounding length), and which touch the box
        std::vector<long> left, right, hit;
        for (long f = 0; f < F; ++f)
        {
            const long s = S + f * H;
            if (s + N <= t0 && s >= t0 - ctx) left.push_back (f);
            else if (s >= t1 && s + N <= t1 + ctx) right.push_back (f);
            if (s + N > t0 && s < t1) hit.push_back (f);
        }
        const bool haveL = left.size() >= 2, haveR = right.size() >= 2;
        if (hit.empty() || (! haveL && ! haveR)) return;
        const bool horizontal = p.direction != RepairDirection::UpDown, vertical = p.direction != RepairDirection::LeftRight;
        const double strength = juce::jlimit (0.0, 1.0, p.strength);
        const double wgt = juce::jlimit (0.02, 0.98, p.weighting >= 0.0 ? p.weighting : 0.5);
        const double wExp = std::log (wgt) / std::log (0.5);
        const int vW = juce::jmax (4, (int) std::ceil (juce::jlimit (5.0, 1000.0, p.contextPct) / 100.0 * (double) (b1 - b0 + 1)));
        double winEnergy = 0.0; for (int i = 0; i < N; ++i) winEnergy += win[(size_t) i] * win[(size_t) i];
        // the gain of every hit frame and bin (1 = untouched), in the log domain, for every channel
        std::vector<std::vector<std::vector<float>>> G ((size_t) nch, std::vector<std::vector<float>> (hit.size(), std::vector<float> ((size_t) bins, 0.0f)));
        for (int c = 0; c < nch; ++c)
        {
            auto& Xc = X[(size_t) c];
            // per bin, the LEVEL OF THE SOUND around the box as a line in dB over time (a steady note, a decaying harmonic, a floor of noise): fitted to the last clean frames before the box
            // and the first ones after it, weighted towards the box, and evaluated at the two edges of the box. Inside the box the level is carried from one edge value to the other
            // (so a harmonic that is dying away keeps dying away at its own rate and joins up with itself on the other side); 'spread' is how much the sound at that pitch varies around its line
            // (a tonal line hardly varies: nothing is added to it; noise varies: it keeps its natural scatter)
            std::vector<float> lvL, lvR, spL, spR;
            if (horizontal && ! p.consonant)
            {
                lvL.assign ((size_t) bins, 0.0f); lvR.assign ((size_t) bins, 0.0f); spL.assign ((size_t) bins, 0.0f); spR.assign ((size_t) bins, 0.0f);
                const int K = juce::jmax (6, (int) (0.06 * sr / (double) H));
                for (int side = 0; side < 2; ++side)
                {
                    const auto& fr = side == 0 ? left : right; if (side == 0 ? ! haveL : ! haveR) continue;
                    const int n = (int) fr.size(), k = juce::jmin (K, n);
                    const double edge = side == 0 ? (double) t0 : (double) t1;
                    for (int b = b0; b <= b1; ++b)
                    {
                        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, mn = 1.0e9, mx = -1.0e9;
                        std::vector<double> ys, xs2, ws;
                        for (int i = 0; i < k; ++i)
                        {
                            const long f = side == 0 ? fr[(size_t) (n - 1 - i)] : fr[(size_t) i];            // i = 0 is the frame nearest to the box
                            const double y = std::log (std::abs (Xc[(size_t) f][(size_t) b]) + 1.0e-9);
                            const double xt = (double) (S + f * H) + 0.5 * (double) N, w = std::pow (0.88, (double) i);
                            ys.push_back (y); xs2.push_back ((xt - edge) / (double) H); ws.push_back (w);
                            sw += w; sx += w * xs2.back(); sy += w * y; sxx += w * xs2.back() * xs2.back(); sxy += w * xs2.back() * y; mn = juce::jmin (mn, y); mx = juce::jmax (mx, y);
                        }
                        const double den = sw * sxx - sx * sx;
                        double slope = std::abs (den) > 1.0e-9 ? (sw * sxy - sx * sy) / den : 0.0;
                        double icpt = (sy - slope * sx) / sw;                                              // the line's level at the edge of the box (x = 0)
                        icpt = juce::jlimit (mn - 0.4, mx + 0.4, icpt);
                        double var = 0; for (size_t i = 0; i < ys.size(); ++i) { const double r = ys[i] - (icpt + slope * xs2[i]); var += ws[i] * r * r; }
                        (side == 0 ? lvL : lvR)[(size_t) b] = (float) icpt;
                        (side == 0 ? spL : spR)[(size_t) b] = (float) std::sqrt (var / sw);
                    }
                }
            }
            auto& Gc = G[(size_t) c];
            if (p.consonant)
            {
                // CONSONANT mode: the background on each side is the power, per bin, of the quieter frames near the box (a consonant next to the box is louder in this band and
                // is left out); the side chosen by consonantSides is the target. The level in the box is measured smoothed (a few bins, three frames) and only
                // turned down where it stands above the target, by one smooth gain: the fine grain of the sound stays (a box with no consonant in it is left as it is: no hole),
                // and the singers' vowels that carry on through the box keep their level (they are there on the side that is matched)
                const int w = juce::jmax (1, (int) std::lround (150.0 / hzPerBin));
                const int K = juce::jmax (8, (int) (bgSeconds * sr / (double) H));
                std::vector<double> bgL ((size_t) bins, -1.0), bgR ((size_t) bins, -1.0);
                for (int side = 0; side < 2; ++side)
                {
                    const auto& fr = side == 0 ? left : right; if (side == 0 ? ! haveL : ! haveR) continue;
                    const int n = (int) fr.size(), k = juce::jmin (K, n);
                    std::vector<std::pair<double, long>> en;
                    for (int i = 0; i < k; ++i)
                    {
                        const long f = side == 0 ? fr[(size_t) (n - 1 - i)] : fr[(size_t) i];
                        double e = 0.0; for (int b = b0; b <= b1; ++b) e += std::norm (Xc[(size_t) f][(size_t) b]);
                        en.push_back ({ e, f });
                    }
                    std::sort (en.begin(), en.end());
                    const int use = juce::jmax (juce::jmin (4, k), (int) std::ceil (bgPart * (double) k));
                    auto& bg = side == 0 ? bgL : bgR;
                    for (int b = b0; b <= b1; ++b)
                    {
                        double pw = 0.0; int cnt = 0;
                        for (int i = 0; i < use; ++i)
                            for (int db = -w; db <= w; ++db) { const int bb = b + db; if (bb < 0 || bb >= bins) continue; pw += std::norm (Xc[(size_t) en[(size_t) i].second][(size_t) bb]); ++cnt; }
                        bg[(size_t) b] = pw / juce::jmax (1, cnt);
                    }
                }
                const double wLeft = sides != nullptr ? sides->wLeft : 0.5;
                // the level just outside each edge (per bin, smoothed like the level in the box): where the main consonant ends (or starts), and where the sound on the
                // clean side carries on
                std::vector<double> edL ((size_t) bins, -1.0), edR ((size_t) bins, -1.0);
                {
                    const int kE = juce::jmax (1, (int) std::lround (edgeSeconds * sr / (double) H));
                    for (int side = 0; side < 2; ++side)
                    {
                        const auto& fr = side == 0 ? left : right; if (fr.empty()) continue;
                        const int n = (int) fr.size(), k = juce::jmin (kE, n);
                        auto& ed = side == 0 ? edL : edR;
                        for (int b = b0; b <= b1; ++b)
                        {
                            double pw = 0.0; int cnt = 0;
                            for (int i = 0; i < k; ++i)
                            {
                                const long f = side == 0 ? fr[(size_t) (n - 1 - i)] : fr[(size_t) i];
                                for (int db = -w; db <= w; ++db) { const int bb = b + db; if (bb < 0 || bb >= bins) continue; pw += std::norm (Xc[(size_t) f][(size_t) bb]); ++cnt; }
                            }
                            ed[(size_t) b] = pw / juce::jmax (1, cnt);
                        }
                    }
                }
                // the shape of the target over the box (all in dB, i.e. log power): the background of the clean side in the middle. From the start of the box the sound
                // that was going on (e.g. the main consonant before a late copy) DIES AWAY to that background in a straight line in dB, like a natural decay, rather
                // than stopping dead; towards the end of the box the target rises to meet the sound that actually follows, so there is no step back up where the box ends
                const double gapS = (double) gap / sr;
                const double tDecay = juce::jlimit (decayMin, decayMax, decayPart * gapS) * sr, tOnset = juce::jlimit (onsetMin, onsetMax, onsetPart * gapS) * sr;
                for (size_t hi2 = 0; hi2 < hit.size(); ++hi2)
                {
                    const long f = hit[hi2], s = S + f * H;
                    double inGap = 0.0;
                    for (long n = juce::jmax (s, t0); n < juce::jmin (s + N, t1); ++n) inGap += win[(size_t) (n - s)] * win[(size_t) (n - s)];
                    const double wf = juce::jlimit (0.0, 1.0, inGap / winEnergy * 1.6);
                    if (wf <= 0.0) continue;
                    const double centre = (double) s + 0.5 * (double) N;
                    const double dL = juce::jmax (0.0, centre - (double) t0), dR = juce::jmax (0.0, (double) t1 - centre);     // how far into the box from each edge
                    // how much each edge's own level still counts here (1 at the edge, 0 once its ramp is over)
                    // (the sound before the box dies away into it; the sound after it is met by a quicker rise, as a consonant starts faster than it fades)
                    auto ramp = [] (double d, double len) { return len <= 1.0 ? 0.0 : 1.0 - juce::jlimit (0.0, 1.0, d / len); };
                    const double kL = ramp (dL, tDecay), kR = ramp (dR, tOnset);
                    for (int b = b0; b <= b1; ++b)
                    {
                        double tgt;                                                                 // log of the background power to match
                        if (bgL[(size_t) b] > 0.0 && bgR[(size_t) b] > 0.0)
                            tgt = wLeft * std::log (bgL[(size_t) b] + 1.0e-30) + (1.0 - wLeft) * std::log (bgR[(size_t) b] + 1.0e-30);
                        else if (bgL[(size_t) b] > 0.0 || bgR[(size_t) b] > 0.0) tgt = std::log ((bgL[(size_t) b] > 0.0 ? bgL[(size_t) b] : bgR[(size_t) b]) + 1.0e-30);
                        else continue;
                        if (edL[(size_t) b] > 0.0 && kL > 0.0) tgt = juce::jmax (tgt, tgt + kL * (std::log (edL[(size_t) b] + 1.0e-30) - tgt));
                        if (edR[(size_t) b] > 0.0 && kR > 0.0) tgt = juce::jmax (tgt, tgt + kR * (std::log (edR[(size_t) b] + 1.0e-30) - tgt));
                        double pw = 0.0; int cnt = 0;
                        for (long df = -1; df <= 1; ++df)
                        {
                            const long ff = f + df; if (ff < 0 || ff >= F) continue;
                            for (int db = -w; db <= w; ++db) { const int bb = b + db; if (bb < 0 || bb >= bins) continue; pw += std::norm (Xc[(size_t) ff][(size_t) bb]); ++cnt; }
                        }
                        pw /= juce::jmax (1, cnt);
                        const double lg = 0.5 * (tgt + std::log (bgMargin) - std::log (pw + 1.0e-30));       // log of the amplitude gain
                        if (lg >= 0.0) continue;
                        double wb = 1.0;
                        const double hz = (double) b * hzPerBin;
                        if (b - b0 < 3) wb = juce::jmin (wb, (double) (b - b0 + 1) / 4.0);
                        if (b1 - b < 3) wb = juce::jmin (wb, (double) (b1 - b + 1) / 4.0);
                        if (hz < f0 || hz > f1) wb *= 0.5;
                        if (bandLo > 0.0) wb *= smoothStepLog (hz, bandLo);
                        if (bandHi < 1.0e8) wb *= 1.0 - smoothStepLog (hz, bandHi);
                        Gc[hi2][(size_t) b] = (float) (lg * wf * wb * strength);
                    }
                }
            }
            else
            for (size_t hi2 = 0; hi2 < hit.size(); ++hi2)
            {
                const long f = hit[hi2], s = S + f * H;
                double inGap = 0.0;                                                        // the part of this frame's window energy that lies in the box
                for (long n = juce::jmax (s, t0); n < juce::jmin (s + N, t1); ++n) inGap += win[(size_t) (n - s)] * win[(size_t) (n - s)];
                const double wf = juce::jlimit (0.0, 1.0, inGap / winEnergy * 1.6);
                if (wf <= 0.0) continue;
                const double centre = (double) s + 0.5 * (double) N;
                double alpha = juce::jlimit (0.0, 1.0, (centre - (double) t0) / (double) gap);
                alpha = std::pow (alpha, wExp);
                // the level above and below the box in this frame (for the vertical direction)
                double below = 0.0, above = 0.0; int nb = 0, na = 0;
                if (vertical)
                {
                    for (int b = juce::jmax (0, b0 - vW); b < b0; ++b) { below += std::abs (Xc[(size_t) f][(size_t) b]); ++nb; }
                    for (int b = b1 + 1; b <= juce::jmin (bins - 1, b1 + vW); ++b) { above += std::abs (Xc[(size_t) f][(size_t) b]); ++na; }
                    if (nb > 0) below /= nb;
                    if (na > 0) above /= na;
                }
                for (int b = b0; b <= b1; ++b)
                {
                    const double mag = std::abs (Xc[(size_t) f][(size_t) b]);
                    if (mag < 1.0e-12) continue;
                    double th = -1.0, tv = -1.0;
                    if (horizontal)
                    {
                        const double u = hash01 ((unsigned) f, (unsigned) b);
                        const double eL = haveL ? (double) lvL[(size_t) b] + u * (double) spL[(size_t) b] : 0.0, eR = haveR ? (double) lvR[(size_t) b] + u * (double) spR[(size_t) b] : 0.0;
                        if (haveL && haveR) th = std::exp ((1.0 - alpha) * eL + alpha * eR);
                        else th = std::exp (haveL ? eL : eR);
                    }
                    if (vertical)
                    {
                        const double beta = (double) (b - b0) / (double) juce::jmax (1, b1 - b0);
                        if (nb > 0 && na > 0 && below > 0.0 && above > 0.0) tv = std::exp ((1.0 - beta) * std::log (below) + beta * std::log (above));
                        else tv = nb > 0 ? below : (na > 0 ? above : -1.0);
                    }
                    double target = th > 0.0 && tv > 0.0 ? std::sqrt (th * tv) : (th > 0.0 ? th : tv);
                    if (target <= 0.0 || mag <= target) continue;
                    double wb = 1.0;                                                       // the edges of the box in pitch, and of this analysis size's part of the pitch range
                    const double hz = (double) b * hzPerBin;
                    if (b - b0 < 3) wb = juce::jmin (wb, (double) (b - b0 + 1) / 4.0);
                    if (b1 - b < 3) wb = juce::jmin (wb, (double) (b1 - b + 1) / 4.0);
                    if (hz < f0 || hz > f1) wb *= 0.5;
                    if (bandLo > 0.0) wb *= smoothStepLog (hz, bandLo);
                    if (bandHi < 1.0e8) wb *= 1.0 - smoothStepLog (hz, bandHi);
                    const double g = target / mag;                                          // < 1
                    Gc[hi2][(size_t) b] = (float) (std::log (g) * wf * wb * strength);
                }
            }
            // smooth the gain a little in time and pitch (no musical noise): a 3 x 3 average of the log gain
            auto Gs = Gc;
            for (size_t i = 0; i < hit.size(); ++i)
                for (int b = b0; b <= b1; ++b)
                {
                    double a = 0.0, wsum = 0.0;
                    for (int di = -1; di <= 1; ++di)
                        for (int db = -1; db <= 1; ++db)
                        {
                            const long ii = (long) i + di; const int bb = b + db;
                            if (ii < 0 || ii >= (long) hit.size() || bb < 0 || bb >= bins) continue;
                            const double w = (di == 0 ? 2.0 : 1.0) * (db == 0 ? 2.0 : 1.0);
                            a += w * Gc[(size_t) ii][(size_t) bb]; wsum += w;
                        }
                    Gs[i][(size_t) b] = (float) (a / wsum);
                }
            // keep the stronger of the smoothed and the original (so a loud click is not diluted)
            for (size_t i = 0; i < hit.size(); ++i) for (int b = b0; b <= b1; ++b) Gc[i][(size_t) b] = juce::jmin (Gc[i][(size_t) b], Gs[i][(size_t) b] * 0.5f + Gc[i][(size_t) b] * 0.5f);
        }
        // linked channels (a stereo pair): ONE gain for all of them, the one found for the louder channel in that frame and bin. Every channel is then turned down
        // by exactly the same amount with its own phase kept, so the time differences between the microphones and the level difference between them are not changed
        if (p.linkChannels && nch > 1)
            for (size_t i = 0; i < hit.size(); ++i)
                for (int b = b0; b <= b1; ++b)
                {
                    int best = 0; double bm = -1.0;
                    for (int c = 0; c < nch; ++c) { const double m = std::abs (X[(size_t) c][(size_t) hit[i]][(size_t) b]); if (m > bm) { bm = m; best = c; } }
                    const float g = G[(size_t) best][i][(size_t) b];
                    for (int c = 0; c < nch; ++c) G[(size_t) c][i][(size_t) b] = g;
                }
        // back to sound: apply the gains, overlap-add the changed frames
        std::vector<char> isHit ((size_t) F, 0); for (auto f : hit) isHit[(size_t) f] = 1;
        for (int c = 0; c < nch; ++c)
        {
            auto& Xc = X[(size_t) c]; auto& Gc = G[(size_t) c];
            for (size_t i = 0; i < hit.size(); ++i)
                for (int b = b0; b <= b1; ++b)
                    Xc[(size_t) hit[i]][(size_t) b] *= std::exp ((double) Gc[i][(size_t) b]);
            std::vector<double> out ((size_t) (E - S), 0.0), norm ((size_t) (E - S), 0.0);
            for (long f = 0; f < F; ++f)
            {
                const long s = S + f * H;
                if (! isHit[(size_t) f] && (s + N <= t0 - N || s >= t1 + N)) continue;
                for (int b = 0; b < bins; ++b) buf[(size_t) b] = Xc[(size_t) f][(size_t) b];
                for (int b = 1; b < N / 2; ++b) buf[(size_t) (N - b)] = std::conj (buf[(size_t) b]);
                buf[0] = std::complex<double> (buf[0].real(), 0.0); buf[(size_t) N / 2] = std::complex<double> (buf[(size_t) N / 2].real(), 0.0);
                fft.inverse (buf);
                const size_t o = (size_t) (f * H);
                for (int i = 0; i < N && o + (size_t) i < out.size(); ++i) { out[o + (size_t) i] += buf[(size_t) i].real() * win[(size_t) i]; norm[o + (size_t) i] += win[(size_t) i] * win[(size_t) i]; }
            }
            // the samples in and around the box (all four overlapping frames present) are replaced; the rest of the sound is untouched
            for (long n = juce::jmax (S, t0 - N); n < juce::jmin (E, t1 + N); ++n)
            {
                const size_t i = (size_t) (n - S);
                if (norm[i] > 0.9) (*xs[(size_t) c])[(size_t) n] = (float) (out[i] / norm[i]);              // all four overlapping frames present
            }
        }
    }
public:
    // ------------------------------------------------------------------------------------------------ declick
    /** Mends the samples [t0,t1) by linear prediction from the sound around them. Returns false if the gap is too long for this method. */
    static bool interpolate (std::vector<float>& x, long t0, long t1, int order = 40, double wnc = 1.0e-6)
    {
        const long L = (long) x.size();
        t0 = juce::jlimit (0L, L, t0); t1 = juce::jlimit (0L, L, t1);
        const long m = t1 - t0;
        if (m < 1) return true;
        if (m > 1500) return false;
        const long ctx = 2048;
        const long s0 = juce::jmax (0L, t0 - ctx), s1 = juce::jmin (L, t1 + ctx);
        const int p = (int) juce::jmin ((long) order, (t0 - s0 + s1 - t1) / 8);
        if (p < 4) return false;
        // autocorrelation from the two clean blocks
        std::vector<double> R ((size_t) p + 1, 0.0);
        auto accumulate = [&] (long a, long b) { for (int k = 0; k <= p; ++k) for (long n = a + k; n < b; ++n) R[(size_t) k] += (double) x[(size_t) n] * (double) x[(size_t) (n - k)]; };
        accumulate (s0, t0); accumulate (t1, s1);
        if (R[0] < 1.0e-12) return true;
        R[0] *= 1.0 + wnc;                                              // a little 'white noise correction': keeps the predictor stable for very tonal sound
        std::vector<double> a = levinson (R, p);                           // a[0] = 1
        // unknowns u_i = x[t0 + i]
        const long eLo = t0, eHi = t1 + p;                                 // residual positions touched by the gap
        auto xv = [&] (long n) -> double { return (n >= 0 && n < L) ? (double) x[(size_t) n] : 0.0; };
        std::vector<double> c ((size_t) (eHi - eLo), 0.0);                 // the part of each residual that comes from known samples
        for (long n = eLo; n < eHi; ++n)
        {
            double sum = 0.0;
            for (int k = 0; k <= p; ++k) { const long q = n - k; if (q >= t0 && q < t1) continue; sum += a[(size_t) k] * xv (q); }
            c[(size_t) (n - eLo)] = sum;
        }
        std::vector<double> Ra ((size_t) p + 1, 0.0);
        for (int d = 0; d <= p; ++d) for (int k = 0; k + d <= p; ++k) Ra[(size_t) d] += a[(size_t) k] * a[(size_t) (k + d)];
        std::vector<double> M ((size_t) m * (size_t) m, 0.0), rhs ((size_t) m, 0.0);
        for (long i = 0; i < m; ++i)
        {
            for (long j = 0; j < m; ++j) { const long d = std::abs (i - j); if (d <= p) M[(size_t) (i * m + j)] = Ra[(size_t) d]; }
            double r = 0.0;
            for (int k = 0; k <= p; ++k) { const long n = t0 + i + k; if (n >= eLo && n < eHi) r += a[(size_t) k] * c[(size_t) (n - eLo)]; }
            rhs[(size_t) i] = -r;
            M[(size_t) (i * m + i)] += 1.0e-9 * Ra[0];
        }
        // Cholesky
        std::vector<double> Lm ((size_t) m * (size_t) m, 0.0);
        for (long i = 0; i < m; ++i)
            for (long j = 0; j <= i; ++j)
            {
                double sum = M[(size_t) (i * m + j)];
                for (long k = 0; k < j; ++k) sum -= Lm[(size_t) (i * m + k)] * Lm[(size_t) (j * m + k)];
                if (i == j) { if (sum <= 0.0) return false; Lm[(size_t) (i * m + i)] = std::sqrt (sum); }
                else Lm[(size_t) (i * m + j)] = sum / Lm[(size_t) (j * m + j)];
            }
        std::vector<double> y ((size_t) m), u ((size_t) m);
        for (long i = 0; i < m; ++i) { double s = rhs[(size_t) i]; for (long k = 0; k < i; ++k) s -= Lm[(size_t) (i * m + k)] * y[(size_t) k]; y[(size_t) i] = s / Lm[(size_t) (i * m + i)]; }
        for (long i = m - 1; i >= 0; --i) { double s = y[(size_t) i]; for (long k = i + 1; k < m; ++k) s -= Lm[(size_t) (k * m + i)] * u[(size_t) k]; u[(size_t) i] = s / Lm[(size_t) (i * m + i)]; }
        for (long i = 0; i < m; ++i) x[(size_t) (t0 + i)] = (float) u[(size_t) i];
        return true;
    }

    /** A gap [t0,t1) filled by predicting forward from the sound before it and backward from the sound after it, cross-faded: it keeps the LEVEL of the sound (a mend by
        interpolation of a long gap can come out too quiet: a hole). */
    static bool extrapolateBlend (std::vector<float>& x, long t0, long t1, int order)
    {
        const long L = (long) x.size(), m = t1 - t0;
        if (m < 1 || t0 < 600 || t1 > L - 600) return false;
        const int p = juce::jlimit (16, 128, order);
        auto predict = [&] (std::vector<double>& seq, long known, long count)                 // seq[0..known) is known; extends it by 'count' samples
        {
            std::vector<double> R ((size_t) p + 1, 0.0);
            const long a0 = juce::jmax (0L, known - 2048);
            for (int k = 0; k <= p; ++k) for (long n = a0 + k; n < known; ++n) R[(size_t) k] += seq[(size_t) n] * seq[(size_t) (n - k)];
            if (R[0] < 1.0e-14) { for (long i = 0; i < count; ++i) seq.push_back (0.0); return; }
            R[0] *= 1.0 + 1.0e-6;
            const auto a = levinson (R, p);
            for (long i = 0; i < count; ++i)
            {
                double v = 0.0; const long n = (long) seq.size();
                for (int k = 1; k <= p; ++k) v -= a[(size_t) k] * seq[(size_t) (n - k)];
                seq.push_back (v);
            }
        };
        std::vector<double> fw (x.begin() + juce::jmax (0L, t0 - 2048), x.begin() + t0);
        const long fk = (long) fw.size(); predict (fw, fk, m);
        std::vector<double> bw; for (long n = juce::jmin (L, t1 + 2048) - 1; n >= t1; --n) bw.push_back ((double) x[(size_t) n]);
        const long bk = (long) bw.size(); predict (bw, bk, m);
        for (long i = 0; i < m; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * ((double) i + 0.5) / (double) m);        // 0 at the start of the gap, 1 at its end
            x[(size_t) (t0 + i)] = (float) ((1.0 - w) * fw[(size_t) (fk + i)] + w * bw[(size_t) (bk + (m - 1 - i))]);
        }
        return true;
    }
    static double rmsRange (const std::vector<float>& x, long a, long b)
    {
        a = juce::jmax (0L, a); b = juce::jmin ((long) x.size(), b);
        if (b <= a) return 0.0;
        double e = 0.0; for (long n = a; n < b; ++n) e += (double) x[(size_t) n] * (double) x[(size_t) n];
        return std::sqrt (e / (double) (b - a));
    }

    /** Mends the gap [g0,g1): the missing sound is predicted from both sides; a mend that came out too quiet (a hole) is redone by predicting from each side and blending. */
    static bool mendGap (std::vector<float>& x, long g0, long g1)
    {
        const long m = g1 - g0;
        const std::vector<float> keep (x.begin() + g0, x.begin() + g1);
        if (! interpolate (x, g0, g1, (int) juce::jlimit (48L, 160L, m / 3))) return false;
        if (m > 48)
        {
            const double rr = 0.5 * (rmsRange (x, g0 - 128, g0) + rmsRange (x, g1, g1 + 128)), rg = rmsRange (x, g0, g1);
            if (rr > 1.0e-7 && rg < 0.55 * rr)
                if (! extrapolateBlend (x, g0, g1, (int) juce::jlimit (32L, 128L, m / 2))) std::copy (keep.begin(), keep.end(), x.begin() + g0);
        }
        if (m >= 24)                                    // a prediction from both sides tends to be a little weak in the middle of a gap: bring it up to the level around it (smoothly, so the edges stay joined)
        {
            const long w = juce::jmax (128L, 3 * m);                       // (long windows: a short one of a low note gives a level that depends on where in its cycle it is)
            const double rb = rmsRange (x, g0 - w, g0), ra = rmsRange (x, g1, g1 + w), rg = rmsRange (x, g0, g1);
            const double target = std::sqrt (juce::jmax (1.0e-12, rb) * juce::jmax (1.0e-12, ra));
            if (rb > 1.0e-6 && ra > 1.0e-6 && rg > 1.0e-9 && rg < 0.6 * target)
            {
                const double k = juce::jmin (2.0, target / rg);
                for (long n = 0; n < m; ++n)
                {
                    const double w = std::sin (juce::MathConstants<double>::pi * ((double) n + 0.5) / (double) m);
                    x[(size_t) (g0 + n)] = (float) ((double) x[(size_t) (g0 + n)] * (1.0 + (k - 1.0) * w * w));
                }
            }
        }
        return true;
    }

    /** Dropouts: a run of digital silence (exact zeros) in the middle of sound, such as a converter or a file-writing glitch leaves. A click finder sees only the jump at each end of it, so the
        silence itself used to stay. The run (and one sample each side, which a dropout often half-covers) is rebuilt from the sound before and after it. Quiet passages and real silence are left alone:
        the sound next to the run must be louder than about -94 dBFS (-70 for runs under 8 samples) on BOTH sides. */
    static int mendDropouts (std::vector<float>& x, long t0, long t1, double sr, std::vector<std::pair<long, long>>* found)
    {
        const long L = (long) x.size();
        const long maxRun = juce::jmin (1500L, (long) (sr * 0.02));
        const float tol = 3.0e-7f;
        int n = 0;
        for (long i = juce::jmax (1L, t0); i < juce::jmin (L - 1, t1);)
        {
            if (std::abs (x[(size_t) i]) > tol) { ++i; continue; }
            long j = i; while (j < L && std::abs (x[(size_t) j]) <= tol) ++j;
            const long run = j - i;
            if (run >= 3 && run <= maxRun && i > 64 && j + 64 < L)
            {
                auto level = [&] (long a, long b) { double s2 = 0; for (long k = a; k < b; ++k) s2 += std::abs (x[(size_t) k]); return s2 / (double) (b - a); };
                const double need = run >= 8 ? 2.0e-5 : 3.0e-4;                       // a long run of exact zeros in sound above about -94 dBFS is a dropout; a very short one must sit in louder sound
                if (level (i - 48, i) > need && level (j, j + 48) > need)
                {
                    const long g0 = i - 1, g1 = j + 1;
                    if (mendGap (x, g0, g1)) { ++n; if (found) found->push_back ({ g0, g1 }); }
                }
            }
            i = j + 1;
        }
        return n;
    }

    /** One pass of the click finder over [t0,t1). Returns how many gaps were mended.
        The sound is whitened block by block (a linear-prediction model of each stretch, so loud and quiet passages are judged by their own standard); a click is either a sample that jumps
        out of the residual, or a short stretch whose residual ENERGY is well above normal (a crackle whose single samples are not extreme). A gap is not mended when the sound after it is much
        louder or quieter than before it (a note starting or stopping is music, not a click), and a mend that came out too quiet is redone by predicting from both sides. */
    static int declickPass (std::vector<float>& x, long t0, long t1, double sensitivity, std::vector<std::pair<long, long>>* found, double sr = 48000.0, bool crackles = true)
    {
        const long L = (long) x.size();
        const int p = juce::jlimit (24, 48, (int) (sr / 3000.0));
        const long ctxM = 1024;
        const long s0 = juce::jmax (0L, t0 - ctxM), s1 = juce::jmin (L, t1 + ctxM);
        if (s1 - s0 < 4L * p) return 0;
        std::vector<double> e ((size_t) (s1 - s0), 0.0);
        // block by block: a prediction model of the stretch (from the block and 1024 samples each side), and its residual over the block
        const long blk = 4096;
        for (long b0 = s0; b0 < s1; b0 += blk)
        {
            const long b1 = juce::jmin (s1, b0 + blk), f0 = juce::jmax (0L, b0 - ctxM), f1 = juce::jmin (L, b1 + ctxM);
            // the model is learnt from a copy in which the grossest spikes (a second difference far out of the normal) are taken out, so a loud click does not spoil the model of its whole block
            std::vector<double> fit ((size_t) (f1 - f0)); for (long n = f0; n < f1; ++n) fit[(size_t) (n - f0)] = (double) x[(size_t) n];
            {
                std::vector<double> d2, mags;
                for (long n = f0 + 1; n + 1 < f1; ++n) { const double d = fit[(size_t) (n - f0)] - 0.5 * (fit[(size_t) (n - f0 - 1)] + fit[(size_t) (n - f0 + 1)]); d2.push_back (d); mags.push_back (std::abs (d)); }
                if (mags.size() > 16)
                {
                    auto mid = mags.begin() + (long) mags.size() / 2; std::nth_element (mags.begin(), mid, mags.end());
                    const double lim = 12.0 * 1.4826 * *mid + 1.0e-12;
                    std::vector<char> bad (fit.size(), 0);
                    for (size_t i = 0; i < d2.size(); ++i) if (std::abs (d2[i]) > lim) for (long q = (long) i - 1; q <= (long) i + 3; ++q) if (q >= 0 && q < (long) bad.size()) bad[(size_t) q] = 1;
                    for (size_t i = 0; i < bad.size();)
                    {
                        if (! bad[i]) { ++i; continue; }
                        size_t j = i; while (j < bad.size() && bad[j]) ++j;                       // [i,j) is a run of spoilt samples: a straight line across it from the good ones either side
                        const double va = i > 0 ? fit[i - 1] : 0.0, vb = j < fit.size() ? fit[j] : va;
                        for (size_t q = i; q < j; ++q) fit[q] = va + (vb - va) * (double) (q - i + 1) / (double) (j - i + 1);
                        i = j;
                    }
                }
            }
            std::vector<double> R ((size_t) p + 1, 0.0);
            for (int k = 0; k <= p; ++k) for (long n = k; n < f1 - f0; ++n) R[(size_t) k] += fit[(size_t) n] * fit[(size_t) (n - k)];
            if (R[0] < 1.0e-14) continue;
            R[0] *= 1.0 + 1.0e-9;
            const auto a = levinson (R, p);
            for (long n = juce::jmax (b0, (long) p); n < b1; ++n) { double sum = 0.0; for (int k = 0; k <= p; ++k) sum += a[(size_t) k] * (double) x[(size_t) (n - k)]; e[(size_t) (n - s0)] = sum; }
        }
        const double hi = juce::jmap (juce::jlimit (1.0, 10.0, sensitivity), 1.0, 10.0, 12.0, 3.0);      // a sample this many 'sigmas' out is a click
        const double lo = juce::jmax (1.6, hi * 0.45);                                                     // ... and its neighbours are part of it while they are above this
        const double energyDb = juce::jmap (juce::jlimit (1.0, 10.0, sensitivity), 1.0, 10.0, 15.0, 6.0);  // a stretch whose residual energy is this many dB above normal is a click
        // the normal size of the residual here: the median of its size in small blocks, and of the neighbouring blocks (so it follows the loudness of the music)
        const long sub = juce::jmax (64L, (long) (sr * 0.004));
        const long nSub = (s1 - s0 + sub - 1) / sub;
        std::vector<double> subMed ((size_t) nSub, 1.0e-9);
        for (long i = 0; i < nSub; ++i)
        {
            std::vector<double> mag; for (long n = s0 + i * sub; n < juce::jmin (s1, s0 + (i + 1) * sub); ++n) mag.push_back (std::abs (e[(size_t) (n - s0)]));
            if (mag.size() < 8) { subMed[(size_t) i] = i > 0 ? subMed[(size_t) i - 1] : 1.0e-9; continue; }
            auto mid = mag.begin() + (long) mag.size() / 2; std::nth_element (mag.begin(), mid, mag.end()); subMed[(size_t) i] = *mid;
        }
        std::vector<double> sig (e.size(), 1.0e-9);
        for (long i = 0; i < nSub; ++i)
        {
            std::vector<double> nb; for (long j = juce::jmax (0L, i - 4); j <= juce::jmin (nSub - 1, i + 4); ++j) nb.push_back (subMed[(size_t) j]);
            auto mid = nb.begin() + (long) nb.size() / 2; std::nth_element (nb.begin(), mid, nb.end());
            const double sigma = juce::jmax (1.0e-9, 1.4826 * *mid);
            for (long n = s0 + i * sub; n < juce::jmin (s1, s0 + (i + 1) * sub); ++n) sig[(size_t) (n - s0)] = sigma;
        }
        auto big = [&] (long n, double k) { return std::abs (e[(size_t) (n - s0)]) > k * sig[(size_t) (n - s0)]; };
        std::vector<char> flag (e.size(), 0);
        const long first0 = juce::jmax (s0 + p, t0), last0 = juce::jmin (s1, t1);
        for (long n = first0; n < last0; ++n) if (big (n, hi)) flag[(size_t) (n - s0)] = 1;
        // a long click does not jump on every sample: follow it outwards while the residual stays clearly above normal (but not for ever)
        const long reach = juce::jmax (24L, (long) (sr * 0.0015));
        for (long n = s0 + p; n < s1; ++n)
            if (flag[(size_t) (n - s0)] == 1)
            {
                for (long m = n + 1; m < s1 && m < n + reach && big (m, lo); ++m) if (! flag[(size_t) (m - s0)]) flag[(size_t) (m - s0)] = 2;
                for (long m = n - 1; m >= s0 + p && m > n - reach && big (m, lo); --m) if (! flag[(size_t) (m - s0)]) flag[(size_t) (m - s0)] = 2;
            }
        // a crackle: a short stretch with far more residual energy than normal, even if no single sample is extreme (only once the loud spikes are mended: they would spoil the model)
        if (crackles)
        {
            const long W = juce::jmax (16L, (long) (sr * 0.0005));
            std::vector<double> cum (e.size() + 1, 0.0), cumN (e.size() + 1, 0.0);
            for (size_t i = 0; i < e.size(); ++i) { const double q = e[i] / sig[i]; cum[i + 1] = cum[i] + q * q; }
            const double ratio = std::pow (10.0, energyDb / 10.0);
            std::vector<char> eflag (e.size(), 0);
            for (long n = juce::jmax (s0 + p + W, t0); n < juce::jmin (s1 - W, t1); ++n)
            {
                const double en = cum[(size_t) (n - s0 + W / 2)] - cum[(size_t) (n - s0 - W / 2)];
                if (en > ratio * (double) W * 1.0) eflag[(size_t) (n - s0)] = 1;                         // (a quiet stretch has about 1 per sample in these units, as sigma is its own scale)
            }
            for (long n = s0; n < s1;)
            {
                if (! eflag[(size_t) (n - s0)]) { ++n; continue; }
                long a = n; while (n < s1 && eflag[(size_t) (n - s0)]) ++n;
                long b = n;                                                                              // [a,b): the stretch; trim its ends to where the residual really is above normal
                while (a < b && ! big (a, 1.8)) ++a;
                while (b > a && ! big (b - 1, 1.8)) --b;
                if (b - a < 3) continue;
                for (long m = a; m < b; ++m) if (! flag[(size_t) (m - s0)]) flag[(size_t) (m - s0)] = 3;
            }
        }
        std::vector<std::pair<long, long>> gaps;
        for (long n = s0; n < s1;)
        {
            if (! flag[(size_t) (n - s0)]) { ++n; continue; }
            long first = n, last = n;
            while (n < s1 && (flag[(size_t) (n - s0)] || (n - last) <= 12)) { if (n < s1 && flag[(size_t) (n - s0)]) last = n; ++n; }
            long g0 = first - 2, g1 = juce::jmax (first + 3, last - p / 2) + 4;       // the damaged samples: where the residual first jumps, to a little after the last big one
            g0 = juce::jmax (t0, g0); g1 = juce::jmin (t1, g1);
            if (! gaps.empty() && g0 - gaps.back().second < 24) gaps.back().second = g1; else gaps.push_back ({ g0, g1 });
        }
        // many separate 'clicks' close together are not clicks: they are the hammer or a key of an instrument, a pencil on a desk ... (an attack): left alone
        {
            std::vector<char> drop (gaps.size(), 0);
            const long win = (long) (sr * 0.003);
            for (size_t i = 0; i < gaps.size(); ++i)
            {
                int near = 0; for (size_t j = 0; j < gaps.size(); ++j) if (j != i && std::abs (gaps[j].first - gaps[i].first) < win) ++near;
                if (near >= 3) drop[i] = 1;
            }
            std::vector<std::pair<long, long>> keepG; for (size_t i = 0; i < gaps.size(); ++i) if (! drop[i]) keepG.push_back (gaps[i]);
            gaps.swap (keepG);
        }
        int mended = 0;
        const long maxGap = juce::jmin (1500L, (long) (sr * 0.016));
        const long ref = juce::jmax (64L, (long) (sr * 0.0025));
        for (auto& g : gaps)
        {
            const long m = g.second - g.first;
            if (m < 1 || m > maxGap) continue;
            if (m > 12)                                                             // a note starting or stopping is music, not a click: after it the residual stays high (or was high before it)
            {
                auto resRms = [&] (long a, long b) { a = juce::jmax (s0 + p, a); b = juce::jmin (s1, b); if (b <= a + 8) return -1.0; std::vector<double> v; for (long n = a; n < b; ++n) v.push_back (std::abs (e[(size_t) (n - s0)] / sig[(size_t) (n - s0)])); auto mid = v.begin() + (long) v.size() / 2; std::nth_element (v.begin(), mid, v.end()); return *mid; };       // (the median: the ringing of a loud click does not count)
                const double rb = resRms (g.first - ref, g.first - 1), ra = resRms (g.second + p, g.second + p + ref);
                if (rb > 0.0 && ra > 0.0 && (ra > 2.5 * rb || rb > 2.5 * ra)) continue;
            }
            if (! mendGap (x, g.first, g.second)) continue;
            ++mended; if (found) found->push_back (g);
        }
        return mended;
    }

    /** Finds clicks in [t0,t1) (samples of x) and mends each one. 'sensitivity' 1..10 (10 = finds the faintest clicks). Returns how many were mended.
        Several passes, so what a first mend uncovers is found too. If the stretch is short (30 ms or less) it is taken to BE the click and is mended as a whole.
        Lost samples (a digital jump where a block of samples went missing, see LostSamples) are put back first, with this track alone deciding how many are missing;
        'lostDone' >= 0 says the caller has already done that (on all the tracks together) and how many it put back in this stretch. */
    static int declick (std::vector<float>& x, long t0, long t1, double sensitivity, std::vector<std::pair<long, long>>* found = nullptr, double sampleRate = 48000.0, int lostDone = -1)
    {
        const long L = (long) x.size();
        t0 = juce::jlimit (0L, L, t0); t1 = juce::jlimit (0L, L, t1);
        if (t1 - t0 < 4) return 0;
        int total = 0;
        if (lostDone < 0)
        {
            const auto sp = LostSamples::find ({ &x }, t0, t1, sensitivity, sampleRate);
            lostDone = LostSamples::mendAll (x, sp, sampleRate, 0, L);
            if (found) for (auto& s : sp) found->push_back ({ s.at, s.at + 1 });
        }
        total += lostDone;
        if (t1 - t0 <= (long) (0.03 * sampleRate))                         // a short area chosen by the user: mend all of it
        {
            if (lostDone > 0)                                               // (the jump of lost samples is mended by putting them back: what is left of a click in the area is mended the usual way)
            {
                for (int pass = 0; pass < 3; ++pass)
                {
                    const int n = declickPass (x, t0, t1, sensitivity, found, sampleRate, false);
                    total += n;
                    if (n == 0) break;
                }
                return total;
            }
            if (interpolate (x, t0, t1, (int) juce::jlimit (40L, 128L, (t1 - t0) / 4))) { if (found) found->push_back ({ t0, t1 }); return 1; }
            return 0;
        }
        if (t1 - t0 < 256) return total;
        total += mendDropouts (x, t0, t1, sampleRate, found);                // runs of digital silence: what is left of their edges is then no click
        for (int pass = 0; pass < 3; ++pass)
        {
            const int n = declickPass (x, t0, t1, sensitivity, found, sampleRate, pass > 0);
            total += n;
            if (n == 0) break;
        }
        return total;
    }

private:
    using Cd = std::complex<double>;
    static std::vector<std::vector<Cd>> analyse (const std::vector<float>& x, long S, long F, int N, int H)
    {
        const long L = (long) x.size();
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<std::vector<Cd>> X ((size_t) F, std::vector<Cd> ((size_t) N / 2 + 1));
        std::vector<Cd> buf ((size_t) N);
        for (long f = 0; f < F; ++f)
        {
            for (int i = 0; i < N; ++i) { const long n = S + f * H + i; buf[(size_t) i] = ((n >= 0 && n < L) ? (double) x[(size_t) n] : 0.0) * win[(size_t) i]; }
            fft.forward (buf);
            for (int b = 0; b <= N / 2; ++b) X[(size_t) f][(size_t) b] = buf[(size_t) b];
        }
        return X;
    }
    /** Overlap-add of the frames Y; only [t0,t1) of x is rewritten, with a short cross-fade at its two edges. */
    static void synthesise (const std::vector<std::vector<Cd>>& Y, std::vector<float>& x, long S, int N, int H, long t0, long t1)
    {
        const long F = (long) Y.size(); const long L = (long) x.size();
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<double> out ((size_t) (F * H + N), 0.0), norm (out.size(), 0.0);
        std::vector<Cd> buf ((size_t) N);
        for (long f = 0; f < F; ++f)
        {
            for (int b = 0; b <= N / 2; ++b) buf[(size_t) b] = Y[(size_t) f][(size_t) b];
            for (int b = 1; b < N / 2; ++b) buf[(size_t) (N - b)] = std::conj (buf[(size_t) b]);
            buf[0] = Cd (buf[0].real(), 0.0); buf[(size_t) N / 2] = Cd (buf[(size_t) N / 2].real(), 0.0);
            fft.inverse (buf);
            const size_t o = (size_t) (f * H);
            for (int i = 0; i < N; ++i) { out[o + (size_t) i] += buf[(size_t) i].real() * win[(size_t) i]; norm[o + (size_t) i] += win[(size_t) i] * win[(size_t) i]; }
        }
        const long xf = juce::jmax (1L, juce::jmin (96L, (t1 - t0) / 4));
        for (long n = juce::jmax (0L, t0); n < juce::jmin (L, t1); ++n)
        {
            const size_t i = (size_t) (n - S);
            if (i >= out.size() || norm[i] < 0.2) continue;
            const double e = juce::jmin (1.0, (double) (n - t0 + 1) / (double) xf, (double) (t1 - n) / (double) xf);
            x[(size_t) n] = (float) ((1.0 - e) * (double) x[(size_t) n] + e * (out[i] / norm[i]));
        }
    }
    /** Up and down: the box is filled with a level that runs smoothly from the sound just below it to the sound just above it (frame by frame, so it follows the music in time). */
    static void fillVertical (std::vector<float>& x, double sr, long t0, long t1, double f0, double f1, double ctxPct)
    {
        const int N = sr > 60000.0 ? 4096 : 2048, H = N / 4, bins = N / 2 + 1;
        const long L = (long) x.size();
        const long S = juce::jmax (0L, t0 - N), E = juce::jmin (L, t1 + N);
        const int b0 = juce::jlimit (0, bins - 1, (int) std::floor (f0 * N / sr)), b1 = juce::jlimit (0, bins - 1, (int) std::ceil (f1 * N / sr));
        const bool haveLo = b0 > 0, haveHi = b1 < bins - 1;
        if ((! haveLo && ! haveHi) || b1 < b0) return;                      // the box covers every frequency: there is nothing above or below to learn from
        const int width = b1 - b0 + 1;
        const int ctx = juce::jmax (2, (int) std::lround (ctxPct / 100.0 * width));
        const long F = (long) std::ceil ((double) (E - S) / H) + 1;
        auto X = analyse (x, S, F, N, H);
        auto meanLog = [&] (const std::vector<Cd>& fr, int a, int b)
        {
            double sum = 0.0; int cnt = 0;
            for (int k = juce::jmax (0, a); k <= juce::jmin (bins - 1, b); ++k) { sum += std::log (std::abs (fr[(size_t) k]) + 1.0e-9); ++cnt; }
            return cnt > 0 ? sum / cnt : -20.0;
        };
        for (long f = 0; f < F; ++f)
        {
            const long centre = S + f * H + N / 2;
            if (centre < t0 || centre >= t1) continue;
            auto& fr = X[(size_t) f];
            const double lo = haveLo ? meanLog (fr, b0 - ctx, b0 - 1) : 0.0, hi = haveHi ? meanLog (fr, b1 + 1, b1 + ctx) : 0.0;
            for (int b = b0; b <= b1; ++b)
            {
                const double pos = ((double) (b - b0) + 0.5) / width;
                const double target = haveLo && haveHi ? lo + pos * (hi - lo) : (haveLo ? lo : hi);
                fr[(size_t) b] = std::polar (std::exp (target), std::arg (fr[(size_t) b]));       // the level of the surroundings, the timing of the original
            }
        }
        synthesise (X, x, S, N, H, t0, t1);
    }
    /** Both directions: the size of every point is the geometric mean of the two results, the phase is the one that continues from the left / right. */
    static void combineGeometric (std::vector<float>& h, const std::vector<float>& v, double sr, long t0, long t1, double f0, double f1)
    {
        const int N = sr > 60000.0 ? 4096 : 2048, H = N / 4, bins = N / 2 + 1;
        const long L = (long) h.size();
        const long S = juce::jmax (0L, t0 - N), E = juce::jmin (L, t1 + N);
        const int b0 = juce::jlimit (0, bins - 1, (int) std::floor (f0 * N / sr)), b1 = juce::jlimit (0, bins - 1, (int) std::ceil (f1 * N / sr));
        const long F = (long) std::ceil ((double) (E - S) / H) + 1;
        auto XH = analyse (h, S, F, N, H); const auto XV = analyse (v, S, F, N, H);
        for (long f = 0; f < F; ++f)
        {
            const long centre = S + f * H + N / 2;
            if (centre < t0 || centre >= t1) continue;
            for (int b = b0; b <= b1; ++b)
            {
                auto& c = XH[(size_t) f][(size_t) b];
                c = std::polar (std::sqrt (std::abs (c) * std::abs (XV[(size_t) f][(size_t) b])), std::arg (c));
            }
        }
        synthesise (XH, h, S, N, H, t0, t1);
    }
    static std::vector<double> levinson (const std::vector<double>& R, int p)
    {
        std::vector<double> a ((size_t) p + 1, 0.0), tmp ((size_t) p + 1);
        a[0] = 1.0; double err = R[0];
        for (int i = 1; i <= p; ++i)
        {
            double acc = R[(size_t) i];
            for (int j = 1; j < i; ++j) acc += a[(size_t) j] * R[(size_t) (i - j)];
            const double k = -acc / err;
            tmp = a;
            for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + k * tmp[(size_t) (i - j)];
            a[(size_t) i] = k;
            err *= (1.0 - k * k);
            if (err <= 1.0e-18) break;
        }
        return a;
    }

    /** The core of the patch: continues the sound to the left of t0 across [t0,t1), only in the bins between f0 and f1 (all of x is rewritten in the gap area). */
    static void fillFromLeft (std::vector<double>& x, double sr, long t0, long t1, double f0, double f1, long ctxSamples = 0)
    {
        const long L = (long) x.size();
        const int N = sr > 60000.0 ? 4096 : 2048, H = N / 4, bins = N / 2 + 1;
        const long S = juce::jmax (0L, t0 - juce::jmax (8L * N, ctxSamples > 0 ? ctxSamples + 2L * N : 0L));
        const long E = juce::jmin (L, t1 + 2L * N);
        const long F = (long) std::ceil ((double) (E - S) / H) + 1;
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        auto sample = [&] (long n) { return (n >= 0 && n < L) ? x[(size_t) n] : 0.0; };
        std::vector<std::vector<std::complex<double>>> X ((size_t) F, std::vector<std::complex<double>> ((size_t) bins));
        std::vector<std::complex<double>> buf ((size_t) N);
        for (long f = 0; f < F; ++f)
        {
            for (int i = 0; i < N; ++i) buf[(size_t) i] = sample (S + f * H + i) * win[(size_t) i];
            fft.forward (buf);
            for (int b = 0; b < bins; ++b) X[(size_t) f][(size_t) b] = buf[(size_t) b];
        }
        long lastClean = -1;
        for (long f = 0; f < F; ++f) if (S + f * H + N <= t0) lastClean = f;
        if (lastClean < 2) return;
        const int b0 = juce::jlimit (0, bins - 1, (int) std::floor (f0 * N / sr)), b1 = juce::jlimit (0, bins - 1, (int) std::ceil (f1 * N / sr));
        // instantaneous frequency of every bin, from the last two clean frames
        std::vector<double> inst ((size_t) bins), phase ((size_t) bins);
        for (int b = 0; b < bins; ++b)
        {
            const double om = 2.0 * dsp::kPi * b / N;
            const double p1 = std::arg (X[(size_t) lastClean][(size_t) b]), p0 = std::arg (X[(size_t) lastClean - 1][(size_t) b]);
            inst[(size_t) b] = om + dsp::wrapPhase (p1 - p0 - om * H) / H;
            phase[(size_t) b] = p1;
        }
        const long M = ctxSamples > 0 ? juce::jmin (lastClean, juce::jmax (2L, ctxSamples / H)) : juce::jmin (lastClean, 24L);
        long g = 0;
        auto Y = X;
        for (long f = lastClean + 1; f < F; ++f, ++g)
        {
            if (S + f * H >= t1) break;                                         // frames wholly after the gap are left alone
            long idx = M > 0 ? g % (2 * M) : 0; if (idx > M) idx = 2 * M - idx;
            const auto& src = X[(size_t) (lastClean - idx)];
            for (int b = b0; b <= b1; ++b)
            {
                phase[(size_t) b] += inst[(size_t) b] * H;
                double w = 1.0;
                if (b - b0 < 3) w = juce::jmin (w, (double) (b - b0 + 1) / 4.0);
                if (b1 - b < 3) w = juce::jmin (w, (double) (b1 - b + 1) / 4.0);
                const auto synth = std::polar (std::abs (src[(size_t) b]), phase[(size_t) b]);
                Y[(size_t) f][(size_t) b] = w * synth + (1.0 - w) * X[(size_t) f][(size_t) b];
            }
            for (int b = 0; b < bins; ++b) if (b < b0 || b > b1) {}              // other bins keep the original
        }
        std::vector<double> out ((size_t) (E - S) + (size_t) N, 0.0), norm (out.size(), 0.0);
        for (long f = 0; f < F; ++f)
        {
            for (int b = 0; b < bins; ++b) buf[(size_t) b] = Y[(size_t) f][(size_t) b];
            for (int b = 1; b < N / 2; ++b) buf[(size_t) (N - b)] = std::conj (buf[(size_t) b]);
            buf[0] = std::complex<double> (buf[0].real(), 0.0); buf[(size_t) N / 2] = std::complex<double> (buf[(size_t) N / 2].real(), 0.0);
            fft.inverse (buf);
            const size_t o = (size_t) f * (size_t) H;
            for (int i = 0; i < N && o + (size_t) i < out.size(); ++i) { out[o + (size_t) i] += buf[(size_t) i].real() * win[(size_t) i]; norm[o + (size_t) i] += win[(size_t) i] * win[(size_t) i]; }
        }
        for (long n = juce::jmax (S, t0 - N); n < juce::jmin (E, t1 + N); ++n)
        {
            const size_t i = (size_t) (n - S);
            if (norm[i] > 0.2) x[(size_t) n] = out[i] / norm[i];
        }
    }
};
} // namespace td
