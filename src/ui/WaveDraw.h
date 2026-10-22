#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "WaveColor.h"
#include "WaveThumb.h"
#include <map>
#include <vector>

namespace td
{
/** The colour the waveforms are drawn in: set it with setWaveColour (instead of g.setColour) so the outline can be a brighter shade of it. */
inline juce::Colour& currentWaveColour() { static juce::Colour c = juce::Colours::white; return c; }
inline void setWaveColour (juce::Graphics& g, juce::Colour c) { currentWaveColour() = c; g.setColour (c); }

/** Fills one channel's waveform as a single connected shape: the top edge left to right, the bottom edge back again. Because it is one anti-aliased
    outline rather than a thin line per pixel column, it never shows gaps or stripes, whatever the track height or the screen's scaling. */
/** bandHeight: the height of the lane the waveform is drawn in, when known (> 0): a band under 10 px tall gets no outline (there the line would be
    the whole picture, and it is two thirds of the drawing work of a window full of small rows). */
inline void fillEnvelope (juce::Graphics& g, float xFirst, const std::vector<float>& tops, const std::vector<float>& bottoms, const std::vector<juce::Colour>* cols = nullptr, float bandHeight = 0.0f)
{
    // Drawn the way Pyramix draws it: every pixel column is a bar from its lowest to its highest sample, the columns joined into one shape with
    // vertical steps between them (no slanting between neighbours), the body filled, and a crisp one-pixel line along the top and the bottom edge,
    // the steps included. Everything sits on whole pixels, so the picture stays sharp at every zoom.
    const size_t n = tops.size();
    if (n == 0 || bottoms.size() != n) return;
    const float x0 = std::floor (xFirst);
    std::vector<float> T (n), B (n);
    for (size_t i = 0; i < n; ++i) { T[i] = std::floor (tops[i] + 0.5f); B[i] = juce::jmax (T[i] + 1.0f, std::floor (bottoms[i] + 0.5f)); }
    juce::Path p;
    p.startNewSubPath (x0, T[0]);
    for (size_t i = 0; i < n; ++i) { p.lineTo (x0 + (float) i, T[i]); p.lineTo (x0 + (float) i + 1.0f, T[i]); }
    for (size_t i = n; i-- > 0;) { p.lineTo (x0 + (float) i + 1.0f, B[i]); p.lineTo (x0 + (float) i, B[i]); }
    p.closeSubPath();
    const auto base = currentWaveColour();
    const bool coloured = cols != nullptr && cols->size() == n;
    auto gradient = [&] (bool outline)                                                // WaveColour: a colour for every column
    {
        const bool solid = chan::solidLook();
        auto tint = [outline, solid] (juce::Colour c)
        {
            if (solid)                                                                    // pale body, thin dark outline; a thump below 100 Hz stays black
            {
                if (c.getBrightness() < 0.1f) return outline ? juce::Colour (0xff000000) : juce::Colour (0xff26262c);
                return outline ? c.withBrightness (c.getBrightness() * 0.2f) : c.interpolatedWith (juce::Colours::white, 0.74f);
            }
            return outline ? (c.getBrightness() < 0.1f ? juce::Colour (0xff6a6a6a) : c.brighter (0.7f)) : c.darker (0.15f);
        };
        const float w = juce::jmax (1.0f, (float) n);
        juce::ColourGradient gr (tint ((*cols)[0]), x0, 0.0f, tint ((*cols)[n - 1]), x0 + w, 0.0f, false);
        const size_t step = juce::jmax ((size_t) 1, n / 400);
        for (size_t i = step; i + 1 < n; i += step) gr.addColour ((double) i / (double) w, tint ((*cols)[i]));
        return gr;
    };
    const bool solid = chan::solidLook();
    if (coloured) g.setGradientFill (gradient (false)); else g.setColour (solid ? base.interpolatedWith (juce::Colours::white, 0.74f) : base.darker (0.25f));
    g.fillPath (p);                          // a calm body ...
    // ... and the line round the outer limit of the waveform (as in SADiE and Pyramix) so the peaks stand out
    if (bandHeight > 0.0f && bandHeight < 10.0f) { g.setColour (base); return; }
    juce::RectangleList<int> line;                                                   // whole pixels: the top and bottom pixel of every column, and the step up or down to the next
    const int ix0 = (int) x0;
    for (size_t i = 0; i < n; ++i)
    {
        const int t = (int) T[i], b = (int) B[i] - 1;
        line.addWithoutMerging ({ ix0 + (int) i, t, 1, 1 });
        if (b > t) line.addWithoutMerging ({ ix0 + (int) i, b, 1, 1 });
        if (i + 1 < n)                                                               // the step to the next column, drawn in whichever of the two reaches further out (so a spike is one column)
        {
            const int nt = (int) T[i + 1], nb = (int) B[i + 1] - 1;
            if (std::abs (nt - t) > 1) line.addWithoutMerging ({ ix0 + (int) (nt < t ? i + 1 : i), juce::jmin (t, nt), 1, std::abs (nt - t) });
            if (std::abs (nb - b) > 1) line.addWithoutMerging ({ ix0 + (int) (nb > b ? i + 1 : i), juce::jmin (b, nb), 1, std::abs (nb - b) });
        }
    }
    if (coloured) g.setGradientFill (gradient (true)); else g.setColour (solid ? base.withBrightness (base.getBrightness() * 0.12f) : base.brighter (0.7f));
    g.fillRectList (line);
    g.setColour (base);
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
inline void drawThumbnailEnvelope (juce::Graphics& g, juce::Rectangle<int> area, const WaveThumb& th, double t0, double t1, float vzoom, const WaveColorData* wc = nullptr)
{
    if (area.isEmpty() || t1 <= t0) return;
    const int nCh = juce::jmax (1, th.getNumChannels());
    const auto clip = g.getClipBounds().getIntersection (area);
    if (clip.isEmpty()) return;
    if (clip.getWidth() < 2 || area.getHeight() < 3) return;                        // a sliver: the clip's fill colour is all there is room for
    const int bandH = juce::jmax (1, area.getHeight() / nCh);
    const double perPx = (t1 - t0) / (double) area.getWidth();
    std::vector<float> tops ((size_t) clip.getWidth()), bots ((size_t) clip.getWidth());
    std::vector<juce::Colour> cols;
    if (wc != nullptr) for (int i = 0; i < clip.getWidth(); ++i) { const double a = t0 + (double) (clip.getX() + i - area.getX()) * perPx; cols.push_back (wc->colourFor (a, a + perPx)); }
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
        fillEnvelope (g, (float) clip.getX(), tops, bots, wc != nullptr ? &cols : nullptr, (float) bandH);
    }
}

/** Sharp waveforms when zoomed in. The ordinary thumbnails (one value per 256 samples) are fine when zoomed out; once fewer than 256 samples
    share a pixel this reads the audio itself, so every pixel shows the true peaks, and when there is more than a pixel per sample it draws
    the actual sample curve. Pieces read from disk are kept for a while, so scrolling and the moving playhead stay quick. */
class DirectWaveCache
{
public:
    static constexpr double kThumbSamplesPerPixelLimit = (double) WaveThumb::kSamplesPerPoint;     // thumbnails hold one value per 256 samples
    explicit DirectWaveCache (juce::AudioFormatManager& fm) : formats (fm) {}
    void forget (const juce::String& path) { cache.erase (path); }

    /** True if it drew the audio itself (the caller then does not draw the thumbnail). */
    /** rateHint: the file's sample rate when the caller knows it (> 0). Then nothing is opened unless the view is zoomed in far enough to need it:
        a redraw at an ordinary zoom never touches the disk (opening a file that was only just recorded can wait on Windows' virus scan). */
    bool draw (juce::Graphics& g, juce::Rectangle<int> area, const juce::File& file, double t0, double t1, float vzoom, const WaveColorData* wc = nullptr,
               double rateHint = 0.0)
    {
        if (area.getWidth() < 1 || area.getHeight() < 1 || t1 <= t0) return false;
        if (rateHint > 0.0 && (t1 - t0) * rateHint / (double) area.getWidth() >= kThumbSamplesPerPixelLimit) return false;
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
                std::vector<float> tops, bots; std::vector<juce::Colour> cols;
                for (int x = clip.getX(); x < clip.getRight(); ++x)
                {
                    if (wc != nullptr) { const double a = t0 + (double) (x - area.getX()) * (t1 - t0) / (double) area.getWidth(); cols.push_back (wc->colourFor (a, a + (t1 - t0) / (double) area.getWidth())); }
                    const juce::int64 s0 = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (t0 * rate + (double) (x - area.getX()) * spp));
                    const juce::int64 s1 = juce::jmin (e->length, juce::jmax (s0 + 1, (juce::int64) std::ceil (t0 * rate + (double) (x + 1 - area.getX()) * spp)));
                    float mn = 1.0f, mx = -1.0f;
                    for (juce::int64 s = s0; s < s1; ++s) { const float v = at (s); mn = juce::jmin (mn, v); mx = juce::jmax (mx, v); }
                    if (mn > mx) { mn = mx = 0.0f; }
                    tops.push_back (yOf (mx)); bots.push_back (yOf (mn));
                }
                fillEnvelope (g, (float) clip.getX(), tops, bots, wc != nullptr ? &cols : nullptr, (float) bandH);
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
    static constexpr juce::int64 kMaxSamples = 1500000;         // never read more than this for one file in one go (a wide window at 255 samples a pixel, and a third more each side)
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
