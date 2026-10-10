#include "Engine.h"
#include <algorithm>
#include <cmath>
#include <numeric>

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
    const Vec base = gaussianFilter1d (pl.c, std::max (0.5, p.smooth / hop), Edge::Nearest);
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

/** The region [lo, hi) where w > 0. */
bool activeRange (const Vec& w, size_t& lo, size_t& hi)
{
    lo = w.size(); hi = 0;
    for (size_t i = 0; i < w.size(); ++i) if (w[i] > 0) { if (lo == w.size()) lo = i; hi = i + 1; }
    return lo < w.size();
}
Vec timeAxis (size_t n, double tSeg0, double sr) { Vec t (n); for (size_t i = 0; i < n; ++i) t[i] = tSeg0 + (double) i / sr; return t; }
} // namespace

Block retuneSegment (const Block& seg, int sr, double tSeg0, const Vec& ts, const Vec& f0, const Vec& off, const Params& p, const Hooks& hooks)
{
    const size_t nch = seg.size(), n = seg[0].size();
    const Vec t = timeAxis (n, tSeg0, sr);
    const Vec f0t = interp (t, ts, f0), offt = interp (t, ts, off);
    Vec f0n (n);
    for (size_t i = 0; i < n; ++i) f0n[i] = f0t[i] * std::pow (2.0, offt[i] / 1200.0);
    const double fi = 0.15, fo = p.fadeOut, hold = p.hold;
    Vec w (n);
    for (size_t i = 0; i < n; ++i)
    {
        double v = clip ((t[i] - (ts.front() - fi)) / fi, 0, 1) * clip (((ts.back() + hold + fo) - t[i]) / fo, 0, 1);
        const double s = std::sin (0.5 * kPi * v); w[i] = s * s;
    }
    size_t lo, hi;
    if (! activeRange (w, lo, hi)) return seg;
    const size_t m = hi - lo;
    const Vec phi0 = cumPhase (Vec (f0t.begin() + (long) lo, f0t.begin() + (long) hi), sr), phi1 = cumPhase (Vec (f0n.begin() + (long) lo, f0n.begin() + (long) hi), sr);
    std::vector<Vec> x (nch, Vec (m)), removed (nch, Vec (m, 0.0)), added (nch, Vec (m, 0.0));
    for (size_t c = 0; c < nch; ++c) for (size_t k = 0; k < m; ++k) x[c][k] = (double) seg[c][lo + k];
    const double fmaxNew = maxOf (Vec (f0n.begin() + (long) lo, f0n.begin() + (long) hi));
    for (int h = 1; h <= p.nharm; ++h)
    {
        hooks.check();
        hooks.report ("Correcting the note", 0.1 + 0.6 * (double) (h - 1) / (double) p.nharm);
        if (h * fmaxNew > 0.45 * sr) break;
        const CVec base = phasor (phi0, h, -1.0), up1 = phasor (phi1, h, 1.0);
        const double bw = p.bw * std::sqrt ((double) h);
        parallelFor (nch, [&] (size_t c)
        {
            CVec z (m);
            for (size_t k = 0; k < m; ++k) z[k] = x[c][k] * base[k];
            const CVec a = lowpassGaussian (z, sr, bw);
            for (size_t k = 0; k < m; ++k)
            {
                const cplx ak = a[k] * w[lo + k];
                removed[c][k] += (ak * std::conj (base[k])).real();
                added[c][k] += (ak * up1[k]).real();
            }
        });
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
                    const Hooks& hooks, int H, double bwMax, double fade, double bwLow)
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
