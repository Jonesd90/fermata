#pragma once
#include <juce_core/juce_core.h>
#include "Ltc.h"
#include <cmath>
#include <optional>

namespace td::timefmt
{
/** The four ways a position on the timeline can be written. */
enum class Mode { time = 0, minSec = 1, timecode = 2, samples = 3 };
constexpr int kModes = 4;

inline const char* modeName (Mode m)
{
    switch (m) { case Mode::time: return "Hours : minutes : seconds . ms"; case Mode::minSec: return "Minutes : seconds . ms";
                 case Mode::timecode: return "Timecode (frames)"; case Mode::samples: return "Samples"; }
    return "";
}

/** How a position is written: the mode, and what the modes need. */
struct Format
{
    Mode   mode = Mode::time;
    double sampleRate = 48000.0;     // for Samples
    int    ltcCode = 1;              // for Timecode: the frame rate, as LtcGenerator::formatInfo numbers them (1 = 25 fps)
    double offsetSeconds = 0.0;      // for Timecode: the timecode at the start of the timeline (3600 = 01:00:00:00)

    double fps() const  { double f; int n; bool d; LtcGenerator::formatInfo (ltcCode, f, n, d); return f; }
    bool   drop() const { double f; int n; bool d; LtcGenerator::formatInfo (ltcCode, f, n, d); return d; }
    int    nominal() const { double f; int n; bool d; LtcGenerator::formatInfo (ltcCode, f, n, d); return n; }
};

/** What to draw: the text, and which stretch of it is padding zeros that can be drawn faintly (the hours and minutes of a clock that
    has only just started). Characters dimFrom ... dimTo - 1 are the faint ones. */
struct Text
{
    juce::String text;
    int dimFrom = 0, dimTo = 0;
};

/** 3.18.6: minutes and seconds as the windows write a time in their text (one helper for what five windows did each in their own way):
    decimals 0: "4:05" (whole seconds, rounded down; nothing below 0:00) - the comment flags, the Align window, the recording monitor;
    1 or 2: "4:05.3" / "4:05.27" - Find other versions, the consonants. */
inline juce::String minSecText (double seconds, int decimals = 0)
{
    if (decimals <= 0)
    {
        const auto t = (juce::int64) std::floor (juce::jmax (0.0, seconds));
        return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
    }
    const int m = (int) (seconds / 60.0); const double s = seconds - 60.0 * m;
    return juce::String (m) + ":" + (s < 10.0 ? "0" : "") + juce::String (s, decimals);
}

namespace detail
{
inline Text clockText (long long totalMs, bool negative, int minutesDigits)   // HH:MM:SS.mmm, or M:SS.mmm
{
    const long long ms = totalMs % 1000, s = (totalMs / 1000) % 60;
    Text t;
    t.text = negative ? "-" : "";
    const int signLen = negative ? 1 : 0;
    if (minutesDigits == 0)
    {
        const long long h = totalMs / 3600000, m = (totalMs / 60000) % 60;
        t.text << juce::String (h).paddedLeft ('0', 2) << ":" << juce::String (m).paddedLeft ('0', 2) << ":" << juce::String (s).paddedLeft ('0', 2)
               << "." << juce::String (ms).paddedLeft ('0', 3);
        // the faint part: the leading zeros of the hours and minutes, but never the last digit of the seconds
        int end = signLen;
        const int secondsUnits = signLen + t.text.substring (signLen).indexOfChar ('.') - 1;
        while (end < secondsUnits && (t.text[end] == '0' || t.text[end] == ':')) ++end;
        t.dimFrom = signLen; t.dimTo = end;
    }
    else
    {
        const long long m = totalMs / 60000;
        t.text << juce::String (m) << ":" << juce::String (s).paddedLeft ('0', 2) << "." << juce::String (ms).paddedLeft ('0', 3);
    }
    return t;
}

/** 1234567 -> "1 234 567". */
inline juce::String grouped (long long n)
{
    const auto magnitude = n < 0 ? 0ull - (unsigned long long) n : (unsigned long long) n;     // (-n of the lowest number overflows: 3.0.0 fuzz test)
    auto digits = juce::String ((juce::uint64) magnitude);
    juce::String out;
    for (int i = 0; i < digits.length(); ++i)
    {
        if (i > 0 && (digits.length() - i) % 3 == 0) out << " ";
        out << digits[i];
    }
    return (n < 0 ? "-" : "") + out;
}

/** The numbers in "1:02:03.5" and the like (the last may have a decimal point). Empty when anything is not a number. */
inline bool numbers (const juce::String& s, juce::Array<double>& out, bool& negative)
{
    out.clear();
    auto t = s.trim();
    negative = t.startsWithChar ('-');
    if (negative) t = t.substring (1).trim();
    if (t.isEmpty()) return false;
    for (auto& part : juce::StringArray::fromTokens (t, ":;", ""))
    {
        auto p = part.trim().replaceCharacter (',', '.');
        if (p.isEmpty() || ! p.containsOnly ("0123456789.") || p.indexOfChar ('.') != p.lastIndexOfChar ('.')) return false;
        out.add (p.getDoubleValue());
    }
    return out.size() <= 4;
}
} // namespace detail

/** The position as the person reads it. Seconds before the start show a minus sign (except in Timecode, which has none). */
inline Text format (const Format& f, double seconds)
{
    if (! std::isfinite (seconds)) seconds = 0.0;
    const bool neg = seconds < 0.0;
    switch (f.mode)
    {
        case Mode::time:
        case Mode::minSec:
        {
            const long long ms = (long long) std::floor (std::fabs (seconds) * 1000.0 + 1.0e-6);       // a clock shows the second that has begun, so it is cut, not rounded
            return detail::clockText (ms, neg && ms > 0, f.mode == Mode::minSec ? 1 : 0);
        }
        case Mode::timecode:
        {
            Text t;
            t.text = timecodeText (seconds + f.offsetSeconds, f.fps(), f.drop());
            int end = 0;
            while (end < 7 && (t.text[end] == '0' || t.text[end] == ':')) ++end;     // faint: leading zeros, but never the last digit of the seconds
            t.dimFrom = 0; t.dimTo = end;
            return t;
        }
        case Mode::samples:
        {
            Text t;
            t.text = detail::grouped ((long long) std::llround (seconds * f.sampleRate));
            return t;
        }
    }
    return {};
}

/** The width a readout is made for. Longer texts exist (a piece of more than 99 minutes, a ten digit sample count): the readout shrinks its digits for those. */
inline juce::String widestText (const Format& f)
{
    switch (f.mode)
    {
        case Mode::time:     return "00:00:00.000";
        case Mode::minSec:   return "00:00.000";
        case Mode::timecode: return "00:00:00:00";
        case Mode::samples:  return "000 000 000";
    }
    return {};
}

/** The text in the readout's box -> seconds, or nothing when it can't be read. Accepted:
    - 90, 90.5, 90s                      seconds
    - 1:30, 1:02:03, 1:02:03.250         minutes:seconds or hours:minutes:seconds (a decimal on the seconds)
    - 1h2m3.5s, 2m10s                    hours, minutes and seconds with their letters
    - in Timecode mode, 01:02:03:04      hours:minutes:seconds:frames (taken at the project's frame rate, less the timeline's start timecode)
    - in Samples mode, 1234567           samples
    The result is not limited (the caller keeps it on the timeline). */
inline std::optional<double> parse (const Format& f, juce::String text)
{
    text = text.trim();
    if (text.isEmpty()) return std::nullopt;

    if (f.mode == Mode::samples)
    {
        auto digits = text.removeCharacters (" ,_");
        if (digits.endsWithIgnoreCase ("smp")) digits = digits.dropLastCharacters (3).trim();
        else if (digits.endsWithIgnoreCase ("samples")) digits = digits.dropLastCharacters (7).trim();
        if (digits.isNotEmpty() && digits.containsOnly ("0123456789") && f.sampleRate > 0.0) return (double) digits.getLargeIntValue() / f.sampleRate;
    }

    // hours, minutes, seconds with their letters
    if (text.containsAnyOf ("hHmMsS") && ! text.containsAnyOf (":;"))
    {
        auto t = text.toLowerCase().removeCharacters (" ");
        double total = 0.0; bool any = false; juce::String num;
        for (int i = 0; i < t.length(); ++i)
        {
            const auto c = t[i];
            if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == ',') { num += juce::String::charToString (c == ',' ? (juce::juce_wchar) '.' : c); continue; }
            if (num.isEmpty() || num.indexOfChar ('.') != num.lastIndexOfChar ('.')) return std::nullopt;
            const double v = num.getDoubleValue(); num.clear();
            if (c == 'h') total += v * 3600.0; else if (c == 'm') total += v * 60.0; else if (c == 's') total += v; else return std::nullopt;
            any = true;
        }
        if (num.isNotEmpty()) return std::nullopt;               // numbers left over with no letter
        return any ? std::optional<double> (total) : std::nullopt;
    }

    juce::Array<double> n; bool neg = false;
    if (! detail::numbers (text, n, neg)) return std::nullopt;

    // timecode: four whole numbers, the last one frames
    if (f.mode == Mode::timecode && n.size() == 4 && text.indexOfChar ('.') < 0)
    {
        const double fps = f.fps(); const int nominal = f.nominal();
        if (n[0] >= 100000.0 || n[1] >= 60.0 || n[2] >= 60.0 || n[3] >= (double) nominal) return std::nullopt;      // (checked before they become whole numbers: a huge hours figure overflowed the frame count - 3.0.0 fuzz test)
        const long long h = (long long) n[0], m = (long long) n[1], s = (long long) n[2], fr = (long long) n[3];
        if (m > 59 || s > 59 || fr >= nominal) return std::nullopt;
        long long frames = ((h * 60 + m) * 60 + s) * nominal + fr;
        if (f.drop())      // two frame numbers are skipped every minute except every tenth minute
        {
            const long long totalMinutes = h * 60 + m;
            frames -= 2 * (totalMinutes - totalMinutes / 10);
        }
        return (double) frames / fps - f.offsetSeconds;
    }

    double seconds = 0.0;
    switch (n.size())
    {
        case 1: seconds = n[0]; break;
        case 2: seconds = n[0] * 60.0 + n[1]; break;
        case 3: seconds = n[0] * 3600.0 + n[1] * 60.0 + n[2]; break;
        default: return std::nullopt;
    }
    return neg ? -seconds : seconds;
}
} // namespace td::timefmt
