#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>
#include <vector>
#include "../core/AudioFormats.h"
#include "../core/CrashReport.h"

namespace td
{
/** The outline of an audio file for drawing: the lowest and highest sample of every 256 samples, per channel, as 16-bit values (so a reverb tail at
    -70 dB still has a shape: JUCE's AudioThumbnail kept 8 bits, and everything under about -42 dB came out as flat steps), plus a coarser level of
    one point per 4096 samples for the zoomed-out views. It is read in the background (a pool of threads that each take or edit window owns), and the part read
    so far can be drawn at any time; a change message is sent as it comes in and when it is done. Saved to and loaded from the Waveforms folder (see WaveStore.h).
    A file that could not be read is drawn as far as it was read but never saved (a flat outline would otherwise stay in the Waveforms folder for good). */
class WaveThumb : public juce::ChangeBroadcaster
{
public:
    static constexpr int kSamplesPerPoint = 256, kCoarse = 16;         // the fine level's span, and the coarse level's span in fine points

    /** The background readers (each take or edit window has its own). */
    class Pool
    {
    public:
        Pool() : pool (2) {}
        ~Pool() { pool.removeAllJobs (true, 20000); }
        juce::ThreadPool pool;
    };

    WaveThumb (juce::AudioFormatManager& fm, Pool& p) : formats (fm), pool (p) {}
    ~WaveThumb() override { if (d != nullptr) d->alive = false; }

    /** Starts reading the file: the header now (the length and the rate are known from here on), the samples in the background. */
    void load (const juce::File& f)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
        if (r == nullptr || r->numChannels < 1 || r->sampleRate <= 0.0) return;
        auto data = std::make_shared<Data>();
        data->rate = r->sampleRate; data->samples = r->lengthInSamples; data->channels = (int) r->numChannels;
        data->points = (data->samples + kSamplesPerPoint - 1) / kSamplesPerPoint;
        data->coarsePoints = (data->points + kCoarse - 1) / kCoarse;
        data->ch.resize ((size_t) data->channels);
        for (auto& c : data->ch) { c.mn.assign ((size_t) data->points, 0); c.mx.assign ((size_t) data->points, 0); c.cmn.assign ((size_t) data->coarsePoints, 0); c.cmx.assign ((size_t) data->coarsePoints, 0); }
        r.reset();
        d = data;
        juce::WeakReference<WaveThumb> weak (this);
        pool.pool.addJob ([weak, data, f] { td::crashreport::nameThisThreadW (L"Waveform reader"); readFile (weak, data, f); });
    }

    double getTotalLength() const noexcept { return d != nullptr && d->rate > 0.0 ? (double) d->samples / d->rate : 0.0; }
    double getSampleRate() const noexcept { return d != nullptr ? d->rate : 0.0; }
    int getNumChannels() const noexcept { return d != nullptr ? d->channels : 0; }
    bool isFullyLoaded() const noexcept { return d != nullptr && d->done.load() >= d->points; }
    bool isEmpty() const noexcept { return d == nullptr; }

    /** The lowest and highest sample (-1 .. 1) between two times, from what has been read so far (0 beyond it). */
    void getApproximateMinMax (double t0, double t1, int channel, float& minValue, float& maxValue) const noexcept
    {
        minValue = maxValue = 0.0f;
        if (d == nullptr || channel < 0 || channel >= d->channels || d->rate <= 0.0) return;
        const juce::int64 done = d->done.load();
        juce::int64 p0 = (juce::int64) (t0 * d->rate / kSamplesPerPoint), p1 = (juce::int64) ((t1 * d->rate + kSamplesPerPoint - 1) / kSamplesPerPoint);
        p0 = juce::jlimit ((juce::int64) 0, done, p0); p1 = juce::jlimit (p0, done, juce::jmax (p0 + 1, p1));
        if (p1 <= p0) return;
        const auto& c = d->ch[(size_t) channel];
        int mn = 32767, mx = -32768;
        juce::int64 p = p0;
        if (p1 - p0 >= 2 * kCoarse)                                          // the coarse level for the middle, the fine level for the ends
        {
            const juce::int64 c0 = (p0 + kCoarse - 1) / kCoarse, c1 = p1 / kCoarse;
            for (; p < c0 * kCoarse; ++p) { mn = juce::jmin (mn, (int) c.mn[(size_t) p]); mx = juce::jmax (mx, (int) c.mx[(size_t) p]); }
            for (juce::int64 k = c0; k < c1; ++k) { mn = juce::jmin (mn, (int) c.cmn[(size_t) k]); mx = juce::jmax (mx, (int) c.cmx[(size_t) k]); }
            p = juce::jmax (p, c1 * kCoarse);
        }
        for (; p < p1; ++p) { mn = juce::jmin (mn, (int) c.mn[(size_t) p]); mx = juce::jmax (mx, (int) c.mx[(size_t) p]); }
        if (mn > mx) return;
        minValue = (float) mn / 32767.0f; maxValue = (float) mx / 32767.0f;
    }

    // ---- the Waveforms folder
    static constexpr juce::int32 kFormat = 1;
    bool saveTo (juce::OutputStream& o) const
    {
        if (! isFullyLoaded() || d->failed.load()) return false;
        o.writeInt (kFormat); o.writeInt (kSamplesPerPoint); o.writeInt (d->channels); o.writeDouble (d->rate); o.writeInt64 (d->samples); o.writeInt64 (d->points);
        for (auto& c : d->ch) { o.write (c.mn.data(), c.mn.size() * 2); o.write (c.mx.data(), c.mx.size() * 2); }
        return true;
    }
    bool loadFrom (juce::InputStream& in)
    {
        if (in.readInt() != kFormat || in.readInt() != kSamplesPerPoint) return false;
        auto data = std::make_shared<Data>();
        data->channels = in.readInt(); data->rate = in.readDouble(); data->samples = in.readInt64(); data->points = in.readInt64();
        if (data->channels < 1 || data->channels > 64 || data->rate <= 0.0 || data->points < 0 || data->points > (juce::int64) 1 << 32) return false;
        data->coarsePoints = (data->points + kCoarse - 1) / kCoarse;
        data->ch.resize ((size_t) data->channels);
        for (auto& c : data->ch)
        {
            c.mn.resize ((size_t) data->points); c.mx.resize ((size_t) data->points);
            const int bytes = (int) (data->points * 2);
            if (in.read (c.mn.data(), bytes) != bytes || in.read (c.mx.data(), bytes) != bytes) return false;
            c.cmn.assign ((size_t) data->coarsePoints, 0); c.cmx.assign ((size_t) data->coarsePoints, 0);
            coarsen (c, 0, data->points);
        }
        data->done = data->points;
        d = data;
        return true;
    }

private:
    struct Channel { std::vector<juce::int16> mn, mx, cmn, cmx; };
    struct Data
    {
        double rate = 0.0; juce::int64 samples = 0, points = 0, coarsePoints = 0; int channels = 0;
        std::vector<Channel> ch;
        std::atomic<juce::int64> done { 0 };               // fine points filled so far (the reader fills them in order)
        std::atomic<bool> alive { true };                  // false once the thumbnail is gone: the reader stops
        std::atomic<bool> failed { false };                // the file could not be (wholly) read: drawn as far as it went, never saved
    };

    static void coarsen (Channel& c, juce::int64 from, juce::int64 to)
    {
        for (juce::int64 k = from / kCoarse; k * kCoarse < to && (size_t) k < c.cmn.size(); ++k)
        {
            int mn = 32767, mx = -32768;
            for (juce::int64 p = k * kCoarse; p < (k + 1) * kCoarse && (size_t) p < c.mn.size(); ++p) { mn = juce::jmin (mn, (int) c.mn[(size_t) p]); mx = juce::jmax (mx, (int) c.mx[(size_t) p]); }
            if (mn <= mx) { c.cmn[(size_t) k] = (juce::int16) mn; c.cmx[(size_t) k] = (juce::int16) mx; }
        }
    }

    /** Tells the thumbnail (on the message thread, if it is still there) that more of it can be drawn. */
    static void tell (const juce::WeakReference<WaveThumb>& weak)
    {
        juce::MessageManager::callAsync ([weak] { if (auto* t = weak.get()) t->sendChangeMessage(); });
    }
    static void readFile (juce::WeakReference<WaveThumb> weak, std::shared_ptr<Data> data, juce::File f)
    {
        juce::AudioFormatManager fm; td::registerAudioFormats (fm);
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) { data->failed = true; data->done = data->points; tell (weak); return; }
        const int block = kSamplesPerPoint * 512;                                  // 131072 samples a time
        juce::AudioBuffer<float> buf (data->channels, block);
        juce::uint32 lastTold = juce::Time::getMillisecondCounter();
        juce::int64 pos = 0, point = 0;
        while (pos < data->samples && data->alive)
        {
            const int n = (int) juce::jmin ((juce::int64) block, data->samples - pos);
            buf.clear();
            if (! r->read (&buf, 0, n, pos, true, true)) { data->failed = true; break; }
            for (int c = 0; c < data->channels; ++c)
            {
                const float* s = buf.getReadPointer (c);
                auto& ch = data->ch[(size_t) c];
                for (int i = 0, p = 0; i < n; i += kSamplesPerPoint, ++p)
                {
                    float mn = 1.0e9f, mx = -1.0e9f;
                    const int e = juce::jmin (n, i + kSamplesPerPoint);
                    for (int k = i; k < e; ++k) { mn = juce::jmin (mn, s[k]); mx = juce::jmax (mx, s[k]); }
                    const auto q = [] (float v) { return (juce::int16) juce::jlimit (-32767, 32767, juce::roundToInt (v * 32767.0f)); };
                    if ((size_t) (point + p) < ch.mn.size()) { ch.mn[(size_t) (point + p)] = q (mn); ch.mx[(size_t) (point + p)] = q (mx); }
                }
            }
            const juce::int64 newPoint = point + (n + kSamplesPerPoint - 1) / kSamplesPerPoint;
            for (auto& ch : data->ch) coarsen (ch, point, newPoint);
            point = newPoint; pos += n;
            data->done = juce::jmin (point, data->points);
            if (juce::Time::getMillisecondCounter() - lastTold > 200) { lastTold = juce::Time::getMillisecondCounter(); tell (weak); }
        }
        if (! data->alive) return;
        data->done = data->points;
        tell (weak);
    }

    juce::AudioFormatManager& formats;
    Pool& pool;
    std::shared_ptr<Data> d;
    JUCE_DECLARE_WEAK_REFERENCEABLE (WaveThumb)
};
} // namespace td
