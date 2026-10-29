#pragma once
#include <juce_core/juce_core.h>
#include "CrashReport.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "Common.h"
#include "CoreReservation.h"
#include "WavRepair.h"

namespace td
{
/** The second recording drive. While a take is being recorded, each of its files is copied to a second folder (normally on another drive),
    a moment behind the recording: this thread reads back what has just been written to the main drive (it is still in the computer's memory, so the
    main drive is hardly touched) and writes it to the second one.

    The main drive always comes first:
      - the audio thread and the main drive's writer know nothing about it (no extra work, no extra delay, no waiting at Stop or Record);
      - it runs at Windows' background priority, for the processor and for the disk;
      - if the second drive cannot keep up (more than maxLagSeconds behind), or cannot be written at all, that file's copy is simply thrown away
        (the take is then only on the main drive) and the next take tries again. Nothing ever waits for the second drive.
    When a take stops, the last moment and the finished header (the length) are copied in the background as well. */
class BackupCopier : private juce::Thread
{
public:
    struct Job
    {
        juce::File src, dst;
        double bytesPerSecond = 0.0;                // how fast the recording grows
        juce::uint32 startedMs = 0;
        std::atomic<bool> srcFinished { false };    // the main file is complete (its header holds the length)
        std::atomic<bool> cancelled { false };      // the take never started (another track's file could not be made): the copy goes, quietly (3.1.3)
        // copier thread only:
        juce::int64 copied = 0;
        bool opened = false, done = false;
        std::unique_ptr<juce::FileInputStream> in;
        std::unique_ptr<juce::FileOutputStream> out;
    };

    BackupCopier() : juce::Thread ("Second drive copier") { startThread (juce::Thread::Priority::low); }
    ~BackupCopier() override { stopThreadOrLog (*this, 20000); }     // (it looks at its exit flag between 4 MB steps; the long wait is for a second drive that is slow to answer: 3.1.2, was 4 s)

    /** Message thread: starts copying 'src' (a file that is being recorded) to 'dst'. Cheap: the folders and the file are made by the copier thread. */
    std::shared_ptr<Job> add (const juce::File& src, const juce::File& dst, double bytesPerSecond)
    {
        auto j = std::make_shared<Job>();
        j->src = src; j->dst = dst; j->bytesPerSecond = bytesPerSecond; j->startedMs = juce::Time::getMillisecondCounter();
        { const std::lock_guard<std::mutex> l (lock); jobs.push_back (j); }
        notify();
        return j;
    }
    /** Message thread: the main file has been closed: copy what is left, and its finished header. */
    static void finish (const std::shared_ptr<Job>& j) { if (j != nullptr) j->srcFinished.store (true); }
    /** Message thread: the recording did not start after all (see AudioEngine::startRecording): the copy is removed without a word. */
    static void cancel (const std::shared_ptr<Job>& j) { if (j != nullptr) j->cancelled.store (true); }

    /** Message thread: the files whose copy was thrown away since the last call (and why). */
    juce::StringArray takeBinned() { const std::lock_guard<std::mutex> l (lock); auto r = binned; binned.clear(); return r; }
    /** How many copies are still going. */
    int busy() const { const std::lock_guard<std::mutex> l (lock); return (int) jobs.size(); }
    /** How far behind the recording the slowest copy is right now, in seconds of audio (0 when every copy has caught up, or none is going). For
        the recording monitor (3.2.0). */
    double worstLagSeconds() const noexcept { return worstLag.load (std::memory_order_relaxed); }

    std::atomic<double> maxLagSeconds { 20.0 };

private:
    void run() override
    {
        td::crashreport::nameThisThread (getThreadName());                // (3.17.0: named for the log)
        CoreReservation::makeThisThreadBackground();
        std::vector<char> buffer ((size_t) kChunk);
        while (! threadShouldExit())
        {
            std::vector<std::shared_ptr<Job>> list;
            { const std::lock_guard<std::mutex> l (lock); list = jobs; }
            bool worked = false;
            double lag = 0.0;
            for (auto& j : list)
            {
                if (threadShouldExit()) break;
                if (! j->done) worked = service (*j, buffer) || worked;
                if (! j->done && j->opened && ! j->srcFinished.load() && j->in != nullptr && j->bytesPerSecond > 0.0)
                    lag = juce::jmax (lag, (double) (j->in->getTotalLength() - j->copied) / j->bytesPerSecond);
            }
            worstLag.store (lag, std::memory_order_relaxed);
            {
                const std::lock_guard<std::mutex> l (lock);
                jobs.erase (std::remove_if (jobs.begin(), jobs.end(), [] (const std::shared_ptr<Job>& x) { return x->done; }), jobs.end());
            }
            wait (worked ? 1 : 40);
        }
        for (auto& j : jobs) { j->out.reset(); j->in.reset(); }      // (closing: whatever was copied stays)
    }

    void bin (Job& j, const juce::String& why)
    {
        j.out.reset(); j.in.reset();
        j.dst.deleteFile();
        j.done = true;
        if (j.cancelled.load()) return;                         // (a take that never started: its main file is going too, and nobody needs telling)
        const std::lock_guard<std::mutex> l (lock);
        binned.add (j.src.getFileName() + ": " + why + "; the main recording is not affected");
    }
    /** The copy stays as it is (the main file could not be read back at the end): it is given a header from its own size and the notice says so. */
    void keepAsItIs (Job& j)
    {
        j.out.reset(); j.in.reset();
        const auto r = wavrepair::inspect (j.dst, true, true);
        j.done = true;
        const double secs = r.ok && r.sampleRate > 0 ? (double) r.frames / r.sampleRate : 0.0;
        const std::lock_guard<std::mutex> l (lock);
        binned.add (j.src.getFileName() + ": THE MAIN RECORDING COULD NOT BE READ BACK when it finished (has the drive gone?); the second drive's copy was kept as it is, "
                    + juce::String ((int) (secs / 60.0)) + " min " + juce::String ((int) std::fmod (secs, 60.0)).paddedLeft ('0', 2) + " s of audio");
    }

    /** One step of one copy (at most kChunk bytes, so the files of a take are copied side by side). True if anything was copied. */
    bool service (Job& j, std::vector<char>& buffer)
    {
        if (j.cancelled.load()) { j.out.reset(); j.in.reset(); j.dst.deleteFile(); j.done = true; return false; }
        if (! j.opened)
        {
            if (! j.dst.getParentDirectory().createDirectory()) { bin (j, "the folder on the second drive could not be made"); return false; }
            j.dst.deleteFile();
            j.out = std::make_unique<juce::FileOutputStream> (j.dst);
            if (j.out->failedToOpen()) { bin (j, "the second drive could not be written"); return false; }
            j.in = std::make_unique<juce::FileInputStream> (j.src);
            if (j.in->failedToOpen()) { bin (j, "the recording could not be read back"); return false; }
            j.opened = true;
        }
        const bool finished = j.srcFinished.load();            // (looked at first: if it is finished, everything is in the file by now)
        j.in->setPosition (j.copied);
        const int got = j.in->read (buffer.data(), (int) buffer.size());
        if (got > 0)
        {
            if (! j.out->write (buffer.data(), (size_t) got)) { bin (j, "the second drive stopped taking data (full or removed?)"); return false; }
            j.copied += got;
        }
        if (! finished)
        {
            // how far behind the recording the copy is: what the main file holds, less what has been copied. (3.1.3. It used to be judged by the time
            // elapsed since the take began, which binned the copy of a main file that had STOPPED growing - a drive that failed - the one time the
            // copy is needed. A copy that has everything the main file has is not behind.)
            const double behind = (double) (j.in->getTotalLength() - j.copied);
            if (behind > maxLagSeconds.load() * j.bytesPerSecond) { bin (j, "the second drive could not keep up"); return false; }
            return got > 0;
        }
        if (got > 0) return true;                               // still catching up with the end
        // the main file is complete and all of it is copied: its header was written last (with the length), so copy the start again
        const auto headLen = juce::jmin ((juce::int64) 65536, j.copied);
        j.in->setPosition (0);
        if (j.in->read (buffer.data(), (int) headLen) == (int) headLen && j.out->setPosition (0) && j.out->write (buffer.data(), (size_t) headLen))
        {
            j.out->flush();
            const bool ok = j.out->getStatus().wasOk();
            j.out.reset(); j.in.reset();
            if (! ok) { bin (j, "the second drive could not be written"); return false; }
            j.done = true;
            return true;
        }
        // The main file cannot be read back (its drive has gone?): the copy may be all there is. It is kept as it stands, with a header made from
        // its own size, rather than thrown away (3.1.3: it was binned as "the finished header could not be copied").
        keepAsItIs (j);
        return false;
    }

    static constexpr int kChunk = 4 << 20;                      // 4 MB per step
    mutable std::mutex lock;
    std::vector<std::shared_ptr<Job>> jobs;
    std::atomic<double> worstLag { 0.0 };
    juce::StringArray binned;
};
} // namespace td
