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
    core is reserved), the 12-core / 24-thread studio PC with 6 cores reserved gets 11. */
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

/** Runs fn (i) for every i in 0 .. items - 1 on up to threadsFor (items) threads (the calling thread is one of them) and returns when all are done.
    fn returns an error text ("" = fine): the first error is returned, and once one has failed, or cancelled() says so, no further item is started
    (the ones under way finish). fn must be safe to run alongside itself on other items: everything it touches is its own item's, or read-only, or locked. */
template <typename Fn, typename Cancelled>
inline juce::String forEach (int items, Fn fn, Cancelled cancelled, int threads = -1)
{
    if (items <= 0) return {};
    const int nT = threads > 0 ? std::min (threads, items) : threadsFor (items);
    std::atomic<int> next { 0 }; std::atomic<bool> stop { false };
    std::mutex errLock; juce::String firstError;
    auto worker = [&]
    {
        for (;;)
        {
            if (stop.load() || cancelled()) break;
            const int i = next.fetch_add (1);
            if (i >= items) break;
            const juce::String err = fn (i);
            if (err.isNotEmpty()) { std::lock_guard<std::mutex> g (errLock); if (firstError.isEmpty()) firstError = err; stop = true; }
        }
    };
    std::vector<std::thread> pool;
    for (int k = 1; k < nT; ++k) pool.emplace_back (worker);
    worker();
    for (auto& th : pool) th.join();
    if (firstError.isEmpty() && cancelled()) return "Cancelled.";
    return firstError;
}
} // namespace td::workers
