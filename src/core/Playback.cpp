#include "Playback.h"
#include "CoreReservation.h"
#include <set>
#include <algorithm>
#include "AudioFormats.h"

namespace td
{
PlaybackSession::PlaybackSession (std::vector<int> counts, std::vector<PlaySegment> segs, juce::int64 startPosition,
                                  juce::int64 endPosition, double sr, int block)
    : juce::Thread ("Playback reader"), channelCounts (std::move (counts)), segments (std::move (segs)),
      startPos (startPosition), endPos (endPosition), renderPos (startPosition), loopEnd (endPosition), loopLen (endPosition - startPosition),
      continueTo (endPosition), sampleRate (sr), maxBlock (juce::jmax (16, block))
{
    td::registerAudioFormats (formats);
    const int maxCh = kMaxTrackChannels;
    scratch.setSize (maxCh, 16384);
    for (int c : channelCounts)
    {
        const int ch = juce::jlimit (1, kMaxTrackChannels, c);
        ring.emplace_back();
        for (int k = 0; k < ch; ++k) ring.back().emplace_back ((size_t) kRing);
        chunk.emplace_back (ch, 8192);
        trackBuf.emplace_back (ch, maxBlock);
        chunk.back().clear(); trackBuf.back().clear();
    }
    position = 0;
}

PlaybackSession::~PlaybackSession()
{
    // The reader may be inside a read from a drive that has gone to sleep (an external disk spinning up takes several seconds; a network share
    // longer). This runs on the Retirer thread, so nothing visible waits for it: better a long wait than a thread killed mid-read (3.1.2: was 3 s).
    stopThreadOrLog (*this, 30000);
}

PlaybackSession::Opener PlaybackSession::makeOpener (juce::AudioFormatManager& fm, int bufBytes, double rate, juce::String* problem)
{
    // Each file is read through a buffer, so the disk is asked for big pieces instead of a small read for every few thousand samples (kinder to a
    // hard drive, and far fewer calls into Windows).
    return [&fm, bufBytes, rate, problem] (const PlaySegment& s) -> std::unique_ptr<juce::AudioFormatReader>
    {
        std::unique_ptr<juce::AudioFormatReader> r;
        if (auto fis = std::make_unique<juce::FileInputStream> (s.file); fis->openedOk())
            r.reset (fm.createReaderFor (std::make_unique<juce::BufferedInputStream> (fis.release(), bufBytes, true)));
        if (r == nullptr) { if (problem != nullptr && problem->isEmpty()) *problem = "Cannot read " + s.file.getFullPathName(); return nullptr; }
        if (rate > 0.0 && std::abs (r->sampleRate - rate) > 0.5)
        {
            if (problem != nullptr && problem->isEmpty()) *problem = s.file.getFileName() + " is " + juce::String (r->sampleRate, 0) + " Hz, not " + juce::String (rate, 0) + " Hz";
            juce::Logger::writeToLog ("Playback: " + s.file.getFullPathName() + " is " + juce::String (r->sampleRate, 0) + " Hz, not " + juce::String (rate, 0) + " Hz: left silent");
            return nullptr;
        }
        return r;
    };
}

juce::String PlaybackSession::open()
{
    // Nothing is opened yet: the files are opened by the disk thread as the play reaches them, and closed again once it has passed them (an edit of
    // thousands of pieces used to open every file here, each with a read buffer, before the first sound). What is checked now: the sample rate of
    // every file as the project recorded it (no disk), and that the files of the first few seconds are there. An edit with thousands of pieces used
    // to ask the disk about every one of its files - a quarter of a second at every press of Play. A file further on that has gone is found by
    // the reader when it gets there: that part is silent, and the log says which file.
    readers.clear(); readers.resize (segments.size());
    const juce::int64 checkTo = startPos + (juce::int64) std::llround (3.0 * sampleRate);
    std::set<juce::String> seen;
    for (auto& s : segments)
    {
        if (s.rate > 0.0 && std::abs (s.rate - sampleRate) > 0.5)
            return s.file.getFileName() + " is " + juce::String (s.rate, 0) + " Hz but the audio device is running at " + juce::String (sampleRate, 0) + " Hz";
        if (s.begin < checkTo && s.end > startPos && seen.insert (s.file.getFullPathName()).second && ! s.file.existsAsFile())
            return "Cannot read " + s.file.getFullPathName();
    }
    const int bufBytes = bufferBytesFor (channelCounts.size() * 3);      // (about three files per track are open at a time: a piece, the next one, a crossfade)
    auto inner = makeOpener (formats, bufBytes, sampleRate, nullptr);
    opener = [this, inner] (const PlaySegment& s) -> std::unique_ptr<juce::AudioFormatReader>
    {
        auto r = inner (s);
        if (r == nullptr && unreadable.insert (s.file.getFullPathName()).second)
            juce::Logger::writeToLog ("Playback: cannot read " + s.file.getFullPathName() + " (that part plays silent)");
        return r;
    };
    return {};
}

void PlaybackSession::start (bool waitForPreload)
{
    // a loop keeps less in hand (see run()), so it needs less to start; otherwise about a third of a second (the reader carries on filling the
    // ring - several seconds - while it plays)
    primeTarget = looping.load() ? 16384 : (int) juce::jlimit ((juce::int64) 16384, (juce::int64) (kRing / 2), (juce::int64) std::llround (0.35 * sampleRate));
    startedMs = juce::Time::getMillisecondCounterHiRes();
    primed.store (waitForPreload, std::memory_order_release);
    startThread (juce::Thread::Priority::high);          // (on the ordinary cores, like the disk writer: it reads seconds ahead, so the reserved cores are left to the callback and the mixer helpers)
    if (! waitForPreload) return;
    const auto deadline = juce::Time::getMillisecondCounter() + 3000;
    const int preload = looping.load() ? 16384 : kRing / 2;
    while (! renderDone.load() && fifo.getNumReady() < preload && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (5);
    firstSoundMs.store (juce::jmax (0.001, juce::Time::getMillisecondCounterHiRes() - startedMs));
}

void PlaybackSession::run()
{
    // The last moment (1 ms) before the loop's end is faded out here, so going round to the start never clicks (the start has its fade-in in the
    // segments). That faded moment is rendered as a piece of its own and its place in the ring is remembered (tails), so a loop let go of
    // after it was read can have it skipped and read again unfaded.
    const juce::int64 tailLen = juce::jlimit ((juce::int64) 1, juce::jmax ((juce::int64) 1, loopLen / 2), (juce::int64) std::llround (0.001 * sampleRate));
    while (! threadShouldExit())
    {
        if (releaseRequest.exchange (false) && looping.load() && endPos == loopEnd)
        {
            looping = false;
            const auto played = position.load();
            juce::int64 firstTail = -1;                                                       // the end of the pass being heard, if it has been read
            for (auto t : tails) if (t + tailLen > played) { firstTail = t; break; }
            if (firstTail >= 0 && firstTail - played > (juce::int64) maxBlock)
            {
                // the end of this pass has been read already (faded, perhaps followed by more passes): skip it and read on unfaded from there
                dropLen.store (written - firstTail); dropTimeline.store (loopEnd - tailLen);
                dropAt.store (firstTail, std::memory_order_release);
                renderPos = loopEnd - tailLen;
            }
            else if (firstTail >= 0)
            {
                // its faded end is being heard already (or is about to be, within a block): let that finish and go on from mark 2 itself - a 1 ms dip
                // rather than a moment heard twice
                dropLen.store (written - (firstTail + tailLen)); dropTimeline.store (loopEnd);
                dropAt.store (firstTail + tailLen, std::memory_order_release);
                renderPos = loopEnd;
            }
            else
            {
                // the reader is still before the end of the pass the audio is in: it simply reads on past the end
                linearTimeline.store (startPos); linearFrom.store (wrapStream, std::memory_order_release);
            }
            endPos = continueTo;
            tails.clear();
        }
        if (renderPos >= endPos)
        {
            if (looping.load() && endPos > startPos) { renderPos = startPos; wrapStream = written; }      // loop: carry on from the start, without a gap
            else { renderDone = true; return; }
        }
        const int space = fifo.getFreeSpace();
        if (space < 4096) { wait (5); continue; }
        if (looping.load() && fifo.getNumReady() >= 32768) { wait (5); continue; }      // a loop is only read ~0.7 s ahead, so switching it off takes effect quickly
        int n = (int) juce::jmin ((juce::int64) juce::jmin (8192, space), endPos - renderPos);
        const bool loopsHere = endPos == loopEnd && loopLen > 2 * tailLen;            // (the loop's own end: faded; once let go of, endPos is further on)
        if (loopsHere && renderPos < loopEnd - tailLen) n = (int) juce::jmin ((juce::int64) n, loopEnd - tailLen - renderPos);
        const bool tail = loopsHere && renderPos >= loopEnd - tailLen;

        renderSegments (readers, segments, renderPos, n, chunk, scratch, &opener);
        if (tail)
        {
            for (auto& tb : chunk)
                for (int i = 0; i < n; ++i)
                {
                    const float g = 1.0f - (float) (renderPos + i - (loopEnd - tailLen) + 1) / (float) tailLen;
                    for (int ch = 0; ch < tb.getNumChannels(); ++ch) tb.getWritePointer (ch)[i] *= juce::jmax (0.0f, g);
                }
            if (renderPos == loopEnd - tailLen)
            {
                const auto played = position.load();                                         // (forget the ends that have been heard)
                tails.erase (std::remove_if (tails.begin(), tails.end(), [&] (juce::int64 t) { return t + tailLen <= played; }), tails.end());
                tails.push_back (written);
            }
        }
        if (! looping.load()) releaseReadersBefore (readers, segments, renderPos + n);     // (a loop comes round again: its files stay open)

        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        for (size_t t = 0; t < ring.size(); ++t)
            for (size_t c = 0; c < ring[t].size(); ++c)
            {
                if (z1 > 0) juce::FloatVectorOperations::copy (ring[t][c].get() + s1, chunk[t].getReadPointer ((int) c), z1);
                if (z2 > 0) juce::FloatVectorOperations::copy (ring[t][c].get() + s2, chunk[t].getReadPointer ((int) c) + z1, z2);
            }
        fifo.finishedWrite (z1 + z2);
        renderPos += z1 + z2;
        written += z1 + z2;
    }
}

int PlaybackSession::readRing (int offset, int n) noexcept
{
    int s1, z1, s2, z2;
    fifo.prepareToRead (n, s1, z1, s2, z2);
    const int got = z1 + z2;
    for (size_t t = 0; t < ring.size(); ++t)
    {
        auto& dst = trackBuf[t];
        for (int c = 0; c < dst.getNumChannels(); ++c)
        {
            const float* src = ring[t][(size_t) c].get();
            if (z1 > 0) dst.copyFrom (c, offset, src + s1, z1);
            if (z2 > 0) dst.copyFrom (c, offset + z1, src + s2, z2);
        }
    }
    fifo.finishedRead (got);
    position.fetch_add (got);
    return got;
}

void PlaybackSession::pull (int n) noexcept
{
    n = juce::jmin (n, maxBlock);
    if (! primed.load (std::memory_order_acquire))
    {
        // a start that did not wait: silence, at the start, until the reader has the first moments ready (or it gives up waiting after 3 s and
        // plays what there is)
        if (renderDone.load() || fifo.getNumReady() >= primeTarget || primingWaited >= (juce::int64) (3.0 * sampleRate))
        {
            firstSoundMs.store (juce::jmax (0.001, juce::Time::getMillisecondCounterHiRes() - startedMs));
            primed.store (true, std::memory_order_release);
        }
        else
        {
            primingWaited += n;
            for (auto& dst : trackBuf) dst.clear (0, n);
            clockSeq.fetch_add (1, std::memory_order_acq_rel);
            clockPos.store (position.load(), std::memory_order_relaxed);
            clockMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            clockBlock.store (0, std::memory_order_relaxed);                   // (the playhead does not glide on meanwhile)
            clockSeq.fetch_add (1, std::memory_order_release);
            return;
        }
    }
    int got = 0;
    while (got < n)
    {
        int want = n - got;
        if (const auto da = dropAt.load (std::memory_order_acquire); da >= 0)       // the loop was let go of after its end had been read: skip that audio
        {
            const auto pos = position.load();
            if (pos >= da)
            {
                const auto len = dropLen.load();
                auto skip = len - (pos - da);                                         // (normally all of it; less if this block had already begun on it)
                const auto over = juce::jmax ((juce::int64) 0, -skip);                // (it even went on into what was read after: the timeline counts that in)
                while (skip > 0)
                {
                    int s1, z1, s2, z2;
                    fifo.prepareToRead ((int) juce::jmin ((juce::int64) (1 << 30), skip), s1, z1, s2, z2);
                    if (z1 + z2 <= 0) break;
                    fifo.finishedRead (z1 + z2); skip -= z1 + z2;
                }
                linearTimeline.store (dropTimeline.load() + over); linearFrom.store (pos, std::memory_order_release);
                dropAt.store (-1);
                continue;
            }
            want = (int) juce::jmin ((juce::int64) want, da - pos);
        }
        const int k = readRing (got, want);
        got += k;
        if (k < want) break;
    }
    if (got < n) for (auto& dst : trackBuf) for (int c = 0; c < dst.getNumChannels(); ++c) dst.clear (c, got, n - got);
    const auto now = position.load();
    clockSeq.fetch_add (1, std::memory_order_acq_rel);                    // (odd: being written)
    clockPos.store (now, std::memory_order_relaxed);
    clockMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    clockBlock.store (got, std::memory_order_relaxed);
    clockSeq.fetch_add (1, std::memory_order_release);
    if (got < n)
    {
        if (renderDone.load()) { if (fifo.getNumReady() == 0) finished = true; }
        else underruns.fetch_add (1);
    }
    else if (renderDone.load() && fifo.getNumReady() == 0)
        finished = true;
}

double PlaybackSession::getSmoothPositionSeconds() const noexcept
{
    if (sampleRate <= 0.0) return 0.0;
    for (int tries = 0; tries < 16; ++tries)
    {
        const auto s0 = clockSeq.load (std::memory_order_acquire);
        if ((s0 & 1u) != 0) continue;
        const auto pos = clockPos.load (std::memory_order_relaxed);
        const double ms = clockMs.load (std::memory_order_relaxed);
        const int block = clockBlock.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (clockSeq.load (std::memory_order_relaxed) != s0) continue;
        if (ms <= 0.0 || finished.load()) return (double) pos / sampleRate;
        const double ahead = juce::jlimit (0.0, (double) block, (juce::Time::getMillisecondCounterHiRes() - ms) * 0.001 * sampleRate);
        return ((double) pos + ahead) / sampleRate;
    }
    return getPositionSeconds();
}

void PlaybackSession::releaseReadersBefore (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers, const std::vector<PlaySegment>& segs, juce::int64 pos)
{
    for (size_t i = 0; i < segs.size() && i < readers.size(); ++i)
        if (readers[i] != nullptr && segs[i].end <= pos) readers[i].reset();
}

void PlaybackSession::renderSegments (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers, const std::vector<PlaySegment>& segs,
                                      juce::int64 from, int n, std::vector<juce::AudioBuffer<float>>& out, juce::AudioBuffer<float>& scratch, const Opener* opener)
{
    for (auto& b : out) { if (n > b.getNumSamples()) return; b.clear (0, n); }
    const juce::int64 to = from + n;
    for (size_t i = 0; i < segs.size(); ++i)
    {
        const auto& s = segs[i];
        if (s.trackIndex < 0 || s.trackIndex >= (int) out.size() || i >= readers.size()) continue;
        const juce::int64 a = juce::jmax (from, s.begin), b = juce::jmin (to, s.end);
        if (b <= a) continue;
        if (readers[i] == nullptr && opener != nullptr && *opener) readers[i] = (*opener) (s);
        auto* reader = readers[i].get();
        if (reader == nullptr) continue;                                                   // (missing, unreadable, or the wrong rate: silence)
        auto& dst = out[(size_t) s.trackIndex];
        const int m = (int) (b - a);
        const int ch = juce::jmin ((int) reader->numChannels, dst.getNumChannels(), scratch.getNumChannels());
        if (ch <= 0 || m > scratch.getNumSamples()) continue;

        // read the file region, zero-filling anything outside the file
        const juce::int64 srcStart = a + s.srcOffset;
        scratch.clear (0, m);
        const juce::int64 fileLen = reader->lengthInSamples;
        const juce::int64 rs = juce::jmax ((juce::int64) 0, srcStart), re = juce::jmin (fileLen, srcStart + m);
        if (re > rs)
        {
            const int dstOff = (int) (rs - srcStart), cnt = (int) (re - rs);
            float* ptrs[kMaxTrackChannels];
            for (int c = 0; c < ch; ++c) ptrs[c] = scratch.getWritePointer (c) + dstOff;
            reader->read (ptrs, ch, rs, cnt);
        }

        // fades and volume changes
        const bool hasIn = s.fadeInLen > 0, hasOut = s.fadeOutLen > 0, hasSteps = ! s.gainSteps.empty();
        if (hasIn || hasOut || hasSteps)
            for (int k = 0; k < m; ++k)
            {
                const juce::int64 t = a + k;
                float g = 1.0f;
                if (hasIn)  g *= t < s.fadeInStart ? 0.0f : t >= s.fadeInStart + s.fadeInLen ? 1.0f
                                 : fadeGain (s.inCurve, (float) (t - s.fadeInStart) / (float) s.fadeInLen, true);
                if (hasOut) g *= t < s.fadeOutStart ? 1.0f : t >= s.fadeOutStart + s.fadeOutLen ? 0.0f
                                 : fadeGain (s.outCurve, (float) (t - s.fadeOutStart) / (float) s.fadeOutLen, false);
                if (hasSteps) g *= s.gainAt (t);
                for (int c = 0; c < ch; ++c) scratch.getWritePointer (c)[k] *= g;
            }

        for (int c = 0; c < ch; ++c)
            juce::FloatVectorOperations::add (dst.getWritePointer (c) + (a - from), scratch.getReadPointer (c), m);
    }
}

// ----------------------------------------------------------------------------- builders
static int trackIndexOf (const Project& p, const juce::Uuid& id)
{
    for (size_t i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].id == id) return (int) i;
    return -1;
}

std::vector<PlaySegment> segmentsForTake (const Project& p, const TakeGroup& g, juce::int64 shift)
{
    std::vector<PlaySegment> v;
    for (auto& f : g.files)
    {
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file; s.srcOffset = -shift; s.rate = g.sampleRate;
        s.begin = shift; s.end = shift + g.lengthSamples;
        const auto inLen  = (juce::int64) std::llround (g.fadeInSeconds * g.sampleRate);
        const auto outLen = (juce::int64) std::llround (g.fadeOutSeconds * g.sampleRate);
        if (inLen > 0)  { s.fadeInStart = s.begin; s.fadeInLen = juce::jmin (inLen, g.lengthSamples / 2); }
        if (outLen > 0) { s.fadeOutLen = juce::jmin (outLen, g.lengthSamples / 2); s.fadeOutStart = s.end - s.fadeOutLen; }
        v.push_back (s);
    }
    return v;
}

int guideTrackIndex (const Project& p, const EditGuide& gd)
{
    if (const int ti = trackIndexOf (p, gd.trackId); ti >= 0 && p.tracks[(size_t) ti].channelCount() == gd.numChannels) return ti;
    for (size_t i = 0; i < p.tracks.size(); ++i)                        // its own track has gone: one of the same name, then any with as many channels
        if (p.tracks[i].name.equalsIgnoreCase (gd.trackName) && p.tracks[i].channelCount() == gd.numChannels) return (int) i;
    for (size_t i = 0; i < p.tracks.size(); ++i)
        if (p.tracks[i].channelCount() == gd.numChannels && ! p.tracks[i].isLtc) return (int) i;
    return -1;
}

std::vector<PlaySegment> segmentsForGuide (const Project& p, const EditDef& e)
{
    std::vector<PlaySegment> v;
    const auto& gd = e.guide;
    if (! gd.active()) return v;
    const int ti = guideTrackIndex (p, gd);
    if (ti < 0) return v;
    PlaySegment s;
    s.trackIndex = ti; s.file = gd.file; s.rate = gd.sampleRate;
    s.srcOffset = gd.srcIn - gd.start;                                  // file sample = timeline sample + srcOffset
    s.begin = juce::jmax ((juce::int64) 0, gd.start); s.end = gd.end();
    if (s.end <= s.begin) return v;
    const auto fade = juce::jmin ((s.end - s.begin) / 2, (juce::int64) std::llround (0.005 * gd.sampleRate));     // (5 ms in and out: the guide's ends never click)
    if (fade > 0) { s.fadeInStart = s.begin; s.fadeInLen = fade; s.fadeOutStart = s.end - fade; s.fadeOutLen = fade; }
    v.push_back (s);
    return v;
}

/** LTC (timecode) is never faded or crossfaded: a piece's LTC starts hard at the first sample of the piece's fade-in and stops hard at the end of its fade-out - or, when the next
    piece begins first (a crossfade), one sample before that piece's fade-in begins, so the pieces of LTC never overlap and the next one starts the moment its piece appears. */
juce::int64 ltcEndOf (const EditRegion& r, const EditRegion* next) noexcept
{
    juce::int64 e = r.fadeOutFinish();
    if (next != nullptr && next->fadeInBegin() > r.fadeInBegin()) e = juce::jmin (e, next->fadeInBegin());
    return e;                                                         // (exclusive: the last LTC sample is e - 1)
}

static void addRegionSegments (const Project& p, const EditRegion& r, std::vector<PlaySegment>& v, const EditRegion* next = nullptr, bool overdub = false)
{
    for (size_t fi = 0; fi < r.files.size(); ++fi)
    {
        auto& f = r.files[fi];
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        if (p.tracks[(size_t) ti].isLtc)
        {
            if (overdub) continue;                                    // overdubs carry no timecode
            PlaySegment l;
            l.trackIndex = ti; l.file = f.file; l.rate = r.sampleRate;
            l.srcOffset = r.srcIn - r.startSample - f.fileStart;
            l.begin = r.fadeInBegin(); l.end = ltcEndOf (r, next);
            if (l.end > l.begin) v.push_back (l);
            continue;
        }
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file; s.rate = r.sampleRate;
        s.srcOffset = r.srcIn - r.startSample - f.fileStart;
        s.begin = r.fadeInBegin();
        s.end   = r.fadeOutFinish();
        if (r.fadeInFinish() > r.fadeInBegin())
            { s.fadeInStart = r.fadeInBegin(); s.fadeInLen = r.fadeInFinish() - r.fadeInBegin(); s.inCurve = r.curve; }
        if (r.fadeOutFinish() > r.fadeOutBegin())
            { s.fadeOutStart = r.fadeOutBegin(); s.fadeOutLen = r.fadeOutFinish() - r.fadeOutBegin(); s.outCurve = r.outCurveValue(); }
        for (size_t k = 0; k < r.gains.size(); ++k)                                  // this file's volume changes, with the level each one starts from
        {
            const auto& g = r.gains[k];
            GainStep st;
            st.at = r.startSample + g.at;
            st.ramp = (juce::int64) std::llround (g.ramp * r.sampleRate);
            st.from = r.levelAfter (fi, k, g.at);
            st.to = fi < g.db.size() ? dbToLinear (g.db[fi]) : 1.0f;
            if (k == 0 && ! g.autoNorm && g.at <= (juce::int64) (0.001 * r.sampleRate))
            {
                // the level given to the piece as a whole (the trim window's Level faders): it holds from the very first sound of the piece, the fade-in included,
                // so the fade only reaches that level, instead of rising to 0 dB and then dropping to it
                st.at = juce::jmin (st.at, s.begin); st.ramp = 0; st.from = st.to;
            }
            s.gainSteps.push_back (st);
        }
        if (s.end > s.begin) v.push_back (s);
    }
}

void AutomationPlan::apply (juce::int64 t, const PlaybackSession* ps) const noexcept
{
    for (auto& it : items)
    {
        if (it.lane.pts.empty()) continue;
        const juce::int64 d = it.lat != nullptr ? (juce::int64) juce::jmax (0, it.lat->load (std::memory_order_relaxed)) : 0;
        const juce::int64 at = d == 0 ? t : ps != nullptr ? ps->getTimelinePositionAgo (d) : t - d;
        if (it.vcaBit != 0)
            for (auto& mb : it.members)                                         // each channel under the VCA: its own lateness
            {
                const juce::int64 dm = mb.lat != nullptr ? (juce::int64) juce::jmax (0, mb.lat->load (std::memory_order_relaxed)) : 0;
                mb.db->set (it.lane.valueAt (dm == 0 ? t : ps != nullptr ? ps->getTimelinePositionAgo (dm) : t - dm, 0.0f));
                mb.mask->fetch_or (it.vcaBit, std::memory_order_relaxed);
            }
        if (it.target != nullptr) it.target->set (it.lane.valueAt (at, it.target->get()));
        else if (it.slot != nullptr)
        {
            const float v = it.lane.valueAt (at, 0.5f, it.steps);                  // (a switch such as a bypass: only its own positions, never the ones between)
            const bool changed = std::abs (v - it.lastSent) > 1.0e-5f;
            if ((changed || it.refresh > 0) && it.slot->setParamNorm (it.paramIndex, v))        // (counted as sent only once the plug-in really got it: a busy slot is tried again)
            {
                it.lastSent = v;
                if (! changed) --it.refresh;
            }
        }
    }
}

bool AutomationPlan::begin() const noexcept
{
    bool any = false;
    for (auto& it : items) if (it.send != nullptr && it.lane.pts.size() > 0) { it.send->autoHeld.store (true); any = true; }
    return any;
}

bool AutomationPlan::end() const noexcept
{
    bool any = false;
    for (auto& it : items)
    {
        for (auto& mb : it.members) mb.mask->fetch_and (~it.vcaBit, std::memory_order_relaxed);
        if (it.send != nullptr && it.send->autoHeld.exchange (false)) any = true;
    }
    return any;
}

std::shared_ptr<AutomationPlan> automationPlanFor (Project& p, const EditDef& e, bool onProcessingMixer, const juce::Uuid& unassignedMixer)
{
    if (! e.hasAutomation()) return nullptr;                   // (the lane view being shown or not does not matter: what is written plays)
    auto plan = std::make_shared<AutomationPlan>();
    for (auto& lane : e.lanes)
    {
        if (lane.pts.empty()) continue;
        MixerState* m = nullptr;
        if (lane.mixerId.isNull())                      // the default is the Edit's own mixer (or, in a render, the mixer that is being rendered)
        {
            if (! unassignedMixer.isNull()) for (auto& mm : p.mixers) if (mm->id == unassignedMixer) m = mm.get();
            if (m == nullptr) m = onProcessingMixer ? nullptr : p.mixerOfEdit (e.id);
            if (m == nullptr && ! p.mixers.empty()) m = p.mixers.front().get();
        }
        else for (auto& mm : p.mixers) if (mm->id == lane.mixerId) m = mm.get();
        if (const auto* vd = p.findVca (lane.trackId); vd != nullptr && m != nullptr)      // a VCA's fader lane (the only automatable thing of a VCA): trackId holds the VCA's id
        {
            if (lane.param != autoparam::fader) continue;
            const int vn = juce::jlimit (0, kMaxVcas - 1, vd->number - 1);
            auto& vs = m->vcas[vn];
            AutomationPlan::Item vi; vi.lane = lane; vi.target = &vs.gainDb; vi.lat = &vs.autoLat;      // (its own fader shows the value for the latest channel)
            vi.vcaBit = 1u << vn;
            for (auto& st : m->stripPool) if (st->vcaMask.load() & vi.vcaBit) vi.members.push_back ({ &st->vcaAutoDb[vn], &st->vcaAutoMask, &st->autoLatOut });
            for (auto& b : m->busPool)    if (b->vcaMask.load() & vi.vcaBit)  vi.members.push_back ({ &b->vcaAutoDb[vn], &b->vcaAutoMask, &b->autoLatOut });
            plan->items.push_back (std::move (vi));
            continue;
        }
        if (const auto* bd = p.findBus (lane.trackId); bd != nullptr && m != nullptr)        // a bus: an Int Bus has the same lanes as a track (gain, fader, pan, sends, plug-ins); an Ext Bus has its fader and its plug-ins
        {
            auto* bs = m->busFor (lane.trackId);
            if (bs == nullptr) continue;
            AutomationPlan::Item bi; bi.lane = lane;
            if (lane.param == autoparam::fader) { bi.target = &bs->gainDb; bi.lat = &bs->autoLatOut; }
            else if (! bd->external && lane.param == autoparam::gain) { bi.target = &bs->inGainDb; bi.lat = &bs->autoLatIn; }       // (an Ext Bus has no gain, pan or sends: its fader and its plug-ins only)
            else if (! bd->external && lane.param == autoparam::pan) { bi.target = &bs->pan; bi.lat = &bs->autoLatOut; }
            else if (! bd->external && autoparam::isSend (lane.param)) { if (auto* sd = bs->sends.find (autoparam::sendDest (lane.param))) { bi.target = &sd->gainDb; bi.lat = &sd->autoLat; bi.send = sd; } }
            else if (int slot = 0, idx = 0; autoparam::parsePlugin (lane.param, slot, idx) && slot < kNumSlots)
            {
                auto& sl = bs->slots[slot];
                if (auto* proc = sl.getProcessorUnsafe(); proc != nullptr && idx < proc->numParams() && proc->paramAutomatable (idx)) { bi.slot = &sl; bi.paramIndex = idx; bi.steps = proc->paramSteps (idx); bi.lat = &sl.autoLat; }
            }
            if (bi.target != nullptr || bi.slot != nullptr) plan->items.push_back (std::move (bi));
            continue;
        }
        const auto* t = p.findTrack (lane.trackId);
        if (m == nullptr || t == nullptr) continue;
        auto* strip = m->stripFor (lane.trackId);
        AutomationPlan::Item it; it.lane = lane;
        if (lane.param == autoparam::fader) { it.target = &strip->gainDb; it.lat = &strip->autoLatOut; }
        else if (lane.param == autoparam::gain) { it.target = &strip->inGainDb; it.lat = &strip->autoLatIn; }
        else if (lane.param == autoparam::pan && t->channelCount() == 1) { it.target = &strip->pan; it.lat = &strip->autoLatOut; }
        else if (autoparam::isSend (lane.param))
        {
            if (auto* sd = strip->sends.find (autoparam::sendDest (lane.param))) { it.target = &sd->gainDb; it.lat = &sd->autoLat; it.send = sd; }    // only sends that exist
        }
        else if (int slot = 0, idx = 0; autoparam::parsePlugin (lane.param, slot, idx) && slot < kNumSlots)
        {
            auto& sl = strip->slots[slot];
            if (auto* proc = sl.getProcessorUnsafe(); proc != nullptr && idx < proc->numParams() && proc->paramAutomatable (idx))
            { it.slot = &sl; it.paramIndex = idx; it.steps = proc->paramSteps (idx); it.lat = &sl.autoLat; }
        }
        if (it.target != nullptr || it.slot != nullptr) plan->items.push_back (std::move (it));
    }
    return plan->items.empty() ? nullptr : plan;
}

std::vector<PlaySegment> segmentsForEdit (const Project& p, const EditDef& e)
{
    std::vector<PlaySegment> v;
    for (size_t i = 0; i < e.regions.size(); ++i) addRegionSegments (p, e.regions[i], v, i + 1 < e.regions.size() ? &e.regions[i + 1] : nullptr);
    for (auto& r : e.overdubs) addRegionSegments (p, r, v, nullptr, true);      // overdubs are simply added: everything plays at the same time
    return v;
}

std::vector<PlaySegment> segmentsForRegionOriginal (const Project& p, const EditRegion& r)
{
    std::vector<PlaySegment> v;
    for (auto& f : r.files)
    {
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file; s.srcOffset = -f.fileStart; s.rate = r.sampleRate;
        s.begin = f.fileStart; s.end = r.sourceLength > 0 ? r.sourceLength : r.srcOut + (juce::int64) (60.0 * r.sampleRate);
        v.push_back (s);
    }
    return v;
}
} // namespace td
