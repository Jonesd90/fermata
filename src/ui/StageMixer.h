#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
/** The stage speaker mixer, top right of the main window. It mixes what the TB (talkback) pair carries: one strip for every input ticked CR (a short fader and a pan knob),
    one for the Playback (the processing mixer's main output) and the output strip (what is actually sent to the TB L / TB R outputs). It only exists while at least one
    input is ticked CR or a TB pair is chosen. Double-click a fader or a knob to put it back to 0 dB / centre. */
class StageSpeakerPanel : public juce::Component, private juce::Timer
{
public:
    static constexpr int kStripW = 78, kHeight = 128, kGap = 10;

    explicit StageSpeakerPanel (AppContext& a) : app (a)
    {
        viewport.setScrollBarsShown (false, true);                 // a sideways scroll bar only when the strips do not fit
        viewport.setViewedComponent (&holder, false);
        addAndMakeVisible (viewport);
        rebuildIfNeeded();
        startTimerHz (20);
    }

    /** True when there is anything to mix (a CR input or a TB pair). */
    bool wanted() const { return ! app.project.crMicInputs.empty() || ! app.project.tbOutputs.empty(); }
    int wantedWidth() const { return (int) strips.size() * kStripW + kGap + 12; }

    /** Called whenever the project changes: rebuilds the strips if the CR inputs, the TB pairs or the names changed. */
    void rebuildIfNeeded()
    {
        juce::String sig;
        for (int i : app.project.crMicInputs) sig << i << ":" << label (i) << ";";
        sig << "|"; for (int o : app.project.tbOutputs) sig << o << ",";
        if (sig == signature && ! strips.isEmpty()) return;
        signature = sig;
        strips.clear(); holder.removeAllChildren();
        auto& p = app.project;
        for (int i : p.crMicInputs)
        {
            auto* s = new Strip (label (i), "mic " + juce::String (i + 1), true);
            s->fader.setValue (p.stageGainOf (i), juce::dontSendNotification);
            s->pan.setValue (p.stagePanOf (i), juce::dontSendNotification);
            s->onChange = [this, i] (float db, float pan) { setInput (i, db, pan); };
            strips.add (s); holder.addAndMakeVisible (s);
        }
        auto* pb = new Strip ("Playback", "processing mixer", false);
        pb->fader.setValue (p.stagePlaybackDb, juce::dontSendNotification);
        pb->onChange = [this] (float db, float) { app.project.stagePlaybackDb = db; push(); };
        strips.add (pb); holder.addAndMakeVisible (pb);
        auto* out = new Strip ("Output", outputName(), false);
        out->fader.setTooltip ("Final level into the talkback speakers. The small light at the top right of this strip goes red when they reach full scale (peak).");
        out->fader.setValue (p.stageOutputDb, juce::dontSendNotification);
        out->isOutput = true;
        out->onChange = [this] (float db, float) { app.project.stageOutputDb = db; push(); };
        strips.add (out); holder.addAndMakeVisible (out);
        resized(); repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        viewport.setBounds (r);
        const bool scrolls = wantedWidth() > r.getWidth();
        const int h = r.getHeight() - (scrolls ? 12 : 0);
        int x = 0;
        for (auto* s : strips)
        {
            if (s->isOutput || (s == strips[strips.size() - 2])) x += kGap / 2;       // a little air before Playback and before Output
            s->setBounds (x, 0, kStripW, h); x += kStripW;
        }
        holder.setSize (juce::jmax (x + 6, viewport.getMaximumVisibleWidth()), h);
    }

private:
    /** The little red light on the Output strip: lit for about a second whenever the stage speakers were driven to (nearly) full scale. */
    void timerCallback() override
    {
        const float pk = app.engine.takeStagePeak();
        if (pk >= 0.98f) holdUntil = juce::Time::getMillisecondCounter() + 1000;
        const bool on = juce::Time::getMillisecondCounter() < holdUntil;
        for (auto* s : strips) if (s->isOutput && s->peakOn != on) { s->peakOn = on; s->repaint(); }
    }
    juce::uint32 holdUntil = 0;

    struct Strip : public juce::Component
    {
        Strip (const juce::String& t, const juce::String& sub, bool hasPan) : title (t), subtitle (sub), withPan (hasPan)
        {
            fader.setSliderStyle (juce::Slider::LinearVertical); fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            fader.setRange (-60.0, 10.0, 0.1); fader.setDoubleClickReturnValue (true, 0.0);
            fader.setTooltip ("Level into the talkback speakers. Double-click for 0 dB; all the way down is off.");
            fader.onValueChange = [this] { if (onChange) onChange ((float) fader.getValue(), (float) pan.getValue()); repaint(); };
            addAndMakeVisible (fader);
            if (withPan)
            {
                pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); pan.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
                pan.setRange (-1.0, 1.0, 0.01); pan.setDoubleClickReturnValue (true, 0.0);
                pan.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
                pan.setTooltip ("Pan in the stage speakers (left / right). Double-click for centre.");
                pan.onValueChange = [this] { if (onChange) onChange ((float) fader.getValue(), (float) pan.getValue()); repaint(); };
                addAndMakeVisible (pan);
            }
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (3, 0);
            r.removeFromTop (28);                                       // the name (two lines at most)
            auto bottom = r.removeFromBottom (14);                      // the dB readout
            fader.setBounds (r.removeFromLeft (30).withTrimmedBottom (0));
            dbArea = bottom.removeFromLeft (30);
            if (withPan) { auto col = r.reduced (2, 0); pan.setBounds (col.removeFromTop (36).withSizeKeepingCentre (34, 34)); panText = col.removeFromTop (14); }
        }
        void paint (juce::Graphics& g) override
        {
            g.setColour (theme::panel);                                  g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f);
            g.setColour (theme::border.withAlpha (0.8f));                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);
            g.setColour (isOutput ? theme::accent : theme::text);        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawFittedText (title, 4, 3, getWidth() - 8, 14, juce::Justification::centred, 1, 0.8f);
            g.setColour (theme::dimText);                                g.setFont (juce::FontOptions (10.0f));
            g.drawFittedText (subtitle, 4, 16, getWidth() - 8, 11, juce::Justification::centred, 1, 0.8f);
            if (isOutput)                                                // the peak light: red when the stage speakers hit full scale
            {
                const auto led = juce::Rectangle<float> (getWidth() - 14.0f, 6.0f, 7.0f, 7.0f);
                g.setColour (peakOn ? juce::Colour (0xffff3030) : juce::Colour (0xff5a2a2a)); g.fillEllipse (led);
                if (peakOn) { g.setColour (juce::Colour (0x66ff3030)); g.drawEllipse (led.expanded (1.5f), 1.5f); }
            }
            const double db = fader.getValue();
            g.setColour (theme::text);                                   g.setFont (juce::FontOptions (11.0f));
            g.drawText (db <= -59.9 ? "off" : juce::String (db, 1), dbArea.expanded (4, 0), juce::Justification::centred);
            if (withPan)
            {
                const double p = pan.getValue();
                g.setColour (theme::dimText);
                g.drawText (std::abs (p) < 0.02 ? "C" : (p < 0 ? "L " : "R ") + juce::String ((int) std::round (std::abs (p) * 100.0)), panText, juce::Justification::centred);
            }
        }
        juce::String title, subtitle; bool withPan = true, isOutput = false, peakOn = false;
        juce::Slider fader, pan; juce::Rectangle<int> dbArea, panText;
        std::function<void (float, float)> onChange;
    };

    juce::String label (int input) const
    {
        if (input < 0 || input >= (int) app.project.inputs.size()) return "Input";
        auto& in = app.project.inputs[(size_t) input];
        return in.name.isNotEmpty() ? in.name : in.driverName;
    }
    juce::String outputName() const
    {
        auto& t = app.project.tbOutputs;
        if (t.empty()) return "no TB output";
        juce::String s = "TB " + juce::String (t.front() + 1) + "+" + juce::String (t.front() + 2);
        if (t.size() > 1) s << " ...";
        return s;
    }
    void setInput (int i, float db, float pan)
    {
        auto& p = app.project;
        if ((int) p.stageGainDb.size() <= i) p.stageGainDb.resize ((size_t) i + 1, 0.0f);
        if ((int) p.stagePan.size() <= i)    p.stagePan.resize ((size_t) i + 1, 0.0f);
        p.stageGainDb[(size_t) i] = db; p.stagePan[(size_t) i] = pan; push();
    }
    void push()
    {
        app.project.markDirty();
        app.engine.setStageMix (app.project.stageGainDb, app.project.stagePan, app.project.stagePlaybackDb, app.project.stageOutputDb);
    }

    AppContext& app;
    juce::Viewport viewport;
    juce::Component holder;
    juce::OwnedArray<Strip> strips;
    juce::String signature;
};
} // namespace td
