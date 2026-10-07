#pragma once
#include "Uikit.h"

namespace td
{
/** The start-up picture: the Fermata mark with the build number, in the middle of the screen for about four seconds. */
class SplashScreen : public juce::Component, private juce::Timer
{
public:
    explicit SplashScreen (const juce::String& build, bool onDesktop = true) : buildText (build)
    {
        setOpaque (true);
        setSize (560, 330);
        setAlwaysOnTop (true);
        setInterceptsMouseClicks (true, false);
        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        setTopLeftPosition (area.getCentre().x - getWidth() / 2, area.getCentre().y - getHeight() / 2);
        if (onDesktop) { addToDesktop (juce::ComponentPeer::windowIsTemporary); setVisible (true); toFront (false); }
        shownAt = juce::Time::getMillisecondCounter();
        if (auto* peer = getPeer()) peer->performAnyPendingRepaintsNow();     // paint it now, before the program gets busy starting up
    }

    /** Called when the program is ready: the picture stays until it has been up for four seconds in all. */
    void startCountdown (std::function<void()> done)
    {
        onDone = std::move (done);
        const auto elapsed = (int) (juce::Time::getMillisecondCounter() - shownAt);
        startTimer (juce::jmax (300, 4000 - elapsed));
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.fillAll (theme::window);
        g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
        drawFermataLogo (g, juce::Rectangle<float> (b.getCentreX() - 150.0f, 34.0f, 300.0f, 150.0f), theme::text, theme::accent);
        g.setColour (theme::text);
        g.setFont (juce::Font (juce::FontOptions ("Georgia", 54.0f, juce::Font::bold)));
        g.drawText ("fermata", getLocalBounds().withTrimmedTop (176).withHeight (66), juce::Justification::centred, false);
        g.setColour (theme::dimText);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("classical music recording", getLocalBounds().withTrimmedTop (240).withHeight (22), juce::Justification::centred, false);
        g.setColour (theme::accent);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (buildText, getLocalBounds().withTrimmedTop (284).withHeight (24), juce::Justification::centred, false);
    }
    void mouseDown (const juce::MouseEvent&) override { finish(); }       // a click closes it early

private:
    void timerCallback() override { finish(); }
    void finish()
    {
        stopTimer();
        if (auto cb = std::exchange (onDone, nullptr)) cb();
    }
    juce::String buildText;
    juce::uint32 shownAt = 0;
    std::function<void()> onDone;
};
} // namespace td
