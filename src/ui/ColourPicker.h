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
        setSize (kCols * (kSw + kGap) + kGap + 2 * kPad, kPad + kTitleH + kRows * (kSw + kGap) + kGap + 6 + kRollH + 8 + 24 + kPad);
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
        {   // the colour roll: every hue, smoothly (drag along it); it keeps the brightness and strength of the colour the channel has now
            const auto r = rollRect();
            juce::ColourGradient grad (juce::Colours::red, (float) r.getX(), 0.0f, juce::Colours::red, (float) r.getRight(), 0.0f, false);
            grad.point1 = { (float) r.getX(), 0.0f }; grad.point2 = { (float) r.getRight(), 0.0f };
            grad.clearColours();
            for (int i = 0; i <= 24; ++i) grad.addColour ((double) i / 24.0, juce::Colour::fromHSV ((float) i / 24.0f, rollS(), rollV(), 1.0f));
            g.setGradientFill (grad); g.fillRect (r);
            g.setColour (juce::Colour (0x55000000)); g.drawRect (r, 1);
            if (app.project.channelColour (id) != 0)                                     // a marker at the current hue
            {
                const float x = (float) r.getX() + current.getHue() * (float) r.getWidth();
                g.setColour (juce::Colours::white); g.fillRect (x - 1.5f, (float) r.getY() - 2.0f, 3.0f, (float) r.getHeight() + 4.0f);
                g.setColour (juce::Colour (0xaa000000)); g.drawRect (x - 1.5f, (float) r.getY() - 2.0f, 3.0f, (float) r.getHeight() + 4.0f, 1.0f);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (kPad).withTop (kPad + kTitleH + kRows * (kSw + kGap) + kGap + 4 + kRollH + 8);
        autoButton.setBounds (r.removeFromLeft (r.getWidth() / 2).reduced (1));
        otherButton.setBounds (r.reduced (1));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (rollRect().expanded (0, 6).contains (e.getPosition())) { rolling = true; setFromRoll (e.x); return; }
        const auto& pal = chan::palette();
        for (int i = 0; i < (int) pal.size(); ++i)
            if (swatch (i).contains (e.getPosition()))
            {
                app.project.setChannelColour (id, pal[(size_t) i]);
                repaint();
                return;
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override { if (rolling) setFromRoll (e.x); }
    void mouseUp (const juce::MouseEvent&) override { rolling = false; }

private:
    static constexpr int kCols = 8, kRows = 2, kSw = 22, kGap = 3, kPad = 8, kTitleH = 16, kRollH = 22;
    bool rolling = false;
    juce::Rectangle<int> rollRect() const { return { kPad + kGap, kPad + kTitleH + kRows * (kSw + kGap) + kGap + 6, getWidth() - 2 * (kPad + kGap), kRollH }; }
    float rollS() const { return app.project.channelColour (id) != 0 ? juce::jlimit (0.35f, 1.0f, chan::of (app.project, id).getSaturation()) : 0.65f; }
    float rollV() const { return app.project.channelColour (id) != 0 ? juce::jlimit (0.45f, 1.0f, chan::of (app.project, id).getBrightness()) : 0.9f; }
    void setFromRoll (int x)
    {
        const auto r = rollRect();
        const float h = juce::jlimit (0.0f, 1.0f, (float) (x - r.getX()) / (float) juce::jmax (1, r.getWidth()));
        app.project.setChannelColour (id, juce::Colour::fromHSV (h, rollS(), rollV(), 1.0f).getARGB());
        repaint();
    }
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
} // namespace td
