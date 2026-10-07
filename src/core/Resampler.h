#pragma once
#include "Common.h"
#include <vector>
#include <cmath>

namespace td
{
/** High quality sample-rate converter (windowed sinc, Kaiser window): about -110 dB of aliasing and imaging, no delay (the output sample k is exactly
    at the time k / outRate). One channel; feed it blocks of any size with process(), then call finish() once. Used for the Mastering exports. */
class SincResampler
{
public:
    SincResampler (double inRate, double outRate, int zeroCrossings = 32, double beta = 11.0)
        : step (inRate / outRate)
    {
        const double ratio = outRate / inRate;
        const double fc = ratio < 1.0 ? ratio * 0.985 : 1.0;                 // cut-off as a fraction of the INPUT Nyquist
        const double halfWidth = (double) zeroCrossings / fc;                 // in input samples
        taps = (int) std::ceil (halfWidth);
        const int n = (2 * taps + 1) * kRes + 2;
        table.assign ((size_t) n, 0.0f); diff.assign ((size_t) n, 0.0f);
        const double i0b = bessel0 (beta);
        for (int i = 0; i < n; ++i)
        {
            const double x = ((double) i / kRes) - (double) taps;               // input samples from the centre
            const double a = x / halfWidth;
            double w = 0.0;
            if (std::abs (a) < 1.0) w = bessel0 (beta * std::sqrt (1.0 - a * a)) / i0b;
            const double px = juce::MathConstants<double>::pi * fc * x;
            const double s = std::abs (px) < 1.0e-12 ? 1.0 : std::sin (px) / px;
            table[(size_t) i] = (float) (fc * s * w);
        }
        for (int i = 0; i + 1 < n; ++i) diff[(size_t) i] = table[(size_t) i + 1] - table[(size_t) i];
        buf.assign ((size_t) taps, 0.0f);                                       // history before the first sample: silence
        base = -(juce::int64) taps;
    }

    /** Adds input samples; appends every output sample that can be finished to 'out'. */
    void process (const float* in, size_t numIn, std::vector<float>& out)
    {
        buf.insert (buf.end(), in, in + numIn);
        totalIn += (juce::int64) numIn;
        produce (out, false);
    }
    /** After the last block: the remaining outputs (the end of the input is treated as silence). */
    void finish (std::vector<float>& out)
    {
        buf.insert (buf.end(), (size_t) taps + 2, 0.0f);
        produce (out, true);
    }
    /** How many samples the whole conversion of n input samples makes. */
    juce::int64 outputLength (juce::int64 n) const { return (juce::int64) std::floor ((double) n / step + 0.5); }

private:
    static constexpr int kRes = 1024;
    static double bessel0 (double x)
    {
        double sum = 1.0, term = 1.0; const double q = x * x * 0.25;
        for (int k = 1; k < 60; ++k) { term *= q / ((double) k * k); sum += term; if (term < sum * 1.0e-17) break; }
        return sum;
    }
    void produce (std::vector<float>& out, bool last)
    {
        const juce::int64 total = last ? outputLength (totalIn) : (juce::int64) 0x7fffffffffffffffLL;
        for (;;)
        {
            if (last && outCount >= total) break;
            const double t = (double) outCount * step;                         // input time of the next output sample
            const juce::int64 fl = (juce::int64) std::floor (t);
            const juce::int64 firstNeeded = fl - taps + 1, lastNeeded = fl + taps;
            if (lastNeeded >= base + (juce::int64) buf.size()) break;          // not enough input yet
            const double f1 = 1.0 - (t - (double) fl);                         // in (0, 1]
            const double fpos = f1 * kRes;
            const int ip = (int) std::floor (fpos); const float fr = (float) (fpos - (double) ip);
            const float* x = buf.data() + (size_t) (firstNeeded - base);
            double s1 = 0.0, s2 = 0.0;
            const float* tb = table.data() + ip; const float* df = diff.data() + ip;
            for (int j = 0; j < 2 * taps; ++j)
            {
                const double v = (double) x[j];
                s1 += v * (double) tb[(size_t) j * kRes];
                s2 += v * (double) df[(size_t) j * kRes];
            }
            out.push_back ((float) (s1 + (double) fr * s2));
            ++outCount;
            // drop the input that is no longer needed (keeps the buffer small)
            const juce::int64 keepFrom = (juce::int64) std::floor ((double) outCount * step) - taps + 1;
            if (keepFrom - base > 65536)
            {
                buf.erase (buf.begin(), buf.begin() + (long) (keepFrom - base));
                base = keepFrom;
            }
        }
    }

    double step;
    int taps = 1;
    std::vector<float> table, diff, buf;
    juce::int64 base = 0, totalIn = 0, outCount = 0;
};
} // namespace td
