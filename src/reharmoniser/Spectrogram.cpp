#include "Spectrogram.h"
#include <algorithm>
#include <cmath>

namespace rehar
{
namespace
{
double powOfLevel (int v) { return std::pow (10.0, ((double) v / 2.0 - 127.5) / 10.0); }
const float* powTable()
{
    static float t[256]; static bool done = false;
    if (! done) { for (int i = 0; i < 256; ++i) t[i] = (float) powOfLevel (i); done = true; }
    return t;
}
}

void Spectrogram::build (const std::vector<Span>& chans, int sampleRate, Detail detail, const Hooks& hooks)
{
    if (chans.empty() || chans[0].n == 0) throw std::runtime_error ("There is no audio to analyse.");
    sr = sampleRate;
    const size_t nch = chans.size();
    std::vector<std::vector<float>> xs (nch);
    for (size_t c = 0; c < nch; ++c) sr2 = decimateTo (chans[c].p, chans[c].n, sr, xs[c]);
    gain.assign (nch, 1.0);
    const double dsec = detail == Detail::Pitch ? 0.34 : (detail == Detail::Balanced ? 0.17 : 0.085);
    const size_t NL = (size_t) std::pow (2.0, std::nearbyint (std::log2 (dsec * sr2)));
    const size_t NS = std::min ((size_t) std::pow (2.0, std::nearbyint (std::log2 (kShortS * sr2))), NL);
    N = NL;
    const size_t n = xs[0].size();
    hop = std::max ((size_t) std::nearbyint (0.01 * sr2), (size_t) std::ceil ((double) n / kMaxFrames));
    const double dfL = (double) sr2 / (double) NL, dfS = (double) sr2 / (double) NS;
    df = dfL;
    nb = std::min ((size_t) (std::min (kFmax, sr2 / 2.0) / dfL) + 2, NL / 2 + 1);
    nf = n / hop + 1;
    // the cross-over between the two windows, per bin
    std::vector<float> wl (nb); std::vector<size_t> s0 (nb); std::vector<float> sw (nb);
    for (size_t k = 0; k < nb; ++k)
    {
        const double fax = (double) k * dfL;
        double u = std::log (std::max (fax, 1.0) / kCrossLo) / std::log (kCrossHi / kCrossLo); u = std::min (1.0, std::max (0.0, u));
        wl[k] = (float) (1.0 - u * u * (3.0 - 2.0 * u));
        const double sp = std::min (std::max (fax / dfS, 0.0), (double) (NS / 2) - 1.001);
        s0[k] = (size_t) sp; sw[k] = (float) (sp - (double) s0[k]);
    }
    const Vec winL = hanning (NL), winS = hanning (NS);
    double sumL = 0, sumS = 0; for (double v : winL) sumL += v; for (double v : winS) sumS += v;
    const double nL = 2.0 / sumL, nS = 2.0 / sumS, makeup = 10.0 * std::log10 ((double) NL / (double) NS) * 0.5;
    db.assign (nch, {});
    std::atomic<int> done { 0 };
    std::atomic<bool> stop { false };
    parallelFor (nch, [&] (size_t c)
    {
        if (stop) return;
        std::vector<uint8_t> out (nf * nb);
        std::vector<float> xp (NL / 2 + n + NL, 0.0f);
        std::copy (xs[c].begin(), xs[c].end(), xp.begin() + (long) (NL / 2));
        Vec bufL (NL), bufS (NS), ml, ms;
        for (size_t k = 0; k < nf; ++k)
        {
            if ((k & 63) == 0 && hooks.cancelled && hooks.cancelled()) { stop = true; return; }
            const size_t i = k * hop;
            for (size_t q = 0; q < NL; ++q) bufL[q] = (double) xp[i + q] * winL[q];
            for (size_t q = 0; q < NS; ++q) bufS[q] = (double) xp[i + NL / 2 - NS / 2 + q] * winS[q];
            rfftMagnitude (bufL.data(), NL, NL, ml);
            rfftMagnitude (bufS.data(), NS, NS, ms);
            for (size_t b = 0; b < nb; ++b)
            {
                const float dl = (float) (20.0 * std::log10 (ml[b] * nL + 1e-9) + makeup);
                const float ds0 = (float) (20.0 * std::log10 (ms[s0[b]] * nS + 1e-9)), ds1 = (float) (20.0 * std::log10 (ms[s0[b] + 1] * nS + 1e-9));
                const float dsi = ds0 * (1.0f - sw[b]) + ds1 * sw[b];
                const float d = wl[b] * dl + (1.0f - wl[b]) * dsi;
                const float v = (d + 127.5f) * 2.0f;
                out[k * nb + b] = (uint8_t) std::min (255.0f, std::max (0.0f, v));
            }
        }
        db[c] = std::move (out);
        const int d = ++done; hooks.report ("Analysing the channels", (double) d / (double) nch);
    });
    if (stop) throw Cancelled();
    findRef();
}

void Spectrogram::combineFrame (size_t frame, std::vector<float>& outDb) const
{
    const float* pw = powTable();
    outDb.assign (nb, 0.0f);
    std::vector<float> P (nb, 0.0f); bool any = false;
    for (size_t c = 0; c < db.size(); ++c)
    {
        const float g2 = (float) (gain[c] * gain[c]); if (g2 <= 0.0f || db[c].empty()) continue;
        const uint8_t* a = db[c].data() + frame * nb;
        for (size_t b = 0; b < nb; ++b) P[b] += pw[a[b]] * g2;
        any = true;
    }
    for (size_t b = 0; b < nb; ++b) outDb[b] = any ? 10.0f * std::log10 (P[b] + 1e-20f) : -200.0f;
}

void Spectrogram::findRef()
{
    const size_t step = std::max<size_t> (1, nf / 400);
    Vec all; std::vector<float> d;
    for (size_t k = 0; k < nf; k += step) { combineFrame (k, d); for (float v : d) all.push_back ((double) v); }
    ref = all.empty() ? 0.0 : percentile (std::move (all), 99.7);
}

void Spectrogram::setGains (const Vec& g)
{
    if (g.size() != gain.size()) return;
    gain = g; findRef();
}
} // namespace rehar
