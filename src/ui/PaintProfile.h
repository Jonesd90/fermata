#pragma once
#include <juce_core/juce_core.h>
#include <map>

namespace td::paintprof
{
/** A development aid: with the environment variable FERMATA_PAINT_PROFILE=1 the painting code adds up how long its named parts take and writes a
    line to the log (and to stdout) every 30 paints of each window, and at once for any single paint of 15 ms or more. Off by default; then a
    Scope costs one bool test. */
inline bool enabled()
{
    static const bool on = juce::SystemStats::getEnvironmentVariable ("FERMATA_PAINT_PROFILE", "0") == "1";
    return on;
}
struct Totals
{
    std::map<juce::String, double> ms, thisMs; std::map<juce::String, int> calls, thisCalls;
    int paints = 0; double paintMs = 0.0;
};
inline Totals& totals (const char* window) { static std::map<juce::String, Totals> all; return all[window]; }

struct Scope
{
    Scope (const char* window, const char* part) : w (window), p (part), t0 (enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0) {}
    ~Scope() { finish(); }
    /** Ends this part and starts the next one. */
    void next (const char* part) { finish(); p = part; if (enabled()) t0 = juce::Time::getMillisecondCounterHiRes(); }
    void finish()
    {
        if (! enabled() || p == nullptr) return;
        auto& t = totals (w); const auto now = juce::Time::getMillisecondCounterHiRes();
        t.ms[p] += now - t0; ++t.calls[p]; t.thisMs[p] += now - t0; ++t.thisCalls[p]; t0 = now;
    }
    const char* w; const char* p; double t0;
};

inline void write (const juce::String& line) { juce::Logger::writeToLog (line); std::printf ("%s\n", line.toRawUTF8()); std::fflush (stdout); }

/** Call at the end of a window's paint. */
inline void paintDone (const char* window, double wholeMs, int clipW = 0, int clipH = 0)
{
    if (! enabled()) return;
    auto& t = totals (window);
    t.paintMs += wholeMs; ++t.paints;
    if (wholeMs >= 8.0 || juce::String (window).contains ("change"))
    {
        juce::String line = juce::String (window) + " one paint of " + juce::String (clipW) + "x" + juce::String (clipH) + ": " + juce::String (wholeMs, 1) + " ms;";
        for (auto& kv : t.thisMs) line << " " << kv.first << " " << juce::String (kv.second, 1) << " ms (" << juce::String (t.thisCalls[kv.first]) << " calls)";
        write (line);
    }
    t.thisMs.clear(); t.thisCalls.clear();
    if (t.paints < 30) return;
    juce::String line = juce::String (window) + " paint: " + juce::String (t.paintMs / t.paints, 1) + " ms average over " + juce::String (t.paints) + ";";
    for (auto& kv : t.ms) line << " " << kv.first << " " << juce::String (kv.second / t.paints, 1) << " ms (" << juce::String (t.calls[kv.first] / t.paints) << " calls)";
    write (line);
    t = Totals();
}
} // namespace td::paintprof
