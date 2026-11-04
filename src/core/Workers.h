#pragma once
#include "CoreReservation.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace td::workers
{
/** 3.30.0: how many threads a heavy offline job may use for 'items' independent pieces of work (the tracks of a ReTouch, the files of a write-back):
    the machine's logical processors, less those reserved for the audio engine, less one for the windows - never fewer than 1, never more than there are
    items, and no more than 16 (past that the memory bandwidth, not the cores, is the limit). So it scales itself: a four-core laptop gets 3 (2 when a
    core is reserved), the 12-core / 24-thread studio PC with 6 cores (12 logical processors) reserved gets 11. */
inline int threadsFor (int items, int hardwareThreads, int reservedForAudio)
{
    int n = hardwareThreads - std::max (0, reservedForAudio) - 1;
    n = std::max (1, std::min (n, 16));
    return std::max (1, std::min (std::max (0, items), n));
}
inline int threadsFor (int items)
{
    return threadsFor (items, (int) std::thread::hardware_concurrency(), CoreReservation::get().reservedLogicalProcessors());
}

/** 3.30.1: the threads for 'items' pieces of work of about 'bytesPerItem' each: as threadsFor, but never so many that their working memory together
    would pass half of the machine's (a fix over a long area on many tracks: the memory is multiplied by the threads, and running out used to be a
    message, never a crash - see forEach). 0 bytes: no memory limit. */
inline int threadsForMemory (int items, juce::int64 bytesPerItem)
{
    int n = threadsFor (items);
    if (bytesPerItem > 0)
    {
        const juce::int64 budget = (juce::int64) juce::SystemStats::getMemorySizeInMegabytes() * 1024 * 1024 / 2;
        if (budget > 0) n = std::max (1, std::min (n, (int) std::max<juce::int64> (1, budget / bytesPerItem)));
    }
    return n;
}

/** Runs fn (i) for every i in 0 .. items - 1 on up to threadsFor (items) threads (the calling thread is one of them) and returns when all are done.
    fn returns an error text ("" = fine): the first error is returned, and once one has failed, or cancelled() says so, no further item is started
    (the ones under way finish). fn must be safe to run alongside itself on other items: everything it touches is its own item's, or read-only, or locked.
    3.30.1: an exception inside fn (running out of memory, above all) is an error text like any other, never the end of the program - a std::thread
    that lets one escape ends the program, and so does unwinding past a joinable std::thread; the AudioJob's own catch used to cover this work. */
template <typename Fn, typename Cancelled>
inline juce::String forEach (int items, Fn fn, Cancelled cancelled, int threads = -1)
{
    if (items <= 0) return {};
    const int nT = threads > 0 ? std::min (threads, items) : threadsFor (items);
    std::atomic<int> next { 0 }; std::atomic<bool> stop { false };
    std::mutex errLock; juce::String firstError;
    auto fail = [&] (const juce::String& err) { std::lock_guard<std::mutex> g (errLock); if (firstError.isEmpty()) firstError = err; stop = true; };
    auto worker = [&]
    {
        for (;;)
        {
            if (stop.load() || cancelled()) break;
            const int i = next.fetch_add (1);
            if (i >= items) break;
            juce::String err;
            try { err = fn (i); }
            catch (const std::bad_alloc&) { err = "Not enough memory for this fix: try a shorter area, or fewer tracks at once."; }
            catch (...)                   { err = "Something went wrong while processing."; }
            if (err.isNotEmpty()) fail (err);
        }
    };
    std::vector<std::thread> pool;
    for (int k = 1; k < nT; ++k)
    {
        try { pool.emplace_back (worker); }
        catch (...) { break; }                                               // (a thread that cannot be made: the ones made, and this thread, do the work)
    }
    worker();
    for (auto& th : pool) th.join();
    if (firstError.isEmpty() && cancelled()) return "Cancelled.";
    return firstError;
}
} // namespace td::workers
