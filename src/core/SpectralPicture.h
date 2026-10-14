#pragma once
/** The picture of the sound for the Spectral Repair window: a time-frequency "reassigned" spectrogram made for the part that is in view, at the size it is shown.
    A Gaussian window is used (very low side lobes), and the energy of every analysis point is moved to where the sound really is (the instantaneous time and
    frequency), which makes tones thin sharp lines and clicks thin vertical lines instead of blurs. Plain C++17 + JUCE core types only. */
#include "Dsp.h"
#include <atomic>
#include <functional>
#include <thread>
#include <juce_core/juce_core.h>

namespace td
{
/** How pitch is spread up the picture: a blend of the straight line (even Hz) and the logarithm (even octaves), or the Mel scale. */
struct PictureAxis
{
    double fLo = 40.0, fHi = 20000.0, warp = 1.0; bool mel = false;
    static double melOf (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); }
    static double hzOfMel (double m) { return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0); }
    double posOf (double hz) const
    {
        hz = juce::jlimit (fLo, fHi, hz);
        if (mel) return (melOf (hz) - melOf (fLo)) / juce::jmax (1.0e-9, melOf (fHi) - melOf (fLo));
        const double lin = (hz - fLo) / (fHi - fLo), lg = std::log (hz / fLo) / std::log (fHi / fLo);
        return (1.0 - warp) * lin + warp * lg;
    }
    double hzOf (double pos) const
    {
        pos = juce::jlimit (0.0, 1.0, pos);
        if (mel) return hzOfMel (melOf (fLo) + pos * (melOf (fHi) - melOf (fLo)));
        if (warp > 0.999) return fLo * std::pow (fHi / fLo, pos);
        if (warp < 0.001) return fLo + pos * (fHi - fLo);
        double lo = fLo, hi = fHi;
        for (int i = 0; i < 40; ++i) { const double m = 0.5 * (lo + hi); if (posOf (m) < pos) lo = m; else hi = m; }
        return 0.5 * (lo + hi);
    }
    bool same (const PictureAxis& o) const { return std::abs (fLo - o.fLo) < 1.0e-6 && std::abs (fHi - o.fHi) < 1.0e-6 && std::abs (warp - o.warp) < 1.0e-9 && mel == o.mel; }
};

struct PictureOptions
{
    bool   reassign = true;        // move the energy to where the sound really is (sharp lines)
    double detail = 0.5;           // 0 = finer pitch detail (a longer window), 1 = finer time detail (a shorter one); 0.5 = chosen from the zoom
    double reduceAboveSec = 240.0; // a view longer than this is drawn with fewer analysis points
};

/** A finished picture: the level in dB (0 = a full-scale tone) of every pixel, row 0 at the top. */
struct DetailPicture
{
    int w = 0, h = 0; double t0 = 0.0, t1 = 0.0; PictureAxis axis; juce::int64 id = 0;
    std::vector<float> db;
    bool valid() const { return w > 1 && h > 1 && db.size() == (size_t) w * (size_t) h; }
};

namespace picture
{
/** The picture of [t0,t1] seconds (from the first sample of the tracks) of every track, the loudest track at every point. Returns an invalid picture if cancelled. */
inline DetailPicture render (const std::vector<std::vector<float>>& tracks, double sr, double t0, double t1, int W, int H, const PictureAxis& axis, const PictureOptions& opt,
                             const std::function<bool()>& cancelled = {}, const std::function<void (float)>& progress = {})
{
    DetailPicture pic; pic.w = W; pic.h = H; pic.t0 = t0; pic.t1 = t1; pic.axis = axis;
    if (tracks.empty() || W < 2 || H < 2 || t1 <= t0 || sr < 8000.0) return pic;
    const double span = t1 - t0, secPerPx = span / (double) W;
    // the window: Gaussian, its width chosen from the zoom (about 6 pixels wide) unless the 'detail' control moves it
    const double fwhm = juce::jlimit (0.008, 0.040, secPerPx * 6.0) * std::pow (2.0, -(juce::jlimit (0.0, 1.0, opt.detail) - 0.5) * 2.0);
    const double sigma = juce::jmax (8.0, fwhm / 2.3548 * sr);                  // samples
    int N = 512; while ((double) N < 13.6 * sigma && N < 65536) N <<= 1;
    const double Nd = (double) N;
    const bool reduced = span > opt.reduceAboveSec;
    const double framesWanted = juce::jmin (6000.0, (reduced ? 1.0 : 1.5) * (double) W);
    const double hop = juce::jmax (1.0, span * sr / framesWanted);
    const long F = (long) std::ceil (span * sr / hop);
    const int kLo = juce::jmax (1, (int) std::floor (axis.fLo * Nd / sr) - 1), kHi = juce::jmin (N / 2 - 1, (int) std::ceil (axis.fHi * Nd / sr) + 1);
    if (kHi <= kLo) return pic;
    // the window
    std::vector<double> win ((size_t) N / 2 + 1); double sumH = 0.0;
    for (int t = 0; t <= N / 2; ++t) { win[(size_t) t] = std::exp (-0.5 * (double) t * (double) t / (sigma * sigma)); sumH += win[(size_t) t] * (t == 0 ? 1.0 : 2.0); }
    // where each pitch row sits, and how much a pixel is worth (so the level does not depend on the size of the picture)
    std::vector<double> rowHz ((size_t) H + 1);
    for (int r = 0; r <= H; ++r) rowHz[(size_t) r] = axis.hzOf (1.0 - ((double) r - 0.5) / (double) (H - 1));
    std::vector<float> rowScale ((size_t) H);
    const double cellArea = hop * (sr / Nd);                                    // samples x Hz of one analysis point
    const double pxSamples = span * sr / (double) W;
    const double norm2 = (2.0 / sumH) * (2.0 / sumH);
    for (int r = 0; r < H; ++r)
    {
        const double pxHz = juce::jmax (1.0e-3, std::abs (rowHz[(size_t) r] - rowHz[(size_t) r + 1]));
        rowScale[(size_t) r] = (float) (norm2 / juce::jmax (1.0e-9, pxSamples * pxHz / cellArea));
    }
    // row of each bin and how fast the row moves with the pitch (the reassigned pitch is within a bin of the bin's own)
    std::vector<float> rowOfBin ((size_t) N / 2 + 1, -1.0f), rowSlope ((size_t) N / 2 + 1, 0.0f);
    for (int k = kLo; k <= kHi; ++k)
    {
        const double hz = (double) k * sr / Nd, d = 0.25 * sr / Nd;
        const double p0 = axis.posOf (juce::jmax (axis.fLo, hz - d)), p1 = axis.posOf (juce::jmin (axis.fHi, hz + d));
        const double dh = juce::jmin (axis.fHi, hz + d) - juce::jmax (axis.fLo, hz - d);
        rowOfBin[(size_t) k] = (float) ((1.0 - axis.posOf (hz)) * (double) (H - 1));
        rowSlope[(size_t) k] = dh > 0.0 ? (float) (-(p1 - p0) / dh * (double) (H - 1)) : 0.0f;
    }
    const long startSample = (long) std::llround (t0 * sr);
    const long totalSamples = [&] { long n = 0; for (auto& t : tracks) n = juce::jmax (n, (long) t.size()); return n; }();
    const dsp::FFT fft (N);
    const size_t npx = (size_t) W * (size_t) H;
    std::atomic<int> nextTrack { 0 }, doneTracks { 0 }; std::atomic<bool> stop { false };
    const int nT = juce::jmax (1, juce::jmin ((int) tracks.size(), (int) juce::jmin (8u, juce::jmax (1u, std::thread::hardware_concurrency()))));
    std::vector<std::vector<float>> best ((size_t) nT);
    auto worker = [&] (int who)
    {
        auto& mx = best[(size_t) who]; mx.assign (npx, 0.0f);
        std::vector<float> acc (npx);
        std::vector<std::complex<double>> z ((size_t) N);
        for (;;)
        {
            const int ti = nextTrack.fetch_add (1);
            if (ti >= (int) tracks.size() || stop.load()) break;
            const auto& x = tracks[(size_t) ti]; const long L = (long) x.size();
            std::fill (acc.begin(), acc.end(), 0.0f);
            for (long m = 0; m < F; ++m)
            {
                if ((m & 15) == 0 && cancelled && cancelled()) { stop = true; break; }
                const long c = startSample + (long) std::llround ((double) m * hop + 0.5 * hop);
                if (c + N / 2 < 0 || c - N / 2 >= L) continue;
                double energy = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    const int tau = i < N / 2 ? i : i - N;
                    const long n = c + tau; const double s = (n >= 0 && n < L) ? (double) x[(size_t) n] : 0.0;
                    const double h = win[(size_t) std::abs (tau)];
                    z[(size_t) i] = std::complex<double> (s * h, s * h * (double) tau);
                    energy += s * s;
                }
                if (energy < 1.0e-18) continue;
                fft.forward (z);
                for (int k = kLo; k <= kHi; ++k)
                {
                    const auto zk = z[(size_t) k], zn = std::conj (z[(size_t) (N - k)]);
                    const auto A = 0.5 * (zk + zn);                                     // spectrum with the window
                    const auto B = (zk - zn) / std::complex<double> (0.0, 2.0);         // spectrum with the window times time
                    const double P = std::norm (A);
                    if (P < 1.0e-22) continue;
                    double tHat = (double) c, rowF = (double) rowOfBin[(size_t) k];
                    if (opt.reassign)
                    {
                        const auto q = B * std::conj (A) / P;
                        const double dt = q.real();                                      // samples
                        const double dw = q.imag() / (sigma * sigma);                   // rad / sample
                        if (std::abs (dt) > 0.5 * Nd) continue;
                        const double dHz = dw * sr / (2.0 * dsp::kPi);
                        if (std::abs (dHz) > 2.0 * sr / Nd) continue;
                        tHat += dt; rowF += (double) rowSlope[(size_t) k] * dHz;
                    }
                    const int col = (int) std::floor ((tHat / sr - t0) / span * (double) W);
                    const int row = (int) std::floor (rowF + 0.5);
                    if (col < 0 || col >= W || row < 0 || row >= H) continue;
                    acc[(size_t) row * (size_t) W + (size_t) col] += (float) P;
                }
            }
            if (stop.load()) break;
            for (int r = 0; r < H; ++r)
            {
                const float sc = rowScale[(size_t) r];
                float* a = acc.data() + (size_t) r * (size_t) W; float* d = mx.data() + (size_t) r * (size_t) W;
                for (int cx = 0; cx < W; ++cx) { const float v = a[cx] * sc; if (v > d[cx]) d[cx] = v; }
            }
            if (progress) progress ((float) (doneTracks.fetch_add (1) + 1) / (float) tracks.size());
        }
    };
    std::vector<std::thread> pool;
    for (int i = 1; i < nT; ++i) pool.emplace_back (worker, i);
    worker (0);
    for (auto& th : pool) th.join();
    if (stop.load()) return DetailPicture();
    pic.db.assign (npx, -200.0f);
    for (size_t i = 0; i < npx; ++i)
    {
        float v = 0.0f; for (auto& b : best) if (! b.empty() && b[i] > v) v = b[i];
        if (v > 1.0e-20f) pic.db[i] = 10.0f * std::log10 (v);
    }
    (void) totalSamples;
    return pic;
}
} // namespace picture
} // namespace td
