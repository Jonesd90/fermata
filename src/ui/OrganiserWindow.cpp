#include "OrganiserWindow.h"

namespace td
{
const std::vector<ScreenLayout>& OrganiserComponent::layouts()
{
    using R = juce::Rectangle<float>;
    static const std::vector<ScreenLayout> l
    {
        { "1 window, whole screen",              { R (0, 0, 1, 1) }, { "Window" } },
        { "2 windows: top and bottom",           { R (0, 0, 1, .5f), R (0, .5f, 1, .5f) }, { "Top", "Bottom" } },
        { "2 windows: side by side",             { R (0, 0, .5f, 1), R (.5f, 0, .5f, 1) }, { "Left", "Right" } },
        { "3 windows: three rows",               { R (0, 0, 1, 1.0f / 3), R (0, 1.0f / 3, 1, 1.0f / 3), R (0, 2.0f / 3, 1, 1.0f / 3) }, { "Top", "Middle", "Bottom" } },
        { "3 windows: three columns",            { R (0, 0, 1.0f / 3, 1), R (1.0f / 3, 0, 1.0f / 3, 1), R (2.0f / 3, 0, 1.0f / 3, 1) }, { "Left", "Middle", "Right" } },
        { "3 windows: one on top, two below",    { R (0, 0, 1, .5f), R (0, .5f, .5f, .5f), R (.5f, .5f, .5f, .5f) }, { "Top", "Bottom left", "Bottom right" } },
        { "3 windows: one left, two right",      { R (0, 0, .5f, 1), R (.5f, 0, .5f, .5f), R (.5f, .5f, .5f, .5f) }, { "Left", "Top right", "Bottom right" } },
        { "4 windows: a grid",                   { R (0, 0, .5f, .5f), R (.5f, 0, .5f, .5f), R (0, .5f, .5f, .5f), R (.5f, .5f, .5f, .5f) }, { "Top left", "Top right", "Bottom left", "Bottom right" } },
    };
    return l;
}

OrganiserComponent::OrganiserComponent (AppContext& a) : app (a)
{
    heading.setText ("Choose how the screen is divided, then which window goes in each part.", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    heading.setColour (juce::Label::textColourId, theme::text);
    addAndMakeVisible (heading);
    hint.setText ("The first window listed fills the first part (for two windows top and bottom, that is the top). Windows you minimised come back.", juce::dontSendNotification);
    hint.setFont (juce::FontOptions (12.0f)); hint.setColour (juce::Label::textColourId, theme::dimText);
    hint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (hint);
    status.setColour (juce::Label::textColourId, theme::warn);
    addAndMakeVisible (status);

    int i = 0;
    for (auto& l : layouts())
    {
        auto* c = choices.add (new OrganiserLayoutButton (l));
        const int idx = i++;
        c->onClick = [this, idx] { chooseLayout (idx); };
        addAndMakeVisible (c);
    }
    arrangeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    arrangeButton.onClick = [this] { arrange(); };
    refreshButton.onClick = [this] { refillWindows(); };
    addAndMakeVisible (arrangeButton); addAndMakeVisible (refreshButton);
    refillWindows();
    chooseLayout (1);
    setSize (640, 360);
}

void OrganiserComponent::paint (juce::Graphics& g) { g.fillAll (theme::window); }

void OrganiserComponent::refillWindows()
{
    windowList = app.listWindows ? app.listWindows() : std::vector<std::pair<juce::String, juce::String>>();
    for (int s = 0; s < slotBoxes.size(); ++s)
    {
        const auto keep = slotBoxes[s]->getSelectedId();
        slotBoxes[s]->clear (juce::dontSendNotification);
        slotBoxes[s]->addItem ("(leave alone)", 1);
        for (size_t w = 0; w < windowList.size(); ++w) slotBoxes[s]->addItem (windowList[w].second, (int) w + 2);
        slotBoxes[s]->setSelectedId (keep > 0 && keep - 2 < (int) windowList.size() ? keep : juce::jmin ((int) windowList.size() + 1, s + 2), juce::dontSendNotification);
    }
}

void OrganiserComponent::chooseLayout (int index)
{
    layoutIndex = index;
    for (int i = 0; i < choices.size(); ++i) choices[i]->setToggleState (i == index, juce::dontSendNotification);
    const auto& l = layouts()[(size_t) index];
    slotLabels.clear(); slotBoxes.clear();
    for (size_t s = 0; s < l.cells.size(); ++s)
    {
        auto* lab = slotLabels.add (new juce::Label ({}, l.labels[s] + ":"));
        lab->setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (lab);
        addAndMakeVisible (slotBoxes.add (new juce::ComboBox()));
    }
    refillWindows();
    status.setText ({}, juce::dontSendNotification);
    resized();
}

void OrganiserComponent::resized()
{
    auto r = getLocalBounds().reduced (14);
    { auto h = r.removeFromTop (24); hint.setBounds (h.removeFromRight (22).withSizeKeepingCentre (20, 20)); heading.setBounds (h); }
    r.removeFromTop (6);
    auto row = r.removeFromTop (64);
    for (auto* c : choices) c->setBounds (row.removeFromLeft (72).reduced (2));
    r.removeFromTop (10);
    for (int s = 0; s < slotBoxes.size(); ++s)
    {
        auto line = r.removeFromTop (32);
        slotLabels[s]->setBounds (line.removeFromLeft (120));
        slotBoxes[s]->setBounds (line.removeFromLeft (420).reduced (0, 3));
        r.removeFromTop (2);
    }
    auto bottom = r.removeFromBottom (36);
    arrangeButton.setBounds (bottom.removeFromLeft (200).reduced (2));
    refreshButton.setBounds (bottom.removeFromLeft (150).reduced (2));
    status.setBounds (r.removeFromBottom (22));
}

void OrganiserComponent::arrange()
{
    const auto& l = layouts()[(size_t) layoutIndex];
    std::vector<std::pair<juce::String, juce::Rectangle<float>>> placements;
    juce::StringArray used;
    for (int s = 0; s < slotBoxes.size(); ++s)
    {
        const int id = slotBoxes[s]->getSelectedId();
        if (id < 2 || (size_t) id - 2 >= windowList.size()) continue;
        const auto& key = windowList[(size_t) id - 2].first;
        if (used.contains (key)) { status.setText ("The same window is chosen twice: pick a different one for each part.", juce::dontSendNotification); return; }
        used.add (key);
        placements.push_back ({ key, l.cells[(size_t) s] });
    }
    if (placements.empty()) { status.setText ("Choose at least one window.", juce::dontSendNotification); return; }
    status.setText ({}, juce::dontSendNotification);
    if (app.placeWindows) app.placeWindows (placements);
}
} // namespace td
