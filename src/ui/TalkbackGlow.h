#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace td
{
/** The talkback borders: a thin, slowly pulsing PURPLE line right at the edge of the screen while the CR mic is open, and just inside it
    a slowly pulsing YELLOW glow while the TB speakers are fed with playback. Used by the Take Display screens and the main-screen overlay. */
inline void paintTalkbackGlow (juce::Graphics& g, juce::Rectangle<float> area, bool crOpen, bool tbPlayback)
{
    if (! crOpen && ! tbPlayback) return;
    const double t = (double) juce::Time::getMillisecondCounter() / 1000.0;
    const float side = juce::jmin (area.getWidth(), area.getHeight());
    const float thin = juce::jmax (3.0f, side * 0.006f);
    float inset = 0.0f;
    if (crOpen)
    {
        const float pulse = 0.5f + 0.5f * (float) std::sin (t * juce::MathConstants<double>::twoPi / 3.2);       // one slow breath every 3.2 s
        g.setColour (juce::Colour (0xffa83cff).withAlpha (0.35f + 0.65f * pulse));
        g.drawRect (area, thin);
        inset = thin;
    }
    if (tbPlayback)
    {
        const float pulse = 0.5f + 0.5f * (float) std::sin (t * juce::MathConstants<double>::twoPi / 2.6 + 1.0);
        const float depth = juce::jmax (10.0f, side * 0.028f);
        const int steps = 10;
        for (int i = 0; i < steps; ++i)                                     // brightest at the edge, fading inwards
        {
            const float f = 1.0f - (float) i / (float) steps;
            g.setColour (juce::Colour (0xffffd21f).withAlpha ((0.08f + 0.5f * pulse) * f * f));
            g.drawRect (area.reduced (inset + depth * (float) i / (float) steps), depth / (float) steps + 0.6f);
        }
    }
}

/** A see-through window laid over one whole display that draws only the talkback borders. Mouse clicks go straight through it. */
class TalkbackOverlay : public juce::Component, private juce::Timer
{
public:
    TalkbackOverlay (std::function<bool()> cr, std::function<bool()> tb, juce::Rectangle<int> area) : crFn (std::move (cr)), tbFn (std::move (tb))
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (false);
        setBounds (area);
        addToDesktop (juce::ComponentPeer::windowIgnoresMouseClicks | juce::ComponentPeer::windowIsTemporary);
        setAlwaysOnTop (true);
        setVisible (true);
        setBounds (area);
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override { paintTalkbackGlow (g, getLocalBounds().toFloat().reduced (0.5f), crFn(), tbFn()); }
private:
    void timerCallback() override { repaint(); }
    std::function<bool()> crFn, tbFn;
};
} // namespace td
