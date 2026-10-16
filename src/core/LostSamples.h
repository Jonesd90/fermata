#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

namespace td
{
/** Lost samples: the digital click left when a block of samples goes missing (a recorder or network buffer that was dropped, NOT filled with silence: a dropout of that
    kind is mended by SpectralRepair::mendDropouts). The sound jumps in ONE sample, with no ringing at all on either side (nothing that came through a converter can do
    that: its filter rounds every edge off), at exactly the same sample on every track of the recording; after the jump the music simply goes on from a little later.
    So nothing was ADDED that could be taken off (the old 'step with a fading tail' idea was wrong: taking a tail off that is not there put a low thump of its own in), and
    smoothing the jump over a few samples only takes the top of the click away: the music before and after no longer line up in time, which is the low half of the click.

    The mend:  1. find the jump (on every track at once: it is the same event everywhere);
               2. find how many samples are missing: the sound before is continued forwards and the sound after backwards (a long prediction model of each side), and the gap
                  is the length at which those continuations best meet the real sound on the other side, added up over ALL the tracks (one track alone can be fooled);
               3. put that many samples back in, rebuilt from the sound on both sides;
               4. squeeze the time around it back to the original length: the part of up to half a second each side of the click plays a tiny bit faster (128 lost samples
                  over a second: 0.13 %, about 2 cents, far below what anyone can hear), so the file keeps its length and everything outside that second is untouched.
    An acoustic click (a noise in the room, with its reverb) is something else: it is smoothed by the converter and arrives at each microphone at its own time; it is
    left to the ordinary click finder (and its reverb tail to ReTouch / Attenuate). */
struct LostSamples
{
    struct Splice { long at = 0; int lost = 0; };                 // 'at': the first sample after the jump; 'lost': how many samples are missing before it

    /** Burg's method: a prediction model (a[0] = 1) of order p from x[0..n). Always stable, also for very tonal sound. */
    static std::vector<double> burg (const double* x, long n, int p)
    {
        std::vector<double> a (1, 1.0), f (x, x + n), b (x, x + n), an;
        p = (int) juce::jmin ((long) p, n / 2);
        for (int m = 0; m < p; ++m)
        {
            double num = 0.0, den = 0.0;
            for (long i = m + 1; i < n; ++i) { num += f[(size_t) i] * b[(size_t) i - 1]; den += f[(size_t) i] * f[(size_t) i] + b[(size_t) i - 1] * b[(size_t) i - 1]; }
            const double k = den > 0.0 ? -2.0 * num / den : 0.0;
            an.assign (a.size() + 1, 0.0);
            for (size_t i = 0; i < a.size(); ++i) an[i] = a[i];
            for (size_t i = 0; i < a.size(); ++i) an[a.size() - i] += k * a[i];
            a.swap (an);
            for (long i = n - 1; i >= m + 1; --i) { const double fi = f[(size_t) i], bi = b[(size_t) i - 1]; f[(size_t) i] = fi + k * bi; b[(size_t) i] = bi + k * fi; }
        }
        return a;
    }

    /** Continues hist[0..histLen) by 'count' samples with the model a (histLen must be at least the order). */
    static std::vector<double> extrapolate (const double* hist, long histLen, const std::vector<double>& a, long count)
    {
        const int p = (int) a.size() - 1;
        std::vector<double> y (hist + histLen - p, hist + histLen); y.resize ((size_t) (p + count), 0.0);
        for (long i = 0; i < count; ++i)
        {
            double v = 0.0; const double* yy = y.data() + p + i;
            for (int k = 1; k <= p; ++k) v -= a[(size_t) k] * yy[-k];
            y[(size_t) (p + i)] = v;
        }
        return std::vector<double> (y.begin() + p, y.end());
    }

    /** Levinson-Durbin: the prediction model (a[0] = 1) of order p from the autocorrelation R[0..p]. */
    static std::vector<double> levinson (const std::vector<double>& R, int p)
    {
        std::vector<double> a ((size_t) p + 1, 0.0), tmp; a[0] = 1.0;
        double err = R[0];
        for (int i = 1; i <= p; ++i)
        {
            double acc = R[(size_t) i]; for (int j = 1; j < i; ++j) acc += a[(size_t) j] * R[(size_t) (i - j)];
            const double k = err > 0.0 ? -acc / err : 0.0;
            tmp = a; for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + k * tmp[(size_t) (i - j)];
            a[(size_t) i] = k; err *= (1.0 - k * k);
            if (err <= 0.0) break;
        }
        return a;
    }

    /** Rebuilds x[g0,g1) from the sound on both sides: the most likely samples for a prediction model of order 'order' learnt from 'ctx' samples each side (least
        squares), PLUS a random draw of the part the model cannot predict, at exactly the level and colour it leaves out. Without that draw a gap of more than a few
        samples comes out too smooth: a tiny hole in the hiss and air of the recording (the most likely value of noise is no noise). With it the rebuilt stretch has the
        same texture as the sound around it. The draw is the same every time for the same place (a fixed seed), so the preview and the written file agree. */
    static bool fill (std::vector<float>& x, long g0, long g1, int order, long ctx, bool texture = true)
    {
        const long L = (long) x.size();
        g0 = juce::jlimit (0L, L, g0); g1 = juce::jlimit (0L, L, g1);
        const long m = g1 - g0;
        if (m < 1) return true;
        if (m > 4096) return false;
        const long s0 = juce::jmax (0L, g0 - ctx), s1 = juce::jmin (L, g1 + ctx);
        const int p = (int) juce::jmin ((long) order, (g0 - s0 + s1 - g1) / 8);
        if (p < 4) return false;
        // the model, from the two clean blocks (each tapered, so their ends do not count as sound)
        std::vector<double> R ((size_t) p + 1, 0.0);
        auto accumulate = [&] (long a, long b)
        {
            const long n = b - a; if (n <= p) return;
            std::vector<double> w ((size_t) n);
            for (long i = 0; i < n; ++i) w[(size_t) i] = (double) x[(size_t) (a + i)] * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * ((double) i + 0.5) / (double) n));
            for (int k = 0; k <= p; ++k) { double acc = 0.0; for (long i = k; i < n; ++i) acc += w[(size_t) i] * w[(size_t) (i - k)]; R[(size_t) k] += acc; }
        };
        accumulate (s0, g0); accumulate (g1, s1);
        if (R[0] < 1.0e-14) return true;
        R[0] *= 1.0 + 1.0e-9;
        const auto a = levinson (R, p);
        auto xv = [&] (long n) -> double { return (n >= 0 && n < L) ? (double) x[(size_t) n] : 0.0; };
        // what the model leaves of the clean sound (its level is the level of the drawn detail)
        double e2 = 0.0; long ne = 0;
        auto resid = [&] (long a0, long b0) { for (long n = a0 + p; n < b0; ++n) { double s = 0.0; for (int k = 0; k <= p; ++k) s += a[(size_t) k] * xv (n - k); e2 += s * s; ++ne; } };
        resid (s0, g0); resid (g1, s1);
        const double sigma = ne > 0 ? std::sqrt (e2 / (double) ne) : 0.0;
        // unknowns u_i = x[g0 + i]: minimise the prediction error over every residual the gap touches
        const long eLo = g0, eHi = g1 + p;
        std::vector<double> c ((size_t) (eHi - eLo), 0.0);
        for (long n = eLo; n < eHi; ++n)
        {
            double sum = 0.0;
            for (int k = 0; k <= p; ++k) { const long q = n - k; if (q >= g0 && q < g1) continue; sum += a[(size_t) k] * xv (q); }
            c[(size_t) (n - eLo)] = sum;
        }
        std::vector<double> Ra ((size_t) p + 1, 0.0);
        for (int d = 0; d <= p; ++d) for (int k = 0; k + d <= p; ++k) Ra[(size_t) d] += a[(size_t) k] * a[(size_t) (k + d)];
        std::vector<double> M ((size_t) (m * m), 0.0), rhs ((size_t) m, 0.0);
        for (long i = 0; i < m; ++i)
        {
            for (long j = juce::jmax (0L, i - p); j <= juce::jmin (m - 1, i + p); ++j) M[(size_t) (i * m + j)] = Ra[(size_t) std::abs (i - j)];
            double r = 0.0;
            for (int k = 0; k <= p; ++k) { const long n = g0 + i + k; if (n >= eLo && n < eHi) r += a[(size_t) k] * c[(size_t) (n - eLo)]; }
            rhs[(size_t) i] = -r;
            M[(size_t) (i * m + i)] += 1.0e-9 * Ra[0];
        }
        // Cholesky (M is banded: only the band is worked on)
        std::vector<double> Lm ((size_t) (m * m), 0.0);
        for (long i = 0; i < m; ++i)
            for (long j = juce::jmax (0L, i - p); j <= i; ++j)
            {
                double sum = M[(size_t) (i * m + j)];
                for (long k = juce::jmax (0L, i - p); k < j; ++k) sum -= Lm[(size_t) (i * m + k)] * Lm[(size_t) (j * m + k)];
                if (i == j) { if (sum <= 0.0) return false; Lm[(size_t) (i * m + i)] = std::sqrt (sum); }
                else Lm[(size_t) (i * m + j)] = sum / Lm[(size_t) (j * m + j)];
            }
        std::vector<double> y ((size_t) m), u ((size_t) m), z ((size_t) m, 0.0);
        for (long i = 0; i < m; ++i) { double s = rhs[(size_t) i]; for (long k = juce::jmax (0L, i - p); k < i; ++k) s -= Lm[(size_t) (i * m + k)] * y[(size_t) k]; y[(size_t) i] = s / Lm[(size_t) (i * m + i)]; }
        if (texture && sigma > 0.0)
        {
            juce::Random rnd ((juce::int64) (0x5DEECE66DLL ^ ((juce::int64) g0 * 2654435761LL) ^ ((juce::int64) m << 20)));
            for (long i = 0; i < m; i += 2)
            {
                const double r1 = juce::jmax (1.0e-300, (double) rnd.nextDouble()), r2 = (double) rnd.nextDouble();
                const double rad = std::sqrt (-2.0 * std::log (r1));
                z[(size_t) i] = rad * std::cos (2.0 * juce::MathConstants<double>::pi * r2);
                if (i + 1 < m) z[(size_t) i + 1] = rad * std::sin (2.0 * juce::MathConstants<double>::pi * r2);
            }
        }
        // back substitution: u = L^-T (y + sigma z)   (the most likely samples, plus a draw from what is still uncertain about them)
        for (long i = m - 1; i >= 0; --i)
        {
            double s = y[(size_t) i] + sigma * z[(size_t) i];
            for (long k = i + 1; k <= juce::jmin (m - 1, i + p); ++k) s -= Lm[(size_t) (k * m + i)] * u[(size_t) k];
            u[(size_t) i] = s / Lm[(size_t) (i * m + i)];
        }
        for (long i = 0; i < m; ++i) x[(size_t) (g0 + i)] = (float) u[(size_t) i];
        return true;
    }

    /** The jumps in [t0,t1) of one track that look like lost samples: the sound jumps in one sample by far more than its usual second difference (the sensitivity 1..10
        decides how far), does not come back (a spike would), and is perfectly smooth on both sides (no ringing). The edges of a run of digital silence do not count. */
    static std::vector<long> findJumps (const std::vector<float>& x, long t0, long t1, double sensitivity, double sr)
    {
        std::vector<long> out;
        const long L = (long) x.size();
        const long a0 = juce::jmax (8L, t0), a1 = juce::jmin (L - 8, t1);
        if (a1 - a0 < 4) return out;
        const double thr = juce::jmax (6.0, 20.0 - 1.4 * sensitivity);
        const long blk = juce::jmax (64L, (long) (0.004 * sr));
        const long S0 = juce::jmax (2L, a0 - 4 * blk), S1 = juce::jmin (L, a1 + 4 * blk), nb = (S1 - S0) / blk + 1;
        std::vector<double> sg ((size_t) nb), sgm ((size_t) nb), v;
        for (long b = 0; b < nb; ++b)
        {
            v.clear(); for (long n = S0 + b * blk; n < juce::jmin (S1, S0 + (b + 1) * blk); ++n) v.push_back (std::abs ((double) x[(size_t) n] - 2.0 * (double) x[(size_t) n - 1] + (double) x[(size_t) n - 2]));
            if (v.empty()) { sg[(size_t) b] = 1.0e-12; continue; }
            auto mid = v.begin() + (long) v.size() / 2; std::nth_element (v.begin(), mid, v.end()); sg[(size_t) b] = *mid / 0.6745 + 1.0e-12;
        }
        for (long b = 0; b < nb; ++b)
        {
            v.clear(); for (long k = juce::jmax (0L, b - 4); k <= juce::jmin (nb - 1, b + 4); ++k) v.push_back (sg[(size_t) k]);
            auto mid = v.begin() + (long) v.size() / 2; std::nth_element (v.begin(), mid, v.end()); sgm[(size_t) b] = *mid;
        }
        auto c2 = [&] (long q) { return (double) x[(size_t) q + 1] - 2.0 * (double) x[(size_t) q] + (double) x[(size_t) q - 1]; };
        auto silent = [&] (long q) { return std::abs (x[(size_t) q]) <= 3.0e-7f; };
        for (long n = a0; n < a1; ++n)
        {
            const double dprev = (double) x[(size_t) n - 1] - (double) x[(size_t) n - 2], d = (double) x[(size_t) n] - (double) x[(size_t) n - 1], dn = (double) x[(size_t) n + 1] - (double) x[(size_t) n];
            const double jump = d - dprev;
            const double sig = sgm[(size_t) juce::jlimit (0L, nb - 1, (n - S0) / blk)];
            if (std::abs (jump) < thr * sig || std::abs (jump) < 2.0e-4) continue;
            if (std::abs (dn - dprev) > 0.6 * std::abs (jump)) continue;                                           // a spike comes straight back: that is an ordinary click
            const double ring = juce::jmax (juce::jmax (std::abs (c2 (n - 2)), std::abs (c2 (n - 3))), juce::jmax (std::abs (c2 (n + 1)), std::abs (c2 (n + 2))));
            if (ring > 0.3 * std::abs (jump)) continue;                                                             // ringing round the edge: it came through a converter (an acoustic event)
            if ((silent (n) && silent (n + 1) && silent (n + 2)) || (silent (n - 1) && silent (n - 2) && silent (n - 3))) continue;     // the edge of a dropout of digital silence
            out.push_back (n); n += 2;
        }
        return out;
    }

    /** Adds to total[k] (k = 0 .. kMax) how well 'k lost samples at at' fits this track: the sound before, continued forwards by k samples, should meet the sound after
        the jump, and the sound after, continued backwards, should meet the sound before. (Higher = better; each side counts at most 4.) Returns false for a track with
        no sound there or not enough of it on both sides. */
    static bool scoreLost (const std::vector<float>& x, long at, double sr, int kMax, std::vector<double>& total)
    {
        const long L = (long) x.size();
        const int p = juce::jlimit (32, 512, (int) std::lround (0.0053 * sr));
        const long Mw = juce::jmax (48L, (long) (0.005 * sr));
        long T = juce::jmin ((long) (0.05 * sr), juce::jmin (at, L - at));
        if (T < juce::jmax (4L * p, Mw + 8) || at + Mw > L) return false;
        std::vector<double> pre ((size_t) T), post ((size_t) T);
        for (long i = 0; i < T; ++i) { pre[(size_t) i] = (double) x[(size_t) (at - T + i)]; post[(size_t) i] = (double) x[(size_t) (at + T - 1 - i)]; }     // post: the sound after, backwards
        double ea = 0.0, eb = 0.0;
        for (long i = 0; i < Mw; ++i) { ea += (double) x[(size_t) (at + i)] * (double) x[(size_t) (at + i)]; eb += (double) x[(size_t) (at - 1 - i)] * (double) x[(size_t) (at - 1 - i)]; }
        if (ea < 1.0e-12 * (double) Mw || eb < 1.0e-12 * (double) Mw) return false;
        const auto F = extrapolate (pre.data(), T, burg (pre.data(), T, p), kMax + Mw);
        const auto B = extrapolate (post.data(), T, burg (post.data(), T, p), kMax + Mw);
        for (int k = 0; k <= kMax; ++k)
        {
            double e1 = 0.0, e2 = 0.0;
            for (long i = 0; i < Mw; ++i)
            {
                const double u = F[(size_t) (k + i)] - (double) x[(size_t) (at + i)], w = B[(size_t) (k + i)] - (double) x[(size_t) (at - 1 - i)];
                e1 += u * u; e2 += w * w;
            }
            total[(size_t) k] -= juce::jmin (4.0, e1 / ea) + juce::jmin (4.0, e2 / eb);
        }
        return true;
    }

    /** How many samples are missing at 'at', judged on all the tracks together (0 if no track can tell). */
    static int estimateLost (const std::vector<const std::vector<float>*>& tracks, long at, double sr)
    {
        const int kMax = (int) juce::jlimit (64L, 2048L, (long) (0.012 * sr));
        std::vector<double> total ((size_t) kMax + 1, 0.0);
        int used = 0;
        for (auto* t : tracks) if (t != nullptr && scoreLost (*t, at, sr, kMax, total)) ++used;
        if (used == 0) return 0;
        return (int) (std::max_element (total.begin(), total.end()) - total.begin());
    }

    /** The lost-sample jumps in [t0,t1), found on all the tracks together (they are one event, at the same sample everywhere: with three tracks or more it has to be seen on at
        least a fifth of them, two at the least, so a single odd sample in one track is not taken for one), each with the number of samples missing there. */
    static std::vector<Splice> find (const std::vector<const std::vector<float>*>& tracks, long t0, long t1, double sensitivity, double sr)
    {
        std::map<long, int> votes;
        int nt = 0;
        for (auto* t : tracks)
        {
            if (t == nullptr) continue;
            ++nt;
            for (long n : findJumps (*t, t0, t1, sensitivity, sr))
            {
                long key = n;
                if (votes.count (n - 1)) key = n - 1; else if (votes.count (n + 1)) key = n + 1;
                ++votes[key];
            }
        }
        const int need = nt >= 3 ? juce::jmax (2, (int) std::ceil (0.2 * nt)) : 1;
        std::vector<Splice> out;
        for (auto& kv : votes)
        {
            if (kv.second < need) continue;
            if (! out.empty() && kv.first - out.back().at < (long) (0.002 * sr)) continue;
            out.push_back ({ kv.first, estimateLost (tracks, kv.first, sr) });
        }
        return out;
    }

    /** The windowed-sinc kernel used for the time squeeze (Kaiser window, 32 zero crossings each side), as a table of 'phases' between two samples. */
    struct Kernel
    {
        static constexpr int H = 32, P = 512;
        std::vector<float> t;                                                       // (P + 1) rows of 2H taps: row ph is for the fraction ph / P
        Kernel()
        {
            const double beta = 9.0;
            auto i0 = [] (double z) { double s = 1.0, term = 1.0; for (int j = 1; j < 60; ++j) { term *= (z / (2.0 * j)) * (z / (2.0 * j)); s += term; if (term < 1.0e-14 * s) break; } return s; };
            const double ib = i0 (beta);
            t.resize ((size_t) (P + 1) * 2 * H);
            for (int ph = 0; ph <= P; ++ph)
            {
                const double fr = (double) ph / P;
                for (int k = -H + 1; k <= H; ++k)
                {
                    const double u = (double) k - fr;
                    const double r = u / H, w = std::abs (r) >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - r * r)) / ib;
                    const double s = std::abs (u) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * u) / (juce::MathConstants<double>::pi * u);
                    t[(size_t) ph * 2 * H + (size_t) (k + H - 1)] = (float) (s * w);
                }
            }
        }
        static const Kernel& get() { static const Kernel k; return k; }
        /** x at the fractional position pos (pos - H .. pos + H must lie inside x). */
        double at (const std::vector<double>& x, double pos) const
        {
            const long i = (long) std::floor (pos); const double fr = (pos - (double) i) * P;
            const int ph = juce::jmin (P - 1, (int) fr); const double g = fr - ph;
            const float* r0 = t.data() + (size_t) ph * 2 * H; const float* r1 = r0 + 2 * H;
            const double* xx = x.data() + (i - H + 1);
            double s0 = 0.0, s1 = 0.0;
            for (int k = 0; k < 2 * H; ++k) { s0 += (double) r0[k] * xx[k]; s1 += (double) r1[k] * xx[k]; }
            return s0 + g * (s1 - s0);
        }
    };

    /** Puts 'lost' samples back in front of 'at' (rebuilt from the sound on both sides) and squeezes [u0,u1) back to its own length, so the track keeps its length and
        nothing outside [u0,u1) changes. The squeeze reaches up to maxWarp seconds each side, never outside [lim0,lim1). Too little room for the squeeze (less than about
        16 times the lost samples) gives an ordinary mend of the jump instead. Returns true if anything was done. */
    static bool mend (std::vector<float>& x, long at, int lost, double sr, long lim0, long lim1, double maxWarp = 0.5)
    {
        const long L = (long) x.size();
        if (at < 8 || at > L - 8) return false;
        const long H = Kernel::H + 2;
        const long W = (long) (maxWarp * sr);
        const long u0 = juce::jmax (juce::jmax (lim0, at - W), H), u1 = juce::jmin (juce::jmin (lim1, at + W), L - H);
        const long N = u1 - u0;
        if (lost < 2 || at - u0 < 16 || u1 - at < 16 || N < juce::jmax ((long) (0.03 * sr), 16L * lost))
            return fill (x, juce::jmax (u0, at - 3), juce::jmin (u1, at + 4), 48, 2048, false);                // (nothing missing, or no room: the jump is smoothed over a few samples)
        // the sound around the window with the lost samples put back in (as zeros first), then rebuilt from both sides
        const long ctx = (long) (0.084 * sr) + 64;
        const long e0 = juce::jmax (0L, u0 - ctx), e1 = juce::jmin (L, u1 + ctx);
        std::vector<float> xe; xe.reserve ((size_t) (e1 - e0 + lost));
        xe.insert (xe.end(), x.begin() + e0, x.begin() + at);
        xe.insert (xe.end(), (size_t) lost, 0.0f);
        xe.insert (xe.end(), x.begin() + at, x.begin() + e1);
        const long g0 = at - e0, g1 = g0 + lost;
        if (! fill (xe, g0, g1, juce::jlimit (32, 512, (int) std::lround (0.0042 * sr)), (long) (0.084 * sr))) return false;
        std::vector<double> xd (xe.begin(), xe.end());
        // the squeeze: output sample u (u0 <= u < u1) is taken from the extended sound at (u - e0) + lost * S, S running smoothly from 0 to 1 over the window
        // (the speed is even in the middle and eases in and out over the outer 15 % each side, so there is no sudden change of speed anywhere)
        std::vector<double> rate ((size_t) N);
        const double ramp = 0.15;
        for (long i = 0; i < N; ++i)
        {
            const double v = ((double) i + 0.5) / (double) N;
            rate[(size_t) i] = v < ramp ? 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * v / ramp)
                             : v > 1.0 - ramp ? 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (1.0 - v) / ramp) : 1.0;
        }
        double sum = 0.0; for (double r : rate) sum += r;
        const auto& ker = Kernel::get();
        double acc = 0.0;
        for (long i = 0; i < N; ++i)
        {
            const double pos = (double) (u0 + i - e0) + (double) lost * acc / sum;
            x[(size_t) (u0 + i)] = (float) ker.at (xd, pos);
            acc += rate[(size_t) i];
        }
        return true;
    }

    /** Mends several lost-sample jumps of one track, from the last to the first; the squeeze of each one stays clear of its neighbours. Returns how many were mended. */
    static int mendAll (std::vector<float>& x, std::vector<Splice> sp, double sr, long lim0, long lim1, double maxWarp = 0.5)
    {
        std::sort (sp.begin(), sp.end(), [] (const Splice& a, const Splice& b) { return a.at < b.at; });
        int n = 0;
        for (size_t i = sp.size(); i-- > 0;)
        {
            long lo = lim0, hi = lim1;
            if (i > 0) lo = juce::jmax (lo, (sp[i - 1].at + sp[i].at) / 2 + 64);
            if (i + 1 < sp.size()) hi = juce::jmin (hi, (sp[i].at + sp[i + 1].at) / 2 - 64);
            if (mend (x, sp[i].at, sp[i].lost, sr, lo, hi, maxWarp)) ++n;
        }
        return n;
    }
};
} // namespace td
