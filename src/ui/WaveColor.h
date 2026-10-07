#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_events/juce_events.h>
#include "../core/Dsp.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace td
{
/** WaveColour: what colour the waveform of a file is at every moment. The hue says how high the sound is (red low, green middle, blue high),
    the saturation says how tonal it is (vivid = a note, pale = noise), and a thump below 100 Hz (a kicked mic stand, a door) is BLACK.
    One value per block of 4096 samples, worked out once in the background. */
struct WaveColorData
{
    double blockSeconds = 0.1;
    std::vector<juce::uint32> argb;                 // the colour of every block
    std::vector<float> level;                       // how loud every block is (linear, 0..1)

    /** The colour for the time span t0..t1 (seconds): the colour of the loudest block in it. */
    juce::Colour colourFor (double t0, double t1) const
    {
        if (argb.empty()) return juce::Colours::grey;
        const long n = (long) argb.size();
        long b0 = juce::jlimit (0L, n - 1, (long) std::floor (t0 / blockSeconds));
        long b1 = juce::jlimit (b0 + 1, n, (long) std::ceil (t1 / blockSeconds));
        long best = b0; float bl = -1.0f;
        for (long b = b0; b < b1; ++b) if (level[(size_t) b] > bl) { bl = level[(size_t) b]; best = b; }
        return juce::Colour (argb[(size_t) best]);
    }
};

/** Works out the WaveColorData of files in a background thread (one at a time). get() returns nothing until a file is done; onReady then fires. */
class WaveColorCache
{
public:
    WaveColorCache() : pool (1) { formats.registerBasicFormats(); }
    ~WaveColorCache() { *alive = false; pool.removeAllJobs (true, 20000); }

    std::function<void()> onReady;

    std::shared_ptr<const WaveColorData> get (const juce::File& f)
    {
        const auto key = f.getFullPathName() + "|" + juce::String (f.getSize());
        std::lock_guard<std::mutex> lock (m);
        auto it = done.find (key);
        if (it != done.end()) return it->second;
        if (pending.insert (key).second)
            pool.addJob ([this, f, key, alive = alive]
                         {
                             auto d = analyse (f);
                             if (! *alive) return;
                             { std::lock_guard<std::mutex> l (m); done[key] = d; pending.erase (key); }
                             juce::MessageManager::callAsync ([this, alive] { if (*alive && onReady) onReady(); });
                         });
        return nullptr;
    }

    static juce::Colour colourOf (double centroidHz, double tonality)
    {
        // hue: 100 Hz red, 1 kHz green, 8 kHz and above blue (even steps of pitch between)
        const double l = std::log2 (juce::jmax (100.0, centroidHz) / 100.0);                 // 0 .. ~6.3
        const double hue = l <= 3.32 ? 120.0 * l / 3.32 : 120.0 + 120.0 * juce::jmin (1.0, (l - 3.32) / 3.0);
        const float sat = (float) juce::jlimit (0.0, 1.0, 0.15 + 0.85 * tonality);
        return juce::Colour::fromHSV ((float) (hue / 360.0), sat, 0.95f, 1.0f);
    }

private:
    std::shared_ptr<WaveColorData> analyse (const juce::File& f)
    {
        auto d = std::make_shared<WaveColorData>();
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
        if (r == nullptr || r->lengthInSamples < 1 || r->sampleRate <= 0.0) return d;
        const int N = 4096; const double sr = r->sampleRate;
        d->blockSeconds = (double) N / sr;
        const dsp::FFT fft (N); const auto win = dsp::hann (N);
        std::vector<std::complex<double>> buf ((size_t) N);
        std::vector<double> P ((size_t) N / 2 + 1);
        const double binHz = sr / (double) N;
        const int nCh = (int) juce::jmin ((juce::uint32) 2, r->numChannels);
        juce::AudioBuffer<float> chunk (nCh, N * 32);
        for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += (juce::int64) N * 32)
        {
            if (! *alive) return d;
            const int got = (int) juce::jmin ((juce::int64) N * 32, r->lengthInSamples - pos);
            chunk.clear();
            r->read (&chunk, 0, got, pos, true, nCh > 1);
            for (int b = 0; b * N < got; ++b)
            {
                double sumSq = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    const int s = b * N + i; double v = 0.0;
                    if (s < got) { for (int c = 0; c < nCh; ++c) v += chunk.getSample (c, s); v /= (double) nCh; }
                    sumSq += v * v; buf[(size_t) i] = v * win[(size_t) i];
                }
                const float rms = (float) std::sqrt (sumSq / (double) N);
                fft.forward (buf);
                double total = 0.0, low = 0.0, wsum = 0.0, fsum = 0.0, logSum = 0.0; int nb = 0;
                for (int k = 1; k <= N / 2; ++k)
                {
                    const double p = std::norm (buf[(size_t) k]), f = (double) k * binHz;
                    total += p; if (f < 100.0) low += p;
                    if (f >= 100.0 && f <= juce::jmin (12000.0, 0.45 * sr)) { wsum += p; fsum += p * f; logSum += std::log (p + 1.0e-18); ++nb; }
                }
                juce::Colour c;
                if (rms < 1.0e-4f || total <= 0.0) c = juce::Colour (0xff808080);                              // silence: grey
                else if (low > 0.5 * total) c = juce::Colours::black;                                             // a thump below 100 Hz
                else
                {
                    const double centroid = wsum > 0.0 ? fsum / wsum : 1000.0;
                    const double flat = nb > 0 && wsum > 0.0 ? std::exp (logSum / (double) nb) / (wsum / (double) nb) : 0.56;
                    c = colourOf (centroid, 1.0 - juce::jlimit (0.0, 1.0, flat / 0.56));                         // noise has a flatness of about 0.56, a pure note about 0
                }
                d->argb.push_back (c.getARGB()); d->level.push_back (rms);
            }
        }
        return d;
    }

    juce::AudioFormatManager formats;
    juce::ThreadPool pool;
    std::mutex m;
    std::map<juce::String, std::shared_ptr<const WaveColorData>> done;
    std::set<juce::String> pending;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};
}
