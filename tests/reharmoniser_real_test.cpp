// Compares the C++ Re-HarmoniSer core with the Python engine on REAL clips.
// Usage: reharmoniser_real_test <folder made by tools/reharmoniser/make_golden_real.py>   (every sub-folder is one clip)
#include "../src/reharmoniser/Engine.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <filesystem>
using namespace rehar;
static int failures = 0;
static void check (const std::string& what, double err, double tol)
{ const bool ok = err <= tol; if (! ok) ++failures; std::printf ("  %-30s error %-11.4g (limit %-7g) %s\n", what.c_str(), err, tol, ok ? "ok" : "FAIL"); }
template <class T> static std::vector<T> readAll (const std::string& p)
{ std::ifstream f (p, std::ios::binary); if (! f) { std::printf ("cannot open %s\n", p.c_str()); std::exit (2); } f.seekg (0, std::ios::end); const size_t b = (size_t) f.tellg(); f.seekg (0); std::vector<T> v (b / sizeof (T)); f.read ((char*) v.data(), (std::streamsize) b); return v; }
static double maxDiff (const Vec& a, const Vec& b) { if (a.size() != b.size()) { std::printf ("   size %zu vs %zu\n", a.size(), b.size()); return 1e30; } double m = 0; for (size_t i = 0; i < a.size(); ++i) m = std::max (m, std::abs (a[i] - b[i])); return m; }
static double blockRel (const Block& got, const Vec& ref, size_t nch)
{ const size_t n = ref.size() / nch; if (got.size() != nch || got[0].size() != n) { std::printf ("   shape differs (%zu vs %zu frames)\n", got.empty() ? 0 : got[0].size(), n); return 1e30; }
  double mx = 0, md = 0; for (size_t c = 0; c < nch; ++c) for (size_t i = 0; i < n; ++i) { const double r = ref[i * nch + c]; mx = std::max (mx, std::abs (r)); md = std::max (md, std::abs ((double) got[c][i] - r)); } return md / mx; }

int main (int argc, char** argv)
{
    namespace fs = std::filesystem;
    if (argc < 2) { std::puts ("usage: reharmoniser_real_test <folder>"); return 2; }
    std::vector<fs::path> cases; for (auto& e : fs::directory_iterator (argv[1])) if (e.is_directory()) cases.push_back (e.path());
    std::sort (cases.begin(), cases.end());
    for (auto& cd : cases)
    {
        std::map<std::string, std::vector<double>> kv;
        { std::ifstream f (cd / "case.txt"); std::string line; while (std::getline (f, line)) { std::istringstream ss (line); std::string k; ss >> k; double v; while (ss >> v) kv[k].push_back (v); } }
        auto K = [&] (const char* k) { return kv.at (k).front(); };
        const int sr = (int) K ("sr"); const size_t n = (size_t) K ("n"), nch = (size_t) K ("ch");
        std::printf ("%s (%zu channels, %d Hz, %.1f s)\n", cd.filename().string().c_str(), nch, sr, (double) n / sr);
        const auto in = readAll<float> ((cd / "input.f32").string());
        Block data (nch, std::vector<float> (n)); for (size_t i = 0; i < n; ++i) for (size_t c = 0; c < nch; ++c) data[c][i] = in[i * nch + c];
        auto seg = [&] (double a, double b, double& s0) { const size_t i0 = (size_t) std::max (0.0, std::floor (a * sr)), i1 = std::min (n, (size_t) std::floor (b * sr)); Block o (nch); for (size_t c = 0; c < nch; ++c) o[c].assign (data[c].begin() + (long) i0, data[c].begin() + (long) i1); s0 = (double) i0 / sr; return o; };
        auto d64 = [&] (const char* nm) { return readAll<double> ((cd / (std::string (nm) + ".f64")).string()); };
        const double t0 = K ("t0"), t1 = K ("t1"), flo = K ("flo"), fhi = K ("fhi"), a4 = K ("a4"); const int midi = (int) K ("midi");
        auto T0 = std::chrono::steady_clock::now();
        double s0; Block sg = seg (t0 - 0.8, t1 + 0.8, s0);
        const Tracking tk = trackingInputs (spansOf (sg), sr, flo, fhi);
        { std::vector<int> want; for (double v : kv["tracking"]) want.push_back ((int) v); check ("tracking channels", tk.channels == want ? 0 : 1, 0); }
        std::vector<Span> chans; for (int c : tk.channels) chans.push_back ({ sg[(size_t) c].data(), sg[(size_t) c].size() });
        const Track tr = trackNote (chans, tk.weights, sr, s0, t0, t1, flo, fhi);
        check ("track f0 (Hz)", maxDiff (tr.f0, d64 ("track_f0")), 0.02);
        const Vec f0r = refineTrack (chans, tk.weights, sr, s0, tr.ts, tr.f0);
        // the refinement follows tiny phase changes, so rounding differences in quiet places grow a little: 0.1 Hz (a fraction of a cent) is allowed
        check ("refined f0 (Hz)", maxDiff (f0r, d64 ("refined_f0")), 0.1);
        check ("partial hint", detectPartial (chans, tk.weights, sr, s0, tr.ts, f0r) == (int) K ("hint") ? 0 : 1, 0);
        Params p; p.smooth = 0.25; p.ease = 0.1; p.strength = 1.0; p.snap = 0.8; p.keep = 0.2; p.move = 0.0; p.a4 = a4;
        const Plan pl = planCurve (tr.ts, f0r, p, midi);
        check ("plan off (cents)", maxDiff (pl.off, d64 ("plan_off")), 0.3);
        const Vec pyF0 = d64 ("refined_f0"), pyOff = d64 ("plan_off");
        double s02; Block seg2 = seg (K ("ra"), K ("rb"), s02);
        // same inputs as Python used: the retuning itself must agree closely
        check ("retuned, Python's f0 (rel)", blockRel (retuneSegment (seg2, sr, s02, tr.ts, pyF0, pyOff, p), d64 ("retuned"), nch), 2e-3);
        // with its own measured f0 the high overtones may drift a little in the rounding-sensitive places
        check ("retuned, own f0 (rel)", blockRel (retuneSegment (seg2, sr, s02, tr.ts, f0r, pl.off, p), d64 ("retuned"), nch), 0.1);
        if (K ("voices") > 0)
        {
            const double fT = a4 * std::pow (2.0, (midi - 69) / 12.0), win = std::min (0.6, std::max (0.3, (t1 - t0) / 3.0));
            double s03; Block seg3 = seg (t0 - 0.6, t1 + 0.6, s03);
            const VoiceInputs vi = voiceInputs (spansOf (seg3), sr, fT);
            const VoiceScan sc = findVoices (vi, s03, t0, t1, fT, 8, 60.0, win, win / 4);
            check ("voice histogram (rel)", maxDiff (sc.hist, d64 ("voice_hist")) / *std::max_element (sc.hist.begin(), sc.hist.end()), 1e-3);
            check ("voice centres", maxDiff (sc.centres, d64 ("voice_cl")), 0.01);
            const auto C = trackVoices (sc.ts, sc.peaks, sc.centres);
            Vec flat; for (auto& r : C) flat.insert (flat.end(), r.begin(), r.end());
            check ("voice tracks (cents)", maxDiff (flat, d64 ("voice_C")), 0.05);
            Vec offs; for (auto& r : C) { const double m = std::accumulate (r.begin(), r.end(), 0.0) / (double) r.size(); offs.push_back (std::abs (m) > 8 ? -m : 0.0); }
            check ("voice offsets", maxDiff (offs, d64 ("voice_offs")), 0.05);
            double s04; Block seg4 = seg (K ("va"), K ("vb"), s04);
            const VoiceInputs trk = voiceInputs (spansOf (seg4), sr, fT);
            check ("voices retuned (rel)", blockRel (retuneVoices (seg4, sr, s04, fT, sc.ts, C, offs, t0, t1, trk), d64 ("voices_retuned"), nch), 5e-3);
        }
        const RefPoints rp = refPoints (spansOf (data), sr, 0.0);
        const auto gT = d64 ("ref_T");
        if (rp.T.size() != gT.size()) { std::printf ("  ref points %zu vs %zu  FAIL\n", rp.T.size(), gT.size()); ++failures; }
        else check ("ref D (cents)", maxDiff (rp.D, d64 ("ref_D")), 0.05);
        const RefStats rs = refStats (rp.D, rp.W);
        check ("ref A4 (Hz)", std::abs (rs.a4 - kv["ref"][0]), 0.02);
        std::printf ("  (C++ time %.1f s)\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - T0).count());
    }
    std::printf ("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
