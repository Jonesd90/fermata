#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace td
{
/** Makes 'child' a window OWNED by 'owner' (Windows): it always stays in front of the owner, can never disappear behind it, is minimised and
    restored together with it, and has no taskbar button of its own. It is how Fermata's windows behave as parts of the main window.
    Returns false where this is not available (the caller then raises the windows by hand). Safe to call again and again. */
bool makeOwnedBy (juce::Component& child, juce::Component& owner);
/** Puts a window in the topmost band of the desktop again (in front of every program, without taking the keyboard). Does nothing where unavailable. */
void keepTopmost (juce::Component&);

/** Keeps a window (the meter bridge, the big take clock) in front of everything on the desktop: it re-asserts the topmost state every second or so,
    because Windows drops it when a window is minimised / restored or when another topmost window is raised. */
class StayOnTop : private juce::Timer
{
public:
    explicit StayOnTop (juce::Component& c) : target (&c) { startTimer (1000); keepTopmost (c); }
private:
    void timerCallback() override
    {
        if (target == nullptr) { stopTimer(); return; }
        if (target->isVisible()) keepTopmost (*target);
    }
    juce::Component::SafePointer<juce::Component> target;
};
} // namespace td
