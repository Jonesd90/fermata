// Compares the C++ Re-HarmoniSer core with the Python engine's reference results.
// Usage: reharmoniser_test <golden folder>      (made by tools/reharmoniser/make_golden.py)
#include "../src/reharmoniser/Engine.h"
#include "../src/reharmoniser/Spectrogram.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <numeric>
using namespace rehar;

static std::string dir;
static double minOfV (const Vec& v) { return *std::min_element (v.begin(), v.end()); }
static double maxOfV (const Vec& v) { return *std::max_element (v.begin(), v.end()); }
static int failures = 0;
template <class T> static std::vector<T> readAll (const std::string& name)
{
    std::ifstream f (dir + "/" + name, std::ios::binary);
    if (! f) { std::printf ("cannot open %s\n", name.c_str()); std::exit (2); }
    f.seekg (0, std::ios::end); const size_t bytes = (size_t) f.tellg(); f.seekg (0);
    std::vector<T> v (bytes / sizeof (T)); f.read ((char*) v.data(), (std::streamsize) bytes); return v;
}
static void check (const char* what, double err, double tol)
{
    const bool ok = err <= tol; if (! ok) ++failures;
    std::printf ("%-34s error %-12.4g (limit %-8g) %s\n", what, err, tol, ok ? "ok" : "FAIL");
}
static double maxAbsDiff (const Vec& a, const Vec& b)
{
    if (a.size() != b.size()) { std::printf ("   size differs: %zu vs %zu\n", a.size(), b.size()); return 1e30; }
    double m = 0; for (size_t i = 0; i < a.size(); ++i) m = std::max (m, std::abs (a[i] - b[i])); return m;
}
/** Largest difference over all channels, relative to the largest value of the reference. */
static double blockRel (const Block& got, const std::vector<double>& ref, size_t nch)
{
    const size_t n = ref.size() / nch;
    if (got.size() != nch || got[0].size() != n) { std::printf ("   shape differs: %zu x %zu vs %zu x %zu\n", got.size(), got.empty() ? 0 : got[0].size(), nch, n); return 1e30; }
    double mx = 0, md = 0;
    for (size_t c = 0; c < nch; ++c) for (size_t i = 0; i < n; ++i) { const double r = ref[i * nch + c]; mx = std::max (mx, std::abs (r)); md = std::max (md, std::abs ((double) got[c][i] - r)); }
    return md / mx;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::puts ("usage: reharmoniser_test <golden folder>"); return 2; }
    dir = argv[1];
    const int sr = 48000; const size_t nch = 8, n = 384000; const double dur = 8.0;
    const auto in = readAll<float> ("input.f32");
    Block data (nch, std::vector<float> (n));
    for (size_t i = 0; i < n; ++i) for (size_t c = 0; c < nch; ++c) data[c][i] = in[i * nch + c];
    auto seg = [&] (double a, double b, double& s0) { const size_t i0 = (size_t) std::max (0.0, std::floor (a * sr)), i1 = std::min (n, (size_t) std::floor (b * sr)); Block o (nch); for (size_t c = 0; c < nch; ++c) o[c].assign (data[c].begin() + (long) i0, data[c].begin() + (long) i1); s0 = (double) i0 / sr; return o; };
    auto d64 = [&] (const char* nm) { return readAll<double> (nm); };
    auto sel = [&] (const Block& b, const Tracking& t) { std::vector<Span> v; for (int c : t.channels) v.push_back ({ b[(size_t) c].data(), b[(size_t) c].size() }); return v; };

    const double t0 = 2.0, t1 = 5.0, fLo = 200.0, fHi = 242.0;
    double s0; Block sg = seg (t0 - 0.8, t1 + 0.8, s0);
    const Tracking tk = trackingInputs (spansOf (sg), sr, fLo, fHi);
    { const std::vector<int> want = { 6, 7, 1, 3, 0, 2 }; check ("tracking channels", tk.channels == want ? 0 : 1, 0); }
    check ("tracking weights", maxAbsDiff (tk.weights, d64 ("weights.f64")), 1e-9);
    const auto chans = sel (sg, tk);
    const Track tr = trackNote (chans, tk.weights, sr, s0, t0, t1, fLo, fHi);
    check ("track ts", maxAbsDiff (tr.ts, d64 ("track_ts.f64")), 1e-9);
    check ("track f0 (Hz)", maxAbsDiff (tr.f0, d64 ("track_f0.f64")), 0.02);
    const Vec f0r = refineTrack (chans, tk.weights, sr, s0, tr.ts, tr.f0);
    check ("refined f0 (Hz)", maxAbsDiff (f0r, d64 ("refined_f0.f64")), 0.02);
    check ("partial hint", detectPartial (chans, tk.weights, sr, s0, tr.ts, f0r) == 1 ? 0 : 1, 0);

    Params p; p.smooth = 0.25; p.ease = 0.10; p.strength = 1.0; p.snap = 0.8; p.keep = 0.2; p.move = 0.0;
    const Plan pl = planCurve (tr.ts, f0r, p, 57);
    check ("plan c (cents)", maxAbsDiff (pl.c, d64 ("plan_c.f64")), 0.05);
    check ("plan wob", maxAbsDiff (pl.wob, d64 ("plan_wob.f64")), 0.05);
    check ("plan off", maxAbsDiff (pl.off, d64 ("plan_off.f64")), 0.05);
    check ("plan planned", maxAbsDiff (pl.planned, d64 ("plan_planned.f64")), 0.05);

    const double a = std::max (0.0, tr.ts.front() - 0.15 - 0.3), b = std::min (dur, tr.ts.back() + p.hold + p.fadeOut + 0.3);
    double s02; Block seg2 = seg (a, b, s02);
    const Block nw = retuneSegment (seg2, sr, s02, tr.ts, f0r, pl.off, p);
    check ("retuned audio (relative)", blockRel (nw, d64 ("retuned.f64"), nch), 2e-3);
    Params p2 = p; p2.move = -25.0; p2.snap = 0.0; p2.match = false;
    const Plan pl2 = planCurve (tr.ts, f0r, p2, 57);
    check ("plan2 off", maxAbsDiff (pl2.off, d64 ("plan2_off.f64")), 0.05);
    const Block nw2 = retuneSegment (seg2, sr, s02, tr.ts, f0r, pl2.off, p2);
    check ("retuned (no match) relative", blockRel (nw2, d64 ("retuned2.f64"), nch), 2e-3);
    Vec ftarget (f0r.size()); for (size_t i = 0; i < f0r.size(); ++i) ftarget[i] = f0r[i] * std::pow (2.0, pl.off[i] / 1200.0);
    const Block so = soloSegment (seg2, sr, s02, tr.ts, ftarget, p, tr.ts.front(), tr.ts.back());
    check ("solo (relative)", blockRel (so, d64 ("solo.f64"), nch), 2e-3);
    const double bl = std::max (40.0, 0.8 * minOfV (f0r)), bh = std::min (0.45 * sr, 2.2 * maxOfV (f0r));
    const LevelReport lr = levelReport (seg2, nw, sr, s02, tr.ts.front(), tr.ts.back(), bl, bh);
    check ("level rise dB", std::abs (lr.rise - 1.0465928771213733), 0.05);
    check ("level dip dB", std::abs (lr.dip + 1.6567497702382237), 0.05);

    // voices
    const double fT = 220.0, win = std::min (0.6, std::max (0.3, (t1 - t0) / 3.0));
    double s03; Block seg3 = seg (t0 - 0.6, t1 + 0.6, s03);
    const VoiceInputs vi = voiceInputs (spansOf (seg3), sr, fT);
    const VoiceScan sc = findVoices (vi, s03, t0, t1, fT, 8, 60.0, win, win / 4);
    check ("voice ts", maxAbsDiff (sc.ts, d64 ("voice_ts.f64")), 1e-9);
    check ("voice histogram (relative)", maxAbsDiff (sc.hist, d64 ("voice_hist.f64")) / maxOfV (d64 ("voice_hist.f64")), 1e-3);
    check ("voice centres", maxAbsDiff (sc.centres, d64 ("voice_cl.f64")), 0.01);
    const auto C = trackVoices (sc.ts, sc.peaks, sc.centres);
    { Vec flat; for (auto& r : C) flat.insert (flat.end(), r.begin(), r.end()); check ("voice tracks (cents)", maxAbsDiff (flat, d64 ("voice_C.f64")), 0.05); }
    Vec offs (C.size(), 0.0); size_t js = 0; Vec means; for (auto& r : C) means.push_back (std::accumulate (r.begin(), r.end(), 0.0) / (double) r.size());
    for (size_t j = 0; j < means.size(); ++j) if (means[j] > means[js]) js = j;
    offs[js] = -means[js];
    check ("voice offsets", maxAbsDiff (offs, d64 ("voice_offs.f64")), 0.05);
    double s04; Block seg4 = seg (std::max (0.0, t0 - 0.8), std::min (dur, t1 + 0.8), s04);
    const VoiceInputs trk = voiceInputs (spansOf (seg4), sr, fT);
    const Block vr = retuneVoices (seg4, sr, s04, fT, sc.ts, C, offs, t0, t1, trk);
    check ("voices retuned (relative)", blockRel (vr, d64 ("voices_retuned.f64"), nch), 5e-3);

    // brush
    Brush br; br.pts = { { 3.0, 330.0 }, { 3.5, 335.0 }, { 4.0, 340.0 } }; br.rt = 0.2; br.rc = 60.0; br.amount = 1.0; br.harm = true;
    auto reg = brushRegion (br);
    double s05; Block seg5 = seg (std::max (0.0, reg.first), std::min (dur, reg.second), s05);
    const Block bo = brushSegment (seg5, sr, s05, br, 10);
    check ("brush (relative)", blockRel (bo, d64 ("brush_out.f64"), nch), 2e-3);

    // choir reference pitch
    double s06; Block seg6 = seg (0.5, 7.5, s06);
    const RefPoints rp = refPoints (spansOf (seg6), sr, s06);
    const auto gT = d64 ("ref_T.f64"), gD = d64 ("ref_D.f64");
    if (rp.T.size() != gT.size()) { std::printf ("   ref points: %zu vs %zu\n", rp.T.size(), gT.size()); ++failures; }
    else { check ("ref T", maxAbsDiff (rp.T, gT), 1e-6); check ("ref D (cents)", maxAbsDiff (rp.D, gD), 0.05); }
    const RefStats rs = refStats (rp.D, rp.W);
    check ("ref A4 (Hz)", std::abs (rs.a4 - 440.45643567719816), 0.02);
    check ("ref spread (cents)", std::abs (rs.spread - 3.4401479181932264), 0.1);
    // spectrogram picture (the whole file)
    {
        Spectrogram sp; sp.build (spansOf (data), sr);
        check ("spectrogram frames/bins", (sp.nf == 801 && sp.nb == 1367 && sp.hop == 240 && sp.N == 4096) ? 0 : 1, 0);
        check ("spectrogram colour ref (dB)", std::abs (sp.ref - (-16.051145553588867)), 0.1);
        size_t diff = 0, big = 0, total = 0;
        for (size_t c = 0; c < nch; ++c)
        {
            const auto ref = readAll<double> ("spec_ch" + std::to_string (c) + ".f64");
            for (size_t i = 0; i < ref.size(); ++i) { const int d = std::abs ((int) sp.db[c][i] - (int) ref[i]); if (d) ++diff; if (d > 1) ++big; ++total; }
        }
        check ("spectrogram cells off by >1 (of all)", (double) big / (double) total, 1e-5);
        std::printf ("   (%.4f%% of cells differ by 1 level = 0.5 dB, from rounding)\n", 100.0 * (double) diff / (double) total);
    }
    std::printf ("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
