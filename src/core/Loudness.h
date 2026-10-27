#pragma once
#include "Common.h"
#include <vector>

namespace td
{
/** Programme loudness as the broadcasters measure it (ITU-R BS.1770-4 / EBU R 128): the signal through the K-weighting (a high shelf that lifts
    everything above about 1.5 kHz by 4 dB, then a high-pass at 38 Hz, as the ear weighs the spectrum), the mean square taken in 400 ms blocks
    every 100 ms, every block below -70 LUFS left out (the absolute gate), then every block more than 10 LU below the average of the rest
    (the relative gate), and the loudness is the average of what is left: so silence and pauses do not pull the figure down. Channels are
    summed with the standard's weights (front channels 1, surrounds 1.41, no LFE). True peak is the highest sample after 4 x oversampling
    (BS.1770 Annex 2): the peak a converter will make between the samples. Feed it the whole programme block by block, then ask. */
class LoudnessMeter
{
public:
    static constexpr double kAbsoluteGateLufs = -70.0, kRelativeGateLu = -10.0;

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels = juce::jlimit (1, 16, numChannels);
        design();
        hopSamples = juce::jmax (1, juce::roundToInt (sr * 0.1));
        reset();
    }
    void reset()
    {
        for (auto& k : kf) k.reset();
        hopSum.assign ((size_t) channels, 0.0); hopFill = 0;
        hops.clear(); blocks.clear();
        peak = 0.0;
        for (auto& h : history) h.assign (kTaps, 0.0);
    }

    /** The next samples of the programme (any block size). */
    void process (const float* const* chans, int numChannels, int n)
    {
        const int nc = juce::jmin (numChannels, channels);
        if (nc <= 0) return;
        for (int c = 0; c < nc; ++c) truePeakOf (chans[c], n, history[(size_t) c]);
        for (int i = 0; i < n; ++i)                                        // sample by sample across the channels, so a hop closes with every channel at the same place
        {
            for (int c = 0; c < nc; ++c) { const double y = kf[(size_t) c].run ((double) chans[c][i]); hopSum[(size_t) c] += y * y; }
            if (++hopFill == hopSamples) closeHop();
        }
    }
    /** The same for a buffer. */
    void process (const juce::AudioBuffer<float>& b) { process (b.getArrayOfReadPointers(), b.getNumChannels(), b.getNumSamples()); }

    /** The integrated (gated) loudness in LUFS; -inf (-100) for silence or less than 400 ms of sound. */
    double integratedLufs() const
    {
        std::vector<double> all;
        for (double p : blocks) if (toLufs (p) > kAbsoluteGateLufs) all.push_back (p);
        if (all.empty()) return -100.0;
        double mean = 0.0; for (double p : all) mean += p; mean /= (double) all.size();
        const double gate = toLufs (mean) + kRelativeGateLu;
        double sum = 0.0; int n = 0;
        for (double p : all) if (toLufs (p) > gate) { sum += p; ++n; }
        return n > 0 ? toLufs (sum / (double) n) : -100.0;
    }
    /** The true peak (4 x oversampled) in dBTP; -100 for silence. */
    double truePeakDb() const { return peak > 1.0e-10 ? 20.0 * std::log10 (peak) : -100.0; }
    /** How many 400 ms blocks have been measured. */
    int blockCount() const { return (int) blocks.size(); }

    /** The gain (dB) that brings a programme of this loudness to the target. */
    static double gainToTarget (double lufs, double targetLufs) { return targetLufs - lufs; }

private:
    static double toLufs (double power) { return -0.691 + 10.0 * std::log10 (juce::jmax (1.0e-20, power)); }
    static double weightOf (int channel, int numChannels)              // BS.1770: L R C 1.0, LFE 0, Ls Rs 1.41 (5.1 order L R C LFE Ls Rs)
    {
        if (numChannels < 4) return 1.0;
        if (channel == 3) return 0.0;
        return channel >= 4 ? 1.41 : 1.0;
    }
    void closeHop()
    {
        std::vector<double> h (hopSum);
        hopSum.assign ((size_t) channels, 0.0); hopFill = 0;
        hops.push_back (h);
        if (hops.size() >= 4)                                              // a 400 ms block = the last four hops
        {
            double p = 0.0;
            for (int c = 0; c < channels; ++c)
            {
                double s = 0.0; for (size_t k = hops.size() - 4; k < hops.size(); ++k) s += hops[k][(size_t) c];
                p += weightOf (c, channels) * s / (4.0 * (double) hopSamples);
            }
            blocks.push_back (p);
            if (hops.size() > 4) hops.erase (hops.begin());
        }
    }

    // ---- the K-weighting: a high shelf, then a high-pass (the standard's 48 kHz figures come out of these for any sample rate)
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run (double x) noexcept { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void reset() noexcept { z1 = z2 = 0.0; }
    };
    struct KFilter
    {
        Biquad shelf, hp;
        double run (double x) noexcept { return hp.run (shelf.run (x)); }
        void reset() noexcept { shelf.reset(); hp.reset(); }
    };
    void design()
    {
        KFilter k;
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            k.shelf.b0 = (Vh + Vb * K / Q + K * K) / a0; k.shelf.b1 = 2.0 * (K * K - Vh) / a0; k.shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
            k.shelf.a1 = 2.0 * (K * K - 1.0) / a0; k.shelf.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            k.hp.b0 = 1.0; k.hp.b1 = -2.0; k.hp.b2 = 1.0;                   // (as the standard writes them: the gain above the corner is a0, within 0.03 dB of 1)
            k.hp.a1 = 2.0 * (K * K - 1.0) / a0; k.hp.a2 = (1.0 - K / Q + K * K) / a0;
        }
        kf.assign ((size_t) channels, k);
        // the 4 x oversampler for the true peak: a windowed sinc, 12 taps a phase, each phase at unity gain
        for (int p = 0; p < 4; ++p)
        {
            double sum = 0.0;
            for (int t = 0; t < kTaps; ++t)
            {
                const double m = (double) (4 * t + p) - (double) (4 * kTaps - 1) * 0.5;      // the tap's place on the 4 x grid, centred
                const double x = m / 4.0;
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
                const double w = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * m / ((double) (4 * kTaps) * 0.5));   // Hann over the whole span
                phase[p][t] = sinc * w; sum += phase[p][t];
            }
            for (int t = 0; t < kTaps; ++t) phase[p][t] /= sum;
        }
        history.assign ((size_t) channels, std::vector<double> (kTaps, 0.0));
    }
    void truePeakOf (const float* x, int n, std::vector<double>& hist)
    {
        for (int i = 0; i < n; ++i)
        {
            for (int t = kTaps - 1; t > 0; --t) hist[(size_t) t] = hist[(size_t) t - 1];
            hist[0] = (double) x[i];
            for (int p = 0; p < 4; ++p)
            {
                double y = 0.0;
                for (int t = 0; t < kTaps; ++t) y += phase[p][t] * hist[(size_t) t];
                const double a = std::abs (y);
                if (a > peak) peak = a;
            }
        }
    }

    static constexpr int kTaps = 12;
    double sr = 48000.0; int channels = 2, hopSamples = 4800, hopFill = 0;
    std::vector<KFilter> kf;
    std::vector<double> hopSum;
    std::vector<std::vector<double>> hops;                               // the last four hops' sums of squares, per channel
    std::vector<double> blocks;                                          // every 400 ms block's weighted mean square
    double phase[4][kTaps] = {};
    std::vector<std::vector<double>> history;
    double peak = 0.0;
};
} // namespace td
