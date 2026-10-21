#pragma once
#include "SpectralRepair.h"
#include <cstdint>
#include <functional>

namespace td
{
/** The quick picture of a whole edit (Noise mode): the spectrogram of every track merged (the loudest at each point), kept at several
    time resolutions so any zoom can be drawn at once - the finest level has a hop of 10 - 60 ms (at most about 60 000 frames), each
    coarser level four times fewer frames. Levels are stored as bytes (half a dB per step, -127.5 .. 0 dB), so an hour of 12 tracks is
    under 100 MB; a slice of any level is given back as a MergedSpectrogram (floats, no bass layer) for the view. The build is a
    streaming pass over the audio, track by track, through a 'read' callback: nothing is held in memory but the levels. */
struct SpectralPyramid
{
    static constexpr int N = 2048;                       // the analysis window
    struct Level { double hopSeconds = 0.0; int frames = 0; std::vector<std::uint8_t> db; };     // frames * bins
    int bins = 0; double sampleRate = 48000.0, duration = 0.0;
    std::vector<Level> levels;                           // finest first
    bool valid() const { return ! levels.empty() && levels[0].frames > 0; }
    static std::uint8_t toByte (float dB) { return (std::uint8_t) juce::jlimit (0, 255, (int) std::lround ((dB + 127.5f) * 2.0f)); }
    static float fromByte (std::uint8_t b) { return (float) b * 0.5f - 127.5f; }

    /** Starts the levels for 'seconds' of audio: the finest hop is seconds / 60000 (at least 10 ms, at most 60 ms), then x4, x16 ... until a level
        has fewer than 400 frames. */
    void setUp (double sr, double seconds)
    {
        sampleRate = sr; duration = seconds; bins = N / 2 + 1; levels.clear();
        double hop = juce::jlimit (0.010, 0.060, seconds / 60000.0);
        for (;;)
        {
            Level l; l.hopSeconds = hop; l.frames = juce::jmax (1, (int) std::floor (seconds / hop));
            l.db.assign ((size_t) l.frames * (size_t) bins, 0);
            levels.push_back (std::move (l));
            if (levels.back().frames < 400 || levels.size() >= 6) break;
            hop *= 4.0;
        }
    }
    /** The finest level's frame that holds time t (seconds), and the time of a frame's centre. */
    int frameAt (double t) const { return levels.empty() ? 0 : juce::jlimit (0, levels[0].frames - 1, (int) std::floor (t / levels[0].hopSeconds)); }
    double frameTime (int f) const { return levels.empty() ? 0.0 : ((double) f + 0.5) * levels[0].hopSeconds; }

    /** Adds one track's audio to the finest level over the frames [f0, f1): 'read (start, n, dst)' fills n samples of that track from sample 'start'
        (zeros past the end). Every frame takes the LOUDER of what it had and this track, bin by bin; with 'replace' the frames are cleared first
        (a part of the edit made again: call with replace for the first track, then without for the others). Returns false if 'cancelled' said stop. */
    bool addTrack (int f0, int f1, const std::function<void (juce::int64 start, int n, float* dst)>& read, bool replace, const std::function<bool()>& cancelled = {})
    {
        if (levels.empty()) return true;
        auto& L = levels[0];
        f0 = juce::jlimit (0, L.frames, f0); f1 = juce::jlimit (f0, L.frames, f1);
        if (replace) std::fill (L.db.begin() + (size_t) f0 * (size_t) bins, L.db.begin() + (size_t) f1 * (size_t) bins, (std::uint8_t) 0);
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<std::complex<double>> buf ((size_t) N);
        // the frames are read in runs: a run's samples once, each frame's window cut from it
        const int run = 256;
        std::vector<float> samples;
        const double hopS = L.hopSeconds * sampleRate;
        for (int r0 = f0; r0 < f1; r0 += run)
        {
            if (cancelled && cancelled()) return false;
            const int r1 = juce::jmin (f1, r0 + run);
            const juce::int64 s0 = (juce::int64) std::llround ((double) r0 * hopS + 0.5 * hopS) - N / 2;
            const juce::int64 s1 = (juce::int64) std::llround ((double) (r1 - 1) * hopS + 0.5 * hopS) + N / 2 + 1;
            samples.assign ((size_t) (s1 - s0), 0.0f);
            {   // (the read may start before 0: those samples stay zero)
                const juce::int64 a = juce::jmax<juce::int64> (0, s0);
                if (s1 > a) read (a, (int) (s1 - a), samples.data() + (a - s0));
            }
            for (int f = r0; f < r1; ++f)
            {
                const juce::int64 c = (juce::int64) std::llround ((double) f * hopS + 0.5 * hopS) - N / 2 - s0;
                for (int i = 0; i < N; ++i) { const auto q = c + i; buf[(size_t) i] = q >= 0 && q < (juce::int64) samples.size() ? (double) samples[(size_t) q] * win[(size_t) i] : 0.0; }
                fft.forward (buf);
                std::uint8_t* row = L.db.data() + (size_t) f * (size_t) bins;
                for (int b = 0; b < bins; ++b)
                {
                    const float d = (float) (20.0 * std::log10 (std::abs (buf[(size_t) b]) * 4.0 / (double) N + 1.0e-9));   // ~ dBFS of a sine, as SpectralRepair::spectrogram
                    const auto v = toByte (d);
                    if (v > row[b]) row[b] = v;
                }
            }
        }
        return true;
    }
    /** Makes the coarser levels again from the finest, over the finest frames [f0, f1) (or everything). */
    void settle (int f0 = 0, int f1 = -1)
    {
        if (levels.size() < 2) return;
        if (f1 < 0) f1 = levels[0].frames;
        for (size_t k = 1; k < levels.size(); ++k)
        {
            auto& fine = levels[k - 1]; auto& coarse = levels[k];
            const int g0 = juce::jlimit (0, coarse.frames - 1, f0 / 4), g1 = juce::jlimit (g0, coarse.frames - 1, (f1 + 3) / 4);
            for (int g = g0; g <= g1; ++g)
            {
                std::uint8_t* row = coarse.db.data() + (size_t) g * (size_t) bins;
                std::fill (row, row + bins, (std::uint8_t) 0);
                for (int f = g * 4; f < juce::jmin (fine.frames, g * 4 + 4); ++f)
                {
                    const std::uint8_t* src = fine.db.data() + (size_t) f * (size_t) bins;
                    for (int b = 0; b < bins; ++b) if (src[b] > row[b]) row[b] = src[b];
                }
            }
            f0 = g0; f1 = g1 + 1;
        }
    }
    /** The level whose frames best fit 'maxFrames' over [t0, t1]: the finest that has no more than that many there. */
    int levelFor (double t0, double t1, int maxFrames) const
    {
        for (size_t k = 0; k < levels.size(); ++k) if ((t1 - t0) / levels[k].hopSeconds <= (double) maxFrames) return (int) k;
        return (int) levels.size() - 1;
    }
    /** The frames of the best level over [t0, t1] as a MergedSpectrogram (floats, no bass layer), and the exact time its first and last frame centres stand
        for (the view maps them onto the picture). */
    MergedSpectrogram slice (double t0, double t1, int maxFrames, double& dataT0, double& dataT1) const
    {
        MergedSpectrogram s; s.sampleRate = sampleRate; s.fftSize = N; s.bins = bins;
        dataT0 = t0; dataT1 = t1;
        if (! valid()) return s;
        const auto& L = levels[(size_t) levelFor (t0, t1, maxFrames)];
        const int a = juce::jlimit (0, L.frames - 1, (int) std::floor (t0 / L.hopSeconds)), b = juce::jlimit (a, L.frames - 1, (int) std::ceil (t1 / L.hopSeconds));
        s.frames = b - a + 1; s.hopSamples = L.hopSeconds * sampleRate;
        s.db.resize ((size_t) s.frames * (size_t) bins);
        for (int f = a; f <= b; ++f)
        {
            const std::uint8_t* src = L.db.data() + (size_t) f * (size_t) bins; float* dst = s.db.data() + (size_t) (f - a) * (size_t) bins;
            for (int k = 0; k < bins; ++k) dst[k] = fromByte (src[k]);
        }
        dataT0 = ((double) a + 0.5) * L.hopSeconds; dataT1 = ((double) b + 0.5) * L.hopSeconds;
        return s;
    }
    size_t bytes() const { size_t n = 0; for (auto& l : levels) n += l.db.size(); return n; }
};
} // namespace td
