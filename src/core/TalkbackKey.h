#pragma once
#include <juce_core/juce_core.h>

namespace td
{
/** One talkback key (numeric keypad + or -). A short press (under a third of a second) latches the function on, and the next short press latches it off.
    A longer press keeps it on only while the key is held. Feed it the physical key state often; it answers whether the function is on. */
struct TalkbackKey
{
    static constexpr juce::uint32 kHoldMs = 333;
    bool down = false, latched = false, wasLatched = false;
    juce::uint32 downAt = 0;
    juce::uint32 holdMs = kHoldMs;          // a press shorter than this latches; a longer one is momentary

    bool update (bool keyIsDown, juce::uint32 nowMs)
    {
        if (keyIsDown && ! down)      { down = true; downAt = nowMs; wasLatched = latched; }
        else if (! keyIsDown && down)
        {
            down = false;
            latched = (nowMs - downAt < holdMs) ? ! wasLatched : false;     // a tap flips the latch; a long hold ends with it off
        }
        return latched || down;
    }
    bool isOn() const noexcept { return latched || down; }
};
} // namespace td
