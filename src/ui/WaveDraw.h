#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "../core/CrashReport.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "WaveColor.h"
#include "WaveThumb.h"
#include "../core/Common.h"
#include <atomic>
#include <deque>
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
    the actual sample curve.

    The audio of each file is held in a piece of a few seconds (about 5.5 s at 96 kHz, 11 s at 48 kHz), so scrolling and the moving playhead - which
    redraws a thin strip 60 times a second - draw from memory and never wait for the disk. Before the playhead reaches the end of the piece the
    next one is read by a thread of its own (a read-ahead); only a jump to somewhere not held (a new zoom, a click far away, the first draw) is
    read at once, as it always was. (Until 2.93.2 the piece was only a little wider than the strip being drawn, so zoomed in the files were opened
    and read again on nearly every redraw of the playhead: slow on Windows, where each open can wait on the virus scan.) */
class DirectWaveCache
{
public:
    static constexpr double kThumbSamplesPerPixelLimit = (double) WaveThumb::kSamplesPerPoint;     // thumbnails hold one value per 256 samples
    explicit DirectWaveCache (juce::AudioFormatManager& fm) : formats (fm) {}
    ~DirectWaveCache() { reader.stop(); }
    void forget (const juce::String& path) { cache.erase (path); }
    /** How many times a file has been opened and read for a waveform, since the program started (a measure, for the development tests). */
    static std::atomic<int>& readCounter() { static std::atomic<int> n { 0 }; return n; }

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
        if (! prepare (*e, file, need0, need1)) return false;

        const Chunk& ch = *e->cur;
        const int nCh = ch.data.getNumChannels();
        const int bandH = juce::jmax (1, area.getHeight() / juce::jmax (1, nCh));
        for (int c = 0; c < nCh; ++c)
        {
            const float* d = ch.data.getReadPointer (c);
            const float centre = (float) area.getY() + (float) bandH * ((float) c + 0.5f);
            const float half = (float) bandH * 0.5f * vzoom;
            const float lo = (float) area.getY() + (float) bandH * (float) c, hi = lo + (float) bandH;
            auto yOf = [&] (float v) { return juce::jlimit (lo, hi, centre - v * half); };
            auto at = [&] (juce::int64 s) -> float
            {
                const auto i = s - ch.start;
                return juce::isPositiveAndBelow (i, (juce::int64) ch.data.getNumSamples()) ? d[i] : 0.0f;
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
    static constexpr juce::int64 kBehind = 65536, kAhead = 524288;   // a held piece reaches this far before and after what is being drawn (about 5.5 s ahead at 96 kHz)
    static constexpr juce::int64 kBudgetBytes = 256ll * 1024 * 1024; // all the held pieces together: the ones used longest ago are let go beyond this

    struct Chunk
    {
        juce::int64 start = 0, count = 0, fileLength = 0;
        juce::Time modified;
        juce::AudioBuffer<float> data;
        juce::int64 bytes() const { return (juce::int64) data.getNumChannels() * (juce::int64) data.getNumSamples() * 4; }
    };
    /** A piece being read ahead by the reader thread: 'done' is set (after 'chunk') when it has finished. */
    struct Ahead { std::atomic<bool> done { false }; std::shared_ptr<Chunk> chunk; };
    struct Entry
    {
        double rate = 0.0; juce::int64 length = 0;
        std::shared_ptr<Chunk> cur;                               // the part of the file held in memory
        std::shared_ptr<Ahead> next;                              // the part after it, being read
        juce::int64 used = 0;
        juce::uint32 checkedAt = 0;                               // when the file's date was last looked at
    };

    /** Reads [a, b) of the file (cut to what the file holds and to kMaxSamples). Null if it cannot be read. Safe on any thread. */
    static std::shared_ptr<Chunk> readChunk (juce::AudioFormatManager& fm, const juce::File& f, juce::int64 need0, juce::int64 need1, juce::int64 a, juce::int64 b)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));              // opened only while reading: no file stays locked
        if (r == nullptr) return nullptr;
        ++readCounter();
        a = juce::jmax ((juce::int64) 0, a); b = juce::jmin (r->lengthInSamples, b);
        if (b - a > kMaxSamples) { b = a + kMaxSamples; if (b < need1) { a = juce::jmax ((juce::int64) 0, need0); b = juce::jmin (r->lengthInSamples, a + kMaxSamples); } }
        const int n = (int) (b - a);
        if (n <= 0) return nullptr;
        auto c = std::make_shared<Chunk>();
        c->data.setSize ((int) r->numChannels, n, false, false, true);
        c->data.clear();
        if (! r->read (&c->data, 0, n, a, true, true)) return nullptr;
        c->start = a; c->count = n; c->fileLength = r->lengthInSamples; c->modified = f.getLastModificationTime();
        return c;
    }

    Entry* find (const juce::File& f)
    {
        const auto key = f.getFullPathName();
        auto it = cache.find (key);
        if (it == cache.end())
        {
            std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
            if (r == nullptr || r->numChannels < 1) return nullptr;
            if (cache.size() > 80) cache.clear();                  // a simple cap on memory
            Entry e; e.rate = r->sampleRate; e.length = r->lengthInSamples; e.checkedAt = juce::Time::getMillisecondCounter();
            it = cache.emplace (key, std::move (e)).first;
        }
        it->second.used = ++tick;
        return &it->second;
    }

    /** Makes sure e.cur holds [need0, need1): from memory when it can, else (a jump) read now. And, when the playhead is getting near the end of
        what is held, starts reading the next part on the reader thread. */
    bool prepare (Entry& e, const juce::File& f, juce::int64 need0, juce::int64 need1)
    {
        auto covers = [&] (const Chunk* c) { return c != nullptr && need0 >= c->start && need1 <= c->start + c->count; };
        const auto now = juce::Time::getMillisecondCounter();
        if (e.cur != nullptr && now - e.checkedAt > 1000)             // the file was rewritten (a fix, a bounce): what is held is old
        {
            e.checkedAt = now;
            if (e.cur->modified != f.getLastModificationTime()) { e.cur.reset(); e.next.reset(); }
        }
        if (e.next != nullptr && e.next->done.load (std::memory_order_acquire))      // the part read ahead is ready
        {
            if (covers (e.next->chunk.get())) { e.cur = e.next->chunk; e.length = e.cur->fileLength; }
            e.next.reset();
        }
        if (! covers (e.cur.get()))
        {
            e.next.reset();                                            // (whatever was on its way is for somewhere else)
            // read now only a little round what is drawn (a quarter of the usual reach): this is on the message thread, for every file of the window, at
            // every jump or zoom; the rest comes from the reader thread at once (what is held is then short of kAhead / 2, so the read-ahead starts below)
            e.cur = readChunk (formats, f, need0, need1, need0 - kBehind / 4, need1 + kAhead / 4);
            if (! covers (e.cur.get())) { e.cur.reset(); return false; }
            e.length = e.cur->fileLength; e.checkedAt = now;
            trim (&e);
        }
        else if (e.next == nullptr && e.cur->start + e.cur->count < e.length && e.cur->start + e.cur->count - need1 < kAhead / 2)
        {
            e.next = std::make_shared<Ahead>();
            reader.request (formats, f, need0, need1, juce::jmax ((juce::int64) 0, need0 - kBehind), need1 + kAhead, e.next);
        }
        return true;
    }

    /** Lets go of the pieces used longest ago when all of them together hold more than kBudgetBytes (never the one being drawn). */
    void trim (const Entry* keep)
    {
        for (;;)
        {
            juce::int64 total = 0; Entry* oldest = nullptr;
            for (auto& kv : cache)
            {
                auto& en = kv.second;
                if (en.cur == nullptr) continue;
                total += en.cur->bytes();
                if (&en != keep && (oldest == nullptr || en.used < oldest->used)) oldest = &en;
            }
            if (total <= kBudgetBytes || oldest == nullptr) return;
            oldest->cur.reset(); oldest->next.reset();
        }
    }

    /** The thread that reads the pieces ahead of the playhead, one at a time, at low priority. */
    struct Reader : public juce::Thread
    {
        Reader() : juce::Thread ("Waveform read-ahead") {}
        struct Job { juce::AudioFormatManager* fm = nullptr; juce::File file; juce::int64 need0 = 0, need1 = 0, a = 0, b = 0; std::shared_ptr<Ahead> out; };
        void request (juce::AudioFormatManager& fm, const juce::File& f, juce::int64 need0, juce::int64 need1, juce::int64 a, juce::int64 b, std::shared_ptr<Ahead> out)
        {
            { const juce::ScopedLock sl (lock); jobs.push_back (Job { &fm, f, need0, need1, a, b, std::move (out) }); }
            if (! isThreadRunning()) startThread (juce::Thread::Priority::low);
            notify();
        }
        void stop() { signalThreadShouldExit(); notify(); stopThreadOrLog (*this, 20000); }     // (a read from a drive that has gone to sleep can take seconds: 3.1.2, was 5 s)
        void run() override
        {
            td::crashreport::nameThisThread (getThreadName());                // (3.17.0: named for the log)
            while (! threadShouldExit())
            {
                Job j; bool have = false;
                { const juce::ScopedLock sl (lock); if (! jobs.empty()) { j = std::move (jobs.front()); jobs.pop_front(); have = true; } }
                if (! have) { wait (-1); continue; }
                if (j.out.use_count() > 1)                            // (nobody waits for it any more if the cache let it go)
                    j.out->chunk = readChunk (*j.fm, j.file, j.need0, j.need1, j.a, j.b);
                j.out->done.store (true, std::memory_order_release);
            }
        }
        juce::CriticalSection lock; std::deque<Job> jobs;
    };

    juce::AudioFormatManager& formats;
    std::map<juce::String, Entry> cache;
    juce::int64 tick = 0;
    Reader reader;                                                    // (last: stopped first)
};
} // namespace td
