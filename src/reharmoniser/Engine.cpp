#include "Engine.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdlib>

namespace rehar
{
namespace
{
double dbLin (double db) { return db <= -59.5 ? 0.0 : std::pow (10.0, db / 20.0); }
double roundEven (double x) { return std::nearbyint (x); }
size_t pow2Round (double sr2, double seconds) { return (size_t) std::pow (2.0, roundEven (std::log2 (seconds * sr2))); }
double clip (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double smoothstep (double u) { u = clip (u, 0, 1); return u * u * (3 - 2 * u); }

/** np.interp on the frequency axis of a padded FFT (bin i is at i * df), done directly. */
double interpUniform (const Vec& sp, double df, double x)
{
    const double pos = x / df;
    if (pos <= 0.0) return sp.front();
    if (pos >= (double) (sp.size() - 1)) return sp.back();
    const size_t j = (size_t) pos;
    return sp[j] + (sp[j + 1] - sp[j]) * (pos - (double) j);
}

Vec combScore (const Vec& sp, double df, const Vec& fcs, int nharm)
{
    Vec s (fcs.size(), 0.0);
    for (size_t k = 0; k < fcs.size(); ++k)
        for (int h = 1; h <= nharm; ++h) s[k] += std::log (interpUniform (sp, df, fcs[k] * (double) h) + 1e-9);
    return s;
}

/** np.arange (start, stop, step) */
Vec arange (double start, double stop, double step)
{
    const long n = (long) std::ceil ((stop - start) / step);
    Vec v; if (n > 0) v.reserve ((size_t) n);
    for (long i = 0; i < n; ++i) v.push_back (start + (double) i * step);
    return v;
}

struct Decimated { std::vector<std::vector<float>> x; int sr2 = 0; };
Decimated decimateAll (const std::vector<Span>& chans, int sr)
{
    Decimated d; d.x.resize (chans.size());
    parallelFor (chans.size(), [&] (size_t c) { decimateTo (chans[c].p, chans[c].n, sr, d.x[c]); });
    d.sr2 = (sr + 23999) / 24000 <= 1 ? sr : sr / ((sr + 23999) / 24000);
    return d;
}

/** The spectrum of the channels added by power (the prototype's spec_at): sqrt (sum (w * |rfft|)^2) at time t. */
bool powerSpectrumAt (const Decimated& d, const Vec& ws, const Vec& win, size_t N, size_t pad, double tSeg0, double t, Vec& out)
{
    const long i = (long) roundEven ((t - tSeg0) * d.sr2) - (long) (N / 2);
    if (i < 0 || (size_t) i + N > d.x[0].size()) return false;
    Vec buf (N), mag;
    out.assign (N * pad / 2 + 1, 0.0);
    for (size_t c = 0; c < d.x.size(); ++c)
    {
        for (size_t k = 0; k < N; ++k) buf[k] = (double) d.x[c][(size_t) i + k] * win[k];
        rfftMagnitude (buf.data(), N, N * pad, mag);
        for (size_t k = 0; k < out.size(); ++k) { const double v = ws[c] * mag[k]; out[k] += v * v; }
    }
    for (auto& v : out) v = std::sqrt (v);
    return true;
}

/** exp (i * sign * h * phi[k]) */
CVec phasor (const Vec& phi, double h, double sign)
{
    CVec b (phi.size());
    for (size_t k = 0; k < phi.size(); ++k) { const double a = sign * h * phi[k]; b[k] = cplx (std::cos (a), std::sin (a)); }
    return b;
}

Vec cumPhase (const Vec& f, double sr)
{
    Vec phi (f.size()); double acc = 0.0;
    for (size_t i = 0; i < f.size(); ++i) { acc += f[i]; phi[i] = 2.0 * kPi * acc / sr; }
    return phi;
}

double maxOf (const Vec& v) { return v.empty() ? 0.0 : *std::max_element (v.begin(), v.end()); }
double minOf (const Vec& v) { return v.empty() ? 0.0 : *std::min_element (v.begin(), v.end()); }
double meanOf (const Vec& v) { return v.empty() ? 0.0 : std::accumulate (v.begin(), v.end(), 0.0) / (double) v.size(); }
} // namespace

// ================================================================================================== measuring a note
bool looksLikeTimecode (const Span& x, int sr)
{
    if (x.p == nullptr || sr <= 0 || x.n < (size_t) sr) return false;                 // under a second: cannot tell
    const size_t blk = (size_t) (0.2 * sr), sub = std::max<size_t> (8, (size_t) (0.02 * sr)), nb = x.n / blk;
    const size_t step = std::max<size_t> (1, nb / 40);
    int loud = 0, steady = 0;
    for (size_t b = 0; b < nb; b += step)
    {
        const float* p = x.p + b * blk;
        double pk = 0, ss = 0; size_t zc = 0;
        for (size_t k = 0; k < blk; ++k) { const double v = p[k]; pk = std::max (pk, std::abs (v)); ss += v * v; if (k > 0 && ((p[k - 1] < 0.0f) != (p[k] < 0.0f))) ++zc; }
        if (pk < 0.003) continue;                                                      // silence (below -50 dBFS)
        ++loud;
        const double rms = std::sqrt (ss / (double) blk), crest = pk / (rms + 1e-12), zcr = (double) zc / 0.2;
        double m = 0, m2 = 0; size_t cnt = 0;                                          // how much the level of 20 ms pieces moves
        for (size_t q = 0; q + sub <= blk; q += sub) { double e = 0; for (size_t k = 0; k < sub; ++k) e += (double) p[q + k] * (double) p[q + k]; e = std::sqrt (e / (double) sub); m += e; m2 += e * e; ++cnt; }
        m /= (double) std::max<size_t> (1, cnt); const double cv = std::sqrt (std::max (0.0, m2 / (double) std::max<size_t> (1, cnt) - m * m)) / (m + 1e-12);
        if (crest < 1.9 && cv < 0.06 && zcr > 1200.0 && zcr < 6000.0) ++steady;
    }
    return loud >= 5 && steady * 5 >= loud * 4;
}

Tracking trackingInputs (const std::vector<Span>& seg, int sr, double fLo, double fHi, const Vec& visGainDb, int kmax)
{
    const size_t nch = seg.size();
    Vec w (nch, 1.0);
    for (size_t c = 0; c < nch && c < visGainDb.size(); ++c) w[c] = dbLin (visGainDb[c]);
    Tracking t;
    if (nch == 1) { t.channels = { 0 }; t.weights = { 1.0 }; return t; }
    const auto sos = butterBandpass (2, std::max (20.0, 0.8 * fLo), std::min (0.45 * 24000.0, 3.0 * fHi), 24000.0);
    Vec en (nch);
    parallelFor (nch, [&] (size_t c)
    {
        std::vector<float> x2; decimateTo (seg[c].p, seg[c].n, sr, x2);
        Vec xd (x2.begin(), x2.end());
        const Vec y = sosfilt (sos, xd);
        double s = 0.0; for (double v : y) s += v * v;
        en[c] = (y.empty() ? 0.0 : s / (double) y.size()) * w[c] * w[c];
    });
    std::vector<size_t> order (nch); std::iota (order.begin(), order.end(), (size_t) 0);
    std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return en[a] < en[b]; });
    std::reverse (order.begin(), order.end());
    std::vector<size_t> top;
    for (size_t i = 0; i < order.size() && (int) i < kmax; ++i) if (en[order[i]] > 0) top.push_back (order[i]);
    if (top.empty()) throw std::runtime_error ("Every channel is turned down in the visual mixer.");
    double mx = 0.0; for (auto c : top) mx = std::max (mx, w[c]);
    for (auto c : top) { t.channels.push_back ((int) c); t.weights.push_back (w[c] / mx); }
    return t;
}

Track trackNote (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, double hopS)
{
    const auto d = decimateAll (chans, sr);
    const double sr2 = d.sr2;
    const size_t N = pow2Round (sr2, 0.34), pad = 4;
    const Vec win = hanning (N);
    const double df = sr2 / (double) (N * pad);
    const int nharm = (int) std::max (1.0, std::min (6.0, 0.42 * sr2 / fHi));
    const double span = std::max (1.0, 1200.0 * std::log2 (fHi / fLo));
    Vec cand;
    { const long nc = (long) std::ceil ((span + 3.0) / 3.0); for (long k = 0; k < nc; ++k) cand.push_back (fLo * std::pow (2.0, (double) (k * 3) / 1200.0)); }
    Vec acc (cand.size(), 0.0), sp;
    int n = 0;
    for (double t : arange (t0, t1, 0.05))
        if (powerSpectrumAt (d, ws, win, N, pad, tSeg0, t, sp)) { const Vec s = combScore (sp, df, cand, nharm); for (size_t k = 0; k < acc.size(); ++k) acc[k] += s[k]; ++n; }
    if (n == 0) throw std::runtime_error ("Selection is too close to the start/end of the file.");
    const double fc = cand[(size_t) (std::max_element (acc.begin(), acc.end()) - acc.begin())];
    Vec grid;
    { const long ng = (long) std::ceil ((70.1 + 70.0) / 0.5); for (long k = 0; k < ng; ++k) grid.push_back (fc * std::pow (2.0, (-70.0 + (double) k * 0.5) / 1200.0)); }
    Track tr; tr.ts = arange (t0, t1, hopS);
    Vec f0 (tr.ts.size(), 0.0); std::vector<char> ok (tr.ts.size(), 0);
    for (size_t k = 0; k < tr.ts.size(); ++k)
        if (powerSpectrumAt (d, ws, win, N, pad, tSeg0, tr.ts[k], sp))
        {
            const Vec s = combScore (sp, df, grid, nharm);
            f0[k] = grid[(size_t) (std::max_element (s.begin(), s.end()) - s.begin())]; ok[k] = 1;
        }
    Vec tOk, fOk;
    for (size_t k = 0; k < ok.size(); ++k) if (ok[k]) { tOk.push_back (tr.ts[k]); fOk.push_back (f0[k]); }
    if (tOk.size() < 5) throw std::runtime_error ("Could not track a pitch in that selection.");
    f0 = interp (tr.ts, tOk, fOk);
    tr.f0 = gaussianFilter1d (medianFilter (f0, 5), 1.5, Edge::Nearest);
    return tr;
}

namespace
{
/** The comb score of every candidate pitch (every 2 cents inside +-halfCents of fMid) at every time of the grid ts. */
struct CombField
{
    Vec cents, cand;
    std::vector<Vec> emis;
    std::vector<char> ok;
    double fMid = 0.0, halfCents = 0.0;
};
CombField combField (const Decimated& d, const Vec& ws, double tSeg0, const Vec& ts, double fMid, double halfCents)
{
    CombField F; F.fMid = fMid; F.halfCents = halfCents;
    const double sr2 = d.sr2;
    const size_t N = pow2Round (sr2, 0.34), pad = 4;
    const Vec win = hanning (N);
    const double df = sr2 / (double) (N * pad);
    const double fTop = fMid * std::pow (2.0, halfCents / 1200.0);
    const int nharm = (int) std::max (1.0, std::min (6.0, 0.42 * sr2 / fTop));
    const double step = 2.0;
    const int nc = (int) std::ceil (2.0 * halfCents / step) + 1;
    F.cents.resize ((size_t) nc); F.cand.resize ((size_t) nc);
    for (int k = 0; k < nc; ++k) { F.cents[(size_t) k] = -halfCents + step * k; F.cand[(size_t) k] = fMid * std::pow (2.0, F.cents[(size_t) k] / 1200.0); }
    const size_t nf = ts.size();
    F.emis.resize (nf); F.ok.assign (nf, 0);
    Vec sp;
    for (size_t k = 0; k < nf; ++k)
        if (powerSpectrumAt (d, ws, win, N, pad, tSeg0, ts[k], sp))
        {
            Vec sc = combScore (sp, df, F.cand, nharm);
            const double mx = maxOf (sc);
            for (auto& v : sc) v -= mx;
            F.emis[k] = std::move (sc); F.ok[k] = 1;
        }
    size_t nok = 0; for (char c : F.ok) nok += (size_t) c;
    if (nok < 5) throw std::runtime_error ("Could not track a pitch in that selection.");
    return F;
}

/** The best path of the comb score through time, for one voice. The path is drawn towards the middle (prior) and moves smoothly; cents within keepOut of any of the
    'avoid' lines (on the same time grid) are not allowed. Returns the line in Hz. */
Vec combViterbi (const CombField& F, size_t nf, double prior, const std::vector<Vec>& avoid, double keepOut)
{
    const double halfCents = F.halfCents;
    const int nc = (int) F.cents.size();
    const double step = 2.0, lambda = 0.25, dref = 10.0;
    const Vec& cents = F.cents;
    std::vector<Vec> avc;                                                   // the lines to keep clear of, in cents from fMid
    for (auto& l : avoid) { Vec a (nf); for (size_t k = 0; k < nf && k < l.size(); ++k) a[k] = 1200.0 * std::log2 (l[k] / F.fMid); avc.push_back (std::move (a)); }
    std::vector<std::vector<int>> from (nf, std::vector<int> ((size_t) nc, 0));
    Vec prev ((size_t) nc, 0.0);
    bool first = true;
    const int maxJump = (int) std::ceil (45.0 / step);
    for (size_t k = 0; k < nf; ++k)
    {
        Vec cur ((size_t) nc, 0.0);
        for (int c = 0; c < nc; ++c)
        {
            double e = (F.ok[k] ? F.emis[k][(size_t) c] : 0.0) - prior * (cents[(size_t) c] / halfCents) * (cents[(size_t) c] / halfCents);
            for (auto& a : avc) if (std::abs (cents[(size_t) c] - a[k]) < keepOut) { e -= 1.0e4; break; }
            if (first) { cur[(size_t) c] = e; continue; }
            double bv = -1e300; int bi = c;
            for (int q = std::max (0, c - maxJump); q <= std::min (nc - 1, c + maxJump); ++q)
            {
                const double dc = (cents[(size_t) c] - cents[(size_t) q]) / dref;
                const double v = prev[(size_t) q] - lambda * dc * dc;
                if (v > bv) { bv = v; bi = q; }
            }
            cur[(size_t) c] = bv + e; from[k][(size_t) c] = bi;
        }
        prev = cur; first = false;
    }
    int c = (int) (std::max_element (prev.begin(), prev.end()) - prev.begin());
    Vec f0 (nf);
    for (size_t k = nf; k-- > 0;) { f0[k] = F.cand[(size_t) c]; c = from[k][(size_t) c]; }
    return gaussianFilter1d (medianFilter (f0, 5), 1.5, Edge::Nearest);
}

Vec combPath (const Decimated& d, const Vec& ws, double tSeg0, const Vec& ts, double fMid, double halfCents, double prior, const std::vector<Vec>& avoid, double keepOut)
{
    return combViterbi (combField (d, ws, tSeg0, ts, fMid, halfCents), ts.size(), prior, avoid, keepOut);
}
} // namespace

Track trackNoteLocked (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, double hopS, double prior)
{
    const auto d = decimateAll (chans, sr);
    Track tr; tr.ts = arange (t0, t1, hopS);
    const double fMid = std::sqrt (fLo * fHi), hw = std::max (15.0, 600.0 * std::log2 (fHi / fLo));        // the box: its middle and half width in cents
    const CombField F = combField (d, ws, tSeg0, tr.ts, fMid, hw + 6.0);
    const size_t nf = tr.ts.size();
    // WHICH voice: a firm pull towards the middle of the box, so a louder voice beside the boxed one is not taken for it. The path is then rather flat (vibrato is
    // smeared in the picture): only a first guess, the vibrato is followed by the refinement.
    tr.f0 = combViterbi (F, nf, 2.0 * prior, {}, 0.0);
    return tr;
}

int detectPartial (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0)
{
    try
    {
        const auto d = decimateAll (chans, sr);
        const double sr2 = d.sr2;
        const size_t N = pow2Round (sr2, 0.34), pad = 4;
        const Vec win = hanning (N);
        const double df = sr2 / (double) (N * pad);
        const size_t nb = N * pad / 2 + 1;
        const double fm = median (f0);
        Vec P (nb, 0.0), buf (N), mag;
        int n = 0;
        for (int k = 0; k < 30; ++k)
        {
            const double t = ts.front() + (ts.back() - ts.front()) * (double) k / 29.0;           // np.linspace (ts[0], ts[-1], 30)
            const long i = (long) roundEven ((t - tSeg0) * sr2) - (long) (N / 2);
            if (i < 0 || (size_t) i + N > d.x[0].size()) continue;
            for (size_t c = 0; c < d.x.size(); ++c)
            {
                for (size_t q = 0; q < N; ++q) buf[q] = (double) d.x[c][(size_t) i + q] * win[q];
                rfftMagnitude (buf.data(), N, N * pad, mag);
                for (size_t q = 0; q < nb; ++q) { const double v = ws[c] * mag[q]; P[q] += v * v; }
            }
            ++n;
        }
        if (n < 3) return 1;
        Vec db (nb);
        for (size_t q = 0; q < nb; ++q) db[q] = 10.0 * std::log10 (P[q] / (double) n + 1e-18);
        auto level = [&] (double f)
        {
            double best = -999.0; bool any = false;
            for (size_t q = 0; q < nb; ++q) { const double fq = (double) q * df; if (fq >= f * std::pow (2.0, -60.0 / 1200.0) && fq <= f * std::pow (2.0, 60.0 / 1200.0)) { best = any ? std::max (best, db[q]) : db[q]; any = true; } }
            return any ? best : -999.0;
        };
        for (int k : { 4, 3, 2 })
        {
            if (fm / k < 45.0) continue;
            Vec sel;
            for (size_t q = 0; q < nb; ++q) { const double fq = (double) q * df; if (fq >= 0.8 * fm / k && fq <= 1.1 * fm) sel.push_back (db[q]); }
            const double floorDb = median (sel);
            const double Lk = level (fm);
            Vec L; for (int j = 1; j < k; ++j) L.push_back (level (fm * j / k));
            if (Lk < floorDb + 6) continue;
            bool all6 = true, all20 = true;
            for (double l : L) { if (l < floorDb + 6) all6 = false; if (l < Lk - 20) all20 = false; }
            if (all6 && L[0] >= Lk - 12 && all20) return k;
        }
    }
    catch (...) {}
    return 1;
}

Vec refineTrack (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& tsIn, const Vec& f0In, double bw, int nh, int iters, double smoothS)
{
    const Vec& ts = tsIn;
    Vec f0 = gaussianFilter1d (f0In, 4.0, Edge::Nearest);
    const long i0 = std::max<long> (0, (long) ((ts.front() - tSeg0) * sr));
    const long i1 = std::min<long> ((long) chans[0].n, (long) ((ts.back() - tSeg0) * sr));
    if (i1 <= i0) return f0;
    const size_t n = (size_t) (i1 - i0);
    Vec tt (n);
    for (size_t k = 0; k < n; ++k) tt[k] = tSeg0 + (double) (i0 + (long) k) / sr;
    std::vector<Vec> sigs (chans.size());
    for (size_t c = 0; c < chans.size(); ++c) { sigs[c].assign (chans[c].p + i0, chans[c].p + i1); }
    const int dec = std::max (1, (int) (sr / 400));
    Vec td; for (size_t k = 0; k < n; k += (size_t) dec) td.push_back (tt[k]);
    const double rate = (double) sr / dec;
    for (int it = 0; it < iters; ++it)
    {
        const Vec phi = cumPhase (interp (tt, ts, f0), sr);
        Vec num (td.size(), 0.0), den (td.size(), 0.0);
        const double f0max = maxOf (f0);
        for (int h = 1; h <= nh; ++h)
        {
            if (h * f0max > 0.45 * sr) break;
            const CVec base = phasor (phi, h, -1.0);
            std::vector<Vec> pn (sigs.size(), Vec (td.size(), 0.0)), pd (sigs.size(), Vec (td.size(), 0.0));
            parallelFor (sigs.size(), [&] (size_t c)
            {
                CVec z (n);
                for (size_t k = 0; k < n; ++k) z[k] = sigs[c][k] * base[k];
                const CVec lp = lowpassGaussian (z, sr, bw);
                Vec ang (td.size()), p2 (td.size());
                for (size_t k = 0; k < td.size(); ++k) { const cplx a = lp[k * (size_t) dec]; ang[k] = std::atan2 (a.imag(), a.real()); p2[k] = std::norm (a); }
                const Vec g = gradient (unwrap (ang));
                const Vec wgt = gaussianFilter1d (p2, 0.05 * rate);
                for (size_t k = 0; k < td.size(); ++k)
                {
                    const double inst = g[k] * rate / (2.0 * kPi), wg = wgt[k] * ws[c] * ws[c];
                    pn[c][k] = wg * inst / h; pd[c][k] = wg;
                }
            });
            for (size_t c = 0; c < sigs.size(); ++c) for (size_t k = 0; k < td.size(); ++k) { num[k] += pn[c][k]; den[k] += pd[c][k]; }
        }
        Vec q (td.size()); for (size_t k = 0; k < td.size(); ++k) q[k] = num[k] / (den[k] + 1e-30);
        const Vec dlt = gaussianFilter1d (q, smoothS * rate);
        const Vec add = interp (ts, td, dlt);
        for (size_t k = 0; k < f0.size(); ++k) f0[k] += add[k];
    }
    return f0;
}

Vec refineTrackLocked (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& tsIn, const Vec& f0In, double b0, bool wide)
{
    // the line is followed a little beyond the note's ends too (held flat there to begin with), so the filters see real sound right up to its ends
    const double hop = tsIn.size() > 1 ? (tsIn.back() - tsIn.front()) / (double) (tsIn.size() - 1) : 0.02;
    const size_t nPad = (size_t) std::floor (0.2 / hop);
    Vec ts, f0;
    for (size_t k = nPad; k > 0; --k) { ts.push_back (tsIn.front() - (double) k * hop); f0.push_back (f0In.front()); }
    ts.insert (ts.end(), tsIn.begin(), tsIn.end()); f0.insert (f0.end(), f0In.begin(), f0In.end());
    for (size_t k = 1; k <= nPad; ++k) { ts.push_back (tsIn.back() + (double) k * hop); f0.push_back (f0In.back()); }
    const long i0 = std::max<long> (0, (long) ((ts.front() - tSeg0) * sr));
    const long i1 = std::min<long> ((long) chans[0].n, (long) ((ts.back() - tSeg0) * sr));
    if (i1 <= i0) return f0In;
    const size_t n = (size_t) (i1 - i0);
    Vec tt (n);
    for (size_t k = 0; k < n; ++k) tt[k] = tSeg0 + (double) (i0 + (long) k) / sr;
    const size_t useCh = std::min<size_t> (chans.size(), 4);                      // (the strongest come first)
    std::vector<Vec> sigs (useCh);
    for (size_t c = 0; c < useCh; ++c) sigs[c].assign (chans[c].p + i0, chans[c].p + i1);
    const int dec = std::max (1, (int) (sr / 400));
    Vec td; for (size_t k = 0; k < n; k += (size_t) dec) td.push_back (tt[k]);
    const double rate = (double) sr / dec;
    const int nhMax = 8;
    struct Stage { double scale, smoothS; int iters; };
    // from wide and slow to narrow and quick. The wide one starts with a window twice as wide, so a voice that swings far from where the line is (a big vibrato) is still
    // caught: use it only where no other voice is near (they would be caught too).
    static const Stage narrow[] = { { 1.0, 0.03, 4 }, { 0.8, 0.025, 3 }, { 0.6, 0.02, 3 } };
    static const Stage broad[] = { { 2.0, 0.04, 4 }, { 1.4, 0.03, 3 }, { 1.0, 0.025, 3 }, { 0.6, 0.02, 3 } };
    const Stage* stages = wide ? broad : narrow;
    const size_t nStages = wide ? 4 : 3;
    for (size_t si = 0; si < nStages; ++si)
        for (int it = 0, nIt = stages[si].iters; it < nIt; ++it)
        {
            const Stage& st = stages[si];
            const Vec phi = cumPhase (interp (tt, ts, f0), sr);
            const double f0max = maxOf (f0);
            std::vector<Vec> qh, wh;                                    // per overtone: the estimated shift of the fundamental (Hz) and its weight, on the td grid
            for (int h = 1; h <= nhMax; ++h)
            {
                if (h * f0max * 1.05 > 0.45 * sr) break;
                const CVec base = phasor (phi, h, -1.0);
                const double bw = std::min (160.0, b0 * st.scale * (double) h);       // the same width in pitch at every overtone: a neighbour a few Hz away stays out of all of them
                std::vector<Vec> pn (sigs.size(), Vec (td.size(), 0.0)), pd (sigs.size(), Vec (td.size(), 0.0));
                parallelFor (sigs.size(), [&] (size_t c)
                {
                    CVec z (n);
                    for (size_t k = 0; k < n; ++k) z[k] = sigs[c][k] * base[k];
                    const CVec lp = lowpassGaussian (z, sr, bw);
                    Vec ang (td.size()), p2 (td.size());
                    for (size_t k = 0; k < td.size(); ++k) { const cplx a = lp[k * (size_t) dec]; ang[k] = std::atan2 (a.imag(), a.real()); p2[k] = std::norm (a); }
                    const Vec g = gradient (unwrap (ang));
                    const Vec wgt = gaussianFilter1d (p2, 0.05 * rate);
                    for (size_t k = 0; k < td.size(); ++k) { const double inst = g[k] * rate / (2.0 * kPi), wg = wgt[k] * ws[c] * ws[c]; pn[c][k] = wg * inst / h; pd[c][k] = wg; }
                });
                Vec q (td.size(), 0.0), w (td.size(), 0.0);
                for (size_t k = 0; k < td.size(); ++k)
                {
                    double a = 0, b = 0; for (size_t c = 0; c < sigs.size(); ++c) { a += pn[c][k]; b += pd[c][k]; }
                    q[k] = a / (b + 1e-30); w[k] = b * (double) h;
                }
                qh.push_back (std::move (q)); wh.push_back (std::move (w));
            }
            if (qh.empty()) break;
            Vec cons (td.size(), 0.0);
            const double sTol = std::max (0.8, 0.4 * b0 * st.scale);      // how far an overtone may differ from the rest (Hz of the fundamental) before it counts for less
            for (size_t k = 0; k < td.size(); ++k)
            {
                double a = 0, b = 0; for (size_t j = 0; j < qh.size(); ++j) { a += wh[j][k] * qh[j][k]; b += wh[j][k]; }
                double m = a / (b + 1e-30);
                for (int pass = 0; pass < 3; ++pass)
                {
                    a = 0; b = 0;
                    for (size_t j = 0; j < qh.size(); ++j) { const double r = (qh[j][k] - m) / sTol; const double w = wh[j][k] / (1.0 + r * r * r * r); a += w * qh[j][k]; b += w; }
                    m = a / (b + 1e-30);
                }
                cons[k] = m;
            }
            const Vec dlt = gaussianFilter1d (cons, st.smoothS * rate);
            const Vec add = interp (ts, td, dlt);
            for (size_t k = 0; k < f0.size(); ++k) f0[k] += add[k];
        }
    return Vec (f0.begin() + (long) nPad, f0.begin() + (long) (nPad + tsIn.size()));
}

namespace
{
/** Takes the sound of one voice (its overtones 1..nh, each through a window bw0*h Hz wide) out of the channels, in place. sig[c][k] is at tStart + k / sr.
    The voice's line is on the grid ts; nothing is taken outside it (and the ends are eased). */
void peelLine (std::vector<Vec>& sig, int sr, double tStart, const Vec& ts, const Vec& line, double bw0, int nh)
{
    if (sig.empty() || sig[0].empty() || ts.size() < 2) return;
    const size_t n = sig[0].size();
    Vec tt (n), w (n);
    for (size_t k = 0; k < n; ++k)
    {
        tt[k] = tStart + (double) k / sr;
        w[k] = smoothstep ((tt[k] - (ts.front() - 0.3)) / 0.08) * smoothstep (((ts.back() + 0.3) - tt[k]) / 0.08);       // (the line is held flat for 0.3 s beyond its ends)
    }
    const Vec f = interp (tt, ts, line);
    const Vec phi = cumPhase (f, sr);
    const double fmax = maxOf (line);
    for (int h = 1; h <= nh; ++h)
    {
        if (h * fmax * 1.05 > 0.45 * sr) break;
        const CVec base = phasor (phi, h, -1.0);
        parallelFor (sig.size(), [&] (size_t c)
        {
            CVec z (n); for (size_t k = 0; k < n; ++k) z[k] = sig[c][k] * base[k];
            const CVec a = lowpassGaussian (z, sr, bw0 * (double) h);
            for (size_t k = 0; k < n; ++k) sig[c][k] -= w[k] * (a[k] * std::conj (base[k])).real();
        });
    }
}

/** How much sound one voice has in the channels, sample by sample (power, over overtones 1..nh). */
Vec linePower (const std::vector<Vec>& sig, const Vec& ws, int sr, double tStart, const Vec& ts, const Vec& line, int nh, double bw0)
{
    const size_t n = sig[0].size();
    Vec tt (n);
    for (size_t k = 0; k < n; ++k) tt[k] = tStart + (double) k / sr;
    const Vec f = interp (tt, ts, line);
    const Vec phi = cumPhase (f, sr);
    const double fmax = maxOf (line);
    Vec P (n, 0.0);
    for (int h = 1; h <= nh; ++h)
    {
        if (h * fmax * 1.05 > 0.45 * sr) break;
        const CVec base = phasor (phi, h, -1.0);
        std::vector<Vec> part (sig.size());
        parallelFor (sig.size(), [&] (size_t c)
        {
            CVec z (n); for (size_t k = 0; k < n; ++k) z[k] = sig[c][k] * base[k];
            const CVec a = lowpassGaussian (z, sr, bw0 * (double) h);
            part[c].resize (n); for (size_t k = 0; k < n; ++k) part[c][k] = std::norm (a[k]) * ws[c] * ws[c];
        });
        for (const auto& pc : part) for (size_t k = 0; k < n; ++k) P[k] += pc[k];
    }
    return P;
}

/** How much sound one voice has in the channels (power, over overtones 1..nh, inside the line's own time range). */
double lineEnergy (const std::vector<Vec>& sig, const Vec& ws, int sr, double tStart, const Vec& ts, const Vec& line, int nh, double bw0)
{
    const Vec P = linePower (sig, ws, sr, tStart, ts, line, nh, bw0);
    double e = 0;
    for (size_t k = 0; k < P.size(); ++k) { const double t = tStart + (double) k / sr; if (t >= ts.front() && t <= ts.back()) e += P[k]; }
    return e;
}

/** Is the line a voice that is there all through the note? Its sound is compared with what is at the same distance on either side of it, stretch by stretch (0.3 s);
    returns the share of the stretches in which it stands clearly above that, and (total) how much it stands above it over the whole note. */
double persistence (const std::vector<Vec>& sig, const Vec& ws, int sr, double tStart, const Vec& ts, const Vec& line, double* totalRatio = nullptr)
{
    const Vec P = linePower (sig, ws, sr, tStart, ts, line, 4, 8.0);
    std::vector<Vec> R;
    for (double sh : { 45.0, -45.0, 75.0, -75.0 }) { Vec l2 = line; for (auto& x : l2) x *= std::pow (2.0, sh / 1200.0); R.push_back (linePower (sig, ws, sr, tStart, ts, l2, 4, 8.0)); }
    auto sum = [&] (const Vec& v, double a, double b)
    {
        const long i0 = std::max<long> (0, (long) std::ceil ((a - tStart) * sr)), i1 = std::min<long> ((long) v.size(), (long) std::floor ((b - tStart) * sr));
        double e = 0; for (long k = i0; k < i1; ++k) e += v[(size_t) k]; return e;
    };
    auto refMedian = [&] (double a, double b) { Vec r; for (const auto& v : R) r.push_back (sum (v, a, b)); return median (r); };
    if (totalRatio != nullptr) *totalRatio = sum (P, ts.front(), ts.back()) / (refMedian (ts.front(), ts.back()) + 1e-30);
    const double w = 0.3;
    int good = 0, tot = 0;
    for (double a = ts.front() + 0.05; a + w <= ts.back() - 0.05 + 1e-9; a += w)
    {
        ++tot;
        if (sum (P, a, a + w) > 3.0 * refMedian (a, a + w)) ++good;
    }
    return tot > 0 ? (double) good / (double) tot : 1.0;
}
} // namespace

Vec refineTrackPeeled (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0In, const std::vector<Vec>& nb, double b0, bool wide)
{
    const long m = (long) (0.45 * sr);
    const long i0 = std::max<long> (0, (long) ((ts.front() - tSeg0) * sr) - m);
    const long i1 = std::min<long> ((long) chans[0].n, (long) ((ts.back() - tSeg0) * sr) + m);
    if (i1 <= i0) return f0In;
    const double tStart = tSeg0 + (double) i0 / sr;
    std::vector<Vec> clean (chans.size());
    for (size_t c = 0; c < chans.size(); ++c) clean[c].assign (chans[c].p + i0, chans[c].p + i1);
    for (const auto& fn : nb) peelLine (clean, sr, tStart, ts, fn, 8.0, 8);
    std::vector<std::vector<float>> cf (clean.size());
    std::vector<Span> cs;
    for (size_t c = 0; c < clean.size(); ++c) { cf[c].assign (clean[c].begin(), clean[c].end()); cs.push_back ({ cf[c].data(), cf[c].size() }); }
    return refineTrackLocked (cs, ws, sr, tStart, ts, f0In, b0, wide);
}

std::vector<Vec> findNeighbours (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, const Vec& ts, const Vec& f0, double span, int maxVoices)
{
    std::vector<Vec> found;
    if (chans.empty() || ts.size() < 8 || f0.size() != ts.size()) return found;
    const long m = (long) (0.4 * sr);                                              // a little of the music each side of the note, so the picture of the spectrum is whole at its ends
    const long i0 = std::max<long> (0, (long) ((ts.front() - tSeg0) * sr) - m), i1 = std::min<long> ((long) chans[0].n, (long) ((ts.back() - tSeg0) * sr) + m);
    if (i1 - i0 < sr / 2) return found;
    const double tStart = tSeg0 + (double) i0 / sr;
    std::vector<Vec> sig (chans.size());
    for (size_t c = 0; c < chans.size(); ++c) sig[c].assign (chans[c].p + i0, chans[c].p + i1);
    const double eT = lineEnergy (sig, ws, sr, tStart, ts, f0, 4, 8.0);
    if (eT <= 0.0) return found;
    const double peelBw = 16.0;                                                    // (wide: what is left beside a voice - its own vibrato sidebands - must not be taken for a voice)
    peelLine (sig, sr, tStart, ts, f0, peelBw, 8);                                 // the boxed voice out first
    std::vector<Vec> avoid = { f0 };
    const double fMid = median (f0);
    for (int v = 0; v < maxVoices; ++v)
    {
        std::vector<std::vector<float>> cf (sig.size());
        std::vector<Span> cs;
        for (size_t c = 0; c < sig.size(); ++c) { cf[c].assign (sig[c].begin(), sig[c].end()); cs.push_back ({ cf[c].data(), cf[c].size() }); }
        Vec path;
        try { path = combPath (decimateAll (cs, sr), ws, tStart, ts, fMid, span, 0.0, avoid, 7.0); } catch (...) { break; }
        const Vec line = refineTrackLocked (cs, ws, sr, tStart, ts, path, 14.0);
        // is it really a voice? It must stand clear of what is at the same distance on either side of it all through the note (not only in a stretch of it: a
        // line that is only strong where the boxed voice was lost is a piece of that voice), and not be a faint rest of the boxed voice
        const double eN = lineEnergy (sig, ws, sr, tStart, ts, line, 4, 8.0);
        double ratio = 0.0;
        const double share = persistence (sig, ws, sr, tStart, ts, line, &ratio);
        size_t close = 0; for (size_t k = 0; k < line.size(); ++k) if (std::abs (1200.0 * std::log2 (line[k] / f0[k])) < 6.0) ++close;
        if (! (ratio > 4.0 && share >= 0.7 && eN > 0.01 * eT && close * 2 < line.size())) break;
        found.push_back (line); avoid.push_back (line);
        peelLine (sig, sr, tStart, ts, line, peelBw, 8);
    }
    return found;
}

NoteMeasure measureNote (const std::vector<Span>& chans, const Vec& ws, int sr, double tSeg0, double t0, double t1, double fLo, double fHi, const Hooks& hooks, double prior,
                         bool lookForOthers)
{
    NoteMeasure r;
    hooks.report ("Measuring the note", 0.05);
    const Track tr = trackNoteLocked (chans, ws, sr, tSeg0, t0, t1, fLo, fHi, 0.02, prior);
    hooks.check(); hooks.report ("Measuring the note", 0.15);
    // 1) the line of the voice near the first guess, with wide windows to begin with so the swings of a big vibrato are caught
    Vec f0 = refineTrackLocked (chans, ws, sr, tSeg0, tr.ts, tr.f0, 20.0, true);
    hooks.check(); hooks.report (lookForOthers ? "Looking for the voices beside it" : "Measuring the note", 0.3);
    std::vector<Vec> nbs = lookForOthers ? findNeighbours (chans, ws, sr, tSeg0, tr.ts, f0) : std::vector<Vec>();       // "One voice only": nothing beside it
    // 2) With louder voices close by, that line can have gone over to one of them for a while, and the voice that was left is then found as a "neighbour" over the same
    //    stretch. The lines found together hold every voice, so the boxed one is picked again moment by moment: the line whose pitch (the middle value over 0.4 s, so a vibrato or a brief mix-up does not count)
    //    is nearest the middle of the box (a line is only given up for another that is clearly nearer: 8 cents). lines[0] is the boxed voice afterwards.
    const double fMidBox = std::sqrt (fLo * fHi);
    // slow[i][k]: line i's pitch in cents from the middle of the box, the middle value over 0.4 s (a vibrato or a brief mix-up does not count)
    auto slowOf = [fMidBox] (const std::vector<Vec>& lines)
    {
        std::vector<Vec> slow;
        for (auto& l : lines)
        {
            Vec c (l.size()); for (size_t k = 0; k < l.size(); ++k) c[k] = 1200.0 * std::log2 (l[k] / fMidBox);
            Vec med (c.size()); const long half = 10;
            for (long k = 0; k < (long) c.size(); ++k) { Vec w; for (long q = std::max<long> (0, k - half); q <= std::min<long> ((long) c.size() - 1, k + half); ++q) w.push_back (c[(size_t) q]); std::nth_element (w.begin(), w.begin() + (long) (w.size() / 2), w.end()); med[(size_t) k] = w[w.size() / 2]; }
            slow.push_back (med);
        }
        return slow;
    };
    // Puts the boxed voice first, moment by moment. Over stretches (brief = false): the line whose slow pitch is nearest the middle of the box, given up only for a line clearly
    // nearer (8 cents). For brief mix-ups (brief = true, when the lines are already right over the long stretches): the line (of those not far apart in the other way round) whose own
    // pitch at that moment is nearest the slow pitch of the boxed one, and it takes a good reason (30 cents' worth) to change from one line to another.
    auto pickBoxed = [&] (std::vector<Vec>& lines, bool brief)
    {
        const size_t L = lines.size(), nf = lines[0].size();
        if (L < 2) return;
        const std::vector<Vec> slow = slowOf (lines);
        size_t first = 0; double bm = 1e300;
        for (size_t i = 0; i < L; ++i) { double m = 0; for (double v : slow[i]) m += std::abs (v); m /= (double) nf; if (m < bm) { bm = m; first = i; } }
        std::vector<size_t> pick (nf, first);
        if (! brief)
        {
            size_t cur = first;
            for (size_t k = 0; k < nf; ++k)
            {
                double bd = std::abs (slow[cur][k]);
                for (size_t i = 0; i < L; ++i) if (std::abs (slow[i][k]) < bd - 8.0) { bd = std::abs (slow[i][k]); cur = i; }
                pick[k] = cur;
            }
        }
        else
        {
            const double penalty = 30.0, cap = 40.0;
            std::vector<Vec> cost (nf, Vec (L, 0.0)); std::vector<std::vector<size_t>> from (nf, std::vector<size_t> (L, 0));
            for (size_t k = 0; k < nf; ++k)
            {
                for (size_t i = 0; i < L; ++i)
                {
                    const double own = 1200.0 * std::log2 (lines[i][k] / fMidBox), e = std::min (cap, std::abs (own - slow[first][k]));
                    if (k == 0) { cost[k][i] = e; continue; }
                    double bv = 1e300; size_t bi = i;
                    for (size_t q = 0; q < L; ++q) { const double v = cost[k - 1][q] + (q == i ? 0.0 : penalty); if (v < bv) { bv = v; bi = q; } }
                    cost[k][i] = bv + e; from[k][i] = bi;
                }
            }
            size_t st = 0; for (size_t i = 1; i < L; ++i) if (cost[nf - 1][i] < cost[nf - 1][st]) st = i;
            for (size_t k = nf; k-- > 0;) { pick[k] = st; if (k > 0) st = from[k][st]; }
        }
        std::vector<Vec> now = lines;
        for (size_t k = 0; k < nf; ++k) if (pick[k] != 0) std::swap (now[0][k], now[pick[k]][k]);
        lines = now;
    };
    std::vector<Vec> found = { f0 }; for (auto& l : nbs) found.push_back (l);
    pickBoxed (found, false);
    f0 = found[0]; nbs.assign (found.begin() + 1, found.end());
    std::vector<Vec> all = { f0 };
    for (auto& l : nbs) all.push_back (l);
    if (all.size() > 1)
    {
        // two lines whose pitch is about the same are one voice (the first guess was rough, or two singers too close to tell apart): keep the one that follows it better, i.e. holds more of its sound
        const long i0 = std::max<long> (0, (long) ((tr.ts.front() - tSeg0) * sr)), i1 = std::min<long> ((long) chans[0].n, (long) ((tr.ts.back() - tSeg0) * sr));
        std::vector<Vec> sig (chans.size());
        for (size_t c = 0; c < chans.size(); ++c) sig[c].assign (chans[c].p + i0, chans[c].p + i1);
        const double tStart = tSeg0 + (double) i0 / sr;
        Vec en; for (auto& l : all) en.push_back (lineEnergy (sig, ws, sr, tStart, tr.ts, l, 4, 8.0));
        std::vector<char> gone (all.size(), 0);
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j)
            {
                if (gone[i] || gone[j]) continue;
                double md = 0; for (size_t k = 0; k < all[i].size(); ++k) md += 1200.0 * std::log2 (all[i][k] / all[j][k]);
                if (std::abs (md / (double) all[i].size()) < 12.0) gone[en[i] >= en[j] ? j : i] = 1;
            }
        std::vector<Vec> kept; for (size_t i = 0; i < all.size(); ++i) if (! gone[i]) kept.push_back (all[i]);
        all = kept;
    }
    if (all.size() > 1)
    {
        // each voice is followed again with the others taken out; the boxed voice is the one whose line lies nearest the middle of the box
        hooks.check();
        const int sweeps = 2;                                               // the second sweep uses the lines of the first, which are far truer than the first rough ones
        for (int sw = 0; sw < sweeps; ++sw)
        {
        if (sw > 0) pickBoxed (all, true);                                  // in a sweep the lines of two close voices can swap places for a moment (each is taken out of the other's); put them right before the next one
        for (size_t j = 0; j < all.size(); ++j)
        {
            hooks.check(); hooks.report ("Measuring the note", 0.6 + 0.35 * (double) (sw * (int) all.size() + (int) j) / (double) (sweeps * (int) all.size()));
            std::vector<Vec> others; for (size_t i = 0; i < all.size(); ++i) if (i != j) others.push_back (all[i]);
            Vec seed = all[j];
            {   // start from the slow drift of the line, not the line itself: a stretch where it was caught by a neighbour (at a vibrato trough, say) would otherwise be too far from the voice to find it again
                Vec c (seed.size()); for (size_t k = 0; k < seed.size(); ++k) c[k] = std::log2 (seed[k]);
                c = gaussianFilter1d (c, 0.15 / 0.02, Edge::Nearest);
                for (size_t k = 0; k < seed.size(); ++k) seed[k] = std::pow (2.0, c[k]);
            }
            all[j] = refineTrackPeeled (chans, ws, sr, tSeg0, tr.ts, seed, others, 20.0, true);
        }
        }
    }
    pickBoxed (all, true);
    const size_t best = 0;
    r.ts = tr.ts; r.f0 = all[best];
    // after the joint refinement two lines can have ended on the very same sound; a neighbour that follows the note (or another neighbour) to within 8 cents all through is not a voice of its own
    auto rmsCents = [] (const Vec& a, const Vec& b) { double e = 0; for (size_t k = 0; k < a.size() && k < b.size(); ++k) { const double d = 1200.0 * std::log2 (a[k] / b[k]); e += d * d; } return std::sqrt (e / (double) std::max<size_t> (1, a.size())); };
    for (size_t j = 0; j < all.size(); ++j)
    {
        if (j == best || rmsCents (all[j], r.f0) < 8.0) continue;
        bool same = false; for (auto& l : r.nb) if (rmsCents (all[j], l) < 8.0) same = true;
        if (! same) r.nb.push_back (all[j]);
    }
    return r;
}

// ================================================================================================== the correction
Plan planCurve (const Vec& ts, const Vec& f0, const Params& p, int midi, const Vec& pull)
{
    Plan pl;
    pl.refMidi = midi; pl.refHz = midiHz (midi, p.a4); pl.refLabel = noteLabel (midi);
    const size_t n = f0.size();
    pl.c.resize (n);
    for (size_t i = 0; i < n; ++i) pl.c[i] = 1200.0 * std::log2 (f0[i] / pl.refHz);
    double hop = 0.02;
    if (ts.size() > 1) { Vec d (ts.size() - 1); for (size_t i = 0; i + 1 < ts.size(); ++i) d[i] = ts[i + 1] - ts[i]; hop = median (d); }
    Vec base;
    if (p.trendFit)
    {
        // the note's pitch (its slow drift): a straight line fitted through the points near each moment (Gaussian weights, p.smooth wide), which follows a
        // rising or falling note right to its ends. (A plain smoothing lagged at the ends and was pulled down by the scoop a note often starts with, so
        // the "wobble" it left had an offset in it, and "Keep wobble" then kept part of that offset: the note came out sharp or flat.)
        // The first moment of the box (the ease-in) hardly counts: that is where the scoop is.
        base.resize (n);
        const double sig = std::max (hop, p.smooth);
        for (size_t i = 0; i < n; ++i)
        {
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t j = 0; j < n; ++j)
            {
                const double dx = ts[j] - ts[i];
                double w = std::exp (-0.5 * (dx / sig) * (dx / sig));
                if (p.ease > 0 && ts[j] - ts[0] < p.ease) w *= 0.05;
                sw += w; sx += w * dx; sy += w * pl.c[j]; sxx += w * dx * dx; sxy += w * dx * pl.c[j];
            }
            const double det = sw * sxx - sx * sx;
            base[i] = std::abs (det) > 1e-12 ? (sxx * sy - sx * sxy) / det : (sw > 0 ? sy / sw : pl.c[i]);     // the fitted line's value at ts[i]
        }
    }
    else base = gaussianFilter1d (pl.c, std::max (0.5, p.smooth / hop), Edge::Nearest);
    pl.wob.resize (n); pl.off.resize (n); pl.planned.resize (n);
    const bool hasPull = pull.size() == ts.size() && ! pull.empty();
    const double s = clip (p.snap, 0, 1);
    for (size_t i = 0; i < n; ++i)
    {
        pl.wob[i] = pl.c[i] - base[i];
        const double delta = p.move + (hasPull ? pull[i] : 0.0);
        const double planned = (1 - s) * (pl.c[i] + delta) + s * p.keep * pl.wob[i];
        double off = planned - pl.c[i];
        if (p.ease > 0) off *= smoothstep ((ts[i] - ts[0]) / p.ease);
        off *= p.strength;
        pl.off[i] = off; pl.planned[i] = pl.c[i] + off;
    }
    const double med = median (pl.c);
    if (std::abs (med) > 60) { char b[200]; std::snprintf (b, sizeof b, "What was sung is about %+.0f cents from %s. Check the note name.", med, pl.refLabel.c_str()); pl.warning = b; }
    return pl;
}

namespace
{
/** _band_energy_gain: how much the added sound must be scaled, moment by moment, so the band energy is kept. x, removed, added: [channel][sample]. */
Vec bandEnergyGain (const std::vector<Vec>& x, const std::vector<Vec>& removed, const std::vector<Vec>& added, double sr, double lo, double hi, double winS = 0.06, double smoothS = 0.05)
{
    const auto sos = butterBandpass (4, lo, hi, sr);
    const size_t nch = x.size(), n = x[0].size();
    std::vector<Vec> xb (nch), rb (nch), pb (nch);
    parallelFor (nch, [&] (size_t c)
    {
        xb[c] = sosfilt (sos, x[c]);
        Vec r (n); for (size_t i = 0; i < n; ++i) r[i] = x[c][i] - removed[c][i];
        rb[c] = sosfilt (sos, r);
        pb[c] = sosfilt (sos, added[c]);
    });
    const size_t fr = std::max<size_t> (8, (size_t) (winS * sr));
    const size_t hop = fr / 2;
    Vec centres, gs;
    const size_t limit = std::max<size_t> (1, n > fr ? n - fr : 0);
    for (size_t i = 0; i < limit; i += hop)
    {
        const size_t e = std::min (n, i + fr);
        double E = 0, pp = 0, rp = 0, rr = 0;
        for (size_t c = 0; c < nch; ++c)
            for (size_t k = i; k < e; ++k) { E += xb[c][k] * xb[c][k]; pp += pb[c][k] * pb[c][k]; rp += rb[c][k] * pb[c][k]; rr += rb[c][k] * rb[c][k]; }
        double g;
        if (pp < 1e-18) g = 1.0;
        else
        {
            const double disc = rp * rp - pp * (rr - E);
            if (disc < 0) g = -rp / pp;
            else { const double sq = std::sqrt (disc); const double g1 = (-rp + sq) / pp, g2 = (-rp - sq) / pp; g = std::abs (g1 - 1) < std::abs (g2 - 1) ? g1 : g2; }
        }
        gs.push_back (clip (g, 0.6, 1.5));
        centres.push_back ((double) (i + fr / 2));
    }
    gs = gaussianFilter1d (gs, std::max (0.3, smoothS * sr / (double) hop), Edge::Nearest);
    Vec idx (n); for (size_t i = 0; i < n; ++i) idx[i] = (double) i;
    return interp (idx, centres, gs);
}

/** Splits the sound into its voices, overtone by overtone, by sliding least squares: in every short window each voice's overtone is a complex amplitude (and how it is
    changing) times a wave that follows the voice's own line, and all the voices' amplitudes are fitted TOGETHER, so a voice near another is not carried along with it.
    The parts of the voices to be moved (fNew[j] != nullptr) are returned twice: where they were (removed) and as they would be at the new pitch (added).
    x[c][k]; f[j][k] (Hz, every sample); the same length m. */
void separateAndMove (const std::vector<Vec>& x, int sr, const std::vector<const Vec*>& f, const std::vector<const Vec*>& fNew, int nh, double win,
                      std::vector<Vec>& removed, std::vector<Vec>& added, const Hooks& hooks)
{
    const size_t nch = x.size(), m = x[0].size(), K = f.size();
    const size_t W = std::max<size_t> (64, (size_t) (win * sr)), hop = std::max<size_t> (1, W / 4);
    Vec wA (W), u (W);
    for (size_t i = 0; i < W; ++i) { wA[i] = 0.5 - 0.5 * std::cos (2.0 * kPi * ((double) i + 0.5) / (double) W); u[i] = (((double) i + 0.5) - 0.5 * (double) W) / (0.5 * (double) W); }
    std::vector<long> starts;
    for (long st = -(long) (W / 2); st < (long) m - (long) (W / 2) + 1; st += (long) hop) starts.push_back (st);
    const size_t nF = starts.size();
    Vec norm (m, 0.0);
    for (size_t q = 0; q < nF; ++q) for (size_t i = 0; i < W; ++i) { const long k = starts[q] + (long) i; if (k >= 0 && k < (long) m) norm[(size_t) k] += 0.5 * wA[i]; }
    for (auto& v : norm) v = std::max (v, 0.2);
    std::vector<Vec> phase (K), phaseNew (K);
    for (size_t j = 0; j < K; ++j) { phase[j] = cumPhase (*f[j], sr); if (fNew[j] != nullptr) phaseNew[j] = cumPhase (*fNew[j], sr); }
    std::vector<double> fmax (K); for (size_t j = 0; j < K; ++j) fmax[j] = maxOf (*f[j]);
    for (int h = 1; h <= nh; ++h)
    {
        hooks.check();
        std::vector<size_t> act;                                              // the voices that have this overtone below the top of the band
        for (size_t j = 0; j < K; ++j) if ((double) h * fmax[j] * 1.03 < 0.45 * sr && (fNew[j] == nullptr || (double) h * maxOf (*fNew[j]) * 1.03 < 0.45 * sr)) act.push_back (j);
        bool any = false; for (size_t j : act) if (fNew[j] != nullptr) any = true;
        if (! any) continue;
        const size_t Ka = act.size(), B = 2 * Ka;
        std::vector<CVec> e (Ka), eN (Ka);
        for (size_t a = 0; a < Ka; ++a)
        {
            const size_t j = act[a]; e[a].resize (m);
            for (size_t k = 0; k < m; ++k) { const double ang = (double) h * phase[j][k]; e[a][k] = cplx (std::cos (ang), std::sin (ang)); }
            if (fNew[j] != nullptr) { eN[a].resize (m); for (size_t k = 0; k < m; ++k) { const double ang = (double) h * phaseNew[j][k]; eN[a][k] = cplx (std::cos (ang), std::sin (ang)); } }
        }
        // the Gram matrix of every window (the same for all channels), inverted
        std::vector<CVec> ginv (nF);
        parallelFor (nF, [&] (size_t q)
        {
            const long st = starts[q];
            const size_t i0 = (size_t) std::max<long> (0, -st), i1 = (size_t) std::min<long> ((long) W, (long) m - st);
            CVec G (B * B, cplx (0, 0));
            for (size_t a = 0; a < Ka; ++a)
                for (size_t b = a; b < Ka; ++b)
                {
                    cplx s0 (0, 0), s1 (0, 0), s2 (0, 0);
                    for (size_t i = i0; i < i1; ++i)
                    {
                        const cplx pr = std::conj (e[a][(size_t) (st + (long) i)]) * e[b][(size_t) (st + (long) i)] * wA[i];
                        s0 += pr; s1 += pr * u[i]; s2 += pr * (u[i] * u[i]);
                    }
                    G[(2 * a) * B + 2 * b] = s0; G[(2 * a) * B + 2 * b + 1] = s1; G[(2 * a + 1) * B + 2 * b] = s1; G[(2 * a + 1) * B + 2 * b + 1] = s2;
                    if (b != a) { G[(2 * b) * B + 2 * a] = std::conj (s0); G[(2 * b) * B + 2 * a + 1] = std::conj (s1); G[(2 * b + 1) * B + 2 * a] = std::conj (s1); G[(2 * b + 1) * B + 2 * a + 1] = std::conj (s2); }
                }
            for (size_t a = 0; a < B; ++a) G[a * B + a] *= 1.0 + 1.0e-3;                                  // (a touch of regularisation, for voices on top of each other)
            // inverse by Gauss-Jordan
            CVec inv (B * B, cplx (0, 0)); for (size_t a = 0; a < B; ++a) inv[a * B + a] = 1.0;
            for (size_t c = 0; c < B; ++c)
            {
                size_t piv = c; for (size_t r = c + 1; r < B; ++r) if (std::abs (G[r * B + c]) > std::abs (G[piv * B + c])) piv = r;
                if (piv != c) for (size_t k = 0; k < B; ++k) { std::swap (G[c * B + k], G[piv * B + k]); std::swap (inv[c * B + k], inv[piv * B + k]); }
                const cplx d = G[c * B + c]; const cplx di = std::abs (d) > 1e-30 ? 1.0 / d : cplx (0, 0);
                for (size_t k = 0; k < B; ++k) { G[c * B + k] *= di; inv[c * B + k] *= di; }
                for (size_t r = 0; r < B; ++r) if (r != c) { const cplx fct = G[r * B + c]; if (fct == cplx (0, 0)) continue; for (size_t k = 0; k < B; ++k) { G[r * B + k] -= fct * G[c * B + k]; inv[r * B + k] -= fct * inv[c * B + k]; } }
            }
            ginv[q] = std::move (inv);
        });
        // every channel: the amplitudes of each window, and the parts of the moved voices put together again
        parallelFor (nch, [&] (size_t c)
        {
            Vec rem (m, 0.0), add (m, 0.0);
            CVec r (B), co (B);
            for (size_t q = 0; q < nF; ++q)
            {
                const long st = starts[q];
                const size_t i0 = (size_t) std::max<long> (0, -st), i1 = (size_t) std::min<long> ((long) W, (long) m - st);
                for (size_t a = 0; a < Ka; ++a)
                {
                    cplx s0 (0, 0), s1 (0, 0);
                    for (size_t i = i0; i < i1; ++i) { const cplx t = std::conj (e[a][(size_t) (st + (long) i)]) * (x[c][(size_t) (st + (long) i)] * wA[i]); s0 += t; s1 += t * u[i]; }
                    r[2 * a] = s0; r[2 * a + 1] = s1;
                }
                for (size_t a = 0; a < B; ++a) { cplx v (0, 0); for (size_t b = 0; b < B; ++b) v += ginv[q][a * B + b] * r[b]; co[a] = v; }
                for (size_t a = 0; a < Ka; ++a)
                {
                    if (fNew[act[a]] == nullptr) continue;
                    for (size_t i = i0; i < i1; ++i)
                    {
                        const size_t k = (size_t) (st + (long) i);
                        const cplx amp = 2.0 * (co[2 * a] + co[2 * a + 1] * u[i]);                  // (a real wave is half in the positive frequencies, half in the negative)
                        const double sw = 0.5 * wA[i];
                        rem[k] += sw * (amp * e[a][k]).real();
                        add[k] += sw * (amp * eN[a][k]).real();
                    }
                }
            }
            for (size_t k = 0; k < m; ++k) { removed[c][k] += rem[k] / norm[k]; added[c][k] += add[k] / norm[k]; }
        });
    }
}

/** The region [lo, hi) where w > 0. */
bool activeRange (const Vec& w, size_t& lo, size_t& hi)
{
    lo = w.size(); hi = 0;
    for (size_t i = 0; i < w.size(); ++i) if (w[i] > 0) { if (lo == w.size()) lo = i; hi = i + 1; }
    return lo < w.size();
}
Vec timeAxis (size_t n, double tSeg0, double sr) { Vec t (n); for (size_t i = 0; i < n; ++i) t[i] = tSeg0 + (double) i / sr; return t; }
} // namespace

namespace
{
/** A voice on its own: how far (cents) the note's line wanders from where it was over the last 0.6 s (the room's ring-out lags the voice by that much),
    so the band round each overtone can hold the ring-out too. One figure for the whole note (the 95th percentile). */
double loneSpreadCents (const Vec& ts, const Vec& f0)
{
    const size_t n = f0.size();
    if (n < 2) return 0.0;
    Vec c (n); for (size_t i = 0; i < n; ++i) c[i] = 1200.0 * std::log2 (std::max (1.0, f0[i]));
    Vec spread;
    for (size_t i = 0; i < n; ++i)
    {
        if (ts[i] - ts.front() < 0.2) continue;                                       // (the first moment of a note is often a scoop: not its wobble)
        double sp = 0;
        for (size_t j = i; j-- > 0 && ts[i] - ts[j] <= 0.6;) if (ts[j] - ts.front() >= 0.2) sp = std::max (sp, std::abs (c[j] - c[i]));
        spread.push_back (sp);
    }
    return spread.empty() ? 0.0 : percentile (spread, 95.0);
}

/** A voice on its own, before its band is made as wide as its wobble: is there another sound close by that the band would catch? The sound round the note's
    line is looked at (its overtones 1..4, each window of 0.25 s, all channels, in cents from the line): the note and its room make one hump round 0; another
    voice makes a second hump further out. The band is then kept short of that hump. Returns the half-width (cents) the band may have, at most 'wantCents'. */
double loneBandLimit (const std::vector<Vec>& x, int sr, const Vec& fTrack, size_t lo, size_t hi, double wantCents, double ownSpread)
{
    const size_t m = hi - lo, nch = x.size();
    if (m < (size_t) (0.3 * sr)) return wantCents;
    const Vec fSeg (fTrack.begin() + (long) lo, fTrack.begin() + (long) hi);
    const Vec phi = cumPhase (fSeg, sr);
    const double fmean = median (fSeg);
    const double span = 160.0;                                                         // cents either side looked at
    const int nb = 161; Vec prof (nb, 0.0);                                             // 2 c per bin
    const size_t W = (size_t) (0.25 * sr), hop = W / 2;
    const Vec win = hanning (W);
    const size_t nfft = nextFastLen (W * 4);
    CVec buf (nfft);
    for (int h = 1; h <= 4; ++h)
    {
        if ((double) h * fmean * std::pow (2.0, span / 1200.0) > 0.45 * sr) break;
        const CVec base = phasor (phi, h, -1.0);
        for (size_t c = 0; c < nch; ++c)
            for (size_t st = 0; st + W <= m; st += hop)
            {
                std::fill (buf.begin(), buf.end(), cplx (0, 0));
                for (size_t k = 0; k < W; ++k) buf[k] = x[c][lo + st + k] * base[st + k] * win[k];
                fftComplex (buf, false);
                for (int b = 0; b < nb; ++b)
                {
                    const double cents = -span + 2.0 * b;
                    const double offHz = (double) h * fmean * (std::pow (2.0, cents / 1200.0) - 1.0);
                    long k = (long) std::llround (offHz / sr * (double) nfft); k = ((k % (long) nfft) + (long) nfft) % (long) nfft;
                    prof[(size_t) b] += std::norm (buf[(size_t) k]) / (double) h;          // (the lower overtones count most)
                }
            }
    }
    const double own = maxOf (prof); if (own <= 0) return wantCents;
    const int centre = nb / 2;
    // out from the line on each side, the sound should only fall away. Where it turns UP again (by 2.5 dB over what it had fallen to) another sound
    // starts: the band stops before it. (The note's own hump is smeared by its vibrato, so this is read on the smoothed profile.)
    Vec sm (nb); for (int b = 0; b < nb; ++b) { double acc = 0; int cnt = 0; for (int q = std::max (0, b - 2); q <= std::min (nb - 1, b + 2); ++q) { acc += prof[(size_t) q]; ++cnt; } sm[(size_t) b] = acc / cnt; }
    double limit = wantCents;
    for (int side = -1; side <= 1; side += 2)
    {
        double runMin = sm[(size_t) centre]; int minAt = centre;
        for (int b = centre + side; b > 0 && b < nb - 1 && std::abs (-span + 2.0 * b) <= wantCents + 30.0; b += side)
        {
            if (sm[(size_t) b] < runMin) { runMin = sm[(size_t) b]; minAt = b; }
            else if (sm[(size_t) b] > runMin * 1.78 && sm[(size_t) b] > 0.05 * own) { limit = std::min (limit, std::abs (-span + 2.0 * minAt) - 4.0); break; }
        }
    }
    // and beyond the note's own hump it must be quiet (under -15 dB): where other sound lies there, whatever it is, the band stays close to the hump
    for (int side = -1; side <= 1; side += 2)
    {
        double loudest = 0;
        for (int b = centre + side * (int) std::ceil ((ownSpread + 15.0) / 2.0); b >= 0 && b < nb && std::abs (-span + 2.0 * b) <= ownSpread + 60.0; b += side) loudest = std::max (loudest, sm[(size_t) b]);
        if (loudest > 0.03 * own) limit = std::min (limit, ownSpread + 5.0);
    }
    return std::max (limit, 15.0);
}

/** Where the correction of a voice on its own should stop: the room's ring-out after the box is followed (the sound round the last pitch, its overtones 1..3,
    on all channels) until it has died away (40 dB down), or something new starts there (the band's level turns up again), or tailMax. Returns the time the
    fade-out should END (at least tEnd + hold + fadeOut). */
double ringOutEnd (const std::vector<Vec>& x, int sr, double t0, const Vec& fTrack, double tEnd, double hold, double& fadeOut, double tailMax, double flat1, double roll1)
{
    const size_t m = x[0].size(), nch = x.size();
    const double tLast = t0 + (double) (m - 1) / sr;
    const double least = tEnd + hold + fadeOut;
    if (tailMax <= 0.0 || least >= tLast) return std::min (least, tLast);
    const Vec phi = cumPhase (fTrack, sr);
    Vec env (m, 0.0), all (m, 0.0);
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < m; ++k) all[k] += x[c][k] * x[c][k];
    for (int h = 1; h <= 3; ++h)
    {
        const CVec base = phasor (phi, h, -1.0);
        for (size_t c = 0; c < nch; ++c)
        {
            CVec z (m); for (size_t k = 0; k < m; ++k) z[k] = x[c][k] * base[k];
            const CVec a = lowpassFlat (z, sr, flat1 * h, roll1 * h);
            for (size_t k = 0; k < m; ++k) env[k] += std::norm (a[k]);
        }
    }
    auto smooth = [&] (Vec& v)
    {   // smoothed over 60 ms (a running average, twice: a gaussianFilter1d this wide would take seconds)
        const size_t half = (size_t) (0.03 * sr);
        for (int pass = 0; pass < 2; ++pass)
        {
            Vec cs (m + 1, 0.0); for (size_t k = 0; k < m; ++k) cs[k + 1] = cs[k] + v[k];
            for (size_t k = 0; k < m; ++k) { const size_t a = k > half ? k - half : 0, b = std::min (m, k + half + 1); v[k] = (cs[b] - cs[a]) / (double) (b - a); }
        }
    };
    smooth (env); smooth (all);
    auto at = [&] (double t) { return (size_t) std::min<double> ((double) m - 1, std::max (0.0, (t - t0) * sr)); };
    double e0 = 0.0, a0 = 0.0; for (size_t k = at (tEnd - 0.1); k <= at (tEnd); ++k) { e0 = std::max (e0, env[k]); a0 = std::max (a0, all[k]); }
    if (e0 <= 0.0) return least;
    // the singer moving straight on to the next note (legato): the sound leaves this pitch while the voice goes on. Then there is no ring-out to follow,
    // and holding the correction would catch the glide to the next note: the correction fades out as the voice leaves, over the box's last moment instead
    const double share0 = e0 / std::max (a0, 1e-30);                                         // how much of the sound was on this pitch at the end of the box
    for (size_t k = at (tEnd + 0.03); k <= at (tEnd + 0.3) && k < m; ++k)
        if (all[k] > 0.1 * a0 && env[k] < 0.2 * share0 * all[k])                               // the voice goes on (within 10 dB), but no longer on this pitch
        {
            const double tau = t0 + (double) k / sr - tEnd;
            fadeOut = std::max (0.04, tau);
            return tEnd + tau;
        }
    double runMin = e0, tStop = std::min (tLast, tEnd + tailMax);
    for (size_t k = at (tEnd + hold); k < m; ++k)
    {
        const double t = t0 + (double) k / sr;
        if (t >= tEnd + tailMax) { tStop = t; break; }
        runMin = std::min (runMin, env[k]);
        if (env[k] < e0 * 1e-4) { tStop = t; break; }                                         // the ring-out has died away
        if (env[k] > 2.5 * runMin && env[k] > e0 * 1e-3) { tStop = t; break; }                // something new has started on this pitch: stop before it
        if (env[k] > e0 * std::pow (10.0, -0.8 * (t - tEnd - hold))) { tStop = t; break; }   // not dying away (at least 8 dB a second): other voices go on there, not a ring-out
    }
    return std::max (least, std::min (tLast, tStop));
}
} // namespace

Block retuneSegment (const Block& seg, int sr, double tSeg0, const Vec& ts, const Vec& f0, const Vec& off, const Params& p, const Hooks& hooks, const std::vector<Vec>& nb)
{
    const size_t nch = seg.size(), n = seg[0].size();
    const Vec t = timeAxis (n, tSeg0, sr);
    const Vec f0t = interp (t, ts, f0), offt = interp (t, ts, off);
    Vec f0n (n);
    for (size_t i = 0; i < n; ++i) f0n[i] = f0t[i] * std::pow (2.0, offt[i] / 1200.0);
    const double fi = 0.15, hold = p.hold;
    double fo = p.fadeOut;
    const bool lone = p.oneVoice || (p.lone && nb.empty());           // ("One voice only": whatever is in nb is not kept apart)
    // a voice on its own: flat bands as wide as the note's own wobble (plus a margin), so the room's ring-out moves with the note
    const double f0mean = median (f0), f0min = minOf (f0);
    const double ownSpread = lone ? loneSpreadCents (ts, f0) : 0.0;
    double halfCents = lone ? clip (ownSpread + p.loneMargin, 35.0, p.loneMaxHalf) : 0.0;
    if (lone && ! p.oneVoice)
    {   // another sound close by that the finder did not take for a voice (not steady enough): the band stops short of it ("One voice only": there is none)
        size_t lo0, hi0; Vec w0 (n, 0.0);
        const double from = ts.back() - ts.front() > 0.6 ? ts.front() + 0.2 : ts.front();        // (not the scoop at the start, when the note is long enough to spare it)
        for (size_t i = 0; i < n; ++i) if (t[i] >= from && t[i] <= ts.back()) w0[i] = 1.0;
        if (activeRange (w0, lo0, hi0))
        {
            std::vector<Vec> xs (nch, Vec (n)); for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < n; ++k) xs[c][k] = (double) seg[c][k];
            halfCents = loneBandLimit (xs, sr, f0t, lo0, hi0, halfCents, ownSpread);
        }
    }
    double flat1 = lone ? f0mean * (std::pow (2.0, halfCents / 1200.0) - 1.0) : 0.0;
    double roll1 = lone ? std::max (4.0, 0.35 * flat1) : 0.0;
    if (lone && flat1 + roll1 > 0.45 * f0min) { const double k = 0.45 * f0min / (flat1 + roll1); flat1 *= k; roll1 *= k; }     // never reaching the next overtone
    // the band of overtone h, in Hz either side of it: h times the band of the note, but never reaching half way to the next overtone (the bands grow with h, the
    // overtones do not get further apart, so high up they would overlap and some sound would be moved twice)
    auto loneBand = [&] (int h, double& fl, double& rl)
    {
        fl = flat1 * h; rl = roll1 * h;
        if (fl + rl > 0.45 * f0min) { const double k = 0.45 * f0min / (fl + rl); fl *= k; rl *= k; }
    };
    // the window of the correction: a fade in before the box, the box, a hold after it, then the fade out (for a voice on its own: after the ring-out)
    double tFadeEnd = ts.back() + hold + fo;
    std::vector<Vec> xAll;
    if (lone && p.tailMax > 0.0)
    {
        hooks.report ("Following the ring-out", 0.05);
        xAll.assign (nch, Vec (n));
        for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < n; ++k) xAll[c][k] = (double) seg[c][k];
        tFadeEnd = ringOutEnd (xAll, sr, tSeg0, f0t, ts.back(), hold, fo, p.tailMax, flat1, roll1);       // (fo may come back shorter: a legato note)
        xAll.clear();
    }
    Vec w (n);
    for (size_t i = 0; i < n; ++i)
    {
        double v = clip ((t[i] - (ts.front() - fi)) / fi, 0, 1) * clip ((tFadeEnd - t[i]) / fo, 0, 1);
        const double s = std::sin (0.5 * kPi * v); w[i] = s * s;
    }
    size_t lo, hi;
    if (! activeRange (w, lo, hi)) return seg;
    const size_t m = hi - lo;
    const Vec phi0 = cumPhase (Vec (f0t.begin() + (long) lo, f0t.begin() + (long) hi), sr), phi1 = cumPhase (Vec (f0n.begin() + (long) lo, f0n.begin() + (long) hi), sr);
    std::vector<Vec> x (nch, Vec (m)), removed (nch, Vec (m, 0.0)), added (nch, Vec (m, 0.0));
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < m; ++k) x[c][k] = (double) seg[c][lo + k];
    std::vector<Vec> others;                                          // the voices beside the note, as lines over the samples of this stretch
    const Vec tLo (t.begin() + (long) lo, t.begin() + (long) hi);
    if (! p.oneVoice) for (const auto& line : nb) if (line.size() == ts.size()) others.push_back (interp (tLo, ts, line));
    const double fmaxNew = maxOf (Vec (f0n.begin() + (long) lo, f0n.begin() + (long) hi));
    if (! others.empty())
    {
        // with other voices close by, the note and the voices are fitted together, window by window, so only the note's own sound is moved
        hooks.report ("Correcting the note", 0.2);
        const Vec fOld (f0t.begin() + (long) lo, f0t.begin() + (long) hi), fNew (f0n.begin() + (long) lo, f0n.begin() + (long) hi);
        std::vector<const Vec*> fp = { &fOld }, fn = { &fNew };
        for (auto& o : others) { fp.push_back (&o); fn.push_back (nullptr); }
        separateAndMove (x, sr, fp, fn, p.nharm, p.sepWindow, removed, added, hooks);
        for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < m; ++k) { removed[c][k] *= w[lo + k]; added[c][k] *= w[lo + k]; }
    }
    else
    {
    // a voice on its own: its upper overtones are moved too (the first nharm only left the upper ones - the singer's ring, 2.5-4 kHz - at the old pitch,
    // which sounded like a second singer a little off the first). "One voice only": every overtone up to loneTopHz. Found alone without being told: up to
    // the overtone where its band would fill all the room between overtones (above that it would also move whatever else sounds there - another voice's
    // or an accompaniment's upper overtones), at least nharm.
    int hTop = p.nharm;
    if (lone)
    {
        const int hMax = (int) std::floor (p.loneTopHz / std::max (1.0, fmaxNew));
        int hFill = hMax;
        if (! p.oneVoice && flat1 + roll1 > 0.0) hFill = (int) std::floor (0.45 * f0min / (flat1 + roll1));
        hTop = std::max (p.nharm, std::min (hMax, hFill));
    }
    for (int h = 1; h <= hTop; ++h)
    {
        hooks.check();
        hooks.report ("Correcting the note", 0.1 + 0.6 * (double) (h - 1) / (double) hTop);
        if (h * fmaxNew > 0.45 * sr) break;
        const CVec base = phasor (phi0, h, -1.0), up1 = phasor (phi1, h, 1.0);
        const double bw = p.bw * std::sqrt ((double) h);
        double flH = 0.0, rlH = 0.0;
        if (lone) loneBand (h, flH, rlH);
        parallelFor (nch, [&] (size_t c)
        {
            CVec z (m);
            for (size_t k = 0; k < m; ++k) z[k] = x[c][k] * base[k];
            const CVec a = lone ? lowpassFlat (z, sr, flH, rlH) : lowpassGaussian (z, sr, bw);
            for (size_t k = 0; k < m; ++k)
            {
                const cplx ak = a[k] * w[lo + k];
                removed[c][k] += (ak * std::conj (base[k])).real();
                added[c][k] += (ak * up1[k]).real();
            }
        });
    }
    }
    Vec g;
    if (p.match)
    {
        hooks.report ("Matching the level", 0.75);
        const double bandLo = std::max (40.0, 0.8 * minOf (f0)), bandHi = std::min (0.45 * sr, 2.2 * maxOf (f0));
        g = bandEnergyGain (x, removed, added, sr, bandLo, bandHi);
    }
    Block out = seg;
    for (size_t c = 0; c < nch; ++c)
        for (size_t k = 0; k < m; ++k)
            out[c][lo + k] = (float) (x[c][k] - removed[c][k] + added[c][k] * (p.match ? g[k] : 1.0));
    return out;
}

Block soloSegment (const Block& seg, int sr, double tSeg0, const Vec& ts, const Vec& fPts, const Params& p, double tA, double tB, double fade)
{
    const size_t nch = seg.size(), n = seg[0].size();
    const Vec t = timeAxis (n, tSeg0, sr);
    Vec m (n);
    for (size_t i = 0; i < n; ++i) { const double v = clip ((t[i] - tA) / fade, 0, 1) * clip ((tB - t[i]) / fade, 0, 1); const double s = std::sin (0.5 * kPi * v); m[i] = s * s; }
    size_t lo, hi;
    if (! activeRange (m, lo, hi)) return seg;
    const size_t len = hi - lo;
    Vec tsl (t.begin() + (long) lo, t.begin() + (long) hi);
    const Vec ft = interp (tsl, ts, fPts);
    const Vec phi = cumPhase (ft, sr);
    std::vector<Vec> x (nch, Vec (len)), solo (nch, Vec (len, 0.0));
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) x[c][k] = (double) seg[c][lo + k];
    const double ftMax = maxOf (ft);
    for (int h = 1; h <= p.nharm; ++h)
    {
        if (h * ftMax > 0.45 * sr) break;
        const CVec base = phasor (phi, h, -1.0);
        const double bw = p.bw * std::sqrt ((double) h);
        parallelFor (nch, [&] (size_t c)
        {
            CVec z (len); for (size_t k = 0; k < len; ++k) z[k] = x[c][k] * base[k];
            const CVec a = lowpassGaussian (z, sr, bw);
            for (size_t k = 0; k < len; ++k) solo[c][k] += (a[k] * std::conj (base[k])).real();
        });
    }
    double sf = 0, ss = 0; size_t cnt = 0;
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) { const double mm = m[lo + k]; sf += (x[c][k] * mm) * (x[c][k] * mm); ss += (solo[c][k] * mm) * (solo[c][k] * mm); ++cnt; }
    const double rf = std::sqrt (sf / (double) cnt) + 1e-12, rs = std::sqrt (ss / (double) cnt) + 1e-12;
    const double g = clip (0.7 * rf / rs, 1.0, 4.0);
    Block out = seg;
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) { const double mm = m[lo + k]; out[c][lo + k] = (float) (x[c][k] * (1 - mm) + solo[c][k] * g * mm); }
    return out;
}

LevelReport levelReport (const Block& before, const Block& after, int sr, double tSeg0, double tA, double tB, double bandLo, double bandHi)
{
    const auto sos = butterBandpass (4, bandLo, bandHi, sr);
    const size_t n = before[0].size(), nch = before.size();
    Vec mb (n, 0.0), ma (n, 0.0);
    for (size_t c = 0; c < nch; ++c) for (size_t i = 0; i < n; ++i) { mb[i] += (double) before[c][i] / (double) nch; ma[i] += (double) after[c][i] / (double) nch; }
    const Vec xb = sosfilt (sos, mb), yb = sosfilt (sos, ma);
    const size_t fr = (size_t) (0.05 * sr);
    Vec ts, d;
    for (double t : arange (tA, tB, 0.025))
    {
        const long i = (long) ((t - tSeg0) * sr);
        if (i < 0 || (size_t) i + fr > xb.size()) continue;
        double ex = 0, ey = 0;
        for (size_t k = 0; k < fr; ++k) { ex += xb[(size_t) i + k] * xb[(size_t) i + k]; ey += yb[(size_t) i + k] * yb[(size_t) i + k]; }
        ex = std::sqrt (ex / (double) fr) + 1e-12; ey = std::sqrt (ey / (double) fr) + 1e-12;
        ts.push_back (t); d.push_back (20.0 * std::log10 (ey / ex));
    }
    LevelReport r;
    if (d.empty()) return r;
    const size_t imax = (size_t) (std::max_element (d.begin(), d.end()) - d.begin()), imin = (size_t) (std::min_element (d.begin(), d.end()) - d.begin());
    r.rise = d[imax]; r.riseT = ts[imax]; r.dip = d[imin]; r.dipT = ts[imin];
    const double mean = meanOf (d); double v = 0; for (double x : d) v += (x - mean) * (x - mean);
    r.stdev = std::sqrt (v / (double) d.size());
    return r;
}

// ================================================================================================== Erase ReBrush
std::pair<double, double> brushRegion (const Brush& b)
{
    double lo = 1e300, hi = -1e300;
    for (auto& p : b.pts) { lo = std::min (lo, p.first); hi = std::max (hi, p.first); }
    return { lo - b.rt - 0.1, hi + b.rt + 0.1 };
}

Block brushSegment (const Block& seg, int sr, double tSeg0, const Brush& b, int nharm, const Hooks& hooks)
{
    auto pts = b.pts;
    std::sort (pts.begin(), pts.end());
    const double rt = std::max (0.02, b.rt), rc = std::max (5.0, b.rc), amt = clip (b.amount, 0, 1);
    const size_t nch = seg.size(), n = seg[0].size();
    const Vec t = timeAxis (n, tSeg0, sr);
    Vec pt, pf;
    for (size_t i = 0; i < pts.size(); ++i) { pt.push_back (pts[i].first + (double) i * 1e-9); pf.push_back (std::log2 (pts[i].second)); }
    Vec cen;
    if (pt.back() > pt.front()) cen = arange (pt.front(), pt.back() + 1e-9, rt / 4); else cen = { pt.front() };
    Vec wd (n, 0.0);
    for (double tc : cen)
    {
        const long j0 = std::max<long> (0, (long) ((tc - rt - tSeg0) * sr)), j1 = std::min<long> ((long) n, (long) ((tc + rt - tSeg0) * sr) + 1);
        for (long j = j0; j < j1; ++j) { const double u = (t[(size_t) j] - tc) / rt; wd[(size_t) j] = std::max (wd[(size_t) j], std::sqrt (clip (1 - u * u, 0, 1))); }
    }
    Vec m (n);
    for (size_t i = 0; i < n; ++i) { const double s = std::sin (0.5 * kPi * clip (wd[i], 0, 1)); m[i] = s * s * amt; }
    size_t lo = n, hi = 0;
    for (size_t i = 0; i < n; ++i) if (m[i] > 1e-4) { if (lo == n) lo = i; hi = i + 1; }
    if (lo == n) return seg;
    const size_t len = hi - lo;
    Vec tsl (t.begin() + (long) lo, t.begin() + (long) hi);
    Vec ft = interp (tsl, pt, pf); for (auto& v : ft) v = std::pow (2.0, v);
    const Vec phi = cumPhase (ft, sr);
    std::vector<Vec> x (nch, Vec (len)), gone (nch, Vec (len, 0.0));
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) x[c][k] = (double) seg[c][lo + k];
    const int top = b.harm ? nharm : 1;
    const double kk = std::pow (2.0, rc / 1200.0) - 1.0, ftMax = maxOf (ft), ftMean = meanOf (ft);
    for (int h = 1; h <= top; ++h)
    {
        hooks.check();
        if (h * ftMax > 0.45 * sr) break;
        const CVec base = phasor (phi, h, -1.0);
        const double bw = std::max (1.5, 2.0 * h * ftMean * kk);
        parallelFor (nch, [&] (size_t c)
        {
            CVec z (len); for (size_t k = 0; k < len; ++k) z[k] = x[c][k] * base[k];
            const CVec a = lowpassGaussian (z, sr, bw);
            for (size_t k = 0; k < len; ++k) gone[c][k] += (a[k] * std::conj (base[k])).real();
        });
    }
    Block out = seg;
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) out[c][lo + k] = (float) (x[c][k] - gone[c][k] * m[lo + k]);
    return out;
}

// ================================================================================================== Section ReFinement
VoiceInputs voiceInputs (const std::vector<Span>& seg, int sr, double fT, const Vec& visGainDb)
{
    const Tracking tk = trackingInputs (seg, sr, fT, 2.0 * fT, visGainDb);
    VoiceInputs v; v.ws = tk.weights;
    for (int c : tk.channels) { std::vector<float> x2; v.sr = decimateTo (seg[(size_t) c].p, seg[(size_t) c].n, sr, x2); v.chans.emplace_back (x2.begin(), x2.end()); }
    return v;
}

namespace
{
void voicePeaks (const VoiceInputs& in, double tSeg0, double t0, double t1, double fT, int H, double span, double win, double hop, Vec& tsOut, std::vector<std::vector<VoicePeak>>& out)
{
    const double sr = in.sr;
    const size_t n = (size_t) (win * sr);
    const Vec w = hanning (n);
    tsOut = arange (t0 + win / 2, t1 - win / 2 + 1e-9, hop);
    const double k = std::pow (2.0, span / 1200.0);
    const size_t nfft = n * 8;
    out.clear();
    Vec buf (n), mag;
    for (double tc : tsOut)
    {
        std::vector<VoicePeak> all;
        const long i = (long) ((tc - win / 2 - tSeg0) * sr);
        if (i < 0 || (size_t) i + n > in.chans[0].size()) { out.push_back (all); continue; }
        Vec F (nfft / 2 + 1, 0.0);
        for (size_t c = 0; c < in.chans.size(); ++c)
        {
            for (size_t q = 0; q < n; ++q) buf[q] = in.chans[c][(size_t) i + q] * w[q];
            rfftMagnitude (buf.data(), n, nfft, mag);
            for (size_t q = 0; q < F.size(); ++q) F[q] += in.ws[c] * mag[q];
        }
        const double dfr = sr / (double) nfft;
        for (int h = 1; h <= H; ++h)
        {
            const double lo = h * fT / k, hi = h * fT * k;
            if (hi > 0.45 * sr) break;
            Vec Fm, frm;
            for (size_t q = 0; q < F.size(); ++q) { const double fq = (double) q * dfr; if (fq > lo && fq < hi) { Fm.push_back (20.0 * std::log10 (F[q] + 1e-9)); frm.push_back (fq); } }
            if (Fm.size() < 5) continue;
            for (int p : findPeaks (Fm, 6.0))
            {
                double d = 0.0;
                if (p > 0 && p < (int) Fm.size() - 1) { const double a = Fm[(size_t) p - 1], b = Fm[(size_t) p], c = Fm[(size_t) p + 1]; d = 0.5 * (a - c) / (a - 2 * b + c + 1e-12); }
                const double f = frm[(size_t) p] + d * (frm[1] - frm[0]);
                all.push_back ({ 1200.0 * std::log2 (f / (h * fT)), std::pow (10.0, Fm[(size_t) p] / 20.0), h });
            }
        }
        out.push_back (all);
    }
}

double sepHz (int h, double fT, const std::vector<Vec>& C, size_t j)
{
    double s = 1e9;
    for (size_t i = 0; i < C.size(); ++i)
        if (i != j) for (size_t k = 0; k < C[j].size(); ++k) s = std::min (s, h * fT * std::abs (std::pow (2.0, C[i][k] / 1200.0) - std::pow (2.0, C[j][k] / 1200.0)));
    return s;
}

/** The frequency track of one voice that follows its vibrato; false when no overtone is clear of the other voices. */
bool refineVoice (const VoiceInputs& in, double tSeg0, double fT, const Vec& ts, const std::vector<Vec>& C, size_t j, double tA, double tB, Vec& fOut, Vec& ttOut,
                  int iters = 6, double smoothS = 0.012, double bwMin = 9.0, double bwMax = 30.0, int H = 14)
{
    const double sr = in.sr;
    const long i0 = std::max<long> (0, (long) ((tA - tSeg0) * sr)), i1 = std::min<long> ((long) in.chans[0].size(), (long) ((tB - tSeg0) * sr));
    if (i1 <= i0) return false;
    const size_t n = (size_t) (i1 - i0);
    Vec tt (n); for (size_t k = 0; k < n; ++k) tt[k] = tSeg0 + (double) (i0 + (long) k) / sr;
    std::vector<Vec> sigs; for (auto& c : in.chans) sigs.emplace_back (c.begin() + i0, c.begin() + i1);
    const Vec cj = interp (tt, ts, C[j]);
    Vec f0 (n); for (size_t k = 0; k < n; ++k) f0[k] = fT * std::pow (2.0, cj[k] / 1200.0);
    const int dec = std::max (1, (int) ((int) sr / 400));
    Vec td; for (size_t k = 0; k < n; k += (size_t) dec) td.push_back (tt[k]);
    const double rate = sr / dec;
    std::vector<std::pair<int, double>> hs;
    const double f0max = maxOf (f0);
    for (int h = 1; h <= H; ++h)
    {
        if (h * f0max * 1.03 > 0.45 * sr) break;
        const double bw = std::min (bwMax, 0.6 * sepHz (h, fT, C, j));
        if (bw >= bwMin) hs.emplace_back (h, bw);
    }
    if (hs.empty()) return false;
    for (int it = 0; it < iters; ++it)
    {
        const Vec phi = cumPhase (f0, sr);
        Vec num (td.size(), 0.0), den (td.size(), 0.0);
        for (auto& hb : hs)
        {
            const CVec base = phasor (phi, hb.first, -1.0);
            std::vector<Vec> pn (sigs.size(), Vec (td.size(), 0.0)), pd (sigs.size(), Vec (td.size(), 0.0));
            parallelFor (sigs.size(), [&] (size_t c)
            {
                CVec z (n); for (size_t k = 0; k < n; ++k) z[k] = sigs[c][k] * base[k];
                const CVec lp = lowpassGaussian (z, sr, hb.second);
                Vec ang (td.size()), p2 (td.size());
                for (size_t k = 0; k < td.size(); ++k) { const cplx a = lp[k * (size_t) dec]; ang[k] = std::atan2 (a.imag(), a.real()); p2[k] = std::norm (a); }
                const Vec g = gradient (unwrap (ang));
                const Vec wgt = gaussianFilter1d (p2, 0.05 * rate);
                for (size_t k = 0; k < td.size(); ++k) { const double inst = g[k] * rate / (2.0 * kPi), wg = wgt[k] * in.ws[c] * in.ws[c]; pn[c][k] = wg * inst / hb.first; pd[c][k] = wg; }
            });
            for (size_t c = 0; c < sigs.size(); ++c) for (size_t k = 0; k < td.size(); ++k) { num[k] += pn[c][k]; den[k] += pd[c][k]; }
        }
        Vec q (td.size()); for (size_t k = 0; k < td.size(); ++k) q[k] = num[k] / (den[k] + 1e-30);
        const Vec dlt = gaussianFilter1d (q, smoothS * rate);
        const Vec add = interp (tt, td, dlt);
        for (size_t k = 0; k < n; ++k) f0[k] += add[k];
    }
    fOut = f0; ttOut = tt;
    return true;
}
} // namespace

VoiceScan findVoices (const VoiceInputs& in, double tSeg0, double t0, double t1, double fT, int H, double span, double win, double hop)
{
    VoiceScan s;
    voicePeaks (in, tSeg0, t0, t1, fT, H, span, win, hop, s.ts, s.peaks);
    const size_t ng = (size_t) std::ceil ((2 * span + 1e-9) / 0.5 + 1e-9) ;
    s.grid.clear(); for (size_t k = 0; k < ng; ++k) s.grid.push_back (-span + (double) k * 0.5);
    s.hist.assign (s.grid.size(), 0.0);
    for (auto& pks : s.peaks)
        for (auto& pk : pks)
        {
            const double sgm = clip (3.0 / pk.partial, 1.2, 3.0);
            for (size_t g = 0; g < s.grid.size(); ++g) { const double z = (s.grid[g] - pk.cents) / sgm; s.hist[g] += pk.amp * std::exp (-0.5 * z * z); }
        }
    const double hmax = maxOf (s.hist);
    if (hmax <= 0) return s;
    for (int p : findPeaks (s.hist, 0.12 * hmax, -1e300, 8.0)) s.centres.push_back (s.grid[(size_t) p]);
    return s;
}

std::vector<Vec> trackVoices (const Vec& ts, const std::vector<std::vector<VoicePeak>>& peaks, const Vec& cl, double minsep)
{
    const size_t nv = cl.size(), nt = ts.size();
    std::vector<Vec> C (nv, Vec (nt, std::nan ("")));
    Vec cur = cl;
    for (size_t k = 0; k < peaks.size(); ++k)
        for (size_t j = 0; j < nv; ++j)
        {
            double sep = 40.0;
            if (nv > 1) { sep = 1e300; for (size_t i = 0; i < nv; ++i) if (i != j) sep = std::min (sep, std::abs (cur[i] - cur[j])); }
            const double wdt = std::min (std::max (minsep, 0.45 * sep), 18.0);
            double num = 0, den = 0;
            for (auto& pk : peaks[k]) if (std::abs (pk.cents - cur[j]) < wdt) { num += pk.amp * pk.cents; den += pk.amp; }
            if (den > 0) { C[j][k] = num / den; cur[j] = 0.7 * cur[j] + 0.3 * C[j][k]; }
        }
    for (size_t j = 0; j < nv; ++j)
    {
        Vec tk, ck;
        for (size_t k = 0; k < nt; ++k) if (std::isfinite (C[j][k])) { tk.push_back (ts[k]); ck.push_back (C[j][k]); }
        if (tk.size() >= 2) C[j] = interp (ts, tk, ck); else C[j].assign (nt, cl[j]);
    }
    return C;
}

Vec VoiceSet::means() const { Vec m; for (auto& c : C) m.push_back (meanOf (c)); return m; }

VoiceSet detectVoices (const std::vector<Span>& seg, int sr, double tSeg0, double t0, double t1, int midi, double a4, const Vec& visGainDb, const Hooks& hooks)
{
    VoiceSet vs; vs.t0 = t0; vs.t1 = t1; vs.midi = midi; vs.a4 = a4; vs.fT = a4 * std::pow (2.0, (midi - 69) / 12.0);
    const double win = std::min (0.6, std::max (0.3, (t1 - t0) / 3.0));
    if (t1 - t0 < win + 0.1) throw std::runtime_error ("That note is too short to tell the voices apart (it needs about half a second or more).");
    hooks.report ("Finding the voices", 0.1);
    const VoiceInputs in = voiceInputs (seg, sr, vs.fT, visGainDb);
    const VoiceScan sc = findVoices (in, tSeg0, t0, t1, vs.fT, 8, 60.0, win, win / 4);
    if (sc.centres.empty()) throw std::runtime_error ("No steady voices were found round that note.");
    vs.ts = sc.ts;
    vs.C = trackVoices (sc.ts, sc.peaks, sc.centres);
    const double hmax = maxOf (sc.hist);
    for (double c : sc.centres)
    {
        size_t best = 0; double bd = 1e300;
        for (size_t g = 0; g < sc.grid.size(); ++g) if (std::abs (sc.grid[g] - c) < bd) { bd = std::abs (sc.grid[g] - c); best = g; }
        vs.strength.push_back (sc.hist[best] / hmax);
    }
    return vs;
}

Block retuneVoices (const Block& seg, int sr, double tSeg0, double fT, const Vec& ts, const std::vector<Vec>& C, const Vec& offs, double tA, double tB, const VoiceInputs& trk,
                    const Hooks& hooks, int H, double bwMax, double fade, double bwLow, bool jointFit)
{
    const size_t nch = seg.size(), n = seg[0].size();
    const Vec t = timeAxis (n, tSeg0, sr);
    Vec w (n);
    for (size_t i = 0; i < n; ++i) { const double v = clip ((t[i] - (tA - fade)) / fade, 0, 1) * clip (((tB + fade) - t[i]) / fade, 0, 1); const double s = std::sin (0.5 * kPi * v); w[i] = s * s; }
    size_t lo, hi;
    if (! activeRange (w, lo, hi)) return seg;
    const size_t len = hi - lo;
    std::vector<Vec> x (nch, Vec (len)), rem (nch, Vec (len, 0.0)), add (nch, Vec (len, 0.0));
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) x[c][k] = (double) seg[c][lo + k];
    const Vec tt (t.begin() + (long) lo, t.begin() + (long) hi);
    std::vector<size_t> todo;
    for (size_t j = 0; j < C.size(); ++j) if (std::abs (offs[j]) >= 0.5) todo.push_back (j);
    if (jointFit && ! todo.empty())
    {
        // every voice's line (refined where it can be), then one fit of ALL the voices together, window by window: the moved voices' own sound is taken
        // out and put back at the new pitch, the others' is left exactly as it was (the same fit the note correction uses when voices are found beside the note).
        // A narrow band per voice (the old way) could not hold a voice's vibrato without catching the voice beside it.
        std::vector<Vec> lines (C.size()), newLines (C.size());
        for (size_t j = 0; j < C.size(); ++j)
        {
            hooks.check();
            hooks.report ("Following the voices", 0.05 + 0.25 * (double) j / (double) C.size());
            Vec fj, ttj, f0 (len);
            if (! refineVoice (trk, tSeg0, fT, ts, C, j, tA, tB, fj, ttj)) { const Vec cj = interp (tt, ts, C[j]); for (size_t k = 0; k < len; ++k) f0[k] = fT * std::pow (2.0, cj[k] / 1200.0); }
            else f0 = interp (tt, ttj, fj);
            lines[j] = f0;
            if (std::abs (offs[j]) >= 0.5) { newLines[j].resize (len); for (size_t k = 0; k < len; ++k) newLines[j][k] = f0[k] * std::pow (2.0, offs[j] / 1200.0); }
        }
        std::vector<const Vec*> fp, fn;
        for (size_t j = 0; j < C.size(); ++j) { fp.push_back (&lines[j]); fn.push_back (newLines[j].empty() ? nullptr : &newLines[j]); }
        hooks.report ("Moving the voices", 0.3);
        separateAndMove (x, sr, fp, fn, H, 0.2, rem, add, hooks);
        for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) { rem[c][k] *= w[lo + k]; add[c][k] *= w[lo + k]; }
        todo.clear();
    }
    for (size_t nd = 0; nd < todo.size(); ++nd)
    {
        hooks.check();
        const size_t j = todo[nd];
        hooks.report ("Moving the voices", 0.1 + 0.8 * (double) nd / (double) std::max<size_t> (1, todo.size()));
        Vec fj, ttj;
        Vec f0 (len);
        if (! refineVoice (trk, tSeg0, fT, ts, C, j, tA, tB, fj, ttj)) { const Vec cj = interp (tt, ts, C[j]); for (size_t k = 0; k < len; ++k) f0[k] = fT * std::pow (2.0, cj[k] / 1200.0); }
        else f0 = interp (tt, ttj, fj);
        Vec f1 (len); for (size_t k = 0; k < len; ++k) f1[k] = f0[k] * std::pow (2.0, offs[j] / 1200.0);
        const Vec p0 = cumPhase (f0, sr), p1 = cumPhase (f1, sr);
        const double f1max = maxOf (f1);
        for (int h = 1; h <= H; ++h)
        {
            if (h * f1max > 0.45 * sr) break;
            const double bw = std::min (bwMax * std::sqrt ((double) h), 0.7 * sepHz (h, fT, C, j));
            if (bw < bwLow) continue;
            const CVec base = phasor (p0, h, -1.0), up = phasor (p1, h, 1.0);
            parallelFor (nch, [&] (size_t c)
            {
                CVec z (len); for (size_t k = 0; k < len; ++k) z[k] = x[c][k] * base[k];
                const CVec a = lowpassGaussian (z, sr, bw);
                for (size_t k = 0; k < len; ++k) { const cplx ak = a[k] * w[lo + k]; rem[c][k] += (ak * std::conj (base[k])).real(); add[c][k] += (ak * up[k]).real(); }
            });
        }
    }
    Block out = seg;
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < len; ++k) out[c][lo + k] = (float) (x[c][k] - rem[c][k] + add[c][k]);
    return out;
}

// ================================================================================================== Ensemble ReCentre
RefPoints refPoints (const std::vector<Span>& seg, int sr, double segStart, const Vec& visGainDb, int kmax, const Hooks& hooks)
{
    const double dur = (double) seg[0].n / sr;
    if (dur < 2.0) throw std::runtime_error ("Give it at least 2 seconds of music to measure the pitch from.");
    const size_t nch = seg.size();
    Vec w (nch, 1.0); for (size_t c = 0; c < nch && c < visGainDb.size(); ++c) w[c] = dbLin (visGainDb[c]);
    const auto d = decimateAll (seg, sr);
    const double sr2 = d.sr2;
    Vec en (nch);
    for (size_t c = 0; c < nch; ++c) { double s = 0; for (float v : d.x[c]) s += (double) v * (double) v; en[c] = s / (double) d.x[c].size() * w[c] * w[c]; }
    std::vector<size_t> order (nch); std::iota (order.begin(), order.end(), (size_t) 0);
    std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return en[a] < en[b]; });
    std::reverse (order.begin(), order.end());
    std::vector<size_t> top; for (size_t i = 0; i < order.size() && (int) i < kmax; ++i) if (en[order[i]] > 0) top.push_back (order[i]);
    if (top.empty()) throw std::runtime_error ("Every channel is turned down in the visual mixer.");
    const size_t N = pow2Round (sr2, 0.34), pad = 2;
    const Vec win = hanning (N);
    const double df = sr2 / (double) (N * pad);
    const size_t nb = N * pad / 2 + 1;
    std::vector<size_t> band; for (size_t q = 0; q < nb; ++q) { const double f = (double) q * df; if (f >= 60 && f <= 1100) band.push_back (q); }
    const size_t hop = (size_t) (0.1 * sr2);
    const long nfr = ((long) d.x[0].size() - (long) N) / (long) hop;
    struct Frame { double t; Vec cs, ws; };
    std::vector<Frame> frames;
    Vec buf (N), mag;
    for (long k = 0; k < std::max<long> (0, nfr); ++k)
    {
        const size_t i = (size_t) k * hop;
        Vec P (nb, 0.0);
        for (size_t c : top)
        {
            for (size_t q = 0; q < N; ++q) buf[q] = (double) d.x[c][i + q] * win[q];
            rfftMagnitude (buf.data(), N, N * pad, mag);
            for (size_t q = 0; q < nb; ++q) { const double v = w[c] * mag[q]; P[q] += v * v; }
        }
        Vec db (band.size()); for (size_t q = 0; q < band.size(); ++q) db[q] = 10.0 * std::log10 (P[band[q]] + 1e-18);
        const double mx = maxOf (db);
        Frame fr; fr.t = segStart + ((double) i + (double) N / 2.0) / sr2;
        for (int j : findPeaks (db, 10.0, mx - 45.0))
            if (j > 0 && j < (int) db.size() - 1)
            {
                const double a = db[(size_t) j - 1], b = db[(size_t) j], c2 = db[(size_t) j + 1];
                const double dd = 0.5 * (a - c2) / (a - 2 * b + c2 + 1e-12);
                const double f = (double) band[(size_t) j] * df + dd * df;
                fr.cs.push_back (1200.0 * std::log2 (f / 440.0));
                fr.ws.push_back (std::pow (10.0, (b - mx) / 20.0));
            }
        frames.push_back (std::move (fr));
        if (k % 50 == 0 && nfr > 0) hooks.report ("Measuring the choir's pitch", 0.05 + 0.85 * (double) k / (double) nfr);
        hooks.check();
    }
    RefPoints rp;
    for (size_t k = 1; k + 1 < frames.size(); ++k)
    {
        const auto& f = frames[k];
        if (f.cs.empty()) continue;
        const auto& pv = frames[k - 1].cs; const auto& nx = frames[k + 1].cs;
        if (pv.empty() || nx.empty()) continue;
        for (size_t q = 0; q < f.cs.size(); ++q)
        {
            double mp = 1e300, mn = 1e300;
            for (double v : pv) mp = std::min (mp, std::abs (v - f.cs[q]));
            for (double v : nx) mn = std::min (mn, std::abs (v - f.cs[q]));
            if (mp < 25 && mn < 25)
            {
                rp.T.push_back (f.t);
                double dd = std::fmod (f.cs[q] + 50.0, 100.0); if (dd < 0) dd += 100.0;
                rp.D.push_back (dd - 50.0);
                rp.W.push_back (f.ws[q]);
            }
        }
    }
    return rp;
}

RefStats refStats (const Vec& D, const Vec& W, int minN)
{
    RefStats st;
    if ((int) D.size() < minN) return st;
    Vec bins (100, 0.0);
    for (size_t i = 0; i < D.size(); ++i)
    {
        long idx = (long) roundEven (D[i] + 50.0) % 100; if (idx < 0) idx += 100;
        bins[(size_t) clip ((double) idx, 0, 99)] += W[i];
    }
    Vec sm (100, 0.0);
    for (int o = -30; o <= 30; ++o)
    {
        const double g = std::exp (-0.5 * ((double) o / 5.0) * ((double) o / 5.0));
        for (int i = 0; i < 100; ++i) sm[(size_t) i] += g * bins[(size_t) (((i - o) % 100 + 100) % 100)];       // np.roll (bins, o)
    }
    const double mode = (double) (std::max_element (sm.begin(), sm.end()) - sm.begin()) - 50.0;
    Vec dd, ww;
    for (size_t i = 0; i < D.size(); ++i)
    {
        double v = std::fmod (D[i] - mode + 50.0, 100.0); if (v < 0) v += 100.0; v -= 50.0;
        if (std::abs (v) < 20) { dd.push_back (v); ww.push_back (W[i]); }
    }
    if ((int) dd.size() < minN) return st;
    double sw = 0, sd = 0; for (size_t i = 0; i < dd.size(); ++i) { sw += ww[i]; sd += ww[i] * dd[i]; }
    const double meanOff = sd / sw;
    double mean = mode + meanOff;
    double var = 0; for (size_t i = 0; i < dd.size(); ++i) var += ww[i] * (dd[i] - meanOff) * (dd[i] - meanOff);
    st.spread = std::sqrt (var / sw);
    double m2 = std::fmod (mean + 50.0, 100.0); if (m2 < 0) m2 += 100.0; mean = m2 - 50.0;
    st.cents = mean; st.a4 = 440.0 * std::pow (2.0, mean / 1200.0); st.n = (int) dd.size(); st.ok = true;
    return st;
}

std::vector<RefSeriesPoint> refSeries (const RefPoints& rp)
{
    std::vector<RefSeriesPoint> ser;
    if (rp.T.empty()) return ser;
    const double tmax = maxOf (rp.T);
    for (double a = minOf (rp.T); a < tmax; a += 2.0)
    {
        Vec d, w;
        for (size_t i = 0; i < rp.T.size(); ++i) if (rp.T[i] >= a && rp.T[i] < a + 4.0) { d.push_back (rp.D[i]); w.push_back (rp.W[i]); }
        const auto r = refStats (d, w, 12);
        if (r.ok) ser.push_back ({ a + 2.0, r.cents, r.spread, r.n });
    }
    return ser;
}
} // namespace rehar
