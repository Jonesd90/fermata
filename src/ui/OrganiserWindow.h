#pragma once
#include "AppContext.h"

namespace td
{
/** One way of dividing the screen: the fractions of the screen each window gets, in the order of the slots. */
struct ScreenLayout
{
    juce::String name;
    std::vector<juce::Rectangle<float>> cells;
    std::vector<juce::String> labels;
};

/** A small picture of a layout: the screen divided into its cells. */
class OrganiserLayoutButton : public juce::Button
{
public:
    OrganiserLayoutButton (const ScreenLayout& l) : juce::Button (l.name), layout (l) { setClickingTogglesState (false); setTooltip (l.name); }
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();
        g.setColour (on ? theme::accent.withAlpha (0.35f) : over ? theme::button.brighter (0.1f) : theme::button); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? theme::accent : theme::border); g.drawRoundedRectangle (b, 4.0f, on ? 2.0f : 1.0f);
        auto screen = b.reduced (7.0f, 8.0f);
        for (auto& c : layout.cells)
        {
            auto r = juce::Rectangle<float> (screen.getX() + c.getX() * screen.getWidth(), screen.getY() + c.getY() * screen.getHeight(),
                                             c.getWidth() * screen.getWidth(), c.getHeight() * screen.getHeight()).reduced (1.5f);
            g.setColour (theme::text.withAlpha (on ? 0.85f : 0.55f)); g.fillRoundedRectangle (r, 1.5f);
        }
    }
private:
    const ScreenLayout& layout;
};

/** The Display Organiser: pick a layout (2, 3 or 4 windows), choose which window goes where, press Arrange. */
class OrganiserComponent : public juce::Component
{
public:
    explicit OrganiserComponent (AppContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    static const std::vector<ScreenLayout>& layouts();

private:
    void chooseLayout (int index);
    void refillWindows();
    void arrange();

    AppContext& app;
    int layoutIndex = 1;
    juce::OwnedArray<OrganiserLayoutButton> choices;
    juce::OwnedArray<juce::Label> slotLabels;
    juce::OwnedArray<juce::ComboBox> slotBoxes;
    juce::Label heading, status; InfoNote hint;
    juce::TextButton arrangeButton { "Arrange the windows" }, refreshButton { "Refresh the list" };
    std::vector<std::pair<juce::String, juce::String>> windowList;     // key, title
};
} // namespace td
