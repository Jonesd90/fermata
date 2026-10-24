#pragma once
#include "Project.h"
#include <deque>
#include <functional>

namespace td
{
/** Writes prepared saves (Project::SaveJob, made by Project::prepareSave on the message thread) on a thread of its own, in the order they were
    handed over (2.102). Making the text of a big project and asking the drive to take it (the flush) easily took a few hundred milliseconds on a
    busy laptop, and while it did nothing else happened: the screen, the Space bar and the Stop button all waited for it.

    Each job's 'done' callback runs on the message thread, from drain(): the saver asks for that through 'wake' (the program posts a message
    that calls drain), and anything that is about to save again or touch the project's files calls waitUntilIdle and then drain() itself, so a
    save that failed is always known about (Project::saveFailed) before the next one is prepared. */
class BackgroundSaver : private juce::Thread
{
public:
    struct Result { std::unique_ptr<Project::SaveJob> job; bool ok = false; bool windowsFailed = false; juce::String error; };
    using Done = std::function<void (Result&)>;

    /** 'wake' is called on the saver's thread after each job: it should arrange for drain() to be called on the message thread soon. */
    explicit BackgroundSaver (std::function<void()> wake = {}) : juce::Thread ("Fermata saver"), wakeUp (std::move (wake)) {}
    ~BackgroundSaver() override
    {
        waitUntilIdle (-1);                                  // (a save that was handed over is always finished: closing never loses one)
        signalThreadShouldExit(); notify(); stopThread (10000);
    }

    /** Message thread: queues a prepared save. Cheap. */
    void write (std::unique_ptr<Project::SaveJob> job, Done done = {})
    {
        if (job == nullptr) return;
        {
            const juce::ScopedLock sl (lock);
            queue.push_back ({ std::move (job), std::move (done) });
            ++pending;
        }
        if (! isThreadRunning()) startThread (juce::Thread::Priority::low);
        notify();
    }

    /** True while anything handed over has not been written yet. */
    bool isBusy() const noexcept { return pending.load() > 0; }

    /** Waits until everything handed over has been written. ms < 0: as long as it takes. False if the time ran out. */
    bool waitUntilIdle (int ms)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + (ms < 0 ? 1.0e12 : (double) ms);
        while (pending.load() > 0)
        {
            if (juce::Time::getMillisecondCounterHiRes() >= until) return false;
            idle.wait (20);
        }
        return true;
    }

    /** Message thread: runs the callbacks of the jobs that have been written since the last call. Returns how many. */
    int drain()
    {
        std::deque<Finished> list;
        { const juce::ScopedLock sl (lock); list.swap (finished); }
        for (auto& f : list) if (f.done != nullptr) f.done (*f.result);
        return (int) list.size();
    }

    /** What the last job did (for the log and the tests). */
    double lastWriteMs() const noexcept { return lastMs.load(); }
    int written() const noexcept { return writtenCount.load(); }

private:
    struct Item { std::unique_ptr<Project::SaveJob> job; Done done; };
    struct Finished { std::shared_ptr<Result> result; Done done; };

    void run() override
    {
        for (;;)
        {
            Item item;
            {
                const juce::ScopedLock sl (lock);
                if (! queue.empty()) { item = std::move (queue.front()); queue.pop_front(); }
            }
            if (item.job == nullptr) { if (threadShouldExit()) return; wait (500); continue; }

            auto r = std::make_shared<Result>();
            r->job = std::move (item.job);
            try { r->ok = Project::writeSave (*r->job, r->error, &r->windowsFailed); }
            catch (...) { r->ok = false; r->error = "Something went wrong while the project was being written (out of memory?)"; }      // (never leaves the saver stuck)
            lastMs.store (r->job->writeMs);
            ++writtenCount;
            { const juce::ScopedLock sl (lock); finished.push_back ({ r, std::move (item.done) }); }
            --pending;
            idle.signal();
            if (wakeUp != nullptr) wakeUp();
        }
    }

    std::function<void()> wakeUp;
    juce::CriticalSection lock;
    std::deque<Item> queue;
    std::deque<Finished> finished;
    std::atomic<int> pending { 0 }, writtenCount { 0 };
    std::atomic<double> lastMs { 0.0 };
    juce::WaitableEvent idle;
};
} // namespace td
