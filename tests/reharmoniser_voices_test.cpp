// Re-HarmoniSer: tests that need no files. Synthetic voices stand in for a choir.
//   - the timecode detector: finds an LTC-like channel, and does not take singing for it
//   - measuring a boxed note beside a louder voice: the line is the boxed voice's, and the neighbour is found
//   - retuning it (30 cents down, away from the voice beside it, so the levels can be told apart): the note lands where it was told to, the old pitch is gone, and the voice beside it keeps its sound
// Build and run with tools/reharmoniser-check.sh (or: g++ -std=c++17 -O2 -pthread tests/reharmoniser_voices_test.cpp src/reharmoniser/Engine.cpp src/reharmoniser/Dsp.cpp).
#include "../src/reharmoniser/Engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace rehar;

static int failures = 0;
static void expect (const char* what, bool ok, double value, const char* unit = "")
{
    if (! ok) ++failures;
    std::printf ("%-62s %9.3f %-3s %s\n", what, value, unit, ok ? "ok" : "FAIL");
}

struct Lcg { unsigned s = 12345u; double next() { s = s * 1664525u + 1013904223u; return (double) (s >> 8) / 16777216.0 - 0.5; } };

static const int kSr = 48000;

/** One sung voice: pitch in cents from f_ref (a slow drift and a vibrato), ten overtones falling away as 1/h. */
struct Voice { double cents0, vibCents, vibHz, phase; double level; };
static Vec voiceCents (const Voice& v, size_t n, double moveCents = 0.0)
{
    Vec c (n);
    for (size_t i = 0; i < n; ++i) { const double t = (double) i / kSr; c[i] = v.cents0 + moveCents + v.vibCents * std::sin (2.0 * kPi * v.vibHz * t + v.phase) + 3.0 * std::sin (2.0 * kPi * 0.7 * t + 2.0 * v.phase); }
    return c;
}
static Vec voiceHz (const Vec& cents, double fRef) { Vec f (cents.size()); for (size_t i = 0; i < f.size(); ++i) f[i] = fRef * std::pow (2.0, cents[i] / 1200.0); return f; }
static void addVoice (Block& b, const Vec& f, double level, const double* chanGain)
{
    double ph = 0.0;
    for (size_t i = 0; i < f.size(); ++i)
    {
        ph += 2.0 * kPi * f[i] / kSr;
        double s = 0; for (int h = 1; h <= 10; ++h) s += std::sin ((double) h * ph) / (double) h;
        for (size_t c = 0; c < b.size(); ++c) b[c][i] += (float) (level * chanGain[c] * s);
    }
}
/** Phase-blind energy of the overtones 1..6 of a line, in a narrow band, over all channels, between samples a and b. */
static double lineEnergy (const Block& b, const Vec& f, size_t a, size_t e, double bw = 8.0)
{
    const size_t n = e - a; Vec ff (f.begin() + (long) a, f.begin() + (long) e);
    double ph = 0; Vec phi (n); for (size_t i = 0; i < n; ++i) { ph += 2.0 * kPi * ff[i] / kSr; phi[i] = ph; }
    double tot = 0;
    for (int h = 1; h <= 6; ++h)
        for (size_t c = 0; c < b.size(); ++c)
        {
            CVec z (n); for (size_t i = 0; i < n; ++i) z[i] = (double) b[c][a + i] * std::polar (1.0, -(double) h * phi[i]);
            const CVec lp = lowpassGaussian (z, kSr, bw * h);
            for (size_t i = 0; i < n; ++i) tot += std::norm (lp[i]);
        }
    return tot;
}
static double db (double ratio) { return 10.0 * std::log10 (ratio + 1e-30); }

int main()
{
    // ------------------------------------------------------------------ the timecode detector
    {
        const size_t n = (size_t) (4 * kSr);
        std::vector<float> ltc (n), voice (n), quiet (n, 0.0f), noise (n);
        // biphase-mark-like: a bit every 24 samples (2000 bits/s), a flip at every bit start and in the middle of a 1; then softened like a recorded signal
        { Lcg r; double lvl = 0.35; int cnt = 0; for (size_t i = 0; i < n; ++i) { if (i % 24 == 0) { lvl = -lvl; cnt = r.next() > 0 ? 1 : 0; } if (cnt && i % 24 == 12) lvl = -lvl; ltc[i] = (float) lvl; }
          for (int pass = 0; pass < 3; ++pass) { for (size_t i = 1; i < n; ++i) ltc[i] = 0.5f * ltc[i] + 0.5f * ltc[i - 1]; }
          for (auto& v : ltc) v *= 1.5f; }
        { Voice v { 0, 15, 5.5, 0.3, 0.1 }; Block b (1, std::vector<float> (n, 0.0f)); const double g = 1.0; addVoice (b, voiceHz (voiceCents (v, n), 600.0), 0.1, &g); voice = b[0];
          for (size_t i = 0; i < n; ++i) voice[i] *= (float) (0.55 + 0.45 * std::sin (2.0 * kPi * 0.4 * (double) i / kSr)); }                      // phrasing: the level moves
        { Lcg r; for (size_t i = 0; i < n; ++i) noise[i] = (float) (0.2 * r.next()); }
        expect ("timecode channel is recognised", looksLikeTimecode ({ ltc.data(), n }, kSr), 1);
        expect ("singing is not taken for timecode", ! looksLikeTimecode ({ voice.data(), n }, kSr), 0);
        expect ("noise is not taken for timecode", ! looksLikeTimecode ({ noise.data(), n }, kSr), 0);
        expect ("silence is not taken for timecode", ! looksLikeTimecode ({ quiet.data(), n }, kSr), 0);
        expect ("under a second is not judged", ! looksLikeTimecode ({ ltc.data(), (size_t) (kSr / 2) }, kSr), 0);
    }

    // ------------------------------------------------------------------ a boxed note beside a louder voice
    const double fRef = 600.0;
    const size_t n = (size_t) (3.4 * kSr);
    const double t0 = 0.7, t1 = 2.7;
    const Voice target { -30.0, 12.0, 5.5, 0.4, 1.0 }, neigh { 4.0, 8.0, 6.1, 1.9, 1.7 };      // the neighbour is louder and 34 cents above the boxed voice
    const double cg[3] = { 1.0, 0.8, 0.6 };
    const double moveCents = -30.0;                                  // down, away from the voice beside it (so the level at the new pitch is its own)
    const Vec cT = voiceCents (target, n), cN = voiceCents (neigh, n), cTnew = voiceCents (target, n, moveCents);
    const Vec fT = voiceHz (cT, fRef), fN = voiceHz (cN, fRef), fTnew = voiceHz (cTnew, fRef);
    Block mix (3, std::vector<float> (n, 0.0f)), ideal (3, std::vector<float> (n, 0.0f)), nbOnly (3, std::vector<float> (n, 0.0f));
    addVoice (mix, fT, target.level, cg); addVoice (mix, fN, neigh.level, cg);
    addVoice (ideal, fTnew, target.level, cg); addVoice (ideal, fN, neigh.level, cg);
    addVoice (nbOnly, fN, neigh.level, cg);
    { Lcg r; for (auto& ch : mix) for (auto& v : ch) v += (float) (0.004 * r.next()); }

    const double fLo = fRef * std::pow (2.0, -90.0 / 1200.0), fHi = fRef * std::pow (2.0, 30.0 / 1200.0);       // a box round the boxed voice (it is not centred on it)
    const double s0 = 0.0;
    const Tracking tk = trackingInputs (spansOf (mix), kSr, fLo, fHi);
    std::vector<Span> chans; for (int c : tk.channels) chans.push_back ({ mix[(size_t) c].data(), mix[(size_t) c].size() });
    const NoteMeasure m = measureNote (chans, tk.weights, kSr, s0, t0, t1, fLo, fHi);

    double rms = 0; size_t cnt = 0;
    for (size_t k = 0; k < m.ts.size(); ++k) { if (m.ts[k] < t0 + 0.25 || m.ts[k] > t1 - 0.25) continue; const size_t i = (size_t) (m.ts[k] * kSr); const double e = 1200.0 * std::log2 (m.f0[k] / fT[i]); rms += e * e; ++cnt; }
    rms = std::sqrt (rms / (double) std::max<size_t> (1, cnt));
    expect ("the boxed voice's line follows it (rms error)", rms < 3.0, rms, "c");

    expect ("the voice beside it is found", ! m.nb.empty(), (double) m.nb.size());
    if (! m.nb.empty())
    {
        double bestRms = 1e9;
        for (auto& line : m.nb)
        {
            double r2 = 0; size_t k2 = 0;
            for (size_t k = 0; k < m.ts.size(); ++k) { if (m.ts[k] < t0 + 0.25 || m.ts[k] > t1 - 0.25) continue; const size_t i = (size_t) (m.ts[k] * kSr); const double e = 1200.0 * std::log2 (line[k] / fN[i]); r2 += e * e; ++k2; }
            bestRms = std::min (bestRms, std::sqrt (r2 / (double) std::max<size_t> (1, k2)));
        }
        expect ("its line follows it (rms error)", bestRms < 4.0, bestRms, "c");
    }

    // ------------------------------------------------------------------ retune: the boxed note up 30 cents; the neighbour must keep its sound
    Params p; p.a4 = 440.0; p.move = moveCents; p.snap = 0.0; p.ease = 0.0;
    const Plan pl = planCurve (m.ts, m.f0, p, 74);
    const double ta = m.ts.front() - 0.45, tb = m.ts.back() + p.hold + p.fadeOut + 0.3;
    const size_t i0 = (size_t) std::max (0.0, ta * kSr), i1 = std::min (n, (size_t) (tb * kSr));
    auto cut = [&] (const Block& b) { Block o (b.size()); for (size_t c = 0; c < b.size(); ++c) o[c].assign (b[c].begin() + (long) i0, b[c].begin() + (long) i1); return o; };
    auto run = [&] (const std::vector<Vec>& nb) { Block done = retuneSegment (cut (mix), kSr, (double) i0 / kSr, m.ts, m.f0, pl.off, p, {}, nb); Block full = mix; for (size_t c = 0; c < full.size(); ++c) std::copy (done[c].begin(), done[c].end(), full[c].begin() + (long) i0); return full; };
    const Block withNb = run (m.nb), without = run ({});
    const size_t a = (size_t) ((t0 + 0.3) * kSr), e = (size_t) ((t1 - 0.3) * kSr);
    for (int pass = 0; pass < 2; ++pass)
    {
        const Block& out = pass == 0 ? withNb : without;
        const double landing = db (lineEnergy (out, fTnew, a, e) / lineEnergy (ideal, fTnew, a, e));
        const double ghost = db (lineEnergy (out, fT, a, e) / lineEnergy (mix, fT, a, e));
        const double nbMove = db (lineEnergy (out, fN, a, e) / lineEnergy (mix, fN, a, e));
        std::printf ("  %s: landing %+.2f dB, old pitch left %+.1f dB, neighbour %+.2f dB\n", pass == 0 ? "narrow band, neighbour peeled" : "old wide band (no neighbour lines)", landing, ghost, nbMove);
        if (pass == 0)
        {
            expect ("retuned note lands at the new pitch (level error)", std::abs (landing) < 1.0, landing, "dB");
            expect ("the old pitch is gone from it", ghost < -12.0, ghost, "dB");
            expect ("the voice beside it is left as it was", std::abs (nbMove) < 0.5, nbMove, "dB");
        }
    }
    std::printf (failures == 0 ? "ALL OK\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
