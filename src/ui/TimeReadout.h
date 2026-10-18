#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Uikit.h"
#include "ValueEntry.h"
#include "../core/TimeFormat.h"
#include <functional>
#include <vector>

namespace td
{
namespace timereadout
{
/** How every time readout in the program writes the time (they all change together: choose Timecode in one and they all show timecode).
    Keep it in the project or the settings with toString() / fromString(). */
class Shared : public juce::ChangeBroadcaster
{
public:
    timefmt::Mode mode = timefmt::Mode::time;
    int    ltcCode = 1;                 // the frame rate for Timecode (see LtcGenerator::formatInfo; 1 = 25 fps)
    double offsetSeconds = 0.0;         // the timecode at the start of the timeline (3600 = 01:00:00:00)

    void set (timefmt::Mode m, int code, double offset)
    {
        if (m == mode && code == ltcCode && offset == offsetSeconds) return;
        mode = m; ltcCode = juce::jlimit (0, 5, code); offsetSeconds = offset;
        sendChangeMessage();
    }
    juce::String toString() const { return juce::String ((int) mode) + "," + juce::String (ltcCode) + "," + juce::String (offsetSeconds, 3); }
    void fromString (const juce::String& s)
    {
        auto p = juce::StringArray::fromTokens (s, ",", "");
        if (p.size() < 3) return;
        set ((timefmt::Mode) juce::jlimit (0, timefmt::kModes - 1, p[0].getIntValue()), p[1].getIntValue(), p[2].getDoubleValue());
    }
};
inline Shared& shared() { static Shared s; return s; }
} // namespace timereadout

/** A clock for the position on the timeline: big digits that stay put as they change (each digit has its own cell), the hours and minutes
    that have not started yet drawn faintly, and a small label that says how the time is written.

    - setPosition (seconds) every time the position moves (it only redraws when the text changes), or let it fetch the position itself with follow().
    - setState tells it whether the transport is stopped, playing or recording (the digits are white, blue or red).
    - Click the small label, or right-click anywhere, to choose how the time is written: hours:minutes:seconds.ms, minutes:seconds.ms, timecode (any
      frame rate, drop-frame too, and a start timecode such as 01:00:00:00) or samples. The choice is shared by every readout in the program.
    - Double-click to type a time (1:30, 1:02:03.5, 2m10s, 01:02:03:04 in Timecode, a number in Samples) and press Enter: onJump gets the seconds.
      With no onJump the double-click does nothing.
    Under 40 pixels high it shows just the digits (no label line); give it 52 or more to have the label and a caption. */
class TimeReadout : public juce::Component, public juce::SettableTooltipClient, private juce::ChangeListener, private juce::Timer
{
public:
    enum class State { stopped, playing, recording };

    TimeReadout()
    {
        timereadout::shared().addChangeListener (this);
    }
    ~TimeReadout() override { timereadout::shared().removeChangeListener (this); stopTimer(); }

    /** Called with the seconds the person typed (double-click). */
    std::function<void (double seconds)> onJump;
    /** Called when the way of writing the time changed, so the window can give the readout a different width (see getIdealWidth). */
    std::function<void()> onFormatChanged;

    void setPosition (double seconds)
    {
        if (! std::isfinite (seconds)) seconds = 0.0;
        const auto before = timefmt::format (currentFormat(), position).text;
        position = seconds;
        if (timefmt::format (currentFormat(), position).text != before) repaint();
    }
    double getPosition() const noexcept { return position; }

    void setState (State s) { if (s != state) { state = s; repaint(); } }
    State getState() const noexcept { return state; }

    /** For Samples. */
    void setSampleRate (double rate) { if (rate > 0.0 && rate != sampleRate) { sampleRate = rate; repaint(); } }

    /** A few words in the top right corner of the readout ("Edit 1", "of 12:34"). */
    void setCaption (const juce::String& text) { if (text != caption) { caption = text; repaint(); } }

    /** The readout fetches the position (and the state) itself, 'hz' times a second, until stopFollowing(). */
    void follow (std::function<double()> positionSeconds, std::function<State()> stateNow = {}, int hz = 30)
    {
        getPosition_ = std::move (positionSeconds); getState_ = std::move (stateNow);
        startTimerHz (juce::jlimit (1, 60, hz));
        timerCallback();
    }
    void stopFollowing() { stopTimer(); getPosition_ = nullptr; getState_ = nullptr; }

    /** The width that fits the digits of the current way of writing the time, at this height. */
    int getIdealWidth (int height) const
    {
        const auto m = metrics ({ 0, 0, 100000, height });
        return juce::roundToInt (m.widestWidth) + 2 * kPadX;
    }

    void paint (juce::Graphics& g) override
    {
        const auto fmtNow = currentFormat();
        const auto b = getLocalBounds().toFloat();
        const bool bad = badFlash;
        g.setColour (theme::field);
        g.fillRoundedRectangle (b.reduced (0.5f), 4.0f);
        g.setColour (bad ? theme::playhead : theme::border);
        g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, bad ? 2.0f : 1.0f);

        const auto m = metrics (getLocalBounds());
        if (! m.compact)
        {
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.setColour (badgeHover ? theme::text : theme::dimText.withAlpha (0.85f));
            g.drawText (badgeText (fmtNow), m.badge, juce::Justification::centredLeft, false);
            if (caption.isNotEmpty())
            {
                g.setFont (juce::Font (juce::FontOptions (9.5f)));
                g.setColour (theme::dimText.withAlpha (0.85f));
                g.drawText (caption, m.captionArea, juce::Justification::centredRight, true);
            }
        }

        // the digits: each in a cell of its own width, so nothing jiggles as they change
        const auto t = timefmt::format (fmtNow, position);
        float fontH = m.fontHeight, cellW = m.cellW;
        float total = widthOf (t.text, cellW);
        float boxW = m.widestWidth;
        if (total > boxW && total > 0.0f)                       // longer than the readout was made for (100+ minutes, ten digits of samples): smaller digits
        {
            const float k = juce::jmin (1.0f, m.digits.getWidth() / total) ;
            fontH *= k; cellW *= k; total *= k; boxW = total;
        }
        const juce::Font f (juce::FontOptions (fontH, juce::Font::bold));
        const auto cells = cellWidths (t.text, cellW);
        float x = m.digits.getCentreX() - boxW * 0.5f + (boxW - total);    // the right edge stays put when the text is shorter than the widest
        const auto ink = state == State::recording ? theme::playhead : state == State::playing ? theme::accent.brighter (0.55f) : theme::text;
        g.setFont (f);
        for (int i = 0; i < t.text.length(); ++i)
        {
            const bool faint = i >= t.dimFrom && i < t.dimTo;
            g.setColour (faint ? ink.withAlpha (0.28f) : ink);
            // the rectangle is wider than the cell: drawText leaves out a glyph that does not fit its box, and a colon is wider than its cell
            const float cx = x + cells[(size_t) i] * 0.5f, half = cellW * 1.5f;
            g.drawText (t.text.substring (i, i + 1), juce::Rectangle<float> (cx - half, m.digits.getY(), half * 2.0f, m.digits.getHeight()), juce::Justification::centred, false);
            x += cells[(size_t) i];
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) showMenu();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && e.mouseWasClicked() && badgeHit (e.getPosition())) cycleMode();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onJump && ! badgeHit (e.getPosition())) askForTime();
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = badgeHit (e.getPosition());
        if (over != badgeHover) { badgeHover = over; setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor); repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        if (badgeHover) { badgeHover = false; setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); }
    }

private:
    static constexpr int kPadX = 8, kPadY = 3, kStripH = 13;

    struct Metrics
    {
        bool compact = true;
        juce::Rectangle<int> badge, captionArea;
        juce::Rectangle<float> digits;
        float fontHeight = 20.0f, cellW = 12.0f, widestWidth = 100.0f;
    };

    timefmt::Format currentFormat() const
    {
        auto& s = timereadout::shared();
        timefmt::Format f; f.mode = s.mode; f.ltcCode = s.ltcCode; f.offsetSeconds = s.offsetSeconds; f.sampleRate = sampleRate;
        return f;
    }

    static juce::String badgeText (const timefmt::Format& f)
    {
        switch (f.mode)
        {
            case timefmt::Mode::time:     return "H : M : S . MS";
            case timefmt::Mode::minSec:   return "MIN : SEC";
            case timefmt::Mode::timecode: return "TIMECODE  " + fpsName (f.fps(), f.drop());
            case timefmt::Mode::samples:  return "SAMPLES";
        }
        return {};
    }

    /** The width of a digit in this font: the widest of the ten, so every digit fits its cell. */
    static float digitWidth (const juce::Font& f)
    {
        float w = 0.0f;
        for (juce::juce_wchar c = '0'; c <= '9'; ++c) w = juce::jmax (w, juce::GlyphArrangement::getStringWidth (f, juce::String::charToString (c)));
        return w;
    }
    static float cellFor (juce::juce_wchar c, float digitW)
    {
        if (c >= '0' && c <= '9') return digitW;
        if (c == '-') return digitW * 0.62f;
        if (c == ' ') return digitW * 0.42f;
        return digitW * 0.40f;                 // ":" ";" "."
    }
    static std::vector<float> cellWidths (const juce::String& text, float digitW)
    {
        std::vector<float> w;
        for (int i = 0; i < text.length(); ++i) w.push_back (cellFor (text[i], digitW));
        return w;
    }
    static float widthOf (const juce::String& text, float digitW)
    {
        float w = 0.0f;
        for (int i = 0; i < text.length(); ++i) w += cellFor (text[i], digitW);
        return w;
    }

    /** Where the label line and the digits go. */
    static Metrics rects (juce::Rectangle<int> bounds)
    {
        Metrics m;
        m.compact = bounds.getHeight() < 40;
        auto inner = bounds.reduced (kPadX, kPadY);
        if (! m.compact)
        {
            auto strip = inner.removeFromTop (kStripH);
            m.badge = strip.removeFromLeft (juce::jmin (strip.getWidth(), 112));
            m.captionArea = strip;
        }
        m.digits = inner.toFloat();
        return m;
    }

    /** The rectangles, and the biggest digits that fit. */
    Metrics metrics (juce::Rectangle<int> bounds) const
    {
        Metrics m = rects (bounds);
        // the biggest digits that fit the height; shrink if the widest text of this mode would not fit the width
        float fh = juce::jmax (8.0f, m.digits.getHeight() * 0.92f);
        const auto widest = timefmt::widestText (currentFormat());
        float dw = digitWidth (juce::Font (juce::FontOptions (fh, juce::Font::bold)));
        float ww = widthOf (widest, dw);
        if (ww > m.digits.getWidth() && m.digits.getWidth() > 0.0f)
        {
            const float k = m.digits.getWidth() / ww;
            fh = juce::jmax (8.0f, fh * k);
            dw = digitWidth (juce::Font (juce::FontOptions (fh, juce::Font::bold)));
            ww = widthOf (widest, dw);
        }
        m.fontHeight = fh; m.cellW = dw; m.widestWidth = ww;
        return m;
    }

    bool badgeHit (juce::Point<int> p) const
    {
        const auto m = rects (getLocalBounds());
        return ! m.compact && m.badge.contains (p);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        repaint();
        if (onFormatChanged) onFormatChanged();
    }

    void timerCallback() override
    {
        if (getPosition_) setPosition (getPosition_());
        if (getState_) setState (getState_());
    }

    juce::String getTooltip() override
    {
        return juce::String ("The position on the timeline, written as ") + timefmt::modeName (timereadout::shared().mode)
               + ". Right-click to choose how it is written (or click the small label above the digits to go through the choices)"
               + (onJump ? ". Double-click to type a time and go there (1:30, 1:02:03.5 or 2m10s; in Timecode 01:02:03:04; in Samples a number)." : ".");
    }

    void cycleMode()
    {
        auto& s = timereadout::shared();
        s.set ((timefmt::Mode) (((int) s.mode + 1) % timefmt::kModes), s.ltcCode, s.offsetSeconds);
    }

    void showMenu()
    {
        auto& s = timereadout::shared();
        enum { modeBase = 1, rateBase = 20, startId = 40, copyId = 50, goId = 51 };
        juce::PopupMenu m;
        m.addSectionHeader ("Show the position as");
        for (int i = 0; i < timefmt::kModes; ++i)
            m.addItem (modeBase + i, timefmt::modeName ((timefmt::Mode) i), true, (int) s.mode == i);
        m.addSeparator();
        juce::PopupMenu rates;
        for (int code : { 1, 0, 2, 3, 4, 5 })              // 25, 24, 29.97 drop, 29.97, 30, 23.976
            rates.addItem (rateBase + code, LtcGenerator::formatName (code), true, s.ltcCode == code);
        m.addSubMenu ("Timecode frame rate", rates);
        auto tmp = currentFormat(); tmp.mode = timefmt::Mode::timecode; tmp.offsetSeconds = 0.0;
        m.addItem (startId, "Timecode at the start of the timeline: " + timefmt::format (tmp, s.offsetSeconds).text + "...");
        m.addSeparator();
        m.addItem (copyId, "Copy " + timefmt::format (currentFormat(), position).text);
        if (onJump) m.addItem (goId, "Go to a time...");

        juce::Component::SafePointer<TimeReadout> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& sh = timereadout::shared();
            if (r >= modeBase && r < modeBase + timefmt::kModes) sh.set ((timefmt::Mode) (r - modeBase), sh.ltcCode, sh.offsetSeconds);
            else if (r >= rateBase && r < rateBase + 6) sh.set (sh.mode, r - rateBase, sh.offsetSeconds);
            else if (r == startId) safe->askForStartTimecode();
            else if (r == copyId) juce::SystemClipboard::copyTextToClipboard (timefmt::format (safe->currentFormat(), safe->position).text);
            else if (r == goId) safe->askForTime();
        });
    }

    void askForTime()
    {
        if (! onJump) return;
        const auto f = currentFormat();
        const juce::String caption_ = f.mode == timefmt::Mode::timecode ? "Go to (01:02:03:04 or 1:30)"
                                    : f.mode == timefmt::Mode::samples  ? "Go to (a sample number or 1:30)"
                                                                        : "Go to (1:30, 1:02:03.5, 2m10s)";
        juce::Component::SafePointer<TimeReadout> safe (this);
        valueentry::ask (*this, caption_, timefmt::format (f, position).text, [safe] (const juce::String& text)
        {
            if (safe == nullptr) return;
            const auto s = timefmt::parse (safe->currentFormat(), text);
            if (s.has_value() && safe->onJump) safe->onJump (juce::jmax (0.0, *s));
            else safe->flashBad();
        });
    }

    void askForStartTimecode()
    {
        auto tmp = currentFormat(); tmp.mode = timefmt::Mode::timecode; tmp.offsetSeconds = 0.0;
        juce::Component::SafePointer<TimeReadout> safe (this);
        valueentry::ask (*this, "Start timecode (hh:mm:ss:ff)", timefmt::format (tmp, timereadout::shared().offsetSeconds).text,
                         [safe] (const juce::String& text)
        {
            if (safe == nullptr) return;
            auto t = safe->currentFormat(); t.mode = timefmt::Mode::timecode; t.offsetSeconds = 0.0;
            const auto s = timefmt::parse (t, text);
            auto& sh = timereadout::shared();
            if (s.has_value() && *s >= 0.0 && *s < 24.0 * 3600.0) sh.set (sh.mode, sh.ltcCode, *s);
            else safe->flashBad();
        });
    }

    /** The outline goes red for a moment: what was typed could not be read as a time. */
    void flashBad()
    {
        badFlash = true; repaint();
        juce::Component::SafePointer<TimeReadout> safe (this);
        juce::Timer::callAfterDelay (500, [safe] { if (safe != nullptr) { safe->badFlash = false; safe->repaint(); } });
    }

    double position = 0.0, sampleRate = 48000.0;
    State state = State::stopped;
    juce::String caption;
    bool badgeHover = false, badFlash = false;
    std::function<double()> getPosition_;
    std::function<State()> getState_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeReadout)
};
} // namespace td
