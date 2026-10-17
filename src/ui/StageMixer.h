#pragma once
#include "AppContext.h"
#include "Uikit.h"
#include "ValueEntry.h"
#include "DeskLook.h"

namespace td
{
/** The stage speaker mixer, top right of the main window. It mixes what the TB (talkback) pair carries: one small square box for every talkback mic input (a level knob
    and a pan knob), one for the Playback (the processing mixer's main output) and the Output box (what is actually sent to the TB L / TB R outputs). It only exists while
    at least one input is a talkback mic or a TB pair is chosen. Double-click a knob to type a value; Ctrl + click puts it back to 0 dB / centre. */
class StageSpeakerPanel : public juce::Component, private juce::Timer
{
public:
    static constexpr int kStripW = 64, kHeight = 64, kGap = 8;

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
    int wantedWidth() const { return (int) strips.size() * (kStripW + 2) + kGap + 8; }

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
        out->fader.setTooltip ("Final level into the talkback speakers (" + outputName() + "). The small light at the top right of this box goes red when they reach full scale (peak).");
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
            s->setBounds (x, 0, kStripW, juce::jmin (h, kHeight)); x += kStripW + 2;
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
        Strip (const juce::String& t, const juce::String& sub, bool hasPan) : title (t), subtitle (sub), withPan (hasPan), pan (true)
        {
            fader.setRange (-60.0, 10.0, 0.1); fader.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::ctrlModifier);
            fader.onDoubleClick = [this] { valueentry::ask (fader, "Level in dB (or off)", fader.getValue() <= -59.9 ? juce::String ("off") : juce::String (fader.getValue(), 1), [this] (const juce::String& t) { double db = 0.0; if (valueentry::parseDb (t, -60.0, -60.0, 10.0, db)) fader.setValue (db, juce::sendNotificationSync); }); };
            fader.setTooltip (title + " (" + subtitle + "): level into the talkback speakers. Drag up / down, double-click to type a level (Ctrl + click for 0 dB); all the way down is off.");
            fader.onValueChange = [this] { if (onChange) onChange ((float) fader.getValue(), (float) pan.getValue()); repaint(); };
            addAndMakeVisible (fader);
            if (withPan)
            {
                pan.setRange (-1.0, 1.0, 0.01); pan.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::ctrlModifier);
                pan.setRing (juce::Colour (0xffffd23a));
                pan.onDoubleClick = [this] { valueentry::ask (pan, "Pan (L30, C, R45 ...)", std::abs (pan.getValue()) < 0.02 ? juce::String ("C") : (pan.getValue() < 0 ? "L" : "R") + juce::String ((int) std::round (std::abs (pan.getValue()) * 100.0)), [this] (const juce::String& t) { double pv = 0.0; if (valueentry::parsePan (t, pv)) pan.setValue (pv, juce::sendNotificationSync); }); };
                pan.setTooltip ("Pan in the stage speakers (left / right). Double-click to type a position (Ctrl + click for centre).");
                pan.onValueChange = [this] { if (onChange) onChange ((float) fader.getValue(), (float) pan.getValue()); repaint(); };
                addAndMakeVisible (pan);
            }
        }
        void resized() override
        {
            auto r = getLocalBounds();
            if (withPan)
            {
                fader.setBounds (4, 16, 34, 34);
                pan.setBounds (r.getWidth() - 26, 19, 22, 22);
                dbArea = { 0, 50, 42, 12 };
                panText = { r.getWidth() - 28, 41, 26, 12 };
            }
            else
            {
                fader.setBounds (juce::Rectangle<int> (34, 34).withCentre ({ r.getCentreX(), 33 }));
                dbArea = { 0, 50, r.getWidth(), 12 };
            }
        }
        void paint (juce::Graphics& g) override
        {
            const auto& p = desk::pal();
            auto box = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (p.strip);                                       g.fillRoundedRectangle (box, 4.0f);
            g.setColour (isOutput ? juce::Colour (0xffffd23a) : p.cyan); g.fillRect (box.getX() + 3.0f, box.getY(), box.getWidth() - 6.0f, 3.0f);
            g.setColour (p.stripEdge);                                   g.drawRoundedRectangle (box, 4.0f, 1.0f);
            g.setColour (juce::Colours::white);                          g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawFittedText (title, 4, 3, getWidth() - (isOutput ? 18 : 8), 13, juce::Justification::centred, 1, 0.7f);
            if (isOutput)                                                // the peak light: red when the stage speakers hit full scale
            {
                const auto led = juce::Rectangle<float> (getWidth() - 12.0f, 6.0f, 7.0f, 7.0f);
                g.setColour (peakOn ? juce::Colour (0xffff3030) : juce::Colour (0xff5a2a2a)); g.fillEllipse (led);
                if (peakOn) { g.setColour (juce::Colour (0x66ff3030)); g.drawEllipse (led.expanded (1.5f), 1.5f); }
            }
            const double db = fader.getValue();
            g.setColour (p.cream); g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain));
            g.drawText (db <= -59.9 ? "off" : juce::String (db, 1), dbArea, juce::Justification::centred);
            if (withPan)
            {
                const double pv = pan.getValue();
                g.drawText (std::abs (pv) < 0.02 ? "C" : (pv < 0 ? "L" : "R") + juce::String ((int) std::round (std::abs (pv) * 100.0)), panText, juce::Justification::centred);
            }
        }
        juce::String title, subtitle; bool withPan = true, isOutput = false, peakOn = false;
        desk::RingPot fader, pan; juce::Rectangle<int> dbArea, panText;
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
