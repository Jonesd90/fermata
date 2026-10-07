#pragma once
#include "AppContext.h"

namespace td
{
/** The small panel that opens when you click a channel's colour (on the mixer strip, or on a track name in a take / edit window):
    a grid of ready-made colours, "Automatic", and "Other..." for any colour at all. Every window that shows the channel follows at once. */
class ColourPickerPanel : public juce::Component
{
public:
    ColourPickerPanel (AppContext& a, const juce::Uuid& channel) : app (a), id (channel)
    {
        autoButton.setButtonText ("Automatic");
        autoButton.setTooltip ("Go back to the colour the program picks for this kind of channel");
        autoButton.onClick = [this] { app.project.setChannelColour (id, 0); repaint(); };
        addAndMakeVisible (autoButton);
        otherButton.setButtonText ("Other...");
        otherButton.setTooltip ("Pick any colour");
        otherButton.onClick = [this] { openSelector(); };
        addAndMakeVisible (otherButton);
        setSize (kCols * (kSw + kGap) + kGap + 2 * kPad, kPad + kTitleH + kRows * (kSw + kGap) + kGap + 6 + 24 + kPad);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
        g.setColour (theme::dimText); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("COLOUR  -  " + app.project.nodeName (id), kPad, kPad - 2, getWidth() - 2 * kPad, kTitleH, juce::Justification::centredLeft, true);
        const auto current = chan::of (app.project, id);
        const bool custom = app.project.channelColour (id) != 0;
        const auto& pal = chan::palette();
        for (int i = 0; i < (int) pal.size(); ++i)
        {
            const auto r = swatch (i);
            g.setColour (juce::Colour (pal[(size_t) i])); g.fillRect (r);
            const bool isCurrent = custom && juce::Colour (pal[(size_t) i]) == current;
            g.setColour (isCurrent ? juce::Colours::white : juce::Colour (0x55000000)); g.drawRect (r, isCurrent ? 2 : 1);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (kPad).withTop (kPad + kTitleH + kRows * (kSw + kGap) + kGap + 4);
        autoButton.setBounds (r.removeFromLeft (r.getWidth() / 2).reduced (1));
        otherButton.setBounds (r.reduced (1));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto& pal = chan::palette();
        for (int i = 0; i < (int) pal.size(); ++i)
            if (swatch (i).contains (e.getPosition()))
            {
                app.project.setChannelColour (id, pal[(size_t) i]);
                repaint();
                return;
            }
    }

private:
    static constexpr int kCols = 8, kRows = 2, kSw = 22, kGap = 3, kPad = 8, kTitleH = 16;
    juce::Rectangle<int> swatch (int i) const
    {
        return { kPad + kGap + (i % kCols) * (kSw + kGap), kPad + kTitleH + kGap + (i / kCols) * (kSw + kGap), kSw, kSw };
    }

    /** The full colour chooser; the channel follows while you drag in it. */
    struct Selector : public juce::ColourSelector, private juce::ChangeListener
    {
        Selector (AppContext& a, const juce::Uuid& channel)
            : juce::ColourSelector (juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders, 4, 4), app (a), id (channel)
        {
            setCurrentColour (chan::of (app.project, id), juce::dontSendNotification);
            addChangeListener (this);
            setSize (260, 270);
        }
        ~Selector() override { removeChangeListener (this); }
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            app.project.setChannelColour (id, getCurrentColour().withAlpha (1.0f).getARGB());
        }
        AppContext& app; juce::Uuid id;
    };

    void openSelector()
    {
        auto where = getScreenBounds();
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
        juce::CallOutBox::launchAsynchronously (std::make_unique<Selector> (app, id), where, nullptr);
    }

    AppContext& app; juce::Uuid id;
    juce::TextButton autoButton, otherButton;
};

/** Opens the colour panel next to 'target'. */
inline void showColourPickerAt (AppContext& app, const juce::Uuid& channel, juce::Rectangle<int> screenArea)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<ColourPickerPanel> (app, channel), screenArea, nullptr);
}
inline void showColourPicker (AppContext& app, const juce::Uuid& channel, juce::Component& target)
{
    showColourPickerAt (app, channel, target.getScreenBounds());
}
} // namespace td
