#pragma once
#include "Dsp.h"
#include <functional>

namespace td
{
/** A line of pitch against time, drawn by the user: any number of points, +100 cents at the top of the lane, -100 at the bottom,
    0 in the middle. Straight lines join the points; before the first point and after the last one the line stays level. */
struct PitchCurve
{
    struct Point { double t = 0.0; double cents = 0.0; };         // t: seconds from the start of the marked part
    std::vector<Point> pts;                                        // kept sorted by t
    static constexpr double kRange = 100.0;                        // cents at the top / bottom of the lane

    void sortPoints() { std::stable_sort (pts.begin(), pts.end(), [] (const Point& a, const Point& b) { return a.t < b.t; }); }
    /** True when the line is on the 0 line everywhere (nothing would change). */
    bool isFlat() const { for (auto& p : pts) if (std::abs (p.cents) >= 0.005) return false; return true; }
    double centsAt (double t) const
    {
        if (pts.empty()) return 0.0;
        if (t <= pts.front().t) return pts.front().cents;
        if (t >= pts.back().t) return pts.back().cents;
        size_t lo = 0, hi = pts.size() - 1;
        while (hi - lo > 1) { const size_t mid = (lo + hi) / 2; (pts[mid].t <= t ? lo : hi) = mid; }
        const double span = pts[hi].t - pts[lo].t;
        if (span <= 0.0) return pts[hi].cents;
        return pts[lo].cents + (pts[hi].cents - pts[lo].cents) * (t - pts[lo].t) / span;
    }
};

/** Shifts the pitch of audio without changing its length: a phase vocoder (with phase locking around the spectral peaks, so
    chords and strings stay clean) stretches the sound by the pitch ratio, then it is played back faster / slower. Offline use. */
class PitchShifter
{
public:
    /** Returns 'in' shifted by 'cents' (+100 = one semitone up). Same length as the input. */
    static std::vector<float> shift (const std::vector<float>& in, double sampleRate, double cents)
    {
        const size_t L = in.size();
        if (L == 0 || std::abs (cents) < 0.01) return in;
        const double r = std::pow (2.0, cents / 1200.0);                              // pitch ratio = time-stretch ratio
        const int N = sampleRate > 60000.0 ? 8192 : 4096;
        const int Hs = N / 4;
        const double Ha = (double) Hs / r;                                            // analysis hop
        const int P = N;                                                              // padding either side
        std::vector<double> xp ((size_t) L + 2 * (size_t) P, 0.0);
        for (size_t i = 0; i < L; ++i) xp[i + (size_t) P] = (double) in[i];

        const dsp::FFT fft (N);
        const auto win = dsp::hann (N);
        const int bins = N / 2 + 1;
        const long frames = (long) std::ceil ((double) (xp.size() - (size_t) N) / Ha) + 1;
        const size_t outLen = (size_t) ((frames - 1) * Hs + N);
        std::vector<double> out (outLen + (size_t) N, 0.0), norm (outLen + (size_t) N, 0.0);

        std::vector<std::complex<double>> buf ((size_t) N);
        std::vector<double> prevPhase ((size_t) bins, 0.0), synPhase ((size_t) bins, 0.0), mag ((size_t) bins), ph ((size_t) bins), newPh ((size_t) bins);
        std::vector<int> peaks; peaks.reserve ((size_t) bins);
        long prevStart = 0;
        for (long k = 0; k < frames; ++k)
        {
            const long start = (long) std::llround ((double) k * Ha);
            for (int i = 0; i < N; ++i)
            {
                const long idx = start + i;
                buf[(size_t) i] = (idx >= 0 && idx < (long) xp.size()) ? xp[(size_t) idx] * win[(size_t) i] : 0.0;
            }
            fft.forward (buf);
            for (int b = 0; b < bins; ++b) { mag[(size_t) b] = std::abs (buf[(size_t) b]); ph[(size_t) b] = std::arg (buf[(size_t) b]); }

            if (k == 0) { for (int b = 0; b < bins; ++b) newPh[(size_t) b] = ph[(size_t) b]; }
            else
            {
                const double hop = (double) (start - prevStart);
                // peaks: local maxima over +-2 bins
                peaks.clear();
                for (int b = 0; b < bins; ++b)
                {
                    bool pk = true;
                    for (int d = -2; d <= 2 && pk; ++d) { const int q = b + d; if (d != 0 && q >= 0 && q < bins && mag[(size_t) q] > mag[(size_t) b]) pk = false; }
                    if (pk && mag[(size_t) b] > 0.0) peaks.push_back (b);
                }
                if (peaks.empty()) peaks.push_back (0);
                for (size_t pi = 0; pi < peaks.size(); ++pi)
                {
                    const int pb = peaks[pi];
                    const double omega = 2.0 * dsp::kPi * (double) pb / (double) N;
                    const double dphi = dsp::wrapPhase (ph[(size_t) pb] - prevPhase[(size_t) pb] - omega * hop);
                    const double inst = omega + dphi / hop;
                    const double peakSyn = synPhase[(size_t) pb] + inst * (double) Hs;
                    const int lo = pi == 0 ? 0 : (peaks[pi - 1] + pb) / 2 + 1;
                    const int hi = pi + 1 == peaks.size() ? bins - 1 : (pb + peaks[pi + 1]) / 2;
                    for (int b = lo; b <= hi; ++b) newPh[(size_t) b] = peakSyn + (ph[(size_t) b] - ph[(size_t) pb]);   // identity phase locking
                }
            }
            prevStart = start;
            for (int b = 0; b < bins; ++b) { prevPhase[(size_t) b] = ph[(size_t) b]; synPhase[(size_t) b] = newPh[(size_t) b]; }

            for (int b = 0; b < bins; ++b) buf[(size_t) b] = std::polar (mag[(size_t) b], newPh[(size_t) b]);
            for (int b = 1; b < N / 2; ++b) buf[(size_t) (N - b)] = std::conj (buf[(size_t) b]);
            buf[(size_t) (N / 2)] = std::complex<double> (buf[(size_t) (N / 2)].real(), 0.0);
            buf[0] = std::complex<double> (buf[0].real(), 0.0);
            fft.inverse (buf);
            const size_t o = (size_t) k * (size_t) Hs;
            for (int i = 0; i < N; ++i) { out[o + (size_t) i] += buf[(size_t) i].real() * win[(size_t) i]; norm[o + (size_t) i] += win[(size_t) i] * win[(size_t) i]; }
        }
        for (size_t i = 0; i < out.size(); ++i) if (norm[i] > 1.0e-6) out[i] /= norm[i];

        // play the stretched sound back at speed r: output sample j reads position r*j + N/2*(r+1)
        std::vector<float> res (L);
        const double off = 0.5 * (double) N * (r + 1.0);
        auto at = [&] (long i) { return (i >= 0 && i < (long) out.size()) ? out[(size_t) i] : 0.0; };
        for (size_t j = 0; j < L; ++j)
        {
            const double pos = r * (double) j + off;
            const long i0 = (long) std::floor (pos); const double f = pos - (double) i0;
            const double y0 = at (i0 - 1), y1 = at (i0), y2 = at (i0 + 1), y3 = at (i0 + 2);      // cubic (Catmull-Rom)
            const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3, b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c = -0.5 * y0 + 0.5 * y2;
            res[j] = (float) (((a * f + b) * f + c) * f + y1);
        }
        return res;
    }
    /** Shifts the pitch by an amount that changes along the audio: 'centsAt (i)' gives the shift (cents) at input sample i.
        The same phase vocoder as shift(), but its analysis hop follows the local pitch ratio, and the play-back speed follows it too,
        so a slow drift and a sudden jump are both corrected, and the result has the same length as the input. */
    static std::vector<float> shiftCurve (const std::vector<float>& in, double sampleRate, const std::function<double (double)>& centsAt)
    {
        const size_t L = in.size();
        if (L == 0) return in;
        const int N = sampleRate > 60000.0 ? 8192 : 4096;
        const int Hs = N / 4;
        const int P = N;
        std::vector<double> xp ((size_t) L + 2 * (size_t) P, 0.0);
        for (size_t i = 0; i < L; ++i) xp[i + (size_t) P] = (double) in[i];

        // where every analysis frame starts: each hop is the synthesis hop divided by the pitch ratio at that spot
        std::vector<long> starts;
        {
            double pos = 0.0;
            const long lastStart = (long) xp.size() - N;
            for (;;)
            {
                const long st = (long) std::llround (pos);
                starts.push_back (st);
                if (st >= lastStart) break;
                const double cents = std::clamp (centsAt (pos + 0.5 * N - P), -2400.0, 2400.0);
                pos += (double) Hs / std::pow (2.0, cents / 1200.0);
            }
        }
        const long frames = (long) starts.size();

        const dsp::FFT fft (N);
        const auto win = dsp::hann (N);
        const int bins = N / 2 + 1;
        const size_t outLen = (size_t) ((frames - 1) * Hs + N);
        std::vector<double> out (outLen + (size_t) N, 0.0), norm (outLen + (size_t) N, 0.0);

        std::vector<std::complex<double>> buf ((size_t) N);
        std::vector<double> prevPhase ((size_t) bins, 0.0), synPhase ((size_t) bins, 0.0), mag ((size_t) bins), ph ((size_t) bins), newPh ((size_t) bins);
        std::vector<int> peaks; peaks.reserve ((size_t) bins);
        long prevStart = 0;
        for (long k = 0; k < frames; ++k)
        {
            const long start = starts[(size_t) k];
            for (int i = 0; i < N; ++i)
            {
                const long idx = start + i;
                buf[(size_t) i] = (idx >= 0 && idx < (long) xp.size()) ? xp[(size_t) idx] * win[(size_t) i] : 0.0;
            }
            fft.forward (buf);
            for (int b = 0; b < bins; ++b) { mag[(size_t) b] = std::abs (buf[(size_t) b]); ph[(size_t) b] = std::arg (buf[(size_t) b]); }

            if (k == 0) { for (int b = 0; b < bins; ++b) newPh[(size_t) b] = ph[(size_t) b]; }
            else
            {
                const double hop = (double) (start - prevStart);
                peaks.clear();
                for (int b = 0; b < bins; ++b)
                {
                    bool pk = true;
                    for (int d = -2; d <= 2 && pk; ++d) { const int q = b + d; if (d != 0 && q >= 0 && q < bins && mag[(size_t) q] > mag[(size_t) b]) pk = false; }
                    if (pk && mag[(size_t) b] > 0.0) peaks.push_back (b);
                }
                if (peaks.empty()) peaks.push_back (0);
                for (size_t pi = 0; pi < peaks.size(); ++pi)
                {
                    const int pb = peaks[pi];
                    const double omega = 2.0 * dsp::kPi * (double) pb / (double) N;
                    const double dphi = dsp::wrapPhase (ph[(size_t) pb] - prevPhase[(size_t) pb] - omega * hop);
                    const double inst = omega + dphi / hop;
                    const double peakSyn = synPhase[(size_t) pb] + inst * (double) Hs;
                    const int lo = pi == 0 ? 0 : (peaks[pi - 1] + pb) / 2 + 1;
                    const int hi = pi + 1 == peaks.size() ? bins - 1 : (pb + peaks[pi + 1]) / 2;
                    for (int b = lo; b <= hi; ++b) newPh[(size_t) b] = peakSyn + (ph[(size_t) b] - ph[(size_t) pb]);
                }
            }
            prevStart = start;
            for (int b = 0; b < bins; ++b) { prevPhase[(size_t) b] = ph[(size_t) b]; synPhase[(size_t) b] = newPh[(size_t) b]; }

            for (int b = 0; b < bins; ++b) buf[(size_t) b] = std::polar (mag[(size_t) b], newPh[(size_t) b]);
            for (int b = 1; b < N / 2; ++b) buf[(size_t) (N - b)] = std::conj (buf[(size_t) b]);
            buf[(size_t) (N / 2)] = std::complex<double> (buf[(size_t) (N / 2)].real(), 0.0);
            buf[0] = std::complex<double> (buf[0].real(), 0.0);
            fft.inverse (buf);
            const size_t o = (size_t) k * (size_t) Hs;
            for (int i = 0; i < N; ++i) { out[o + (size_t) i] += buf[(size_t) i].real() * win[(size_t) i]; norm[o + (size_t) i] += win[(size_t) i] * win[(size_t) i]; }
        }
        for (size_t i = 0; i < out.size(); ++i) if (norm[i] > 1.0e-6) out[i] /= norm[i];

        // Play the stretched sound back at the local speed: input time t (unpadded samples) sits at stretched position S(t), found
        // between the frame centres (input centre of frame k = start_k + N/2 - P, stretched centre = k*Hs + N/2). Where the ratio is
        // constant this is the same line as in shift().
        std::vector<double> ic ((size_t) frames), sc ((size_t) frames);
        for (long k = 0; k < frames; ++k) { ic[(size_t) k] = (double) starts[(size_t) k] + 0.5 * N - P; sc[(size_t) k] = (double) k * Hs + 0.5 * N; }
        std::vector<float> res (L);
        auto at = [&] (long i) { return (i >= 0 && i < (long) out.size()) ? out[(size_t) i] : 0.0; };
        long seg = 0;
        for (size_t j = 0; j < L; ++j)
        {
            const double t = (double) j;
            while (seg + 2 < frames && ic[(size_t) seg + 1] <= t) ++seg;
            double pos;
            if (frames < 2) pos = sc[0] + (t - ic[0]);
            else
            {
                const double dI = ic[(size_t) seg + 1] - ic[(size_t) seg];
                const double slope = dI > 0.0 ? (sc[(size_t) seg + 1] - sc[(size_t) seg]) / dI : 1.0;
                pos = sc[(size_t) seg] + (t - ic[(size_t) seg]) * slope;
            }
            const long i0 = (long) std::floor (pos); const double f = pos - (double) i0;
            const double y0 = at (i0 - 1), y1 = at (i0), y2 = at (i0 + 1), y3 = at (i0 + 2);
            const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3, b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c = -0.5 * y0 + 0.5 * y2;
            res[j] = (float) (((a * f + b) * f + c) * f + y1);
        }
        return res;
    }
};
} // namespace td