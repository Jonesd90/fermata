#pragma once
#include "Dsp.h"
#include <juce_core/juce_core.h>
#include <cstdint>
#include <atomic>
#include <complex>
#include <functional>
#include <limits>
#include <vector>

namespace td::denoise
{
/** Fermata's background denoiser: offline, one track at a time, with a room-tone profile learned from a part of the recording that holds
    nothing but the room (the user marks it). The design, and why (see docs/DEVELOPMENT-NOTES.md, "Denoiser"):

    - a long analysis window (85 ms: 11.7 Hz per bin at 96 kHz), so a sustained partial sits in one narrow bin far above the per-bin noise
      and a weak upper harmonic is not mistaken for noise - the usual cause of denoised strings and voices sounding dull;
    - the a priori SNR of every bin by the decision-directed method run FORWARDS and BACKWARDS over the whole file (the larger of the two is
      used): backwards sees a note coming, so its first frame is not softened; forwards follows a tail down;
    - the tail rule: in each band the SNR estimate - and the gain - may not fall faster than the room's own decay, measured per band from the
      file itself (or set by the user), so a reverberation tail is followed down through the noise instead of being cut when it crosses a
      threshold. There is no threshold anywhere: the gain is a soft curve (the MMSE log-spectral-amplitude estimator) of the estimated SNR;
    - signal presence: where nothing but noise is heard (a posteriori SNR averaged over a small and a wide neighbourhood is at 0 dB) the gain
      goes to the residual, so quiet stretches come out at exactly the amount of reduction asked for;
    - harmonic protection: a narrow, prominent spectral peak that persists over several frames keeps its own measured SNR;
    - the residual: the gain never falls below it (-12 dB by default): the room tone is reduced, not removed, which sounds natural;
    - the removed part is the time-domain difference (input - output), exact, for listening to what was taken out.

    The reference implementation (Python) and its bench are in the development notes; tests/core_test.cpp checks this port against it. */

struct Params
{
    double windowSeconds = 0.085;      // the analysis window (a power of two samples nearest this)
    double amountDb = 0.0;             // scales the noise profile: +3 removes more, -3 less
    double residualDb = -12.0;         // the gain floor: how much of the room tone is left
    double tailSeconds = 0.0;          // 0 = measure the room's decay per band from the file; else an RT60-like time for every band
    double tailMarginDbPerSecond = 6.0;// the estimate may fall this much faster (dB/s) than the room decays
    double ddAlpha = 0.85;             // decision-directed smoothing per hop
    int    freqSmoothBins = 2;         // +- bins of SNR smoothing
    int    timeSmoothFrames = 1;       // +- frames of final gain smoothing
    bool   harmonicProtect = true;
    int    peakFrames = 3;             // a peak must persist this many frames ...
    double peakSnr = 3.0;              // ... at this a posteriori SNR (linear) or more ...
    double peakProminenceDb = 8.0;     // ... standing this far above the bins 4-7 away
    double hfProtectHz = 0.0;          // > 0: above this frequency the attenuation is limited to hfProtectDb
    double hfProtectDb = 12.0;
    bool   presence = true;            // signal-presence weighting
    double presLocalLo = 1.5, presLocalHi = 6.0, presGlobalLo = 0.8, presGlobalHi = 3.0, presQMax = 0.98;
    bool   tailPrior = true;           // the SNR estimate itself follows the room's decay
    double tailSeedSnr = 2.0;          // ... starting only from an SNR of at least this (linear; 2 = 3 dB)
    int    groupMode = 1;              // the other tracks of the take: 0 = ignored, 1 = a cell they hear as music is kept here too, 2 = ... and a cell none of them hears is treated as noise

    int fftSize (double sampleRate) const
    {
        const double want = windowSeconds * sampleRate;
        int n = 1; while ((double) n * 1.41421356 < want) n *= 2;
        return juce::jlimit (512, 65536, n);
    }
};

/** The noise profile of one channel: the mean power per bin of the room tone, in the STFT's own scale. */
struct Profile
{
    int fftSize = 0; double sampleRate = 0.0;
    std::vector<double> psd;                               // fftSize / 2 + 1 bins
    bool valid() const { return fftSize > 0 && ! psd.empty(); }
    /** The room tone's level (dB FS, RMS) as learned. */
    double levelDb() const
    {
        if (! valid()) return -200.0;
        double s = 0.0; for (size_t k = 0; k < psd.size(); ++k) s += psd[k] * (k == 0 || k + 1 == psd.size() ? 1.0 : 2.0);
        // |X|^2 summed over bins of a Hann-windowed frame of N samples: mean square of the frame ~ s / (N * sum(w^2)) with sum(w^2) = 3N/8
        const double ms = s / ((double) fftSize * 0.375 * (double) fftSize);
        return 10.0 * std::log10 (juce::jmax (ms, 1e-30));
    }
};

/** The room's decay per 1/3-octave band (dB/s), as measured from a file; NaN = not seen in that band. */
struct Decay { std::vector<double> edges, rates; };

namespace detail
{
inline double dbOf (double p) { return 10.0 * std::log10 (juce::jmax (p, 1e-30)); }

/** E1(x), the exponential integral, x > 0 (series below 1, continued fraction above). */
inline double expint1 (double x)
{
    if (x <= 0.0) return 1e30;
    if (x < 1.0)
    {
        double sum = 0.0, term = 1.0;
        for (int k = 1; k < 80; ++k) { term *= -x / (double) k; const double t = -term / (double) k; sum += t; if (std::abs (t) < 1e-17 * std::abs (sum)) break; }
        return -0.5772156649015329 - std::log (x) + sum;
    }
    const double fpmin = 1e-300;
    double b = x + 1.0, c = 1.0 / fpmin, d = 1.0 / b, h = d;
    for (int i = 1; i < 200; ++i)
    {
        const double a = -(double) i * (double) i;
        b += 2.0; d = 1.0 / (a * d + b); c = b + a / c;
        const double del = c * d; h *= del;
        if (std::abs (del - 1.0) < 1e-16) break;
    }
    return h * std::exp (-x);
}

inline double smoothstep (double a, double lo, double hi) { const double u = juce::jlimit (0.0, 1.0, (a - lo) / (hi - lo)); return u * u * (3.0 - 2.0 * u); }

/** A spectrogram: bins x frames, frame-major is NOT used - bin-major rows (one row per bin, frames along it) make the per-bin recursions cheap. */
struct Grid
{
    int bins = 0, frames = 0; std::vector<float> v;
    Grid() = default; Grid (int b, int t) : bins (b), frames (t), v ((size_t) b * (size_t) t, 0.0f) {}
    float* row (int b) { return v.data() + (size_t) b * (size_t) frames; }
    const float* row (int b) const { return v.data() + (size_t) b * (size_t) frames; }
    float& at (int b, int t) { return v[(size_t) b * (size_t) frames + (size_t) t]; }
    float at (int b, int t) const { return v[(size_t) b * (size_t) frames + (size_t) t]; }
};

/** Triangular smoothing over +-k along frames (each bin's row), edges renormalised - as the reference's _smooth_frames. */
inline void smoothFrames (const Grid& in, Grid& out, int k)
{
    if (k <= 0) { out = in; return; }
    out = Grid (in.bins, in.frames);
    std::vector<double> w ((size_t) (2 * k + 1)); double ws = 0.0;
    for (int i = 0; i <= 2 * k; ++i) { w[(size_t) i] = (double) (i <= k ? i + 1 : 2 * k + 1 - i); ws += w[(size_t) i]; }
    for (auto& x : w) x /= ws;
    std::vector<double> norm ((size_t) in.frames, 0.0);
    for (int i = 0; i <= 2 * k; ++i) { const int s = i - k; for (int t = 0; t < in.frames; ++t) { const int u = t - s; if (u >= 0 && u < in.frames) norm[(size_t) t] += w[(size_t) i]; } }
    for (int b = 0; b < in.bins; ++b)
    {
        const float* r = in.row (b); float* o = out.row (b);
        for (int t = 0; t < in.frames; ++t)
        {
            double acc = 0.0;
            for (int i = 0; i <= 2 * k; ++i) { const int u = t - (i - k); if (u >= 0 && u < in.frames) acc += w[(size_t) i] * r[u]; }
            o[t] = (float) (acc / norm[(size_t) t]);
        }
    }
}

/** The same across bins (each frame's column). */
inline void smoothBins (const Grid& in, Grid& out, int k)
{
    if (k <= 0) { out = in; return; }
    out = Grid (in.bins, in.frames);
    std::vector<double> w ((size_t) (2 * k + 1)); double ws = 0.0;
    for (int i = 0; i <= 2 * k; ++i) { w[(size_t) i] = (double) (i <= k ? i + 1 : 2 * k + 1 - i); ws += w[(size_t) i]; }
    for (auto& x : w) x /= ws;
    std::vector<double> norm ((size_t) in.bins, 0.0);
    for (int i = 0; i <= 2 * k; ++i) { const int s = i - k; for (int b = 0; b < in.bins; ++b) { const int u = b - s; if (u >= 0 && u < in.bins) norm[(size_t) b] += w[(size_t) i]; } }
    std::vector<double> acc ((size_t) in.frames);
    for (int b = 0; b < in.bins; ++b)
    {
        std::fill (acc.begin(), acc.end(), 0.0);
        for (int i = 0; i <= 2 * k; ++i)
        {
            const int u = b - (i - k); if (u < 0 || u >= in.bins) continue;
            const float* r = in.row (u); const double wi = w[(size_t) i];
            for (int t = 0; t < in.frames; ++t) acc[(size_t) t] += wi * r[t];
        }
        float* o = out.row (b); const double n = norm[(size_t) b];
        for (int t = 0; t < in.frames; ++t) o[t] = (float) (acc[(size_t) t] / n);
    }
}

/** The denoiser's FFT (pocketfft, in DenoiseFft.cpp): n real samples -> n/2+1 bins, unnormalised; and back, scaled by 1/n. */
void realForward (const double* in, int n, std::complex<double>* out);
void realInverse (const std::complex<double>* in, int n, double* out);

/** The STFT of the reference: periodic Hann, hop N/4, N zeros before the signal and 2N after, whole frames only. */
struct Stft
{
    int n = 0, hop = 0, pad = 0; std::vector<double> win;
    explicit Stft (int size) : n (size), hop (size / 4), pad (size), win (dsp::hann (size)) {}
    int framesFor (juce::int64 len) const { return (int) (1 + (len + 3 * (juce::int64) n - n) / hop); }

    /** X[bin][frame] (complex) of x (len samples). */
    void forward (const float* x, juce::int64 len, std::vector<std::complex<float>>& X, int& frames) const
    {
        frames = framesFor (len);
        const int bins = n / 2 + 1;
        X.assign ((size_t) bins * (size_t) frames, std::complex<float> (0.0f, 0.0f));
        std::vector<double> a ((size_t) n); std::vector<std::complex<double>> spec ((size_t) bins);
        for (int t = 0; t < frames; ++t)
        {
            const juce::int64 s0 = (juce::int64) t * hop - pad;                     // the real index of the frame's first sample
            for (int i = 0; i < n; ++i) { const juce::int64 j = s0 + i; a[(size_t) i] = j >= 0 && j < len ? (double) x[j] * win[(size_t) i] : 0.0; }
            realForward (a.data(), n, spec.data());
            for (int k = 0; k < bins; ++k) X[(size_t) k * (size_t) frames + (size_t) t] = std::complex<float> ((float) spec[(size_t) k].real(), (float) spec[(size_t) k].imag());
        }
    }

    /** Overlap-add of the frames back into len samples (window-sum-square normalised). */
    void inverse (const std::vector<std::complex<float>>& X, int frames, juce::int64 len, float* out) const
    {
        const int bins = n / 2 + 1;
        const juce::int64 total = (juce::int64) n + (juce::int64) hop * (frames - 1);
        std::vector<double> y ((size_t) total, 0.0), wss ((size_t) total, 0.0);
        std::vector<std::complex<double>> spec ((size_t) bins); std::vector<double> a ((size_t) n);
        for (int t = 0; t < frames; ++t)
        {
            for (int k = 0; k < bins; ++k) spec[(size_t) k] = std::complex<double> (X[(size_t) k * (size_t) frames + (size_t) t]);
            realInverse (spec.data(), n, a.data());
            const juce::int64 s0 = (juce::int64) t * hop;
            for (int i = 0; i < n; ++i) { const double w = win[(size_t) i]; y[(size_t) (s0 + i)] += a[(size_t) i] * w; wss[(size_t) (s0 + i)] += w * w; }
        }
        for (juce::int64 i = 0; i < len; ++i) { const auto j = (size_t) (i + pad); out[i] = (float) (y[j] / juce::jmax (wss[j], 1e-6)); }
    }
};

/** Pass 1: the band energies per frame that the decay measurement needs (from P = |X|^2). */
inline std::vector<double> thirdOctaveEdges (double sr)
{
    std::vector<double> e;
    for (int k = 0; k < 28; ++k) { const double f = 50.0 * std::pow (2.0, (double) k / 3.0); if (f < sr * 0.49) e.push_back (f); }
    return e;
}

/** The room's decay rate per band from the band energies E[band][frame] (dB): the median slope of the monotone decays that last >= 0.4 s
    and start >= 15 dB above the band's noise; NaN where fewer than two were seen. As measure_decay in the reference. */
inline std::vector<double> decayRates (const std::vector<std::vector<double>>& E, const std::vector<double>& bandNoiseDb, double dt)
{
    std::vector<double> rates (E.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t b = 0; b < E.size(); ++b)
    {
        const auto& e = E[b]; const int T = (int) e.size(); if (T < 3) continue;
        std::vector<double> Es ((size_t) T);
        for (int t = 0; t < T; ++t) Es[(size_t) t] = ((t > 0 ? e[(size_t) t - 1] : 0.0) + e[(size_t) t] + (t + 1 < T ? e[(size_t) t + 1] : 0.0)) / 3.0;
        const double Nf = bandNoiseDb[b];
        std::vector<double> slopes;
        for (int t = 1; t < T - 1;)
        {
            if (Es[(size_t) t] >= Es[(size_t) t - 1] && Es[(size_t) t] >= Es[(size_t) t + 1] && Es[(size_t) t] > Nf + 15.0)
            {
                double runMin = Es[(size_t) t]; int u = t + 1;
                while (u < T && Es[(size_t) u] <= runMin + 1.5 && Es[(size_t) u] > Nf + 6.0) { runMin = juce::jmin (runMin, Es[(size_t) u]); ++u; }
                if ((double) (u - t) * dt >= 0.4)
                {
                    // least-squares slope of Es[t..u) against time
                    const int m = u - t; double sx = 0, sy = 0, sxx = 0, sxy = 0;
                    for (int i = 0; i < m; ++i) { const double x = i * dt, yv = Es[(size_t) (t + i)]; sx += x; sy += yv; sxx += x * x; sxy += x * yv; }
                    const double slope = -((double) m * sxy - sx * sy) / juce::jmax (1e-12, (double) m * sxx - sx * sx);
                    if (slope > 1.0) slopes.push_back (slope);
                }
                t = juce::jmax (u, t + 1);
            }
            else ++t;
        }
        if (slopes.size() >= 2) { std::sort (slopes.begin(), slopes.end()); const size_t k = slopes.size(); rates[b] = k % 2 ? slopes[k / 2] : 0.5 * (slopes[k / 2 - 1] + slopes[k / 2]); }
    }
    return rates;
}

/** Rates per bin (dB/s): unmeasured bands from the nearest measured, a 3-band median, kept within 6..60 dB/s. As band_rate_per_bin. */
inline std::vector<double> ratePerBin (const std::vector<double>& freqs, const std::vector<double>& edges, std::vector<double> rates, double defaultRate)
{
    std::vector<double> r (freqs.size(), defaultRate);
    std::vector<size_t> good; for (size_t b = 0; b < rates.size(); ++b) if (! std::isnan (rates[b])) good.push_back (b);
    std::vector<double> filled = rates;
    if (good.empty()) std::fill (filled.begin(), filled.end(), defaultRate);
    else
        for (size_t b = 0; b < rates.size(); ++b)
            if (std::isnan (filled[b]))
            {
                size_t best = good[0]; for (auto g : good) if (std::abs ((int) g - (int) b) < std::abs ((int) best - (int) b)) best = g;
                filled[b] = rates[best];
            }
    std::vector<double> sm = filled;
    for (size_t b = 0; b < filled.size(); ++b)
    {
        std::vector<double> m; for (size_t j = (b > 0 ? b - 1 : 0); j < juce::jmin (filled.size(), b + 2); ++j) m.push_back (filled[j]);
        std::sort (m.begin(), m.end()); sm[b] = m.size() == 3 ? m[1] : m.size() == 2 ? 0.5 * (m[0] + m[1]) : m[0];
    }
    for (auto& v : sm) v = juce::jlimit (6.0, 60.0, v);
    for (size_t b = 0; b + 1 < edges.size(); ++b)
        for (size_t k = 0; k < freqs.size(); ++k) if (freqs[k] >= edges[b] && freqs[k] < edges[b + 1]) r[k] = sm[b];
    for (size_t k = 0; k < freqs.size(); ++k) { if (freqs[k] < edges.front()) r[k] = sm.front(); if (freqs[k] >= edges.back()) r[k] = sm.back(); }
    return r;
}
} // namespace detail

/** Learns the profile of one channel from one or more room-tone regions (each at least ~0.5 s). */
inline Profile learnProfile (const std::vector<std::vector<float>>& regions, double sampleRate, const Params& p)
{
    Profile prof; prof.fftSize = p.fftSize (sampleRate); prof.sampleRate = sampleRate;
    detail::Stft st (prof.fftSize);
    const int bins = prof.fftSize / 2 + 1;
    std::vector<double> acc ((size_t) bins, 0.0); juce::int64 count = 0;
    std::vector<std::complex<float>> X; int frames = 0;
    for (auto& r : regions)
    {
        if (r.empty()) continue;
        st.forward (r.data(), (juce::int64) r.size(), X, frames);
        const int edge = st.n / st.hop;
        const int t0 = frames > 2 * edge + 2 ? edge : 0, t1 = frames > 2 * edge + 2 ? frames - edge : frames;
        for (int k = 0; k < bins; ++k)
        {
            double s = 0.0; const auto* row = X.data() + (size_t) k * (size_t) frames;
            for (int t = t0; t < t1; ++t) s += (double) std::norm (row[t]);
            acc[(size_t) k] += s;
        }
        count += t1 - t0;
    }
    if (count == 0) return {};
    prof.psd.assign ((size_t) bins, 0.0);
    for (int k = 0; k < bins; ++k) acc[(size_t) k] /= (double) count;
    for (int k = 0; k < bins; ++k)
    {
        const double v = k == 0 ? acc[0] : k == bins - 1 ? acc[(size_t) k] : 0.25 * acc[(size_t) k - 1] + 0.5 * acc[(size_t) k] + 0.25 * acc[(size_t) k + 1];
        prof.psd[(size_t) k] = juce::jmax (v, 1e-20);
    }
    return prof;
}

/** Pass 1 over a whole channel: measures the room's decay per 1/3-octave band. 'read' fills n samples from 'start' (zeros beyond the end). */
inline Decay measureDecay (const Profile& prof, const Params& p, juce::int64 length,
                           const std::function<void (juce::int64 start, int n, float* dst)>& read,
                           const std::function<bool (double progress)>& keepGoing = {})
{
    Decay d; const double sr = prof.sampleRate;
    detail::Stft st (prof.fftSize);
    const int bins = prof.fftSize / 2 + 1;
    d.edges = detail::thirdOctaveEdges (sr);
    const size_t nb = d.edges.size() - 1;
    std::vector<int> bandOf ((size_t) bins, -1);
    for (int k = 0; k < bins; ++k) { const double f = (double) k * sr / prof.fftSize; for (size_t b = 0; b < nb; ++b) if (f >= d.edges[b] && f < d.edges[b + 1]) bandOf[(size_t) k] = (int) b; }
    std::vector<double> bandNoise (nb, 0.0); for (int k = 0; k < bins; ++k) if (bandOf[(size_t) k] >= 0) bandNoise[(size_t) bandOf[(size_t) k]] += prof.psd[(size_t) k];
    std::vector<double> bandNoiseDb (nb); for (size_t b = 0; b < nb; ++b) bandNoiseDb[b] = detail::dbOf (bandNoise[b]);
    // the frames of the whole channel, in pieces (the framing is the reference's: zeros before and after)
    const int frames = st.framesFor (length);
    std::vector<std::vector<double>> E (nb, std::vector<double> ((size_t) frames, 0.0));
    const int piece = 512;                                                           // frames per piece
    const int lead = st.n / st.hop;                                                  // (4) local frames that cover the local zero padding
    std::vector<float> local; std::vector<std::complex<float>> X;
    for (int f0 = 0; f0 < frames; f0 += piece)
    {
        const int f1 = juce::jmin (frames, f0 + piece);
        // global frame f covers the real samples [f*hop - n, f*hop). A local STFT of L = real[g0 ..) with g0 = f0*hop - n has its frame t+lead
        // covering exactly global frame f0+t (its own padding of n zeros lands before g0, where the global frames f0.. never look).
        const juce::int64 g0 = (juce::int64) f0 * st.hop - st.n, llen = (juce::int64) (f1 - f0 + lead) * st.hop;
        local.assign ((size_t) llen, 0.0f);
        const juce::int64 a = juce::jmax<juce::int64> (0, g0), b = juce::jmin<juce::int64> (length, g0 + llen);
        if (b > a) read (a, (int) (b - a), local.data() + (a - g0));
        int lf = 0; st.forward (local.data(), llen, X, lf);
        for (int t = 0; t < f1 - f0 && t + lead < lf; ++t)
            for (int k = 0; k < bins; ++k)
            {
                const int bd = bandOf[(size_t) k]; if (bd < 0) continue;
                E[(size_t) bd][(size_t) (f0 + t)] += (double) std::norm (X[(size_t) k * (size_t) lf + (size_t) (t + lead)]);
            }
        if (keepGoing && ! keepGoing ((double) f1 / (double) frames)) { d.rates.assign (nb, std::numeric_limits<double>::quiet_NaN()); return d; }
    }
    for (auto& e : E) for (auto& v : e) v = detail::dbOf (v);
    d.rates = detail::decayRates (E, bandNoiseDb, (double) st.hop / sr);
    return d;
}

/** What the analysis of one stretch leaves behind, up to the signal-presence map: the gain is made from it by finish(). Big (several
    bins x frames grids): one per stretch per thread. */
struct Analysis
{
    int bins = 0, T = 0; juce::int64 len = 0;
    std::vector<std::complex<float>> X;                    // the STFT (bin-major)
    detail::Grid gamma, xi, prior, pres;                   // a posteriori SNR, a priori SNR (smoothed, protected), the tail prior, presence 0..1
    std::vector<unsigned char> prot;                       // harmonic protection (bins x T)
    std::vector<float> stepDb;                             // the tail rule on the gain: dB a bin's gain may fall per hop
    std::vector<double> freqs;
};

/** The analysis of one stretch of a channel (len samples): everything up to the presence map, which is what the other tracks of a take can use.
    'decay' from measureDecay (or empty: measured from x itself). */
inline void analyse (const Profile& prof, const Params& p, const Decay& decay, const float* x, juce::int64 len, Analysis& A)
{
    using namespace detail;
    const double sr = prof.sampleRate;
    Stft st (prof.fftSize);
    const int bins = prof.fftSize / 2 + 1;
    int T = 0;
    st.forward (x, len, A.X, T);
    A.bins = bins; A.T = T; A.len = len;
    const double dt = (double) st.hop / sr;
    A.freqs.assign ((size_t) bins, 0.0); for (int k = 0; k < bins; ++k) A.freqs[(size_t) k] = (double) k * sr / prof.fftSize;
    const auto& freqs = A.freqs;

    Grid P (bins, T); A.gamma = Grid (bins, T);
    const double amount = std::pow (10.0, p.amountDb / 10.0);
    for (int k = 0; k < bins; ++k)
    {
        const auto* xr = A.X.data() + (size_t) k * (size_t) T; float* pr = P.row (k); float* gr = A.gamma.row (k);
        const float N = (float) (prof.psd[(size_t) k] * amount);
        for (int t = 0; t < T; ++t) { pr[t] = std::norm (xr[t]); gr[t] = pr[t] / N; }
    }
    Grid& gamma = A.gamma;

    // the room's decay per band -> the fall allowed per hop (power ratio), per bin
    Decay dec = decay;
    if (dec.rates.empty())
    {
        dec.edges = thirdOctaveEdges (sr);
        const size_t nb = dec.edges.size() - 1;
        std::vector<std::vector<double>> E (nb, std::vector<double> ((size_t) T, 0.0)); std::vector<double> bn (nb, 0.0);
        for (int k = 0; k < bins; ++k)
        {
            int b = -1; for (size_t j = 0; j < nb; ++j) if (freqs[(size_t) k] >= dec.edges[j] && freqs[(size_t) k] < dec.edges[j + 1]) b = (int) j;
            if (b < 0) continue;
            bn[(size_t) b] += prof.psd[(size_t) k]; const float* pr = P.row (k);
            for (int t = 0; t < T; ++t) E[(size_t) b][(size_t) t] += pr[t];
        }
        for (auto& e : E) for (auto& v : e) v = dbOf (v);
        for (auto& v : bn) v = dbOf (v);
        dec.rates = decayRates (E, bn, dt);
    }
    if (p.tailSeconds > 0.0) for (auto& r : dec.rates) r = 60.0 / p.tailSeconds;
    auto rateBin = ratePerBin (freqs, dec.edges, dec.rates, 30.0);
    std::vector<float> fall ((size_t) bins); A.stepDb.assign ((size_t) bins, 0.0f);
    for (int k = 0; k < bins; ++k) { const double r = rateBin[(size_t) k] + p.tailMarginDbPerSecond; fall[(size_t) k] = (float) std::pow (10.0, -r * dt / 10.0); A.stepDb[(size_t) k] = (float) (r * dt); }

    // decision-directed a priori SNR, forwards (with the tail prior) and backwards
    auto dd = [&] (Grid& xi, bool backwards)
    {
        xi = Grid (bins, T);
        const float a = (float) p.ddAlpha;
        for (int k = 0; k < bins; ++k)
        {
            const float* g = gamma.row (k); float* o = xi.row (k);
            float prevClean = 0.0f; bool first = true;
            for (int i = 0; i < T; ++i)
            {
                const int t = backwards ? T - 1 - i : i;
                const float inst = juce::jmax (g[t] - 1.0f, 0.0f);
                const float v = first ? inst : a * prevClean + (1.0f - a) * inst;
                first = false; o[t] = v;
                const float gw = v / (1.0f + v); prevClean = gw * gw * g[t];
            }
        }
    };
    Grid xiF, xiB, tmp;
    dd (xiF, false);
    A.prior = Grid (bins, T);
    if (p.tailPrior)
    {
        Grid seed; smoothFrames (xiF, tmp, 2); smoothBins (tmp, seed, 2);
        const float thr = (float) p.tailSeedSnr;
        for (int k = 0; k < bins; ++k)
        {
            const float* sd = seed.row (k); float* pr = A.prior.row (k); float* f = xiF.row (k); const float fk = fall[(size_t) k];
            float prev = 0.0f; pr[0] = 0.0f;                                                 // (as the reference: the prior starts at the second frame)
            for (int t = 1; t < T; ++t) { const float sv = sd[t] >= thr ? sd[t] : 0.0f; const float v = juce::jmax (sv, prev * fk); pr[t] = v; prev = v; f[t] = juce::jmax (f[t], v); }
        }
    }
    dd (xiB, true);
    A.xi = Grid (bins, T);
    for (size_t i = 0; i < A.xi.v.size(); ++i) A.xi.v[i] = juce::jmax (xiF.v[i], xiB.v[i]);
    xiB = Grid(); xiF = Grid();
    Grid& xi = A.xi;
    if (p.freqSmoothBins > 0)
    {
        for (auto& v : xi.v) v = (float) dbOf ((double) v + 1e-6);
        smoothBins (xi, tmp, p.freqSmoothBins);
        for (size_t i = 0; i < xi.v.size(); ++i) xi.v[i] = (float) std::pow (10.0, (double) tmp.v[i] / 10.0);
    }
    // harmonic protection: narrow, prominent, persistent peaks keep their measured SNR
    A.prot.clear();
    if (p.harmonicProtect)
    {
        Grid Ps; smoothFrames (P, Ps, 1);
        std::vector<unsigned char> peak ((size_t) bins * (size_t) T, 0);
        const float promin = (float) std::pow (10.0, p.peakProminenceDb / 10.0), snr = (float) p.peakSnr;
        auto at = [&] (int k, int t) { return Ps.at (k, t); };                            // (only bins 8 .. bins-9 are looked at: no wrap-round needed)
        for (int k = 8; k < bins - 8; ++k)
            for (int t = 0; t < T; ++t)
            {
                const float c = at (k, t);
                float around = 0.0f; for (int d = 4; d <= 7; ++d) around += at (k + d, t) + at (k - d, t);
                around /= 8.0f;
                if (c > at (k - 1, t) && c >= at (k + 1, t) && c > at (k - 2, t) && c >= at (k + 2, t) && c > around * promin && gamma.at (k, t) > snr)
                    peak[(size_t) k * (size_t) T + (size_t) t] = 1;
            }
        std::vector<unsigned char> persist = peak;
        for (int d = 1; d < p.peakFrames; ++d)
            for (int k = 0; k < bins; ++k) for (int t = T - 1; t >= d; --t) persist[(size_t) k * (size_t) T + (size_t) t] &= peak[(size_t) k * (size_t) T + (size_t) (t - d)];
        for (int d = 1; d < p.peakFrames; ++d)
            for (int k = 0; k < bins; ++k) for (int t = 0; t + d < T; ++t) persist[(size_t) k * (size_t) T + (size_t) t] |= persist[(size_t) k * (size_t) T + (size_t) (t + d)];
        A.prot.assign (persist.size(), 0);
        for (int k = 0; k < bins; ++k)
            for (int t = 0; t < T; ++t)
            {
                const auto i = (size_t) k * (size_t) T + (size_t) t;
                const int km = k > 0 ? k - 1 : bins - 1, kp = k + 1 < bins ? k + 1 : 0;              // (as the reference's roll; the end bins are never peaks)
                A.prot[i] = persist[i] | persist[(size_t) km * (size_t) T + (size_t) t] | persist[(size_t) kp * (size_t) T + (size_t) t];
            }
        for (size_t i = 0; i < A.prot.size(); ++i) if (A.prot[i]) xi.v[i] = juce::jmax (xi.v[i], juce::jmax (gamma.v[i] - 1.0f, 0.0f));
    }
    // signal presence: how likely it is that this bin holds anything but noise (OM-LSA style). 1 everywhere when presence weighting is off.
    A.pres = Grid (bins, T);
    if (p.presence)
    {
        Grid gdb (bins, T);
        for (size_t i = 0; i < gdb.v.size(); ++i) gdb.v[i] = (float) dbOf (p.tailPrior ? juce::jmax ((double) gamma.v[i], 1.0 + (double) A.prior.v[i]) : (double) gamma.v[i]);
        Grid sm, loc, glo; smoothFrames (gdb, sm, 2); smoothBins (sm, loc, 2); smoothBins (sm, glo, 16);
        for (size_t i = 0; i < A.pres.v.size(); ++i)
        {
            const double pri = smoothstep (loc.v[i], p.presLocalLo, p.presLocalHi) * smoothstep (glo.v[i], p.presGlobalLo, p.presGlobalHi);
            const double q = juce::jlimit (0.0, p.presQMax, 1.0 - pri);
            const double xv = xi.v[i], gv = gamma.v[i];
            const double v = juce::jlimit (0.0, 500.0, xv / (1.0 + xv) * gv);
            double pres = 1.0 / (1.0 + q / (1.0 - q) * (1.0 + xv) * std::exp (-v));
            if (! A.prot.empty() && A.prot[i]) pres = 1.0;
            A.pres.v[i] = (float) pres;
        }
    }
    else std::fill (A.pres.v.begin(), A.pres.v.end(), 1.0f);
}

/** The gain from an analysis, and the output. 'others' (optional, bins x T, 0..255): what the other tracks of the take hear in each cell - with
    Params::groupMode 1 a cell they hear as music is kept here too (presence raised to 0.85 of theirs at most); with 2 a cell none of them hears is
    pushed towards the residual as well. */
inline void finish (const Profile& prof, const Params& p, Analysis& A, const unsigned char* others, float* out)
{
    using namespace detail;
    const int bins = A.bins, T = A.T;
    Grid& xi = A.xi; Grid& gamma = A.gamma;
    Grid G (bins, T);
    for (size_t i = 0; i < G.v.size(); ++i)
    {
        const double xv = xi.v[i], gv = gamma.v[i];
        const double v = juce::jlimit (1e-8, 500.0, xv / (1.0 + xv) * gv);
        G.v[i] = (float) juce::jmin (1.0, xv / (1.0 + xv) * std::exp (0.5 * expint1 (v)));
    }
    const double gmin = std::pow (10.0, p.residualDb / 20.0);
    if (p.presence)
    {
        const double logGmin = std::log (gmin);
        for (size_t i = 0; i < G.v.size(); ++i)
        {
            double pres = A.pres.v[i];
            if (others != nullptr && p.groupMode > 0)
            {
                const double e = others[i] / 255.0;
                pres = juce::jmax (pres, 0.85 * e);
                if (p.groupMode >= 2 && (A.prot.empty() || ! A.prot[i])) pres = juce::jmin (pres, 0.25 + 0.75 * e);
            }
            G.v[i] = (float) std::exp (pres * std::log (juce::jmax ((double) G.v[i], 1e-6)) + (1.0 - pres) * logGmin);
        }
    }
    for (auto& g : G.v) g = juce::jmax (g, (float) gmin);
    // the tail rule on the gain too, the high-frequency ceiling, the final smoothing
    for (int k = 0; k < bins; ++k)
    {
        float* g = G.row (k); const float step = A.stepDb[(size_t) k];
        float prev = 20.0f * std::log10 (g[0]); g[0] = prev;
        for (int t = 1; t < T; ++t) { const float cur = 20.0f * std::log10 (g[t]); const float v = juce::jmax (cur, prev - step); g[t] = v; prev = v; }
        if (p.hfProtectHz > 0.0 && A.freqs[(size_t) k] >= p.hfProtectHz) for (int t = 0; t < T; ++t) g[t] = juce::jmax (g[t], (float) -p.hfProtectDb);
        for (int t = 0; t < T; ++t) g[t] = std::pow (10.0f, g[t] / 20.0f);
    }
    if (p.timeSmoothFrames > 0) { Grid tmp; smoothFrames (G, tmp, p.timeSmoothFrames); G = tmp; }
    for (size_t i = 0; i < G.v.size(); ++i) { const float g = juce::jlimit ((float) gmin, 1.0f, G.v[i]); A.X[i] *= g; }
    Stft st (prof.fftSize);
    st.inverse (A.X, T, A.len, out);
}

/** Processes one stretch of a channel in memory on its own: x (len samples) -> out (len samples). This is the single-track algorithm (what the
    reference implements); processChannel feeds it pieces of a long file with enough context on each side. */
inline void processSegment (const Profile& prof, const Params& p, const Decay& decay, const float* x, juce::int64 len, float* out)
{
    Analysis A; analyse (prof, p, decay, x, len, A); finish (prof, p, A, nullptr, out);
}

/** The presence map of one stretch (bins x frames, 0..255), for the other tracks of a take; the framing is that of analyse(). */
inline void presenceSegment (const Profile& prof, const Params& p, const Decay& decay, const float* x, juce::int64 len, std::vector<unsigned char>& pres, int& bins, int& frames)
{
    Analysis A; analyse (prof, p, decay, x, len, A);
    bins = A.bins; frames = A.T;
    pres.assign (A.pres.v.size(), 0);
    for (size_t i = 0; i < pres.size(); ++i) pres[i] = (unsigned char) juce::jlimit (0, 255, (int) std::lround (A.pres.v[i] * 255.0f));
}

/** Spreads a presence map in place: each cell takes the largest value within +-dFrames and +-dBins (the delay between mics, and a vibrato's reach). */
inline void dilate (std::vector<unsigned char>& m, int bins, int frames, int dFrames, int dBins)
{
    std::vector<unsigned char> t (m.size());
    for (int k = 0; k < bins; ++k)                              // along frames
    {
        const unsigned char* r = m.data() + (size_t) k * (size_t) frames; unsigned char* o = t.data() + (size_t) k * (size_t) frames;
        for (int f = 0; f < frames; ++f) { unsigned char v = 0; for (int d = -dFrames; d <= dFrames; ++d) { const int u = f + d; if (u >= 0 && u < frames) v = juce::jmax (v, r[u]); } o[f] = v; }
    }
    for (int k = 0; k < bins; ++k)                              // along bins
    {
        unsigned char* o = m.data() + (size_t) k * (size_t) frames;
        for (int f = 0; f < frames; ++f) { unsigned char v = 0; for (int d = -dBins; d <= dBins; ++d) { const int u = k + d; if (u >= 0 && u < bins) v = juce::jmax (v, t[(size_t) u * (size_t) frames + (size_t) f]); } o[f] = v; }
    }
}

/** For N maps of the same size, each one's "others" map: the largest value any OTHER map has in the cell (the two largest per cell are found once). */
inline void othersMax (const std::vector<std::vector<unsigned char>>& maps, std::vector<std::vector<unsigned char>>& others)
{
    const size_t n = maps.size();
    others.assign (n, {});
    if (n < 2) { for (size_t i = 0; i < n; ++i) others[i].assign (maps[i].size(), 0); return; }
    const size_t cells = maps[0].size();
    for (size_t i = 0; i < n; ++i) others[i].assign (cells, 0);
    for (size_t c = 0; c < cells; ++c)
    {
        unsigned char m1 = 0, m2 = 0; size_t i1 = 0;
        for (size_t i = 0; i < n; ++i) { const unsigned char v = maps[i][c]; if (v > m1) { m2 = m1; m1 = v; i1 = i; } else if (v > m2) m2 = v; }
        for (size_t i = 0; i < n; ++i) others[i][c] = i == i1 ? m2 : m1;
    }
}

/** The whole of one channel, in pieces with context, bounded memory. 'read' fills n samples from 'start' (zeros past the end);
    'write' receives the output in order. Returns false if cancelled. */
inline bool processChannel (const Profile& prof, const Params& p, const Decay& decay, juce::int64 length,
                            const std::function<void (juce::int64 start, int n, float* dst)>& read,
                            const std::function<void (const float* src, int n)>& write,
                            const std::function<bool (double progress)>& keepGoing = {},
                            double pieceSeconds = 15.0, double beforeSeconds = 6.0, double afterSeconds = 2.0)
{
    const double sr = prof.sampleRate;
    const juce::int64 piece = (juce::int64) (pieceSeconds * sr), before = (juce::int64) (beforeSeconds * sr), after = (juce::int64) (afterSeconds * sr);
    if (length <= piece + before + after)
    {
        std::vector<float> x ((size_t) length, 0.0f), y ((size_t) length, 0.0f);
        if (length > 0) read (0, (int) length, x.data());
        processSegment (prof, p, decay, x.data(), length, y.data());
        write (y.data(), (int) length);
        return ! (keepGoing && ! keepGoing (1.0));
    }
    std::vector<float> x, y;
    for (juce::int64 start = 0; start < length; start += piece)
    {
        const juce::int64 end = juce::jmin (length, start + piece);
        const juce::int64 s0 = juce::jmax<juce::int64> (0, start - before), s1 = juce::jmin (length, end + after);
        x.assign ((size_t) (s1 - s0), 0.0f); y.assign ((size_t) (s1 - s0), 0.0f);
        read (s0, (int) (s1 - s0), x.data());
        processSegment (prof, p, decay, x.data(), s1 - s0, y.data());
        write (y.data() + (start - s0), (int) (end - start));
        if (keepGoing && ! keepGoing ((double) end / (double) length)) return false;
    }
    return true;
}
} // namespace td::denoise
