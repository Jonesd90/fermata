#pragma once
#include "MixerLook.h"

namespace td
{
/** Called after a switch so the choice is remembered (set by the application). */
inline std::function<void (bool dark)>& themeSaveHook() { static std::function<void (bool)> f; return f; }

namespace themeswitch
{
/** Gives colours that were set by hand (from the old palette) the matching colour of the new palette. */
inline void recolour (juce::Component& c, const std::array<juce::uint32, 18>& oldP, const std::array<juce::Colour*, 18>& newP)
{
    static const int ids[] = {
        juce::ResizableWindow::backgroundColourId,
        juce::TextButton::buttonColourId, juce::TextButton::buttonOnColourId, juce::TextButton::textColourOffId, juce::TextButton::textColourOnId,
        juce::ComboBox::backgroundColourId, juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId, juce::ComboBox::focusedOutlineColourId,
        juce::TextEditor::backgroundColourId, juce::TextEditor::textColourId, juce::TextEditor::highlightColourId, juce::TextEditor::highlightedTextColourId,
        juce::TextEditor::outlineColourId, juce::TextEditor::focusedOutlineColourId,
        juce::Label::backgroundColourId, juce::Label::textColourId, juce::Label::outlineColourId,
        juce::Slider::backgroundColourId, juce::Slider::thumbColourId, juce::Slider::trackColourId, juce::Slider::textBoxTextColourId,
        juce::Slider::textBoxBackgroundColourId, juce::Slider::textBoxOutlineColourId, juce::Slider::textBoxHighlightColourId,
        juce::ToggleButton::textColourId, juce::ToggleButton::tickColourId, juce::ToggleButton::tickDisabledColourId,
        juce::ListBox::backgroundColourId, juce::ListBox::outlineColourId, juce::ListBox::textColourId,
        juce::ScrollBar::backgroundColourId, juce::ScrollBar::thumbColourId, juce::ScrollBar::trackColourId,
        juce::TableHeaderComponent::backgroundColourId, juce::TableHeaderComponent::textColourId, juce::TableHeaderComponent::outlineColourId };
    for (int id : ids)
    {
        if (! c.isColourSpecified (id)) continue;
        const auto v = c.findColour (id).getARGB();
        for (size_t i = 0; i < oldP.size(); ++i)
            if (oldP[i] == v) { c.setColour (id, *newP[i]); break; }
    }
    for (auto* ch : c.getChildren()) if (ch != nullptr) recolour (*ch, oldP, newP);
}
}

/** Switches the whole program between the dark and the light look: every window, the mixers, and the choice is remembered. */
inline void switchTheme (bool dark)
{
    if (theme::isDark() == dark) return;
    const auto oldP = theme::snapshot();
    theme::setDark (dark);
    setMixDark (dark);
    if (auto* l = FermataLookAndFeel::instance()) l->applyTheme();
    const auto newP = theme::all();
    auto& desk = juce::Desktop::getInstance();
    for (int i = 0; i < desk.getNumComponents(); ++i)
        if (auto* c = desk.getComponent (i))
        {
            themeswitch::recolour (*c, oldP, newP);
            c->sendLookAndFeelChange();
            c->repaint();
        }
    if (themeSaveHook()) themeSaveHook() (dark);
}

/** Makes the program use switchTheme for the buttons in the windows. Call once at start. */
inline void installThemeSwitch() { themeSwitchHook() = [] (bool dark) { switchTheme (dark); }; }
} // namespace td
