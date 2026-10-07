#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace td
{
/** Makes 'child' a window OWNED by 'owner' (Windows): it always stays in front of the owner, can never disappear behind it, is minimised and
    restored together with it, and has no taskbar button of its own. It is how Fermata's windows behave as parts of the main window.
    Returns false where this is not available (the caller then raises the windows by hand). Safe to call again and again. */
bool makeOwnedBy (juce::Component& child, juce::Component& owner);
} // namespace td
