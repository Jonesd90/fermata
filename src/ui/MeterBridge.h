#pragma once
#include "AppContext.h"

namespace td
{
/** The meter bridge: a floating window of tall vertical meters, one for every driver input (BEFORE any fader - what arrives from the
    preamps) and one for every driver output (AFTER the faders - what leaves for the speakers / headphones / foldback). Press B to open or close. */
class MeterBridgeComponent : public juce::Component, private PeakListener
{
public:
    explicit MeterBridgeComponent (AppContext& a) : app (a)
    {
        resetButton.setButtonText ("Reset peaks");
        resetButton.setTooltip ("Clears every peak-hold marker. A peak above -2 dBFS stays marked until you press this; others fade after 10 s.");
        resetButton.onClick = [this] { for (auto* m : inMeters) m->resetPeak(); for (auto* m : outMeters) m->resetPeak(); };
        addAndMakeVisible (resetButton);
        caption.setText ("Meter bridge   -   inputs: before the faders      outputs: after the faders", juce::dontSendNotification);
        caption.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (caption);
        viewport.setViewedComponent (&holder, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);
        for (auto* l : { &inCaption, &outCaption })
        {
            l->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            l->setColour (juce::Label::textColourId, theme::dimText);
            holder.addAndMakeVisible (l);
        }
        inCaption.setText ("INPUTS (pre-fader)", juce::dontSendNotification);
        outCaption.setText ("OUTPUTS (post-fader)", juce::dontSendNotification);
        holder.addAndMakeVisible (inScale); holder.addAndMakeVisible (outScale);
        rebuild();
        app.project.structure.addChangeListener (&relay);
        relay.fn = [this] { rebuild(); resized(); };
        app.peakListeners.add (this);
        setSize (1100, 420);
    }
    ~MeterBridgeComponent() override
    {
        app.peakListeners.remove (this);
        app.project.structure.removeChangeListener (&relay);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        auto top = r.removeFromTop (28);
        resetButton.setBounds (top.removeFromRight (110).reduced (2));
        caption.setBounds (top);
        viewport.setBounds (r);
        layoutHolder();
    }

private:
    struct Relay : juce::ChangeListener { std::function<void()> fn; void changeListenerCallback (juce::ChangeBroadcaster*) override { if (fn) fn(); } };
    struct Cell : juce::Component, juce::SettableTooltipClient
    {
        Cell() { meter.setInterceptsMouseClicks (false, false); addAndMakeVisible (meter); addAndMakeVisible (number); number.setJustificationType (juce::Justification::centred); number.setFont (juce::FontOptions (10.5f)); number.setMinimumHorizontalScale (0.6f); number.setInterceptsMouseClicks (false, false); }
        void resized() override { auto r = getLocalBounds(); number.setBounds (r.removeFromBottom (16)); meter.setBounds (r.reduced (1, 0)); }
        LevelMeter meter { true }; juce::Label number;
    };

    void rebuild()
    {
        inCells.clear(); outCells.clear(); inMeters.clear(); outMeters.clear();
        for (int i = 0; i < (int) app.project.inputs.size(); ++i)
        {
            auto* c = inCells.add (new Cell());
            c->number.setText (juce::String (i + 1), juce::dontSendNotification);
            c->setTooltip ("Input " + app.project.inputLabel (i));
            holder.addAndMakeVisible (c); inMeters.add (&c->meter);
        }
        for (int i = 0; i < (int) app.project.outputs.size(); ++i)
        {
            auto* c = outCells.add (new Cell());
            c->number.setText (juce::String (i + 1), juce::dontSendNotification);
            c->setTooltip ("Output " + app.project.outputLabel (i));
            holder.addAndMakeVisible (c); outMeters.add (&c->meter);
        }
    }
    void layoutHolder()
    {
        const int nIn = inCells.size(), nOut = outCells.size();
        const int availW = juce::jmax (200, viewport.getWidth() - 4);
        const int scaleW = 26, gap = 30, capH = 18;
        const int cellW = juce::jlimit (14, 34, (availW - 2 * scaleW - gap) / juce::jmax (1, nIn + nOut));
        const int h = juce::jmax (120, viewport.getHeight() - viewport.getScrollBarThickness());
        const int meterTop = capH, meterBottom = h - 16;
        int x = 0;
        inCaption.setBounds (x, 0, juce::jmax (80, nIn * cellW + scaleW), capH);
        inScale.setBounds (x, meterTop, scaleW, meterBottom - meterTop); x += scaleW;
        for (auto* c : inCells) { c->setBounds (x, meterTop, cellW, h - meterTop); x += cellW; }
        x += gap;
        outCaption.setBounds (x, 0, juce::jmax (80, nOut * cellW + scaleW), capH);
        outScale.setBounds (x, meterTop, scaleW, meterBottom - meterTop); x += scaleW;
        for (auto* c : outCells) { c->setBounds (x, meterTop, cellW, h - meterTop); x += cellW; }
        holder.setSize (juce::jmax (x, availW), h);
    }
    void peaksUpdated() override
    {
        for (int i = 0; i < inMeters.size() && (size_t) i < app.inPeaks.size(); ++i)   inMeters[i]->setLevel (app.inPeaks[(size_t) i]);
        for (int i = 0; i < outMeters.size() && (size_t) i < app.outPeaks.size(); ++i) outMeters[i]->setLevel (app.outPeaks[(size_t) i]);
    }

    AppContext& app;
    Relay relay;
    juce::Label caption, inCaption, outCaption;
    juce::TextButton resetButton;
    juce::Viewport viewport; juce::Component holder;
    MeterScale inScale, outScale;
    juce::OwnedArray<Cell> inCells, outCells;
    juce::Array<LevelMeter*> inMeters, outMeters;
};
} // namespace td
