#pragma once
#include <juce_events/juce_events.h>
#include <atomic>

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
    in the log afterwards, with its cause. At most 30 lines a minute. */
class StallWatch : private juce::Timer, private juce::Thread
{
public:
    StallWatch() : juce::Thread ("Fermata stall watch")
    {
        beat.store (juce::Time::getMillisecondCounterHiRes());
        startTimerHz (20);
        startThread (juce::Thread::Priority::low);
    }
    ~StallWatch() override
    {
        stopTimer();
        signalThreadShouldExit(); notify(); stopThread (2000);
    }

    static constexpr double kPeriodMs = 50.0, kReportMs = 150.0;

private:
    void timerCallback() override { beat.store (juce::Time::getMillisecondCounterHiRes()); }

    void run() override
    {
        bool inStall = false;
        double lastBeatBefore = 0.0;
        const char* seen[4] = {};
        int nSeen = 0, linesThisMinute = 0, suppressed = 0;
        double minuteStart = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            wait (20);
            const double t = juce::Time::getMillisecondCounterHiRes();
            const double b = beat.load();
            if (t - b > kPeriodMs + kReportMs * 0.5)                 // the beats have stopped: the message thread is busy
            {
                if (! inStall) { inStall = true; lastBeatBefore = b; nSeen = 0; }
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
            if (suppressed > 0) { line << "; " << suppressed << " more in the minute before were not logged"; suppressed = 0; }
            juce::Logger::writeToLog (line);
        }
    }

    std::atomic<double> beat { 0.0 };
    std::atomic<int> count { 0 };
    std::atomic<double> longest { 0.0 };
};
} // namespace td
