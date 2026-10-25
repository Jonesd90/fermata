#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_events/juce_events.h>
#include "../core/Dsp.h"
#include "WaveStore.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include "../core/AudioFormats.h"

namespace td
{
namespace chan
{
/** Waveform look: false (the default) = the dark clip with the WaveColour (colour by frequency) waveform on it, as always; true = a bright clip with a pale waveform and a thin dark outline (the colours are washed out, so it is off). */
inline bool& solidLook() { static bool b = false; return b; }
}

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
    WaveColorCache() : pool (1) { td::registerAudioFormats (formats); }
    ~WaveColorCache() { *alive = false; pool.removeAllJobs (true, 20000); }

    std::function<void()> onReady;
    std::function<juce::File()> folder;              // the Waveforms folder: worked-out colours are kept there between runs

    std::shared_ptr<const WaveColorData> get (const juce::File& f)
    {
        const auto key = f.getFullPathName() + "|" + juce::String (f.getSize());
        std::lock_guard<std::mutex> lock (m);
        auto it = done.find (key);
        if (it != done.end()) return it->second;
        if (pending.insert (key).second)
            pool.addJob ([this, f, key, alive = alive]
                         {
                             const auto dir = folder ? folder() : juce::File();
                             auto d = dir != juce::File() ? loadSaved (dir, f) : nullptr;
                             if (d == nullptr)
                             {
                                 d = analyse (f);
                                 if (*alive && dir != juce::File()) save (dir, f, *d);
                             }
                             if (! *alive) return;
                             { std::lock_guard<std::mutex> l (m); done[key] = d; pending.erase (key); }
                             juce::MessageManager::callAsync ([this, alive] { if (*alive && onReady) onReady(); });
                         });
        return nullptr;
    }

    /** Forgets what was worked out for this file (the next draw works it out again from the audio). */
    void forget (const juce::File& f)
    {
        const auto key = f.getFullPathName() + "|" + juce::String (f.getSize());
        std::lock_guard<std::mutex> lock (m);
        done.erase (key);
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
    static std::shared_ptr<WaveColorData> loadSaved (const juce::File& dir, const juce::File& audio)
    {
        const auto f = wavestore::fileFor (dir, audio, ".fwcol");
        if (! f.existsAsFile()) return nullptr;
        juce::FileInputStream in (f);
        if (! in.openedOk() || ! wavestore::readHeader (in, wavestore::kMagicColour, audio)) return nullptr;
        auto d = std::make_shared<WaveColorData>();
        d->blockSeconds = in.readDouble();
        const auto n = in.readInt64();
        if (n < 0 || n > 50000000 || d->blockSeconds <= 0.0) return nullptr;
        d->argb.resize ((size_t) n); d->level.resize ((size_t) n);
        if (in.read (d->argb.data(), (int) (n * 4)) != (int) (n * 4) || in.read (d->level.data(), (int) (n * 4)) != (int) (n * 4)) return nullptr;
        return d;
    }
    static void save (const juce::File& dir, const juce::File& audio, const WaveColorData& d)
    {
        if (d.argb.empty() || ! audio.existsAsFile() || ! dir.createDirectory().wasOk()) return;
        const auto f = wavestore::fileFor (dir, audio, ".fwcol");
        juce::TemporaryFile tmp (f);
        {
            juce::FileOutputStream out (tmp.getFile());
            if (! out.openedOk()) return;
            wavestore::writeHeader (out, wavestore::kMagicColour, audio);
            out.writeDouble (d.blockSeconds); out.writeInt64 ((juce::int64) d.argb.size());
            out.write (d.argb.data(), d.argb.size() * 4); out.write (d.level.data(), d.level.size() * 4);
        }
        tmp.overwriteTargetFileWithTemporary();
    }
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
                    const double p = std::norm (buf[(size_t) k]), hz = (double) k * binHz;
                    total += p; if (hz < 100.0) low += p;
                    if (hz >= 100.0 && hz <= juce::jmin (12000.0, 0.45 * sr)) { wsum += p; fsum += p * hz; logSum += std::log (p + 1.0e-18); ++nb; }
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
