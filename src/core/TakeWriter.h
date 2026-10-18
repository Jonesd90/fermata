#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace td
{
/** Puts one take file on disk. The audio thread hands it blocks of samples and never waits; a disk thread (a juce::TimeSliceThread) writes them.
    It replaces juce::AudioFormatWriter::ThreadedWriter, which threw away the result of every disk write (a full drive went unnoticed), dropped a
    block that did not fit so the rest of the take moved earlier in time, rang the disk thread once per track per block, and wrote 24-bit words one
    step low above -6 dBFS.

    What this one does instead:
      - A write that fails (the drive is full, or gone) is remembered (failed()) so the program can raise the alarm. The file is left with a good header
        for what did reach the disk.
      - A block that does not fit the buffer (the disk is far behind) is counted and its length is written later as SILENCE, at the place in the file
        where it belongs, so the file keeps its length and stays in time with the other tracks. (The buffer holds several seconds, so this only
        happens when the disk has stalled for that long.)
      - The float samples are turned into 24-bit words exactly: a word that came in from a 24-bit converter goes out unchanged (a null test passes).
      - Every few seconds the header is written again, so a file cut short by a crash or a power cut is readable without mending.
      - It never wakes the disk thread itself: the engine does that once per block for all the tracks (notifyThread). */
class TakeWriter : private juce::TimeSliceClient
{
public:
    /** writer: a 24-bit integer WAV writer (owned from here), best on a FileOutputStream with a SMALL buffer (1 KB): the audio goes to the file in big
        pieces anyway, and a big stream buffer would hold the last moments back from the disk. bufferSamples: frames the buffer holds (seconds of
        audio: this is how long the disk may stall). headerEverySamples: how often the header is refreshed (0 = only at the end). */
    /** minWriteSamples: the disk is only written to once this much is waiting (except at the end, and before an overrun's silence). Many small
        writes are what slow a drive down, not how much is written: 40 files written in small pieces made ~2500 writes a second, which one disk
        thread could not keep up with; in pieces of half a second it is ~80. 0 = write whatever is there. */
    TakeWriter (std::unique_ptr<juce::AudioFormatWriter> w, juce::TimeSliceThread& t, int bufferSamples, int headerEverySamples, int minWriteSamples = 0)
        : writer (std::move (w)), thread (t), numCh ((int) writer->getNumChannels()), fifo (juce::jmax (1024, bufferSamples) + 1),     // (a fifo keeps one slot free)
          headerEvery (headerEverySamples), minWrite (juce::jlimit (0, juce::jmin (kChunk, juce::jmax (1024, bufferSamples) / 4), minWriteSamples))
    {
        ring.assign ((size_t) numCh, std::vector<float> ((size_t) fifo.getTotalSize(), 0.0f));
        ints.resize ((size_t) numCh);
        for (auto& v : ints) v.assign ((size_t) kChunk, 0);
        zeros.assign ((size_t) kChunk, 0);
        ptrs.assign ((size_t) numCh + 1, nullptr);
        thread.addTimeSliceClient (this);
    }

    /** Message thread: writes what is left (and the silence owed), then finishes the file (the header with its length). */
    ~TakeWriter() override
    {
        thread.removeTimeSliceClient (this);
        closing = true;
        while (drain() == 0) {}
        payDebt (pendingLost.load (std::memory_order_acquire));            // a drop at the very end: its time is still kept
        writer.reset();                                                     // (the WAV writer's destructor writes the final header)
    }

    // ---- audio thread ----
    /** Hands over one block. False when it did not fit (the disk is more than the buffer behind): the block is lost, and that much silence is
        written in its place later. chans: numChannels pointers of n samples each. */
    bool write (const float* const* chans, int n) noexcept
    {
        if (n <= 0) return true;
        if (failedFlag.load (std::memory_order_relaxed)) return true;           // nothing more can reach the disk: nothing to count
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        if (z1 + z2 < n) { if (firstOverrun.load (std::memory_order_relaxed) < 0) firstOverrun.store (accepted + pendingLost.load (std::memory_order_relaxed), std::memory_order_relaxed); lose (n); return false; }
        // a drop happened before this block: mark where its silence goes (after everything accepted so far, before this block)
        if (const auto lost = pendingLost.load (std::memory_order_relaxed); lost > 0)
        {
            int m1, k1, m2, k2;
            markers.prepareToWrite (1, m1, k1, m2, k2);
            if (k1 > 0) { markAt[(size_t) m1] = accepted; markLost[(size_t) m1] = lost; markers.finishedWrite (1); }
            else tailLost.fetch_add (lost, std::memory_order_relaxed);     // (a very long run of drop / accept: the silence goes at the end instead)
            pendingLost.store (0, std::memory_order_relaxed);
        }
        for (int c = 0; c < numCh; ++c)
        {
            const float* src = chans[c];
            if (z1 > 0) juce::FloatVectorOperations::copy (ring[(size_t) c].data() + s1, src, z1);
            if (z2 > 0) juce::FloatVectorOperations::copy (ring[(size_t) c].data() + s2, src + z1, z2);
        }
        fifo.finishedWrite (n);
        accepted += n;
        if (const float fill = fillPercent(); fill > peakFill.load (std::memory_order_relaxed)) peakFill.store (fill, std::memory_order_relaxed);
        return true;
    }
    /** Audio thread: this block could not even be offered (nothing to write from): its time is kept as silence. */
    void skip (int n) noexcept { if (n > 0 && ! failedFlag.load (std::memory_order_relaxed)) lose (n); }

    // ---- any thread ----
    bool failed() const noexcept               { return failedFlag.load (std::memory_order_relaxed); }
    int  overruns() const noexcept             { return overrunCount.load (std::memory_order_relaxed); }
    juce::int64 samplesLost() const noexcept   { return lostTotal.load (std::memory_order_relaxed); }
    juce::int64 samplesWritten() const noexcept { return written.load (std::memory_order_relaxed); }
    int bufferSamples() const noexcept         { return fifo.getTotalSize() - 1; }
    /** The longest any single write to the disk took so far (ms): a drive that stalls shows here. */
    double longestWriteMs() const noexcept     { return maxWriteUs.load (std::memory_order_relaxed) / 1000.0; }
    /** The fullest the buffer has been (percent). */
    float peakFillPercent() const noexcept     { return peakFill.load (std::memory_order_relaxed); }
    /** Where in the take (frames accepted before it) the first overrun happened; -1 = none. */
    juce::int64 firstOverrunAt() const noexcept { return firstOverrun.load (std::memory_order_relaxed); }
    /** How full the buffer is, in percent (100 = the next block would not fit). */
    float fillPercent() const noexcept         { return 100.0f * (float) fifo.getNumReady() / (float) juce::jmax (1, fifo.getTotalSize() - 1); }
    /** Frames the buffer can take right now (audio thread). */
    int freeSpace() const noexcept             { return fifo.getFreeSpace(); }
    /** Frames the file's header said the last time it was written (0 until the first refresh). */
    juce::int64 samplesInHeader() const noexcept { return headerAt.load (std::memory_order_relaxed); }
    int numChannels() const noexcept           { return numCh; }

    /** The exact 24-bit word of a sample (full scale 1.0), in the top 24 bits of an int as juce::AudioFormatWriter::write wants it. */
    static int toInt24 (float x) noexcept
    {
        const float v = x * 8388608.0f;
        if (! (v > -1.0e9f && v < 1.0e9f)) return 0;                                   // (not a number: silence rather than anything lrint might do)
        const long r = std::lrintf (v);
        return (int) ((juce::uint32) (r > 8388607L ? 8388607L : r < -8388608L ? -8388608L : r) << 8);
    }

private:
    static constexpr int kChunk = 65536;                // the most frames in one disk write
    static constexpr int kMarkers = 256;

    void lose (int n) noexcept
    {
        pendingLost.fetch_add (n, std::memory_order_relaxed);
        lostTotal.fetch_add (n, std::memory_order_relaxed);
        overrunCount.fetch_add (1, std::memory_order_relaxed);
    }

    int useTimeSlice() override { return drain(); }

    /** Writes the silence owed (debt frames) in chunks; false when the disk failed. */
    bool payDebt (juce::int64 debt)
    {
        while (debt > 0 && ! failedFlag.load (std::memory_order_relaxed))
        {
            const int n = (int) juce::jmin<juce::int64> (debt, kChunk);
            for (int c = 0; c < numCh; ++c) ptrs[(size_t) c] = zeros.data();
            if (! put (n)) return false;
            debt -= n;
        }
        return ! failedFlag.load (std::memory_order_relaxed);
    }

    bool put (int n)
    {
        const auto t0 = juce::Time::getHighResolutionTicks();
        const bool ok = writer->write (ptrs.data(), n);
        const auto us = (juce::int64) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1.0e6);
        if (us > maxWriteUs.load (std::memory_order_relaxed)) maxWriteUs.store (us, std::memory_order_relaxed);
        if (! ok) { failedFlag.store (true, std::memory_order_relaxed); return false; }
        written.fetch_add (n, std::memory_order_relaxed);
        sinceHeader += n;
        if (headerEvery > 0 && sinceHeader >= headerEvery) { sinceHeader = 0; writer->flush(); headerAt.store (written.load (std::memory_order_relaxed), std::memory_order_release); }
        return true;
    }

    /** One step: silence that is due here, then up to kChunk frames of audio. 0 = did something (call again at once), 10 = nothing to do (ms). */
    int drain()
    {
        if (failedFlag.load (std::memory_order_relaxed))
        {   // the disk is gone: keep the buffer empty so the audio thread is not slowed, but write nothing
            int s1, z1, s2, z2; fifo.prepareToRead (fifo.getNumReady(), s1, z1, s2, z2); fifo.finishedRead (z1 + z2);
            return 10;
        }
        // silence owed at this point of the file?
        int m1, k1, m2, k2;
        markers.prepareToRead (1, m1, k1, m2, k2);
        const bool haveMark = k1 > 0;
        if (haveMark && markAt[(size_t) m1] == drained)
        {
            const auto lost = markLost[(size_t) m1];
            markers.finishedRead (1);
            payDebt (lost);
            return 0;
        }
        if (! closing && ! haveMark && fifo.getNumReady() < minWrite) return 20;          // wait until a big piece can be written (see minWriteSamples)
        int want = kChunk;
        if (haveMark) want = (int) juce::jmin<juce::int64> (want, markAt[(size_t) m1] - drained);
        int s1, z1, s2, z2;
        fifo.prepareToRead (want, s1, z1, s2, z2);
        if (z1 <= 0) { if (closing) { const auto t = tailLost.exchange (0); if (t > 0) payDebt (t); } return 10; }
        convert (s1, z1, 0);
        if (z2 > 0) convert (s2, z2, z1);
        for (int c = 0; c < numCh; ++c) ptrs[(size_t) c] = ints[(size_t) c].data();
        const int n = z1 + z2;
        fifo.finishedRead (n);
        drained += n;
        put (n);
        return 0;
    }

    void convert (int start, int n, int offset) noexcept
    {
        for (int c = 0; c < numCh; ++c)
        {
            const float* src = ring[(size_t) c].data() + start;
            int* dst = ints[(size_t) c].data() + offset;
            for (int i = 0; i < n; ++i) dst[i] = toInt24 (src[i]);
        }
    }

    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::TimeSliceThread& thread;
    const int numCh;
    juce::AbstractFifo fifo;
    std::vector<std::vector<float>> ring;               // per channel (written by the audio thread, read by the disk thread, in the parts the fifo hands each)
    std::vector<std::vector<int>> ints;
    std::vector<int> zeros;
    std::vector<const int*> ptrs;
    const int headerEvery;
    const int minWrite;
    int sinceHeader = 0;                                // disk thread
    juce::int64 accepted = 0;                           // audio thread: frames put into the fifo so far
    juce::int64 drained = 0;                            // disk thread: frames taken out so far
    juce::AbstractFifo markers { kMarkers };            // where silence is owed: after 'markAt' accepted frames, 'markLost' of it
    juce::int64 markAt[kMarkers] {}, markLost[kMarkers] {};
    std::atomic<juce::int64> pendingLost { 0 }, tailLost { 0 }, lostTotal { 0 }, written { 0 }, headerAt { 0 };
    std::atomic<int> overrunCount { 0 };
    std::atomic<juce::int64> maxWriteUs { 0 }, firstOverrun { -1 };
    std::atomic<float> peakFill { 0.0f };
    std::atomic<bool> failedFlag { false }, closing { false };

    JUCE_DECLARE_NON_COPYABLE (TakeWriter)
};
} // namespace td
