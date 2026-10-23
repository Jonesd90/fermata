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
    auto run = [&] (const std::vector<Vec>& nb, bool lone) { Params q = p; q.lone = lone; Block done = retuneSegment (cut (mix), kSr, (double) i0 / kSr, m.ts, m.f0, pl.off, q, {}, nb); Block full = mix; for (size_t c = 0; c < full.size(); ++c) std::copy (done[c].begin(), done[c].end(), full[c].begin() + (long) i0); return full; };
    const Block withNb = run (m.nb, true), without = run ({}, true), withoutNarrow = run ({}, false);
    const size_t a = (size_t) ((t0 + 0.3) * kSr), e = (size_t) ((t1 - 0.3) * kSr);
    for (int pass = 0; pass < 3; ++pass)
    {
        const Block& out = pass == 0 ? withNb : pass == 1 ? without : withoutNarrow;
        const double landing = db (lineEnergy (out, fTnew, a, e) / lineEnergy (ideal, fTnew, a, e));
        const double ghost = db (lineEnergy (out, fT, a, e) / lineEnergy (mix, fT, a, e));
        const double nbMove = db (lineEnergy (out, fN, a, e) / lineEnergy (mix, fN, a, e));
        std::printf ("  %s: landing %+.2f dB, old pitch left %+.1f dB, neighbour %+.2f dB\n", pass == 0 ? "neighbour found: fitted together" : pass == 1 ? "neighbour missed, lone bands (as wide as the wobble)" : "neighbour missed, narrow Gaussian bands (2.56)", landing, ghost, nbMove);
        if (pass == 0)
        {
            expect ("retuned note lands at the new pitch (level error)", std::abs (landing) < 1.0, landing, "dB");
            expect ("the old pitch is gone from it", ghost < -12.0, ghost, "dB");
            expect ("the voice beside it is left as it was", std::abs (nbMove) < 0.5, nbMove, "dB");
        }
    }

    // ------------------------------------------------------------------ a voice on its own in a room: the ring-out must move with it, nothing left at the old pitch
    {
        // the room: what the voice sang up to 0.4 s ago rings on (a hundred delayed, decaying copies), so the room's sound is spread over the whole vibrato
        const size_t n2 = (size_t) (3.4 * kSr);
        const Voice lone { 30.0, 22.0, 5.8, 0.2, 1.0 };                  // 30 cents sharp, a big vibrato
        const Vec cL = voiceCents (lone, n2), cLnew = voiceCents (lone, n2, -30.0);
        const Vec fL = voiceHz (cL, fRef), fLnew = voiceHz (cLnew, fRef);
        Block dry (3, std::vector<float> (n2, 0.0f)), dryNew (3, std::vector<float> (n2, 0.0f));
        addVoice (dry, fL, lone.level, cg); addVoice (dryNew, fLnew, lone.level, cg);
        auto withRoom = [&] (const Block& d)
        {
            Block o = d; Lcg r;
            for (size_t c = 0; c < o.size(); ++c)
                for (int k = 0; k < 100; ++k)
                {
                    const size_t delay = (size_t) ((0.01 + 0.39 * (r.next() + 0.5)) * kSr); const double gain = 0.12 * (r.next() + 0.5) * std::exp (-3.0 * (double) delay / (0.4 * kSr));
                    for (size_t i = delay; i < n2; ++i) o[c][i] += (float) (gain * d[c][i - delay]);
                }
            for (auto& ch : o) for (auto& v : ch) v += (float) (0.003 * r.next());
            return o;
        };
        const Block room = withRoom (dry), roomIdeal = withRoom (dryNew);
        const double lo2 = fRef * std::pow (2.0, -40.0 / 1200.0), hi2 = fRef * std::pow (2.0, 90.0 / 1200.0);
        const Tracking tk2 = trackingInputs (spansOf (room), kSr, lo2, hi2);
        std::vector<Span> ch2; for (int c : tk2.channels) ch2.push_back ({ room[(size_t) c].data(), room[(size_t) c].size() });
        const NoteMeasure m2 = measureNote (ch2, tk2.weights, kSr, 0.0, t0, t1, lo2, hi2);
        expect ("a voice on its own: no other voice is found beside it", m2.nb.empty(), (double) m2.nb.size());
        Params p2; p2.a4 = 440.0; p2.move = -30.0; p2.snap = 0.0; p2.ease = 0.0;
        const Plan pl2 = planCurve (m2.ts, m2.f0, p2, 74);
        const double ta2 = m2.ts.front() - 0.45, tb2 = m2.ts.back() + p2.hold + p2.fadeOut + p2.tailMax + 0.3;
        const size_t j0 = (size_t) std::max (0.0, ta2 * kSr), j1 = std::min (n2, (size_t) (tb2 * kSr));
        auto run2 = [&] (bool loneBands)
        {
            Params q = p2; q.lone = loneBands;
            Block seg (room.size()); for (size_t c = 0; c < seg.size(); ++c) seg[c].assign (room[c].begin() + (long) j0, room[c].begin() + (long) j1);
            Block done = retuneSegment (seg, kSr, (double) j0 / kSr, m2.ts, m2.f0, pl2.off, q);
            Block full = room; for (size_t c = 0; c < full.size(); ++c) std::copy (done[c].begin(), done[c].end(), full[c].begin() + (long) j0); return full;
        };
        const Block wide = run2 (true), narrow = run2 (false);
        const size_t a2 = (size_t) ((t0 + 0.3) * kSr), e2 = (size_t) ((t1 - 0.3) * kSr);
        double ghostWide = 0, ghostNarrow = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const Block& out = pass == 0 ? wide : narrow;
            const double landing = db (lineEnergy (out, fLnew, a2, e2) / lineEnergy (roomIdeal, fLnew, a2, e2));
            const double ghost = db (lineEnergy (out, fL, a2, e2) / lineEnergy (room, fL, a2, e2));
            // the room's sound round the old pitch (+-45 cents, all of it) against what an in-tune voice would have left there
            const double spread = db (lineEnergy (out, fL, a2, e2, 0.045 * fRef) / lineEnergy (roomIdeal, fL, a2, e2, 0.045 * fRef));
            std::printf ("  %s: landing %+.2f dB, old pitch left %+.1f dB, room round the old pitch vs ideal %+.1f dB\n", pass == 0 ? "flat bands as wide as the wobble" : "narrow Gaussian bands (2.56)", landing, ghost, spread);
            if (pass == 0) { ghostWide = ghost; expect ("lone voice: lands at the new pitch (level error)", std::abs (landing) < 2.0, landing, "dB"); expect ("lone voice: the old pitch is gone", ghost < -18.0, ghost, "dB"); expect ("lone voice: the room round the old pitch is as an in-tune voice leaves it", std::abs (spread) < 3.0, spread, "dB"); }
            else ghostNarrow = ghost;
        }
        expect ("lone voice: the wide bands leave far less of the old pitch than the narrow ones", ghostWide < ghostNarrow - 6.0, ghostWide - ghostNarrow, "dB");
    }

    // ------------------------------------------------------------------ "One voice only" (2.64)
    {
        // on the two-voice mix: nothing is looked for beside the note, and a line handed in anyway is not kept apart
        const NoteMeasure m1 = measureNote (chans, tk.weights, kSr, s0, t0, t1, fLo, fHi, {}, 20.0, false);
        expect ("one voice only: nothing is looked for beside the note", m1.nb.empty(), (double) m1.nb.size());
        Params q = p; q.oneVoice = true;
        const Block a1 = retuneSegment (cut (mix), kSr, (double) i0 / kSr, m.ts, m.f0, pl.off, q, {}, m.nb);
        const Block b1 = retuneSegment (cut (mix), kSr, (double) i0 / kSr, m.ts, m.f0, pl.off, q, {}, {});
        bool same = true; for (size_t c = 0; c < a1.size() && same; ++c) same = a1[c] == b1[c];
        expect ("one voice only: a line found beside it is not kept apart", same, same ? 1.0 : 0.0);

        // a voice with a strong upper spectrum (24 overtones): ALL of them must move, not only the first ten (the upper ones left at the old pitch sound like a second
        // singer a little off the first)
        const double fR = 300.0; const size_t n3 = (size_t) (3.0 * kSr);
        const Voice v3 { 30.0, 15.0, 5.5, 0.7, 1.0 };
        const Vec c3 = voiceCents (v3, n3), c3new = voiceCents (v3, n3, -30.0);
        const Vec f3 = voiceHz (c3, fR), f3new = voiceHz (c3new, fR);
        auto addRich = [&] (Block& b, const Vec& f)
        {
            double ph = 0.0;
            for (size_t i = 0; i < f.size(); ++i)
            {
                ph += 2.0 * kPi * f[i] / kSr;
                double sm = 0; for (int h = 1; h <= 24; ++h) sm += std::sin ((double) h * ph) / std::pow ((double) h, 0.7);
                for (size_t c = 0; c < b.size(); ++c) b[c][i] += (float) (0.3 * cg[c] * sm);
            }
        };
        Block rich (2, std::vector<float> (n3, 0.0f)); addRich (rich, f3);
        { Lcg r; for (auto& ch : rich) for (auto& v : ch) v += (float) (0.002 * r.next()); }
        const double lo3 = fR * std::pow (2.0, -40.0 / 1200.0), hi3 = fR * std::pow (2.0, 90.0 / 1200.0);
        const Tracking tk3 = trackingInputs (spansOf (rich), kSr, lo3, hi3);
        std::vector<Span> ch3; for (int c : tk3.channels) ch3.push_back ({ rich[(size_t) c].data(), rich[(size_t) c].size() });
        const NoteMeasure m3 = measureNote (ch3, tk3.weights, kSr, 0.0, 0.6, 2.4, lo3, hi3, {}, 20.0, false);
        Params p3; p3.a4 = 440.0; p3.move = -30.0; p3.snap = 0.0; p3.ease = 0.0; p3.oneVoice = true;
        const Plan pl3 = planCurve (m3.ts, m3.f0, p3, 62);
        // the energy of overtones hA..hB along a line, in a band 20 Hz wide whatever the overtone (the old and new lines are 50-90 Hz apart up there)
        auto upper = [&] (const Block& b, const Vec& f, size_t a, size_t e, int hA, int hB)
        {
            const size_t len = e - a; double ph = 0; Vec phi (len); for (size_t i = 0; i < len; ++i) { ph += 2.0 * kPi * f[a + i] / kSr; phi[i] = ph; }
            double tot = 0;
            for (int h = hA; h <= hB; ++h) for (size_t c = 0; c < b.size(); ++c)
            {
                CVec z (len); for (size_t i = 0; i < len; ++i) z[i] = (double) b[c][a + i] * std::polar (1.0, -(double) h * phi[i]);
                const CVec lp = lowpassGaussian (z, kSr, 20.0); for (size_t i = 0; i < len; ++i) tot += std::norm (lp[i]);
            }
            return tot;
        };
        const size_t a3 = (size_t) (0.9 * kSr), e3 = (size_t) (2.1 * kSr);
        auto run3 = [&] (double topHz)
        {
            Params q3 = p3; q3.loneTopHz = topHz;
            return retuneSegment (rich, kSr, 0.0, m3.ts, m3.f0, pl3.off, q3);
        };
        const Block all = run3 (8000.0), ten = run3 (0.0);
        const double ghostAll = db (upper (all, f3, a3, e3, 12, 22) / upper (rich, f3, a3, e3, 12, 22));
        const double ghostTen = db (upper (ten, f3, a3, e3, 12, 22) / upper (rich, f3, a3, e3, 12, 22));
        Block rich3new (2, std::vector<float> (n3, 0.0f)); addRich (rich3new, f3new);
        const double landAll = db (upper (all, f3new, a3, e3, 12, 22) / upper (rich3new, f3new, a3, e3, 12, 22));
        std::printf ("  overtones 12-22 at the old pitch: %+.1f dB moving them all, %+.1f dB moving only the first ten; landing %+.2f dB\n", ghostAll, ghostTen, landAll);
        expect ("one voice only: the upper overtones leave the old pitch", ghostAll < -15.0, ghostAll, "dB");
        expect ("one voice only: and land at the new pitch (level error)", std::abs (landAll) < 2.0, landAll, "dB");
        expect ("(with only the first ten moved they stayed behind)", ghostTen > -3.0, ghostTen, "dB");
    }
    // ------------------------------------------------------------------ a note struck under the voice (2.91)
    {
        // A piano note is struck inside the box, an octave below the voice: its 2nd, 4th, 6th ... overtones lie on the voice's 1st, 2nd, 3rd ..., 20 cents below it, and are louder
        // than the voice's. The line of the boxed voice used to be taken by them (the steady sound had no one to compare with: no voice was found beside it, the line
        // stayed on the piano all through the note, and the piano's overtones were moved with it). The window before the strike stops 0.6 s ahead of it: a measuring
        // window reaches that far, and the strike is in it.
        const double fV = 600.0, fP = 300.0;
        const size_t n4 = (size_t) (4.4 * kSr);
        const double t0b = 0.7, t1b = 3.8, tOn = 2.0;
        const Voice sop { 20.0, 10.0, 5.5, 0.9, 1.0 };
        const Vec cS = voiceCents (sop, n4), cSnew = voiceCents (sop, n4, 35.0);
        const Vec fS = voiceHz (cS, fV), fSnew = voiceHz (cSnew, fV);
        const double gv[3] = { 1.0, 0.8, 0.6 }, gp[3] = { 0.5, 0.8, 1.2 };
        auto addPiano = [&] (Block& b)
        {
            for (size_t i = (size_t) (tOn * kSr); i < n4; ++i)
            {
                const double t = (double) i / kSr - tOn, env = std::min (1.0, t / 0.005) * std::exp (-t / 0.9);
                double sm = 0; for (int h = 1; h <= 16; ++h) sm += std::sin (2.0 * kPi * fP * (double) h * (double) i / kSr) * (h % 2 == 0 ? 2.4 : 1.0) / (double) h;
                for (size_t c = 0; c < b.size(); ++c) b[c][i] += (float) (2.0 * gp[c] * env * sm);
            }
        };
        Block mixP (3, std::vector<float> (n4, 0.0f)), idealP (3, std::vector<float> (n4, 0.0f));
        addVoice (mixP, fS, 1.0, gv); addPiano (mixP);
        addVoice (idealP, fSnew, 1.0, gv); addPiano (idealP);
        { Lcg r; for (auto& ch : mixP) for (auto& v : ch) v += (float) (0.004 * r.next()); }
        const double loP = fV * std::pow (2.0, -35.0 / 1200.0), hiP = fV * std::pow (2.0, 85.0 / 1200.0);        // a box round the voice
        const Tracking tkP = trackingInputs (spansOf (mixP), kSr, loP, hiP);
        std::vector<Span> chP; for (int c : tkP.channels) chP.push_back ({ mixP[(size_t) c].data(), mixP[(size_t) c].size() });
        const NoteMeasure mP = measureNote (chP, tkP.weights, kSr, 0.0, t0b, t1b, loP, hiP);
        auto rmsOver = [&] (double ta, double tb)
        {
            double r2 = 0; size_t k2 = 0;
            for (size_t k = 0; k < mP.ts.size(); ++k) { if (mP.ts[k] < ta || mP.ts[k] > tb) continue; const size_t i = (size_t) (mP.ts[k] * kSr); const double e = 1200.0 * std::log2 (mP.f0[k] / fS[i]); r2 += e * e; ++k2; }
            return std::sqrt (r2 / (double) std::max<size_t> (1, k2));
        };
        const double rmsBefore = rmsOver (t0b + 0.25, tOn - 0.6), rmsDuring = rmsOver (tOn + 0.05, tOn + 1.2), rmsAfter = rmsOver (tOn + 1.3, t1b - 0.25);
        std::printf ("  the voice's line against the truth: before the strike %.1f c, while the piano is loud %.1f c, after %.1f c; %zu sound(s) kept apart\n", rmsBefore, rmsDuring, rmsAfter, mP.nb.size());
        expect ("struck note: the line follows the voice before the strike (rms error)", rmsBefore < 3.0, rmsBefore, "c");
        expect ("struck note: and while the piano is loudest (the line is not taken by it)", rmsDuring < 3.0, rmsDuring, "c");
        expect ("struck note: and after it", rmsAfter < 3.0, rmsAfter, "c");
        expect ("struck note: the piano is found beside the voice", ! mP.nb.empty(), (double) mP.nb.size());
        // the retune, with the piano handed in as a sound that is there from the strike on (as a measure that finds it only for a part of the note hands it in), as a sound
        // there all through the note, and not found at all
        Params pP; pP.a4 = 440.0; pP.move = 35.0; pP.snap = 0.0; pP.ease = 0.0;
        const Plan plP = planCurve (mP.ts, mP.f0, pP, 74);
        const double taP = mP.ts.front() - 0.45, tbP = mP.ts.back() + pP.hold + pP.fadeOut + 0.3;
        const size_t j0 = (size_t) std::max (0.0, taP * kSr), j1 = std::min (n4, (size_t) (tbP * kSr));
        Block segP (3); for (size_t c = 0; c < 3; ++c) segP[c].assign (mixP[c].begin() + (long) j0, mixP[c].begin() + (long) j1);
        const Vec fPiano (n4, fP);
        Vec lineP (mP.ts.size (), fP), actP (mP.ts.size (), 0.0);                                   // the piano's line on the grid of the note, and when it sounds
        for (size_t k = 0; k < mP.ts.size(); ++k) actP[k] = std::min (1.0, std::max (0.0, (mP.ts[k] - (tOn - 0.05)) / 0.1));
        auto runP = [&] (const std::vector<Vec>& nb, const std::vector<Vec>& act)
        {
            const Block done = retuneSegment (segP, kSr, (double) j0 / kSr, mP.ts, mP.f0, plP.off, pP, {}, nb, act);
            Block out = mixP; for (size_t c = 0; c < 3; ++c) std::copy (done[c].begin(), done[c].end(), out[c].begin() + (long) j0); return out;
        };
        const size_t aE = (size_t) (1.0 * kSr), eE = (size_t) (1.8 * kSr), aP = (size_t) ((tOn + 0.15) * kSr), eP = (size_t) ((tOn + 1.2) * kSr);
        for (int pass = 0; pass < 3; ++pass)
        {
            const Block out = pass == 0 ? runP ({ lineP }, { actP }) : pass == 1 ? runP ({ lineP }, {}) : runP ({}, {});
            const double earlyLanding = db (lineEnergy (out, fSnew, aE, eE) / lineEnergy (idealP, fSnew, aE, eE));
            const double landing = db (lineEnergy (out, fSnew, aP, eP) / lineEnergy (idealP, fSnew, aP, eP));
            const double ghost = db (lineEnergy (out, fS, aP, eP) / lineEnergy (mixP, fS, aP, eP));
            const double pianoMoved = db (lineEnergy (out, fPiano, aP, eP, 3.0) / lineEnergy (idealP, fPiano, aP, eP, 3.0));
            std::printf ("  %s: voice lands %+.2f dB before the strike, %+.2f dB after it; old pitch left %+.1f dB; the piano's overtones %+.2f dB\n",
                         pass == 0 ? "piano kept apart from its strike on" : pass == 1 ? "piano kept apart all through the note" : "piano not found (the whole band moves)", earlyLanding, landing, ghost, pianoMoved);
            if (pass == 0)
            {
                expect ("struck note: the voice lands at the new pitch before the strike (level error)", std::abs (earlyLanding) < 2.0, earlyLanding, "dB");
                expect ("struck note: and while the piano sounds", std::abs (landing) < 2.0, landing, "dB");
                expect ("struck note: the old pitch is gone from the voice", ghost < -10.0, ghost, "dB");
                expect ("struck note: the piano's own overtones are left as they were", std::abs (pianoMoved) < 2.0, pianoMoved, "dB");
            }
            if (pass == 2) expect ("(with the piano not found, its overtones are moved with the voice)", pianoMoved < -3.0, pianoMoved, "dB");
        }
    }
    std::printf (failures == 0 ? "ALL OK\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
