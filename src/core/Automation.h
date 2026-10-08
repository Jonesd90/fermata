#pragma once
#include "Common.h"

namespace td
{
/** Parameter names of an automation lane: "gain" (before the strip), "fader", "pan", "send:<destination id>" (a bus send / track send of the strip),
    "plugin:<insert slot>:<parameter number>" (an automatable parameter of the VST3 in that insert slot, value 0..1). */
namespace autoparam
{
inline const juce::String fader = "fader", pan = "pan", gain = "gain";      // "gain": the level going INTO the mixer strip (before the inserts and the fader)
inline juce::String send (const juce::Uuid& dest)            { return "send:" + dest.toString(); }
inline juce::String plugin (int slot, int index)             { return "plugin:" + juce::String (slot) + ":" + juce::String (index); }
inline bool isSend (const juce::String& p)                   { return p.startsWith ("send:"); }
inline bool isPlugin (const juce::String& p)                 { return p.startsWith ("plugin:"); }
inline juce::Uuid sendDest (const juce::String& p)           { return juce::Uuid (p.fromFirstOccurrenceOf ("send:", false, false)); }
/** Fills slot / index; false if 'p' is not a well-formed plug-in parameter name. */
inline bool parsePlugin (const juce::String& p, int& slot, int& index)
{
    if (! isPlugin (p)) return false;
    juce::StringArray a; a.addTokens (p, ":", "");
    if (a.size() != 3) return false;
    slot = a[1].getIntValue(); index = a[2].getIntValue();
    return slot >= 0 && index >= 0;
}
}

/** One automation point. It remembers WHICH AUDIO it sits on (a piece of the edit and a sample inside that piece's source file), not a time:
    the time on the edit's timeline is worked out from where that audio currently is, so the point stays on the same musical moment
    when the edit ripples, slides, is re-ordered or trimmed. 'time' is the worked-out position (and the last known one, used to guess
    where a point should go if its audio is removed). A point with no region sits at a fixed time. */
struct AutoPoint
{
    juce::Uuid   id;
    juce::Uuid   region = juce::Uuid::null();               // null = fixed at 'time'
    juce::Uuid   take = juce::Uuid::null();                 // the take that audio came from: if the piece was split or trimmed, the point follows the audio into the new piece
    juce::int64  srcPos = 0;           // sample inside the piece's files
    juce::int64  time = 0;             // timeline sample
    float        value = 0.0f;         // fader: dB. pan: -1 .. +1
    bool         floating = false;     // its audio was removed: the position is a guess, for the user to check
};

/** One automated parameter of one channel strip. */
struct AutoLane
{
    juce::Uuid   id;
    juce::Uuid   trackId = juce::Uuid::null();
    juce::String param { autoparam::fader };
    juce::Uuid   mixerId = juce::Uuid::null();              // null = the Edit's own mixer (the processing mixer if it has none)
    bool         locked = false;       // the padlock: a locked lane cannot be edited
    std::vector<AutoPoint> pts;        // sorted by 'time'

    static constexpr float kFaderOff = -100.0f, kFaderMax = 12.0f;
    bool isPlugin() const noexcept { return autoparam::isPlugin (param); }
    float defaultValue() const noexcept { return isPlugin() ? 0.5f : 0.0f; }
    float clampValue (float v) const noexcept
    {
        if (param == autoparam::pan) return juce::jlimit (-1.0f, 1.0f, v);
        if (isPlugin()) return juce::jlimit (0.0f, 1.0f, v);              // a plug-in parameter is 0..1
        return juce::jlimit (kFaderOff, kFaderMax, v);                    // fader and sends: dB
    }

    /** The value at timeline sample t: flat before the first point and after the last, a straight line between points. */
    float valueAt (juce::int64 t, float whenEmpty) const noexcept
    {
        if (pts.empty()) return whenEmpty;
        if (t <= pts.front().time) return pts.front().value;
        if (t >= pts.back().time)  return pts.back().value;
        size_t i = 1; while (i < pts.size() && pts[i].time <= t) ++i;
        const auto& a = pts[i - 1]; const auto& b = pts[i];
        if (b.time <= a.time) return b.value;
        return a.value + (b.value - a.value) * (float) (t - a.time) / (float) (b.time - a.time);
    }

    void sortPoints()
    {
        std::stable_sort (pts.begin(), pts.end(), [] (const AutoPoint& x, const AutoPoint& y) { return x.time < y.time; });
    }
};
} // namespace td
