#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <thread>
#include "CoreReservation.h"
#if JUCE_INTEL
 #include <immintrin.h>
#endif

namespace td
{
/** A few helper threads that let the mixers of one audio block run side by side (the processing mixer, the cue mixers, an edit's mixer),
    instead of one after another on the driver's single audio thread. The audio thread hands out the jobs and does some of them itself, so a helper
    that wakes late never holds anything up: it simply finds nothing left to do. Nothing here allocates or locks on the audio thread.

    Each job is claimed through one atomic "ticket" that holds the block's number, how many jobs there are and the next one to take, so a helper can
    never take a job of a block that has already finished. The helpers live on the reserved CPU cores when there are any (see CoreReservation). */
class MixerWorkers
{
public:
    using JobFn = void (*) (void* ctx, int index);

    MixerWorkers() = default;
    ~MixerWorkers() { stop(); }

    /** Message thread: starts 'count' helpers (0 = everything runs on the audio thread, as before). */
    void start (int count)
    {
        stop();
        for (int i = 0; i < count; ++i)
        {
            auto* w = workers.add (new Worker (*this, i));
            w->startThread (juce::Thread::Priority::highest);
            CoreReservation::get().registerAudioThread (w->getThreadId());
        }
    }
    void stop()
    {
        for (auto* w : workers) { CoreReservation::get().unregisterAudioThread (w->getThreadId()); w->signalThreadShouldExit(); w->wake.signal(); }
        for (auto* w : workers) w->stopThread (2000);
        workers.clear();
    }
    int size() const noexcept { return workers.size(); }

    /** Any thread: at most this many helpers take part in a block (the rest sleep). With CPU cores reserved for the audio, it is the number of
        reserved logical processors less one (the audio thread itself), so no two audio threads ever share a processor; a helper pre-empted in the
        middle of a job would hold the whole block up. A big number = all of them. */
    void setLimit (int maxHelpers) noexcept { limit.store (juce::jmax (0, maxHelpers), std::memory_order_relaxed); }
    int  limit_() const noexcept { return limit.load (std::memory_order_relaxed); }

    /** Audio thread: runs fn (ctx, 0), fn (ctx, 1) ... fn (ctx, count - 1), using the helpers and this thread. Returns when every job has finished. */
    void run (int count, JobFn fn, void* ctx) noexcept
    {
        if (count <= 0) return;
        const int helpers = juce::jmin (workers.size(), count - 1, limit.load (std::memory_order_relaxed));
        if (helpers <= 0) { for (int i = 0; i < count; ++i) fn (ctx, i); return; }
        jobFn.store (fn, std::memory_order_relaxed);
        jobCtx.store (ctx, std::memory_order_relaxed);
        done.store (0, std::memory_order_relaxed);
        gen = (gen + 1) & 0xffffffu; if (gen == 0) gen = 1;
        ticket.store (makeTicket (gen, (juce::uint32) count, 0), std::memory_order_release);
        for (int i = 0; i < helpers; ++i) workers.getUnchecked (i)->wake.signal();
        int idx = 0;
        while (claim (gen, idx)) { fn (ctx, idx); done.fetch_add (1, std::memory_order_acq_rel); }
        while (done.load (std::memory_order_acquire) < count) pause();
        ticket.store (makeTicket (gen, 0, 0), std::memory_order_release);              // closed: nothing more to take from this block
    }

private:
    static juce::uint64 makeTicket (juce::uint32 g, juce::uint32 total, juce::uint32 next) noexcept
    {
        return ((juce::uint64) g << 40) | ((juce::uint64) (total & 0xfffffu) << 20) | (juce::uint64) (next & 0xfffffu);
    }
    bool claim (juce::uint32 g, int& idx) noexcept
    {
        auto t = ticket.load (std::memory_order_acquire);
        for (;;)
        {
            if ((juce::uint32) (t >> 40) != g) return false;
            const auto total = (juce::uint32) ((t >> 20) & 0xfffffu), next = (juce::uint32) (t & 0xfffffu);
            if (next >= total) return false;
            if (ticket.compare_exchange_weak (t, t + 1, std::memory_order_acq_rel, std::memory_order_acquire)) { idx = (int) next; return true; }
        }
    }
    static void pause() noexcept
    {
       #if JUCE_INTEL
        _mm_pause();
       #else
        std::this_thread::yield();
       #endif
    }

    struct Worker : public juce::Thread
    {
        Worker (MixerWorkers& o, int i) : juce::Thread ("Mixer helper " + juce::String (i + 1)), owner (o) {}
        void run() override
        {
            juce::FloatVectorOperations::disableDenormalisedNumberSupport();          // as on the audio thread: decaying reverb tails must not slow the CPU down
            while (! threadShouldExit())
            {
                wake.wait (-1);
                if (threadShouldExit()) break;
                CoreReservation::get().audioThreadCheck();                           // the reserved cores (when any) and Windows' "Pro Audio" class, as for the audio thread
                const auto g = (juce::uint32) (owner.ticket.load (std::memory_order_acquire) >> 40);
                int idx = 0;
                while (owner.claim (g, idx))
                {
                    owner.jobFn.load (std::memory_order_relaxed) (owner.jobCtx.load (std::memory_order_relaxed), idx);
                    owner.done.fetch_add (1, std::memory_order_acq_rel);
                }
            }
        }
        MixerWorkers& owner;
        juce::WaitableEvent wake;
    };

    juce::OwnedArray<Worker> workers;
    std::atomic<juce::uint64> ticket { 0 };
    std::atomic<int> done { 0 };
    std::atomic<JobFn> jobFn { nullptr };
    std::atomic<void*> jobCtx { nullptr };
    std::atomic<int> limit { 1 << 20 };
    juce::uint32 gen = 0;                       // audio thread only
};
} // namespace td
