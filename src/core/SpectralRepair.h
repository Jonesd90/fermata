#pragma once
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

/** The controls of a spectral repair (like iZotope RX's Spectral Repair). */
struct RepairParams
{
    double strength = 1.0;                              // 0 .. 1: how much of the repair replaces the original inside the box
    RepairDirection direction = RepairDirection::LeftRight;
    double contextPct = 100.0;                          // how much surrounding sound is used, as a % of the box (its length in time, its height in frequency)
    double weighting = 0.5;                             // 0 = only the sound BEFORE the box, 1 = only the sound AFTER it, 0.5 = both equally (cross-fade)
};

/** A spectrogram of several tracks merged into one picture (the loudest track at every point), in dB. */
struct MergedSpectrogram
{
    int bins = 0, frames = 0;
    double sampleRate = 48000.0, hopSamples = 512.0, fftSize = 2048.0;
    std::vector<float> db;                       // frames * bins
    float at (int frame, int bin) const { return db[(size_t) frame * (size_t) bins + (size_t) bin]; }
    double hzToBin (double hz) const { return hz * fftSize / sampleRate; }
};

class SpectralRepair
{
public:
    /** The merged spectrogram of all the tracks (each a mono vector of the same length). */
    static MergedSpectrogram spectrogram (const std::vector<std::vector<float>>& tracks, double sr, int maxFrames = 1600)
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

    // ------------------------------------------------------------------------------------------------ declick
    /** Mends the samples [t0,t1) by linear prediction from the sound around them. Returns false if the gap is too long for this method. */
    static bool interpolate (std::vector<float>& x, long t0, long t1, int order = 40)
    {
        const long L = (long) x.size();
        t0 = juce::jlimit (0L, L, t0); t1 = juce::jlimit (0L, L, t1);
        const long m = t1 - t0;
        if (m < 1) return true;
        if (m > 700) return false;
        const long ctx = 2048;
        const long s0 = juce::jmax (0L, t0 - ctx), s1 = juce::jmin (L, t1 + ctx);
        const int p = (int) juce::jmin ((long) order, (t0 - s0 + s1 - t1) / 8);
        if (p < 4) return false;
        // autocorrelation from the two clean blocks
        std::vector<double> R ((size_t) p + 1, 0.0);
        auto accumulate = [&] (long a, long b) { for (int k = 0; k <= p; ++k) for (long n = a + k; n < b; ++n) R[(size_t) k] += (double) x[(size_t) n] * (double) x[(size_t) (n - k)]; };
        accumulate (s0, t0); accumulate (t1, s1);
        if (R[0] < 1.0e-12) return true;
        R[0] *= 1.0 + 1.0e-9;
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

    /** Finds clicks in [t0,t1) (samples of x) and mends each one. 'sensitivity' 1..10 (10 = finds the faintest clicks). Returns how many were mended. */
    static int declick (std::vector<float>& x, long t0, long t1, double sensitivity, std::vector<std::pair<long, long>>* found = nullptr)
    {
        const long L = (long) x.size();
        t0 = juce::jlimit (0L, L, t0); t1 = juce::jlimit (0L, L, t1);
        const int p = 32;
        if (t1 - t0 < 256) return 0;
        const long s0 = juce::jmax (0L, t0 - 1024), s1 = juce::jmin (L, t1 + 1024);
        std::vector<double> R ((size_t) p + 1, 0.0);
        for (int k = 0; k <= p; ++k) for (long n = s0 + k; n < s1; ++n) R[(size_t) k] += (double) x[(size_t) n] * (double) x[(size_t) (n - k)];
        if (R[0] < 1.0e-12) return 0;
        R[0] *= 1.0 + 1.0e-9;
        const auto a = levinson (R, p);
        std::vector<double> e ((size_t) (s1 - s0), 0.0);
        for (long n = s0 + p; n < s1; ++n) { double sum = 0.0; for (int k = 0; k <= p; ++k) sum += a[(size_t) k] * (double) x[(size_t) (n - k)]; e[(size_t) (n - s0)] = sum; }
        const double thresh = juce::jmap (juce::jlimit (1.0, 10.0, sensitivity), 1.0, 10.0, 14.0, 4.5);
        std::vector<char> flag (e.size(), 0);
        const long block = 4096;
        for (long b0 = s0; b0 < s1; b0 += block)
        {
            const long b1 = juce::jmin (s1, b0 + block);
            std::vector<double> mag; for (long n = b0; n < b1; ++n) mag.push_back (std::abs (e[(size_t) (n - s0)]));
            if (mag.size() < 16) continue;
            auto mid = mag.begin() + (long) mag.size() / 2; std::nth_element (mag.begin(), mid, mag.end());
            const double sigma = juce::jmax (1.0e-9, 1.4826 * *mid);
            for (long n = b0; n < b1; ++n) if (std::abs (e[(size_t) (n - s0)]) > thresh * sigma && n >= t0 && n < t1) flag[(size_t) (n - s0)] = 1;
        }
        std::vector<std::pair<long, long>> gaps;
        for (long n = s0; n < s1;)
        {
            if (! flag[(size_t) (n - s0)]) { ++n; continue; }
            long first = n, last = n;
            while (n < s1 && (flag[(size_t) (n - s0)] || (n - last) <= 3 + 0)) { if (n < s1 && flag[(size_t) (n - s0)]) last = n; ++n; }
            long g0 = first - 1, g1 = juce::jmax (first + 3, last - p / 2 + 1) + 2;       // the damaged samples: where the residual first jumps, to a little after the last big one
            gaps.push_back ({ juce::jmax (t0, g0), juce::jmin (t1, g1) });
        }
        int mended = 0;
        for (auto& g : gaps) if (interpolate (x, g.first, g.second, 40)) { ++mended; if (found) found->push_back (g); }
        return mended;
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
