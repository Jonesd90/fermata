#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace td
{
/** Double-click a mixer control to type the value you want. */
namespace valueentry
{
/** A slider whose double-click does what 'onDoubleClick' says (open the value box) instead of jumping back to its default.
    Ctrl + click still puts it back to its default value when the slider was given one with setDoubleClickReturnValue (true, value, ctrlModifier). */
class TypedSlider : public juce::Slider
{
public:
    std::function<void()> onDoubleClick;
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (onDoubleClick) onDoubleClick(); else juce::Slider::mouseDoubleClick (e);
    }
};

/** The little box that opens beside a control: Enter applies what was typed, Escape or a click elsewhere leaves the value alone. */
class Box : public juce::Component
{
public:
    Box (const juce::String& caption, const juce::String& current, std::function<void (const juce::String&)> onText) : done (std::move (onText))
    {
        addAndMakeVisible (label); addAndMakeVisible (editor);
        label.setText (caption, juce::dontSendNotification); label.setFont (juce::FontOptions (12.0f));
        editor.setText (current, false); editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [this] { commit(); };
        editor.onEscapeKey = [this] { close(); };
        setSize (230, 62);
    }
    void resized() override { label.setBounds (8, 4, getWidth() - 16, 18); editor.setBounds (8, 26, getWidth() - 16, 28); }
    void parentHierarchyChanged() override { if (isShowing()) editor.grabKeyboardFocus(); }
    void visibilityChanged() override { if (isShowing()) editor.grabKeyboardFocus(); }
private:
    void commit() { const auto t = editor.getText().trim(); if (t.isNotEmpty() && done) done (t); close(); }
    void close() { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); }
    juce::Label label; juce::TextEditor editor; std::function<void (const juce::String&)> done;
};

inline void ask (juce::Component& beside, const juce::String& caption, const juce::String& current, std::function<void (const juce::String&)> onText)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<Box> (caption, current, std::move (onText)), beside.getScreenBounds(), nullptr);
}

/** "-3", "-3.5 dB", "+2", "0", and "-inf" / "inf" / "off" (= 'offValue') -> true with the value kept inside lo .. hi. */
inline bool parseDb (juce::String t, double offValue, double lo, double hi, double& out)
{
    t = t.trim().toLowerCase();
    if (t == "-inf" || t == "inf" || t == "off" || t == "-oo" || t == "-infinity" || t == "minus infinity") { out = offValue; return true; }
    if (t.endsWith ("db")) t = t.dropLastCharacters (2).trim();
    if (! t.containsAnyOf ("0123456789") || ! t.containsOnly ("0123456789.,+- ")) return false;
    out = juce::jlimit (lo, hi, t.replaceCharacter (',', '.').getDoubleValue());
    return true;
}

/** "L30", "R 45", "C", "centre", "0", "-30" (left) / "30" (right) as a percentage, or "L" / "R" alone (full) -> -1 .. +1. */
inline bool parsePan (juce::String t, double& out)
{
    t = t.trim().toLowerCase();
    if (t == "c" || t == "centre" || t == "center" || t == "mid") { out = 0.0; return true; }
    double sign = 0.0;
    if (t.startsWith ("l")) { sign = -1.0; t = t.substring (1).trim(); }
    else if (t.startsWith ("r")) { sign = 1.0; t = t.substring (1).trim(); }
    if (t.isEmpty()) { if (sign == 0.0) return false; out = sign; return true; }
    if (t.endsWith ("%")) t = t.dropLastCharacters (1).trim();
    if (! t.containsAnyOf ("0123456789") || ! t.containsOnly ("0123456789.,+- ")) return false;
    const double v = t.replaceCharacter (',', '.').getDoubleValue() / 100.0;
    out = juce::jlimit (-1.0, 1.0, sign == 0.0 ? v : sign * std::abs (v));
    return true;
}
} // namespace valueentry
} // namespace td
