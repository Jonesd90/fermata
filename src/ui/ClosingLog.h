#pragma once
#include <juce_core/juce_core.h>

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
} // namespace td
