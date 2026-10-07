#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

namespace td::sessionclock
{
/** "17:30", "7:05", "1730" -> hours and minutes. Hours up to 23 for a time of day, up to 99 for a length of time. */
inline bool parseHHMM (const juce::String& text, int& h, int& m, bool lengthOfTime)
{
    auto allDigits = [] (const juce::String& s) { return s.isNotEmpty() && s.containsOnly ("0123456789"); };
    const auto t = text.trim();
    if (t.containsChar (':'))
    {
        auto parts = juce::StringArray::fromTokens (t, ":", "");
        if (parts.size() != 2) return false;
        parts.trim();
        if (! allDigits (parts[0]) || ! allDigits (parts[1]) || parts[0].length() > 2 || parts[1].length() > 2) return false;
        h = parts[0].getIntValue(); m = parts[1].getIntValue();
    }
    else if (t.length() == 4 && allDigits (t)) { h = t.substring (0, 2).getIntValue(); m = t.substring (2).getIntValue(); }
    else return false;
    return m <= 59 && h <= (lengthOfTime ? 99 : 23);
}

/** The next time the clock shows h:m (local time), as milliseconds since 1970. A time that passed less than 12 hours ago counts as today
    (the session is then over time); anything older means tomorrow. */
inline juce::int64 endAtMs (juce::int64 nowMs, int h, int m)
{
    const juce::Time now (nowMs);
    juce::Time target (now.getYear(), now.getMonth(), now.getDayOfMonth(), h, m, 0, 0, true);
    if (nowMs - target.toMilliseconds() > 12LL * 3600 * 1000)
        target = juce::Time (now.getYear(), now.getMonth(), now.getDayOfMonth() + 1, h, m, 0, 0, true);
    return target.toMilliseconds();
}

/** Whole seconds left (counts down to 0, then goes negative: -1, -2 ...). */
inline juce::int64 remainingSeconds (juce::int64 endMs, juce::int64 nowMs) { return (juce::int64) std::ceil ((double) (endMs - nowMs) / 1000.0); }

/** 01:02:03, or -00:04:53 when negative. */
inline juce::String formatClock (juce::int64 seconds)
{
    const auto a = seconds < 0 ? -seconds : seconds;
    return juce::String (seconds < 0 ? "-" : "") + juce::String::formatted ("%02d:%02d:%02d", (int) (a / 3600), (int) ((a / 60) % 60), (int) (a % 60));
}

/** The time of day, 24 hour, HH:MM:SS. */
inline juce::String timeOfDay (juce::int64 nowMs) { return juce::Time (nowMs).formatted ("%H:%M:%S"); }
} // namespace td::sessionclock
