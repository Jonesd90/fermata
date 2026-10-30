#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
/** The writing of a small two-line lamp in a take window's marks row (MATCHING, MAIN, 2ND, DEVICE): its name small on top, its word under it, with
    equal room above, between and below. (3.18.1: a touch smaller, and squeezed sideways only when a long word - NOT DELIVERING - cannot fit otherwise;
    they looked crammed.) The box keeps its size. */
inline void paintLampWords (juce::Graphics& g, juce::Rectangle<float> box, const juce::String& name, const juce::String& word)
{
    const float h = box.getHeight();
    const float nameH = juce::jlimit (6.5f, 7.5f, h * 0.28f), wordH = juce::jlimit (7.5f, 9.5f, h * 0.36f);
    const float gap = juce::jmax (0.0f, (h - nameH - wordH) / 3.0f);
    auto inner = box.reduced (3.0f, 0.0f);
    inner.removeFromTop (gap);
    g.setFont (juce::FontOptions (nameH, juce::Font::bold));
    g.drawText (name, inner.removeFromTop (nameH + 1.0f), juce::Justification::centred, false);
    inner.removeFromTop (juce::jmax (0.0f, gap - 1.0f));
    g.setFont (juce::FontOptions (wordH, juce::Font::bold));
    g.drawFittedText (word, inner.removeFromTop (wordH + 2.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
}

/** The three lamps of the recording monitor (3.2.0): MAIN drive, 2ND drive, DEVICE. Each is a small box with a word in it, green when all is
    well, amber for a warning, red for a problem, grey when idle or off; the tooltip says why. Shown permanently in every take window's head, and
    at the top of the Recording monitor window. A click opens the Recording monitor. What they show comes from AppContext::recordMonitor. */
class RecordLamps : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit RecordLamps (AppContext& a, bool bigLamps = false) : app (a), big (bigLamps)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
        startTimerHz (5);
        refresh();
    }
    /** The width the three lamps want for a given height. */
    static int idealWidth (int height) { return 3 * lampWidth (height) + 2 * kGap; }
    static constexpr int kGap = 3;
    static int lampWidth (int height) { return juce::jmax (72, height * 3); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const int w = (r.getWidth() - 2 * kGap) / 3;
        const char* names[3] = { "MAIN", "2ND", "DEVICE" };
        for (int i = 0; i < 3; ++i)
        {
            auto box = r.removeFromLeft (w).toFloat();
            r.removeFromLeft (kGap);
            const auto& l = lamps[i];
            const auto fill = colourFor (l.level);
            g.setColour (fill); g.fillRoundedRectangle (box, 3.0f);
            g.setColour (theme::border); g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);
            const auto ink = l.level == 1 ? juce::Colour (0xff1a1200) : l.level <= -1 ? theme::dimText : juce::Colours::white;
            g.setColour (ink);
            if (big)
            {
                auto inner = box.reduced (6.0f, 4.0f);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (names[i], inner.removeFromTop (16.0f), juce::Justification::centredLeft, false);
                g.setFont (juce::FontOptions (juce::jmin (20.0f, inner.getHeight() - 2.0f), juce::Font::bold));
                g.drawFittedText (l.word, inner.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            }
            else if (box.getHeight() >= 19.0f)
            {
                // two lines: the name small on top, the word under it (NOT DELIVERING, COPY LOST ... are too long for one line in a marks row)
                paintLampWords (g, box, names[i], l.word);
            }
            else
            {
                g.setFont (juce::FontOptions (juce::jmin (11.0f, box.getHeight() * 0.5f), juce::Font::bold));
                g.drawFittedText (juce::String (names[i]) + "  " + l.word, box.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
            }
        }
    }
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && app.showRecordMonitor) app.showRecordMonitor(); }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int w = (getWidth() - 2 * kGap) / 3;
        const int i = juce::jlimit (0, 2, e.x / juce::jmax (1, w + kGap));
        setTooltip (lamps[(size_t) i].detail + "\n\nClick: the Recording monitor window.");
    }
    static juce::Colour colourFor (int level)
    {
        switch (level)
        {
            case 0:  return juce::Colour (0xff2e9a52);
            case 1:  return juce::Colour (0xffe8b921);
            case 2:  return juce::Colour (0xffd93838);
            default: return theme::row;
        }
    }
private:
    void timerCallback() override { refresh(); }
    void refresh()
    {
        AppContext::Lamp now[3] = { app.mainDriveLamp(), app.secondDriveLamp(), app.deviceLamp() };
        bool changed = false;
        for (int i = 0; i < 3; ++i)
            if (now[i].level != lamps[i].level || now[i].word != lamps[i].word) { changed = true; }
        for (int i = 0; i < 3; ++i) lamps[i] = now[i];
        if (changed) repaint();
    }
    AppContext& app;
    bool big;
    AppContext::Lamp lamps[3];
};

/** The Recording monitor window (3.2.0): the three lamps large, a line for each (what is behind the colour), the take being recorded, and the list of
    the take's events with the clock time of each - overruns and lost time, a file that stopped taking data, the device stopping or restarting, the
    second drive's copies thrown away. The list is kept after Stop until the next take starts, so a glance back shows how the take went. The window
    floats above the others (see Main.cpp: it is opened like the meter bridge). */
class RecordMonitorComponent : public juce::Component, private juce::Timer
{
public:
    explicit RecordMonitorComponent (AppContext& a) : app (a), lamps (a, true)
    {
        addAndMakeVisible (lamps);
        for (auto* l : { &mainLine, &secondLine, &deviceLine, &takeLine })
        {
            addAndMakeVisible (*l);
            l->setColour (juce::Label::textColourId, theme::text);
            l->setFont (juce::FontOptions (12.5f));
            l->setMinimumHorizontalScale (0.6f);
        }
        takeLine.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (events);
        events.setMultiLine (true, false); events.setReadOnly (true); events.setScrollbarsShown (true); events.setCaretVisible (false);
        events.setColour (juce::TextEditor::backgroundColourId, theme::field);
        events.setColour (juce::TextEditor::textColourId, theme::text);
        events.setColour (juce::TextEditor::outlineColourId, theme::border);
        events.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        addAndMakeVisible (copyBtn);
        copyBtn.setTooltip ("Copies the take's name and its events to the clipboard (to paste into a session log or a message)");
        copyBtn.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (takeLine.getText() + "\n" + events.getText()); };
        setSize (560, 380);
        startTimerHz (4);
        refresh();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        lamps.setBounds (r.removeFromTop (54));
        r.removeFromTop (6);
        mainLine.setBounds (r.removeFromTop (20));
        secondLine.setBounds (r.removeFromTop (20));
        deviceLine.setBounds (r.removeFromTop (20));
        r.removeFromTop (8);
        auto head = r.removeFromTop (24);
        copyBtn.setBounds (head.removeFromRight (120));
        takeLine.setBounds (head);
        r.removeFromTop (4);
        events.setBounds (r);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
private:
    void timerCallback() override { refresh(); }
    void refresh()
    {
        const auto& m = app.recordMonitor;
        mainLine.setText ("Main drive:  " + app.mainDriveLamp().detail, juce::dontSendNotification);
        secondLine.setText ("Second drive:  " + app.secondDriveLamp().detail, juce::dontSendNotification);
        deviceLine.setText ("Device:  " + app.deviceLamp().detail, juce::dontSendNotification);
        juce::String take = m.takeName.isEmpty() ? juce::String ("No take has been recorded since the program started") : (m.recording ? "RECORDING  " : "Last take:  ") + m.takeName;
        if (m.recording && m.startedMs != 0)
        {
            const int secs = (int) ((juce::Time::getMillisecondCounter() - m.startedMs) / 1000);
            take << "   " << juce::String (secs / 60) << ":" << juce::String (secs % 60).paddedLeft ('0', 2);
        }
        takeLine.setText (take, juce::dontSendNotification);
        takeLine.setColour (juce::Label::textColourId, m.recording ? juce::Colour (0xffff6b6b) : theme::text);
        if (m.events.size() != shownEvents || (! m.events.empty() && m.events.back().text != lastText))
        {
            juce::String text;
            for (auto& e : m.events) text << e.time << "  " << (e.bad ? "!  " : "   ") << e.text << "\n";
            events.setText (text, false);
            events.moveCaretToEnd();
            shownEvents = m.events.size(); lastText = m.events.empty() ? juce::String() : m.events.back().text;
        }
    }
    AppContext& app;
    RecordLamps lamps;
    juce::Label mainLine, secondLine, deviceLine, takeLine;
    juce::TextEditor events;
    juce::TextButton copyBtn { "Copy events" };
    size_t shownEvents = (size_t) -1; juce::String lastText;
};
} // namespace td
