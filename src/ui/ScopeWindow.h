#pragma once
#include "AppContext.h"
#include "MixerLook.h"

namespace td
{
/** A stereo phase scope (goniometer) with a correlation meter, for any audio track, Int Bus or Ext Bus of one mixer.
    Mono sound draws a vertical line, left-only a line leaning left, out-of-phase sound a horizontal line. */
class PhaseScopeComponent : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    PhaseScopeComponent (AppContext&, const juce::Uuid& mixerId);
    ~PhaseScopeComponent() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { fillSources(); }
    MixerState* mixer() const;
    void fillSources();
    void chooseSource();
    void drawTrace();

    AppContext& app;
    juce::Uuid mixerId;
    juce::Label sourceCaption, gainCaption, readout;
    juce::ComboBox sourceBox, gainBox;
    struct Source { bool isTrack; juce::Uuid id; };
    std::vector<Source> sources;
    juce::String lastSig;
    juce::Image trail;
    juce::Rectangle<int> scopeArea, corrArea;
    std::vector<float> bufL, bufR;
    float correlation = 0.0f, lvlL = 0.0f, lvlR = 0.0f;
    bool updating = false;
};
} // namespace td
