#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <vector>

namespace td
{
/** Word-length reduction. Everything Fermata computes is 32-bit float; whenever that is cut down to a fixed word length
    (a 24 or 16 bit file, or a converter fed at a chosen word length) TPDF dither of +-1 LSB is added first and the sum is
    rounded to the nearest step, so the quantisation error is noise that does not depend on the music (no distortion). */
namespace dither
{
    /** Fast white noise, no allocation: safe on the audio thread. */
    inline juce::uint32 next (juce::uint32& s) noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

    /** Triangular noise in (-1, 1) LSB: the difference of two uniform values. */
    inline float tpdf (juce::uint32& s) noexcept
    {
        const float a = (float) (next (s) >> 8) * (1.0f / 16777216.0f);
        const float b = (float) (next (s) >> 8) * (1.0f / 16777216.0f);
        return a - b;
    }

    /** x (full scale = 1.0) to the nearest whole step of a signed word of `bits` bits, returned as an integer step count. */
    inline int quantise (float x, int bits, bool withDither, juce::uint32& s) noexcept
    {
        const float scale = (float) (1 << (bits - 1));
        float v = x * scale;
        if (withDither) v += tpdf (s);
        v = std::nearbyint (v);
        return (int) juce::jlimit (-scale, scale - 1.0f, v);
    }

    /** In place: the samples become exact multiples of one step of a `bits`-bit word (what a converter of that width keeps).
        Anything at or beyond full scale is left alone, exactly as it was: the driver treats overs as it always did. */
    inline void reduceInPlace (float* d, int n, int bits, juce::uint32& s) noexcept
    {
        const float scale = (float) (1 << (bits - 1)), inv = 1.0f / scale, top = (scale - 1.0f) * inv;
        for (int i = 0; i < n; ++i)
        {
            const float x = d[i];
            if (x >= top || x <= -1.0f) continue;
            const float v = std::nearbyint (x * scale + tpdf (s));
            d[i] = juce::jlimit (-scale, scale - 1.0f, v) * inv;
        }
    }

    /** One generator per channel, so the noise in the channels is not the same noise. */
    struct Bank
    {
        juce::uint32 state[256];
        Bank (juce::uint32 seed = 0x2545F491u) { for (int i = 0; i < 256; ++i) { seed = seed * 1664525u + 1013904223u; state[i] = seed | 1u; } }
    };
}

/** Writes float audio to a juce::AudioFormatWriter. Integer files of 24 bits or less are written with TPDF dither (whatever the
    writer's own float-to-integer step would do is avoided: the integer words are made here). Floating point files and 32-bit integer
    files are written as they are, because no word length is being lost. Pass `useDither = false` for a bit-exact copy. */
class DitherOut
{
public:
    explicit DitherOut (juce::AudioFormatWriter& w, bool useDither = true, juce::uint32 seed = 0x2545F491u)
        : writer (w), enabled (useDither), bank (seed) {}

    /** True when this class makes the integer words (integer file, 24 bits or fewer). */
    bool quantises() const noexcept
    {
        return ! writer.isFloatingPoint() && writer.getBitsPerSample() > 0 && writer.getBitsPerSample() <= 24;
    }

    bool write (const float* const* chans, int numChans, int numSamples)
    {
        if (numSamples <= 0) return true;
        if (! quantises()) return writer.writeFromFloatArrays (chans, numChans, numSamples);
        const int bits = writer.getBitsPerSample(), shift = 32 - bits;
        const int nc = juce::jmax (1, (int) writer.getNumChannels());
        if ((int) ints.size() < nc) ints.resize ((size_t) nc);
        std::vector<const int*> ptrs ((size_t) nc + 1, nullptr);
        for (int c = 0; c < nc; ++c)
        {
            auto& v = ints[(size_t) c];
            v.resize ((size_t) numSamples);
            const float* src = chans[juce::jmin (c, numChans - 1)];
            auto& st = bank.state[c & 255];
            for (int i = 0; i < numSamples; ++i)
                v[(size_t) i] = (int) ((juce::uint32) dither::quantise (src[i], bits, enabled, st) << shift);
            ptrs[(size_t) c] = v.data();
        }
        return writer.write (ptrs.data(), numSamples);
    }

    bool write (const juce::AudioBuffer<float>& b, int start, int numSamples)
    {
        return write (b.getArrayOfReadPointers(), b.getNumChannels(), numSamples, start);
    }

private:
    bool write (const float* const* chans, int numChans, int numSamples, int start)
    {
        std::vector<const float*> p ((size_t) numChans);
        for (int c = 0; c < numChans; ++c) p[(size_t) c] = chans[c] + start;
        return write (p.data(), numChans, numSamples);
    }
    juce::AudioFormatWriter& writer;
    bool enabled;
    dither::Bank bank;
    std::vector<std::vector<int>> ints;
};
}
