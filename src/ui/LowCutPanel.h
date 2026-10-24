#pragma once
#include "AppContext.h"
#include "MixerLook.h"
#include "ValueEntry.h"
#include "../core/LowCut.h"

namespace td
{
namespace lcui
{
/** A small recessed read-out field, and a tiny caption: the mixer's own look (MixerWindow.cpp has the same two for its strips). */
inline void field (juce::Label& l, float size, bool bold)
{
    const auto& t = mixTheme();
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::backgroundColourId, t.field);
    l.setColour (juce::Label::outlineColourId, t.line);
    l.setColour (juce::Label::textColourId, t.text);
    l.setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
    l.setMinimumHorizontalScale (0.8f);
    l.setInterceptsMouseClicks (false, false);
}
inline void caption (juce::Label& l, const juce::String& text, juce::Justification j)
{
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::FontOptions (8.5f, juce::Font::bold));
    l.setColour (juce::Label::textColourId, mixTheme().dim);
    l.setJustificationType (j);
    l.setMinimumHorizontalScale (0.8f);
    l.setInterceptsMouseClicks (false, false);
}
}

/** The panel behind a strip's LC key (a long press, or a right-click): the frequency, swept over the whole range, the slope, the on switch, the
    delay the filter costs, and the curve it makes. Changes go to the filter a moment after the last one (it is designed anew each time). */
class LowCutPanel : public juce::Component, private juce::Timer
{
public:
    LowCutPanel (AppContext& a, LowCut& l, const juce::String& channelName) : app (a), lc (l), name (channelName)
    {
        const double sr = app.engine.getSampleRate() > 0.0 ? app.engine.getSampleRate() : 48000.0;
        onBox.setButtonText ("Low cut on");
        onBox.setToggleState (lc.on.get(), juce::dontSendNotification);
        onBox.onClick = [this] { lc.setOn (onBox.getToggleState()); app.project.markDirty(); app.engine.checkLatency(); refreshText(); repaint(); };
        addAndMakeVisible (onBox);
        linearBox.setButtonText ("Linear phase");
        linearBox.setToggleState (lc.linear.get(), juce::dontSendNotification);
        linearBox.setTooltip ("Unticked (the usual): a zero-latency Butterworth high-pass of this slope - the classic console low cut, no delay, the phase shift any such filter has. "
                              "Ticked: a symmetric FIR, exactly linear phase (nothing smeared), but the channel comes out later by half the filter's length (DELAY) and the "
                              "whole mix with it: for post, not for monitoring.");
        linearBox.onClick = [this] { pending = true; refreshText(); repaint(); startTimer (60); };
        addAndMakeVisible (linearBox);

        freq.setSliderStyle (juce::Slider::LinearHorizontal);
        freq.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        freq.setRange ((double) lowcut::kMinHz, lowcut::maxHz (sr), 0.0);
        freq.setSkewFactorFromMidPoint (std::sqrt ((double) lowcut::kMinHz * lowcut::maxHz (sr)));        // a log scale: every octave the same width
        freq.setValue ((double) lc.hz.get(), juce::dontSendNotification);
        freq.setDoubleClickReturnValue (true, (double) lowcut::kDefaultHz);
        freq.onValueChange = [this] { pending = true; refreshText(); repaint(); startTimer (60); };
        freq.setTooltip ("The cut: -3 dB here (the wall: -6 dB), everything below falls away at the slope. Double-click: 80 Hz. Click the value to type it.");
        addAndMakeVisible (freq);
        lcui::field (freqValue, 11.0f, true);
        freqClick = std::make_unique<valueentry::ReadoutClick> (freqValue, true, [this] { askHz(); });
        addAndMakeVisible (freqValue);

        for (int i = 0; i < lowcut::kNumSlopes; ++i) slopeBox.addItem (lowcut::slopeName (i), i + 1);
        slopeBox.setSelectedId (lc.slope.load() + 1, juce::dontSendNotification);
        slopeBox.onChange = [this] { pending = true; refreshText(); repaint(); startTimer (60); };
        slopeBox.setTooltip ("How steeply the sound falls away below the cut. Zero latency unless Linear phase is ticked; then the steeper, the longer the filter and the delay.");
        addAndMakeVisible (slopeBox);
        lcui::caption (freqCap, "FREQUENCY", juce::Justification::centredLeft); lcui::caption (slopeCap, "SLOPE", juce::Justification::centredLeft); lcui::caption (delayCap, "DELAY", juce::Justification::centredRight);
        addAndMakeVisible (freqCap); addAndMakeVisible (slopeCap); addAndMakeVisible (delayCap);
        lcui::field (delayValue, 10.0f, true);
        delayValue.setTooltip ("How much the filter holds this channel back: none for the zero-latency filter; half the FIR's length (plus the convolver's block) for linear phase. "
                               "Fermata holds every other path back by the same, so nothing goes out of time; the whole mix comes out this much later.");
        addAndMakeVisible (delayValue);
        setSize (340, 258);
        refreshText();
    }
    ~LowCutPanel() override { if (pending) apply(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = mixTheme();
        g.fillAll (theme::panel);
        g.setColour (theme::text); g.setFont (FermataLookAndFeel::displayFont (15.0f));
        g.drawText ("Low cut  -  " + name, 12, 8, getWidth() - 24, 20, juce::Justification::centredLeft, true);
        // the curve: dB against log frequency
        const auto plot = plotRect();
        g.setColour (t.field); g.fillRect (plot);
        const double sr = app.engine.getSampleRate() > 0.0 ? app.engine.getSampleRate() : 48000.0;
        const double f0 = (double) lowcut::kMinHz, f1 = sr * 0.5;
        auto xOf = [&] (double f) { return (float) plot.getX() + (float) plot.getWidth() * (float) (std::log (f / f0) / std::log (f1 / f0)); };
        auto yOf = [&] (double db) { return (float) plot.getBottom() - (float) plot.getHeight() * (float) ((db + 60.0) / 66.0); };     // -60 .. +6 dB
        g.setFont (juce::FontOptions (8.0f));
        for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            if (f >= f1) continue;
            const float x = xOf (f);
            if (x > (float) plot.getRight() - 34.0f) continue;                  // (no room beside the dB figures)
            g.setColour (t.soft.withAlpha (0.5f)); g.fillRect (x, (float) plot.getY(), 1.0f, (float) plot.getHeight());
            g.setColour (t.dim); g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String (f, 0), (int) x + 2, plot.getBottom() - 11, 30, 10, juce::Justification::centredLeft, false);
        }
        for (double db : { 0.0, -12.0, -24.0, -36.0, -48.0 })
        {
            const float y = yOf (db);
            g.setColour (t.soft.withAlpha (db == 0.0 ? 0.9f : 0.5f)); g.fillRect ((float) plot.getX(), y, (float) plot.getWidth(), 1.0f);
            g.setColour (t.dim); g.drawText (juce::String ((int) db), plot.getRight() - 24, (int) y - 10, 22, 10, juce::Justification::centredRight, false);
        }
        const double fc = freq.getValue(); const int slope = slopeBox.getSelectedId() - 1; const bool lin = linearBox.getToggleState();
        juce::Path p; bool started = false;
        for (int i = 0; i <= plot.getWidth(); ++i)
        {
            const double f = f0 * std::pow (f1 / f0, (double) i / (double) plot.getWidth());
            const float x = (float) plot.getX() + (float) i, y = juce::jmax ((float) plot.getY(), yOf (lowcut::targetDb (f, fc, slope, lin)));
            if (! started) { p.startNewSubPath (x, y); started = true; } else p.lineTo (x, y);
        }
        const bool on = onBox.getToggleState();
        g.setColour ((theme::manuscript() ? theme::giltBright() : theme::accent).withAlpha (on ? 1.0f : 0.45f));
        g.strokePath (p, juce::PathStrokeType (1.6f));
        g.setColour (theme::text.withAlpha (0.35f)); g.fillRect (xOf (fc), (float) plot.getY(), 1.0f, (float) plot.getHeight());      // the cut
        g.setColour (theme::border); g.drawRect (plot, 1);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        r.removeFromTop (24);
        auto row = r.removeFromTop (20);
        onBox.setBounds (row.removeFromLeft (110));
        delayValue.setBounds (row.removeFromRight (120)); delayCap.setBounds (row.removeFromRight (44));
        r.removeFromTop (2);
        linearBox.setBounds (r.removeFromTop (20).removeFromLeft (150));
        r.removeFromTop (4);
        row = r.removeFromTop (16); freqCap.setBounds (row.removeFromLeft (90)); freqValue.setBounds (row.removeFromRight (80));
        freq.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        row = r.removeFromTop (20); slopeCap.setBounds (row.removeFromLeft (90)); slopeBox.setBounds (row.removeFromRight (150));
        r.removeFromTop (6);
        plotArea = r;
    }

private:
    juce::Rectangle<int> plotRect() const { return plotArea; }
    void refreshText()
    {
        const double hz = freq.getValue();
        freqValue.setText (hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (hz, hz < 100.0 ? 1 : 0) + " Hz", juce::dontSendNotification);
        const double sr = app.engine.getSampleRate() > 0.0 ? app.engine.getSampleRate() : 48000.0;
        if (linearBox.getToggleState())
        {
            const int lat = LowCut::linearLatencyFor (hz, slopeBox.getSelectedId() - 1, sr);
            delayValue.setText (juce::String (lat) + " = " + juce::String (juce::roundToInt (lat * 1000.0 / sr)) + " ms", juce::dontSendNotification);
        }
        else delayValue.setText ("none", juce::dontSendNotification);
        delayValue.setColour (juce::Label::textColourId, onBox.getToggleState() ? theme::text : theme::dimText);
        slopeBox.setTooltip (linearBox.getToggleState() ? "How steeply the sound falls away below the cut. Every slope is linear phase (nothing is smeared); the steeper, the longer the filter and the delay."
                                                          : "How steeply the sound falls away below the cut: a Butterworth of that order (Brickwall: 24th order, 144 dB / octave). No delay at any slope.");
    }
    void askHz()
    {
        juce::Component::SafePointer<LowCutPanel> safe (this);
        valueentry::ask (freqValue, "Low cut frequency (Hz)", juce::String (freq.getValue(), 1), [safe] (const juce::String& t)
        {
            if (safe == nullptr) return;
            const double v = t.retainCharacters ("0123456789.").getDoubleValue() * (t.containsIgnoreCase ("k") ? 1000.0 : 1.0);
            if (v > 0.0) safe->freq.setValue (juce::jlimit (safe->freq.getMinimum(), safe->freq.getMaximum(), v), juce::sendNotificationSync);
        });
    }
    void timerCallback() override { stopTimer(); apply(); }
    void apply()
    {
        pending = false;
        lc.setDesign ((float) freq.getValue(), slopeBox.getSelectedId() - 1, linearBox.getToggleState());
        app.project.markDirty();
        app.engine.checkLatency();                                      // (a new length: the mixer's delay compensation follows)
        if (onChanged) onChanged();
    }
public:
    std::function<void()> onChanged;                                   // the strip's key redraws its caption
private:
    AppContext& app; LowCut& lc; juce::String name;
    juce::ToggleButton onBox, linearBox; juce::Slider freq; juce::Label freqValue, freqCap, slopeCap, delayCap, delayValue; juce::ComboBox slopeBox;
    std::unique_ptr<valueentry::ReadoutClick> freqClick;
    juce::Rectangle<int> plotArea;
    bool pending = false;
};

/** The LC key on a strip: a click switches the low cut on and off, a long press (or a right-click) opens its panel. The small print beside it
    says what the filter is set to. */
class LowCutKey : public juce::Component, private juce::Timer
{
public:
    LowCutKey (AppContext& a, LowCut& l, std::function<juce::String()> channelName) : app (a), lc (l), nameOf (std::move (channelName))
    {
        key.setButtonText ("LC");
        key.setClickingTogglesState (false);
        key.setTooltip ("Low cut (before the inserts): click to switch it on or off. Hold the key down, or right-click it, to set the frequency, the slope and the kind: zero latency "
                        "(a Butterworth high-pass, the usual) or linear phase (held back by half its length). 80 Hz, 48 dB / octave, zero latency until changed.");
        key.onClick = [this] { if (swallow) { swallow = false; return; } lc.setOn (! lc.on.get()); app.project.markDirty(); app.engine.checkLatency(); refresh(); };
        key.addMouseListener (this, false);
        addAndMakeVisible (key);
        lcui::caption (text, {}, juce::Justification::centredLeft);
        text.setFont (juce::FontOptions (8.5f));
        text.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (text);
        refresh();
    }
    void refresh()
    {
        const bool on = lc.on.get();
        if (key.getToggleState() != on) key.setToggleState (on, juce::dontSendNotification);
        const float hz = lc.hz.get();
        text.setText ((hz >= 1000.0f ? juce::String (hz / 1000.0f, 1) + "k" : juce::String (hz, hz < 100.0f ? 1 : 0)) + (lc.linear.get() ? " " : "  ")
                      + (lc.slope.load() >= lowcut::kBrickwall ? juce::String ("wall") : juce::String (lowcut::slopeDb (lc.slope.load())) + "dB") + (lc.linear.get() ? " lin" : ""), juce::dontSendNotification);   // ("lin": linear phase)
        text.setColour (juce::Label::textColourId, on ? mixTheme().text : mixTheme().dim);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        key.setBounds (r.removeFromLeft (juce::jmin (34, r.getWidth() / 2)).reduced (0, 1));
        r.removeFromLeft (4);
        text.setBounds (r);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != &key) return;
        swallow = false;                                                   // a new press: whatever an earlier one left (the panel's opening can take the click it meant to swallow)
        if (e.mods.isPopupMenu()) { swallow = true; openPanel(); return; }
        startTimer (450);
    }
    void mouseUp (const juce::MouseEvent& e) override { if (e.eventComponent == &key) stopTimer(); }
    void mouseDrag (const juce::MouseEvent& e) override { if (e.eventComponent == &key && e.getDistanceFromDragStart() > 6) stopTimer(); }
    /** Opens the panel (what a long press does). */
    void openPanel()
    {
        auto panel = std::make_unique<LowCutPanel> (app, lc, nameOf ? nameOf() : juce::String());
        juce::Component::SafePointer<LowCutKey> safe (this);
        panel->onChanged = [safe] { if (safe != nullptr) safe->refresh(); };
        auto& box = juce::CallOutBox::launchAsynchronously (std::move (panel), key.getScreenBounds(), nullptr);
        box.setDismissalMouseClicksAreAlwaysConsumed (true);
    }
private:
    void timerCallback() override
    {
        stopTimer();
        swallow = true;                                                    // the click that ends this press must not toggle the filter
        openPanel();
    }
    AppContext& app; LowCut& lc; std::function<juce::String()> nameOf;
    juce::TextButton key; juce::Label text;
    bool swallow = false;
};
} // namespace td
