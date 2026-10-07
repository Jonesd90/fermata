#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <map>
#include <vector>

namespace td
{
/** Fills one channel's waveform as a single connected shape: the top edge left to right, the bottom edge back again. Because it is one anti-aliased
    outline rather than a thin line per pixel column, it never shows gaps or stripes, whatever the track height or the screen's scaling. */
inline void fillEnvelope (juce::Graphics& g, float xFirst, const std::vector<float>& tops, const std::vector<float>& bottoms)
{
    const size_t n = tops.size();
    if (n == 0 || bottoms.size() != n) return;
    juce::Path p;
    p.startNewSubPath (xFirst, tops[0]);
    for (size_t i = 0; i < n; ++i) p.lineTo (xFirst + (float) i + 0.5f, tops[i]);
    p.lineTo (xFirst + (float) n, tops[n - 1]);
    p.lineTo (xFirst + (float) n, juce::jmax (tops[n - 1] + 1.0f, bottoms[n - 1]));
    for (size_t i = n; i-- > 0;) p.lineTo (xFirst + (float) i + 0.5f, juce::jmax (tops[i] + 1.0f, bottoms[i]));
    p.lineTo (xFirst, juce::jmax (tops[0] + 1.0f, bottoms[0]));
    p.closeSubPath();
    g.fillPath (p);
}

/** Instead of a waveform: a hatched block that says the audio is out being processed in other software ("Export for Processing"). */
inline void drawWaitingBlock (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& name, juce::Colour text)
{
    if (area.isEmpty()) return;
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (area);
    g.setColour (text.withAlpha (0.10f)); g.fillRect (area);
    g.setColour (text.withAlpha (0.28f));
    for (int x = area.getX() - area.getHeight(); x < area.getRight(); x += 10)
        g.drawLine ((float) x, (float) area.getBottom(), (float) (x + area.getHeight()), (float) area.getY(), 1.0f);
    g.setColour (text); g.setFont (juce::FontOptions (juce::jmin (13.0f, (float) area.getHeight() * 0.6f), juce::Font::bold));
    g.drawText ("Waiting for corrected audio" + (name.isNotEmpty() ? juce::String (" - ") + name : juce::String()), area.reduced (6, 0), juce::Justification::centred, true);
}

/** The waveform of a (zoomed-out) thumbnail between t0 and t1 seconds, drawn with fillEnvelope for every channel. */
inline void drawThumbnailEnvelope (juce::Graphics& g, juce::Rectangle<int> area, juce::AudioThumbnail& th, double t0, double t1, float vzoom)
{
    if (area.isEmpty() || t1 <= t0) return;
    const int nCh = juce::jmax (1, th.getNumChannels());
    const auto clip = g.getClipBounds().getIntersection (area);
    if (clip.isEmpty()) return;
    const int bandH = juce::jmax (1, area.getHeight() / nCh);
    const double perPx = (t1 - t0) / (double) area.getWidth();
    std::vector<float> tops ((size_t) clip.getWidth()), bots ((size_t) clip.getWidth());
    for (int c = 0; c < nCh; ++c)
    {
        const float centre = (float) area.getY() + (float) bandH * ((float) c + 0.5f), half = (float) bandH * 0.5f * vzoom;
        const float lo = (float) area.getY() + (float) bandH * (float) c, hi = lo + (float) bandH;
        for (int i = 0; i < clip.getWidth(); ++i)
        {
            const double a = t0 + (double) (clip.getX() + i - area.getX()) * perPx;
            float mn = 0.0f, mx = 0.0f;
            th.getApproximateMinMax (a, a + perPx, c, mn, mx);
            tops[(size_t) i] = juce::jlimit (lo, hi, centre - mx * half);
            bots[(size_t) i] = juce::jlimit (lo, hi, centre - mn * half);
        }
        fillEnvelope (g, (float) clip.getX(), tops, bots);
    }
}

/** Sharp waveforms when zoomed in. The ordinary thumbnails (one value per 128 samples) are fine when zoomed out; once fewer than 128 samples
    share a pixel this reads the audio itself, so every pixel shows the true peaks, and when there is more than a pixel per sample it draws
    the actual sample curve. Pieces read from disk are kept for a while, so scrolling and the moving playhead stay quick. */
class DirectWaveCache
{
public:
    static constexpr double kThumbSamplesPerPixelLimit = 128.0;     // thumbnails hold one value per 128 samples
    explicit DirectWaveCache (juce::AudioFormatManager& fm) : formats (fm) {}

    /** True if it drew the audio itself (the caller then does not draw the thumbnail). */
    bool draw (juce::Graphics& g, juce::Rectangle<int> area, const juce::File& file, double t0, double t1, float vzoom)
    {
        if (area.getWidth() < 1 || area.getHeight() < 1 || t1 <= t0) return false;
        auto clip = g.getClipBounds().getIntersection (area);
        if (clip.isEmpty()) return true;

        Entry* e = find (file);
        if (e == nullptr) return false;
        const double rate = e->rate;
        const double spp = (t1 - t0) * rate / (double) area.getWidth();          // samples per pixel
        if (spp >= kThumbSamplesPerPixelLimit || rate <= 0.0) return false;

        const double firstPos = t0 * rate + (double) (clip.getX() - area.getX()) * spp;
        const double lastPos  = t0 * rate + (double) (clip.getRight() - area.getX()) * spp;
        const juce::int64 need0 = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (firstPos) - 2);
        const juce::int64 need1 = juce::jmin (e->length, (juce::int64) std::ceil (lastPos) + 3);
        if (need1 <= need0) return true;
        if (need1 - need0 > kMaxSamples) return false;
        if (! ensure (*e, file, need0, need1)) return false;

        const int nCh = e->data.getNumChannels();
        const int bandH = juce::jmax (1, area.getHeight() / juce::jmax (1, nCh));
        for (int c = 0; c < nCh; ++c)
        {
            const float* d = e->data.getReadPointer (c);
            const float centre = (float) area.getY() + (float) bandH * ((float) c + 0.5f);
            const float half = (float) bandH * 0.5f * vzoom;
            const float lo = (float) area.getY() + (float) bandH * (float) c, hi = lo + (float) bandH;
            auto yOf = [&] (float v) { return juce::jlimit (lo, hi, centre - v * half); };
            auto at = [&] (juce::int64 s) -> float
            {
                const auto i = s - e->start;
                return juce::isPositiveAndBelow (i, (juce::int64) e->data.getNumSamples()) ? d[i] : 0.0f;
            };

            if (spp >= 1.0)                                                  // peaks: a min / max bar for every pixel column
            {
                std::vector<float> tops, bots;
                for (int x = clip.getX(); x < clip.getRight(); ++x)
                {
                    const juce::int64 s0 = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (t0 * rate + (double) (x - area.getX()) * spp));
                    const juce::int64 s1 = juce::jmin (e->length, juce::jmax (s0 + 1, (juce::int64) std::ceil (t0 * rate + (double) (x + 1 - area.getX()) * spp)));
                    float mn = 1.0f, mx = -1.0f;
                    for (juce::int64 s = s0; s < s1; ++s) { const float v = at (s); mn = juce::jmin (mn, v); mx = juce::jmax (mx, v); }
                    if (mn > mx) { mn = mx = 0.0f; }
                    tops.push_back (yOf (mx)); bots.push_back (yOf (mn));
                }
                fillEnvelope (g, (float) clip.getX(), tops, bots);
            }
            else                                                              // more than a pixel per sample: the sample curve itself
            {
                const double pxPerSample = 1.0 / spp;
                const juce::int64 a = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (firstPos) - 1);
                const juce::int64 b = juce::jmin (e->length - 1, (juce::int64) std::ceil (lastPos) + 1);
                juce::Path p; bool started = false;
                for (juce::int64 s = a; s <= b; ++s)
                {
                    const float x = (float) area.getX() + (float) (((double) s - t0 * rate) * pxPerSample);
                    const float y = yOf (at (s));
                    if (! started) { p.startNewSubPath (x, y); started = true; } else p.lineTo (x, y);
                }
                g.strokePath (p, juce::PathStrokeType (1.2f));
                if (pxPerSample >= 6.0)                                       // zoomed right in: a dot on every sample
                    for (juce::int64 s = a; s <= b; ++s)
                        g.fillEllipse ((float) area.getX() + (float) (((double) s - t0 * rate) * pxPerSample) - 2.0f, yOf (at (s)) - 2.0f, 4.0f, 4.0f);
            }
        }
        return true;
    }

private:
    static constexpr juce::int64 kMaxSamples = 400000;          // never read more than this for one file in one go
    struct Entry
    {
        double rate = 0.0; juce::int64 length = 0;
        juce::int64 start = 0, count = 0;                         // the part of the file held in 'data'
        juce::AudioBuffer<float> data;
        juce::int64 used = 0;
    };

    Entry* find (const juce::File& f)
    {
        const auto key = f.getFullPathName();
        auto it = cache.find (key);
        if (it == cache.end())
        {
            std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
            if (r == nullptr || r->numChannels < 1) return nullptr;
            if (cache.size() > 80) cache.clear();                  // a simple cap on memory
            Entry e; e.rate = r->sampleRate; e.length = r->lengthInSamples;
            it = cache.emplace (key, std::move (e)).first;
        }
        it->second.used = ++tick;
        return &it->second;
    }

    bool ensure (Entry& e, const juce::File& f, juce::int64 need0, juce::int64 need1)
    {
        if (e.count > 0 && need0 >= e.start && need1 <= e.start + e.count) return true;
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));      // opened only while reading: no file stays locked
        if (r == nullptr) return false;
        const juce::int64 span = need1 - need0, extra = span / 3;                       // a little more each side, for scrolling
        const juce::int64 a = juce::jmax ((juce::int64) 0, need0 - extra);
        const juce::int64 b = juce::jmin (r->lengthInSamples, need1 + extra);
        const int n = (int) juce::jmin ((juce::int64) kMaxSamples, b - a);
        if (n <= 0) return false;
        e.data.setSize ((int) r->numChannels, n, false, false, true);
        e.data.clear();
        if (! r->read (&e.data, 0, n, a, true, true)) return false;
        e.start = a; e.count = n; e.length = r->lengthInSamples;
        return need0 >= e.start && need1 <= e.start + e.count;
    }

    juce::AudioFormatManager& formats;
    std::map<juce::String, Entry> cache;
    juce::int64 tick = 0;
};
} // namespace td
