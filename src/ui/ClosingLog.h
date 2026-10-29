#pragma once
#include <juce_core/juce_core.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "../core/CrashReport.h"

namespace td
{
/** The closing log (3.15.0): when Fermata closes, each step goes into the log with how long it took ("Closing: project saved (412 ms)"), and the
    last line says it finished ("Closing: finished cleanly in 2.3 s"). The window is gone long before the last steps (the background threads, the
    plug-ins, the network connections), so this shows whether Fermata.exe really ended, and if not, which step it was still in.
    Message thread only. Does nothing until begin() is called. */
namespace closing
{
    struct State { bool active = false; double startMs = 0.0, lastMs = 0.0; };
    inline State& state() { static State s; return s; }

    inline void begin()
    {
        auto& s = state();
        s.active = true;
        s.startMs = s.lastMs = juce::Time::getMillisecondCounterHiRes();
        juce::Logger::writeToLog ("Closing: started");
    }
    /** One step finished: logged with the time since the step before. */
    inline void step (const char* what)
    {
        auto& s = state();
        if (! s.active) return;
        const double now = juce::Time::getMillisecondCounterHiRes();
        juce::Logger::writeToLog ("Closing: " + juce::String (what) + " (" + juce::String (juce::roundToInt (now - s.lastMs)) + " ms)");
        s.lastMs = now;
    }
    /** The last line (call before the log file is closed). */
    inline void end()
    {
        auto& s = state();
        if (! s.active) return;
        juce::Logger::writeToLog ("Closing: finished cleanly in " + juce::String ((juce::Time::getMillisecondCounterHiRes() - s.startMs) / 1000.0, 1) + " s");
        s.active = false;
    }
    /** A marker between an object's members: as members are destroyed in reverse order, a Mark declared just before member X logs its step right
        after X has been destroyed (so the time is X's). */
    struct Mark
    {
        const char* what;
        ~Mark() { step (what); }
    };
}
/** The closing safety net (3.17.0). Started once the project has been saved and the audio device closed: if closing has not finished 15 seconds
    later, where every part of Fermata is goes into the log and the program ends itself; and if it has finished but the program is somehow still
    running 20 seconds after that, the same. A closed Fermata can then never stay behind reading files and holding memory (one was found holding
    22 GB, with the disk busy, long after its window had gone - and Windows would not let its file be replaced). Nothing unsaved is lost: it only
    starts after the save. Its thread is let go of (never joined) and the object is never deleted, so it can watch to the very end. */
class ClosingWatchdog
{
public:
    static void start (unsigned long mainThreadId, const juce::File& logFile)
    {
        auto* w = new ClosingWatchdog();                              // (never deleted: it may still be watching while the program's last objects go)
        instance() = w;
        std::thread ([w, mainThreadId, logFile]
        {
            td::crashreport::nameThisThreadW (L"Closing safety net");
            std::unique_lock<std::mutex> lk (w->m);
            if (! w->cv.wait_for (lk, std::chrono::seconds (15), [w] { return w->done; }))
                w->endNow (mainThreadId, logFile, "Closing: not finished 15 s after the project was saved and the audio device closed - Fermata ends itself now (nothing unsaved is lost)");
            w->cv.wait_for (lk, std::chrono::seconds (20), [] { return false; });     // (the program normally ends long before this)
            w->endNow (mainThreadId, logFile, "Closing: Fermata was still running 20 s after closing had finished - it ends itself now");
        }).detach();
    }
    /** Closing has finished (call before the log file is closed). */
    static void finished()
    {
        if (auto* w = instance()) { { std::lock_guard<std::mutex> g (w->m); w->done = true; } w->cv.notify_all(); }
    }
private:
    static ClosingWatchdog*& instance() { static ClosingWatchdog* p = nullptr; return p; }
    void endNow (unsigned long mainThreadId, const juce::File& logFile, const char* why)
    {
        logFile.appendText (juce::String ("\n") + why + ". Where every part of it was (innermost first):\n" + td::crashreport::describeThreads (mainThreadId, 48, 24) + "\n");
        td::crashreport::endThisProcess();
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

/** The same for any other sequence of steps (3.17.0: opening a project): each step() logs "<prefix>: <what> (N ms)" - the time since the step before -
    so when the program gets stuck, the last line in the log names the step it finished last. Message thread. */
struct StepTimer
{
    explicit StepTimer (juce::String p) : prefix (std::move (p)), startMs (juce::Time::getMillisecondCounterHiRes()), lastMs (startMs)
    {
        juce::Logger::writeToLog (prefix + ": started");
    }
    void step (const juce::String& what)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        juce::Logger::writeToLog (prefix + ": " + what + " (" + juce::String (juce::roundToInt (now - lastMs)) + " ms)");
        lastMs = now;
    }
    void end (const juce::String& what = "finished")
    {
        juce::Logger::writeToLog (prefix + ": " + what + " in " + juce::String ((juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0, 1) + " s");
    }
    juce::String prefix;
    double startMs, lastMs;
};
} // namespace td
