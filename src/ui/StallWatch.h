#pragma once
#include <juce_events/juce_events.h>
#include <atomic>
#include "../core/CoreReservation.h"
#include "../core/CrashReport.h"

namespace td
{
/** What the message thread is busy with just now, in a few words (a string literal), for the stall watch's log lines. Set by BusyLabel. */
inline std::atomic<const char*>& messageThreadLabel() noexcept { static std::atomic<const char*> l { nullptr }; return l; }

/** Labels a stretch of message-thread work for the stall watch: "starting playback", "drawing the edit window" ... (nested: the inner one shows). */
struct BusyLabel
{
    explicit BusyLabel (const char* what) noexcept : previous (messageThreadLabel().exchange (what)) {}
    ~BusyLabel() { messageThreadLabel().store (previous); }
    const char* previous;
    JUCE_DECLARE_NON_COPYABLE (BusyLabel)
};

/** Watches the message thread (2.102): a timer on it beats 20 times a second, and a thread of its own notices when the beats stop. Whenever the
    message thread is held up for 150 ms or more - nothing on the screen moves, Space and Stop wait - a line goes into the log once it is free
    again: how long, at what time, and what it was doing (the labels seen meanwhile, see BusyLabel). So a stall the person notices can be found
    in the log afterwards, with its cause. At most 30 lines a minute.
    Since 3.13.0 each line also says how hard the main thread itself was working through the stall, how much of the processor Fermata and the whole
    PC were using, and whether a laptop was on battery - so a stall with no label can be told apart: Fermata's own drawing (main thread working),
    a PC that is flat out (something else, or too few cores left), or the main thread waiting on Windows or a driver.
    Since 3.14.0, when the main thread has been stuck for 15 seconds a freeze report is written (a minidump: where every thread is at that
    moment, see CrashReport.h) - at most two a session - so a freeze that needs Fermata ending from the Task Manager can still be traced. */
class StallWatch : private juce::Timer, private juce::Thread
{
public:
    StallWatch() : juce::Thread ("Fermata stall watch")
    {
        beat.store (juce::Time::getMillisecondCounterHiRes());
        SystemLoad::rememberMessageThread();                                // (constructed on the message thread)
        startTimerHz (20);
        startThread (juce::Thread::Priority::low);
    }
    ~StallWatch() override
    {
        stopTimer();
        signalThreadShouldExit(); notify(); stopThread (2000);
    }

    static constexpr double kPeriodMs = 50.0, kReportMs = 150.0, kFreezeReportMs = 15000.0;

private:
    void timerCallback() override { beat.store (juce::Time::getMillisecondCounterHiRes()); }

    void run() override
    {
        bool inStall = false;
        double lastBeatBefore = 0.0;
        const char* seen[4] = {};
        int nSeen = 0, linesThisMinute = 0, suppressed = 0;
        double minuteStart = juce::Time::getMillisecondCounterHiRes();
        SystemLoad::Snapshot atStart;
        double lastLoop = juce::Time::getMillisecondCounterHiRes(), watchedSince = 0.0;
        int freezeReports = 0;
        bool reportedThisStall = false;
        while (! threadShouldExit())
        {
            wait (20);
            const double t = juce::Time::getMillisecondCounterHiRes();
            const double b = beat.load();
            if (t - lastLoop > 2000.0) watchedSince = 0.0;                   // this thread did not run either (the PC was asleep): not a freeze
            lastLoop = t;
            if (t - b > kPeriodMs + kReportMs * 0.5)                 // the beats have stopped: the message thread is busy
            {
                if (! inStall) { inStall = true; lastBeatBefore = b; nSeen = 0; atStart = SystemLoad::take(); watchedSince = t; reportedThisStall = false; }
                if (watchedSince <= 0.0) watchedSince = t;
                if (! reportedThisStall && freezeReports < 2 && t - watchedSince >= kFreezeReportMs)
                {
                    reportedThisStall = true;
                    if (const auto f = crashreport::write (nullptr, "freeze"); f != juce::File())
                    {
                        ++freezeReports;
                        juce::Logger::writeToLog ("Stall watch: the main thread has been stuck for " + juce::String (juce::roundToInt ((t - watchedSince) / 1000.0)) + " s - a freeze report was written: " + f.getFullPathName());
                    }
                }
                if (const char* l = messageThreadLabel().load())
                {
                    bool have = false;
                    for (int i = 0; i < nSeen; ++i) have = have || seen[i] == l;
                    if (! have && nSeen < 4) seen[nSeen++] = l;
                }
                continue;
            }
            if (! inStall) continue;
            inStall = false;
            const double busy = (b - lastBeatBefore) - kPeriodMs;
            if (busy < kReportMs) continue;
            count.fetch_add (1);
            if (busy > longest.load()) longest.store (busy);
            if (t - minuteStart > 60000.0) { minuteStart = t; linesThisMinute = 0; }
            if (++linesThisMinute > 30) { ++suppressed; continue; }
            juce::String what;
            for (int i = 0; i < nSeen; ++i) what << (i > 0 ? ", " : "") << seen[i];
            juce::String line;
            if (busy > 30000.0)
                line << "Stall watch: the program did not respond for " << juce::String (busy / 1000.0, 0) << " s (the computer may have been asleep)";
            else
                line << "Stall watch: main thread busy for " << juce::roundToInt (busy) << " ms at " << juce::Time::getCurrentTime().formatted ("%H:%M:%S")
                     << " (" << (what.isNotEmpty() ? what : juce::String ("nothing labelled: drawing or Windows")) << ")";
            if (busy <= 30000.0)
                if (const auto load = SystemLoad::describe (atStart, SystemLoad::take(), true); load.isNotEmpty()) line << "; " << load;
            if (suppressed > 0) { line << "; " << suppressed << " more in the minute before were not logged"; suppressed = 0; }
            juce::Logger::writeToLog (line);
        }
    }

    std::atomic<double> beat { 0.0 };
    std::atomic<int> count { 0 };
    std::atomic<double> longest { 0.0 };
};
} // namespace td
