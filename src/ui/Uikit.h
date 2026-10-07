#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../core/AudioEngine.h"
#include <map>
#include <array>
#include <algorithm>

namespace td
{
/** Colours used everywhere: a dark console look (the same greys as the mixer) with the logo's teal as the accent and fine outlines. */
namespace theme
{
// Plain variables (not constants): switching between the dark and the light look changes them, and everything is drawn from them.
inline juce::Colour window   { 0xff1b1e23 };     // the logo's ground
inline juce::Colour panel    { 0xff2b2f36 };
inline juce::Colour row      { 0xff23272d };
inline juce::Colour rowAlt   { 0xff1f2328 };
inline juce::Colour field    { 0xff15171b };
inline juce::Colour wave     { 0xff181b20 };     // waveform backgrounds
inline juce::Colour waveAlt  { 0xff1e2227 };
inline juce::Colour ruler    { 0xff2b2f36 };
inline juce::Colour grid     { 0xff3b414b };
inline juce::Colour selected { 0xff2b5a70 };
inline juce::Colour text     { 0xffe9edf2 };     // the logo's ink
inline juce::Colour dimText  { 0xff9ba5b4 };
inline juce::Colour border   { 0xff59606c };     // the 1-pixel outline around every control: what makes the interface look crisp
inline juce::Colour accent   { 0xff2a8fb0 };     // the logo's teal: "on", selected, the value part of sliders
inline juce::Colour button   { 0xff373d47 };     // a button at rest
inline juce::Colour warn     { 0xffe0a040 };     // warnings
inline juce::Colour playhead { 0xffff4a4a };
inline juce::Colour wellDark { 0xff0c0e11 };     // the dark wells of the level meters

inline bool& darkFlag() { static bool d = true; return d; }
inline bool isDark() { return darkFlag(); }

/** All the palette colours in a fixed order (for switching and for re-colouring what was coloured by hand). */
inline std::array<juce::Colour*, 18> all()
{
    return { &window, &panel, &row, &rowAlt, &field, &wave, &waveAlt, &ruler, &grid, &selected, &text, &dimText, &border, &accent, &button, &warn, &playhead, &wellDark };
}
inline std::array<juce::uint32, 18> snapshot() { std::array<juce::uint32, 18> a {}; auto c = all(); for (size_t i = 0; i < a.size(); ++i) a[i] = c[i]->getARGB(); return a; }

/** Sets the palette. The dark one is the logo's console look; the light one has the same structure on light greys. */
inline void setDark (bool dark)
{
    darkFlag() = dark;
    static const juce::uint32 d[18] = { 0xff1b1e23, 0xff2b2f36, 0xff23272d, 0xff1f2328, 0xff15171b, 0xff181b20, 0xff1e2227, 0xff2b2f36, 0xff3b414b,
                                        0xff2b5a70, 0xffe9edf2, 0xff9ba5b4, 0xff59606c, 0xff2a8fb0, 0xff373d47, 0xffe0a040, 0xffff4a4a, 0xff0c0e11 };
    static const juce::uint32 l[18] = { 0xffe6eaf0, 0xfff4f6f9, 0xffedf0f4, 0xffe3e7ed, 0xffffffff, 0xffdfe4ea, 0xffd6dce4, 0xffd2d8e0, 0xffbcc4cf,
                                        0xffb5d6e6, 0xff14181f, 0xff4a5566, 0xff7b8696, 0xff1f7f9f, 0xffdfe4eb, 0xffb36b00, 0xffd62828, 0xff0c0e11 };
    auto c = all();
    for (size_t i = 0; i < c.size(); ++i) *c[i] = juce::Colour (dark ? d[i] : l[i]);
}

/** The widget look. */
inline juce::LookAndFeel_V4::ColourScheme scheme()
{
    return juce::LookAndFeel_V4::ColourScheme (window, panel, field, border, text, accent, juce::Colours::white, accent, text);
}
} // namespace theme

/** One grid for every row of buttons, so they are all the same size and line up. */
namespace grid
{
constexpr int btnH = 24, gap = 6, btnW = 116, rowH = btnH + 8;
/** Column 'col' (0-based) of a row; 'span' columns wide. */
inline juce::Rectangle<int> cell (juce::Rectangle<int> row, int col, int span = 1)
{
    return { row.getX() + col * (btnW + gap), row.getY() + (row.getHeight() - btnH) / 2, span * btnW + (span - 1) * gap, btnH };
}

/** One thing in a toolbar that flows: its width, its height and the space after it. */
struct FlowItem
{
    juce::Component* c;
    int w = btnW, h = btnH, gapAfter = gap;
};
/** Puts the items on as few lines as possible, left to right, and starts a new line when the width runs out (so a wide window has one
    line and a narrow one spills over onto two or three). Returns the height used. */
inline int flow (juce::Rectangle<int> area, const std::vector<FlowItem>& items)
{
    std::vector<std::vector<const FlowItem*>> lines (1);
    int x = 0;
    for (auto& it : items)
    {
        if (x > 0 && x + it.w > area.getWidth()) { lines.emplace_back(); x = 0; }
        lines.back().push_back (&it);
        x += it.w + it.gapAfter;
    }
    int y = area.getY();
    for (auto& line : lines)
    {
        int lh = rowH;
        for (auto* it : line) lh = juce::jmax (lh, it->h + 8);
        int px = area.getX();
        for (auto* it : line)
        {
            it->c->setBounds (px, y + (lh - it->h) / 2, it->w, it->h);
            px += it->w + it->gapAfter;
        }
        y += lh;
    }
    return y - area.getY();
}
}


/** Channel colours: every audio track, Int Bus and Ext Bus has one. It tints the mixer strip and the track in the take and edit windows. */
namespace chan
{
inline const std::vector<juce::uint32>& palette()
{
    static const std::vector<juce::uint32> p {
        0xff3f7fc4, 0xff2f9bbd, 0xff2f9e8f, 0xff4fa24a, 0xff93b43a, 0xffd9b327, 0xffe0842b, 0xffd2503f,
        0xffd2579a, 0xffa855b8, 0xff7a5bc4, 0xff4f62c4, 0xff6d7f96, 0xff9c6c45, 0xff8b9099, 0xff3b4352 };
    return p;
}

/** The colour a channel has when you have not picked one: audio tracks blue, Int Buses violet, Ext Buses red. */
inline juce::Colour automaticFor (NodeKind k)
{
    return juce::Colour (k == NodeKind::IntBus ? 0xff7a5bc4 : k == NodeKind::ExtBus ? 0xffc9503f : 0xff3f7fc4);
}

inline juce::Colour of (const Project& p, const juce::Uuid& id)
{
    const auto c = p.channelColour (id);
    return c != 0 ? juce::Colour (c) : automaticFor (p.kindOf (id));
}

/** Black or white, whichever reads better on 'c'. */
inline juce::Colour textOn (juce::Colour c) { return c.getPerceivedBrightness() > 0.62f ? juce::Colour (0xff10141a) : juce::Colours::white; }

/** The waveform colour on a clip: the channel's colour, lightened so it stands out from the dark clip. */
inline juce::Colour waveColour (juce::Colour c) { return c.interpolatedWith (juce::Colours::white, 0.38f); }

/** The dark fill of a clip on a track: the track's colour, deepened so the light waveform shows clearly on it. */
inline juce::Colour clipFill (juce::Colour c, bool selected = false)
{
    return c.withMultipliedSaturation (0.9f).withMultipliedBrightness (selected ? 0.34f : 0.52f);
}
} // namespace chan

/** The Fermata mark: a fermata arc with a dot underneath (the logo's own curve, scaled into 'area'). */
inline void drawFermataLogo (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour ink, juce::Colour dot)
{
    juce::Path p;
    p.startNewSubPath (20.0f, 100.0f);
    p.cubicTo (20.0f, 20.0f, 220.0f, 20.0f, 220.0f, 100.0f);
    p.cubicTo (205.0f, 56.0f, 35.0f, 56.0f, 20.0f, 100.0f);
    p.closeSubPath();
    const auto box = juce::Rectangle<float> (0.0f, 0.0f, 240.0f, 120.0f);
    const auto t = juce::RectanglePlacement (juce::RectanglePlacement::centred).getTransformToFit (box, area);
    g.setColour (ink);   g.fillPath (p, t);
    g.setColour (dot);   g.fillEllipse (juce::Rectangle<float> (110.0f, 84.0f, 20.0f, 20.0f).transformedBy (t));
}

/** The logo as a square image (window / task bar / program icon): the mark on the logo's dark ground. */
inline juce::Image makeFermataIcon (int size)
{
    juce::Image img (juce::Image::ARGB, size, size, true);
    juce::Graphics g (img);
    g.setColour (theme::window);
    g.fillRoundedRectangle (0.0f, 0.0f, (float) size, (float) size, (float) size * 0.2f);
    g.setColour (theme::border.withAlpha (0.9f));
    g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size).reduced ((float) size * 0.01f), (float) size * 0.2f, juce::jmax (1.0f, (float) size * 0.012f));
    drawFermataLogo (g, juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size).reduced ((float) size * 0.12f), theme::text, theme::accent);
    return img;
}

/** The icon of a saved project file: a sheet of paper with a folded corner and the logo on it. */
inline juce::Image makeFermataFileIcon (int size)
{
    juce::Image img (juce::Image::ARGB, size, size, true);
    juce::Graphics g (img);
    const float s = (float) size, fold = s * 0.26f;
    juce::Rectangle<float> page (s * 0.16f, s * 0.05f, s * 0.68f, s * 0.90f);
    juce::Path p;
    p.startNewSubPath (page.getX(), page.getY());
    p.lineTo (page.getRight() - fold, page.getY());
    p.lineTo (page.getRight(), page.getY() + fold);
    p.lineTo (page.getRight(), page.getBottom());
    p.lineTo (page.getX(), page.getBottom());
    p.closeSubPath();
    g.setColour (juce::Colours::black.withAlpha (0.25f)); g.fillPath (p, juce::AffineTransform::translation (s * 0.012f, s * 0.018f));
    g.setColour (juce::Colour (0xfff4f6f9)); g.fillPath (p);
    g.setColour (juce::Colour (0xff9aa3b0)); g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, s * 0.012f)));
    juce::Path corner;                                                       // the folded corner
    corner.startNewSubPath (page.getRight() - fold, page.getY());
    corner.lineTo (page.getRight() - fold, page.getY() + fold);
    corner.lineTo (page.getRight(), page.getY() + fold);
    corner.closeSubPath();
    g.setColour (juce::Colour (0xffd5dbe3)); g.fillPath (corner);
    g.setColour (juce::Colour (0xff9aa3b0)); g.strokePath (corner, juce::PathStrokeType (juce::jmax (1.0f, s * 0.012f)));
    drawFermataLogo (g, juce::Rectangle<float> (page.getX() + page.getWidth() * 0.08f, page.getY() + page.getHeight() * 0.36f, page.getWidth() * 0.84f, page.getHeight() * 0.38f),
                     juce::Colour (0xff1b1e23), theme::accent);
    g.setColour (juce::Colour (0xffb9c1cc));                                 // a few lines of "writing" under the logo
    for (int i = 0; i < 3; ++i)
        g.fillRoundedRectangle (page.getX() + page.getWidth() * 0.14f, page.getY() + page.getHeight() * (0.80f + 0.055f * (float) i), page.getWidth() * (i == 2 ? 0.40f : 0.72f), s * 0.014f, s * 0.007f);
    return img;
}

/** Peak meter, -60 dB .. 0 dB. Only the top of the bar changes colour: green, then orange from -9 dB, red from -2 dB.
    The peak marker holds for 10 seconds, unless the peak went above -2 dBFS: then it stays until resetPeak() is called. */
/** How the mixer's level meters behave (shared by all mixers, remembered between runs): what they show and how fast they move. */
struct MeterSettings
{
    int mode = 0;      // 0 peak, 1 RMS, 2 both (RMS bar inside the peak bar)
    int rise = 0;      // 0 instant .. 3 slow
    int fall = 1;      // 0 fast .. 3 very slow
    int hold = 3;      // index into holdMs
    static float riseCoef (int i) noexcept { static const float v[] { 1.0f, 0.7f, 0.4f, 0.15f }; return v[juce::jlimit (0, 3, i)]; }
    static float fallCoef (int i) noexcept { static const float v[] { 0.70f, 0.82f, 0.92f, 0.97f }; return v[juce::jlimit (0, 3, i)]; }
    static juce::uint32 holdFor (int i) noexcept { static const juce::uint32 v[] { 0, 1000, 3000, 10000, 30000, 0xffffffffu }; return v[juce::jlimit (0, 5, i)]; }
};
inline MeterSettings& meterSettings() { static MeterSettings s; return s; }

class LevelMeter : public juce::Component
{
public:
    static constexpr float kRedDb = -2.0f, kOrangeDb = -9.0f, kFloorDb = -60.0f;
    static constexpr juce::uint32 kHoldMs = 10000;

    explicit LevelMeter (bool vertical = true) : isVertical (vertical) {}

    static float frac (float db) noexcept { return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (0.0f - kFloorDb)); }

    /** Where a level sits on a mixer fader's travel (-80 .. +12 dB, with -20 dB at the middle): the same mapping as the fader,
        so one dB scale serves both and the meter lines up with the fader's markings. */
    static float faderFrac (float db) noexcept
    {
        static const double skew = std::log (0.5) / std::log (60.0 / 92.0);
        return (float) std::pow (juce::jlimit (0.0, 1.0, ((double) db + 80.0) / 92.0), skew);
    }
    /** Makes this meter use the fader's scale (it then reaches up to +12 dB). */
    void useFaderScale() noexcept { faderScale = true; }
    float pos (float db) const noexcept { return faderScale ? faderFrac (db) : frac (db); }

    void setLevel (float linearPeak, float linearRms = 0.0f)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const auto& ms = meterSettings();
        const float rc = faderScale ? MeterSettings::riseCoef (ms.rise) : 1.0f, fc = faderScale ? MeterSettings::fallCoef (ms.fall) : 0.82f;
        const juce::uint32 holdMs = faderScale ? MeterSettings::holdFor (ms.hold) : kHoldMs;
        auto follow = [&] (float& v, float target)
        {
            if (target > v) v += (target - v) * rc;
            else            v = v * fc + target * (1.0f - fc);
        };
        follow (level, linearPeak);
        follow (rmsLevel, linearRms);
        if (linearPeak > juce::Decibels::decibelsToGain (kRedDb)) latched = true;
        if (linearPeak >= hold) { hold = linearPeak; holdTime = now; }
        else if (! latched && holdMs != 0xffffffffu && now - holdTime > holdMs) { hold = linearPeak; holdTime = now; }
        repaint();
    }

    /** The 'Reset peaks' button. */
    void resetPeak() { hold = 0.0f; latched = false; holdTime = juce::Time::getMillisecondCounter(); level = 0.0f; rmsLevel = 0.0f; repaint(); }
    float getHoldDb() const noexcept { return juce::Decibels::gainToDecibels (hold, -100.0f); }
    bool isLatched() const noexcept { return latched; }

    static const std::initializer_list<int>& scaleDbs() { static const std::initializer_list<int> l { 0, -3, -6, -12, -20, -30, -40, -60 }; return l; }
    static const std::initializer_list<int>& faderScaleDbs() { static const std::initializer_list<int> l { 12, 6, 0, -6, -12, -20, -30, -40, -60 }; return l; }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (theme::wellDark);
        g.fillRect (b);
        g.setColour (theme::border); g.drawRect (getLocalBounds(), 1);
        const int mode = faderScale ? meterSettings().mode : 0;
        const float h = pos (getHoldDb());
        const float fo = pos (kOrangeDb), fr = pos (kRedDb);
        auto bar = [&] (float lin, juce::Rectangle<float> area, float alpha)
        {
            const float f = pos (juce::Decibels::gainToDecibels (lin, faderScale ? -80.0f : kFloorDb));
            auto zone = [&] (float from, float to, juce::Colour c)
            {
                to = juce::jmin (to, f);
                if (to <= from) return;
                g.setColour (c.withAlpha (alpha));
                if (isVertical) g.fillRect (area.getX(), area.getBottom() - area.getHeight() * to, area.getWidth(), area.getHeight() * (to - from));
                else            g.fillRect (area.getX() + area.getWidth() * from, area.getY(), area.getWidth() * (to - from), area.getHeight());
            };
            zone (0.0f, fo, juce::Colour (0xff35c46b));
            zone (fo, fr, juce::Colour (0xffff9a1f));
            zone (fr, 1.0f, juce::Colour (0xffe8281e));
        };
        if (mode == 0)      bar (level, b, 1.0f);
        else if (mode == 1) bar (rmsLevel, b, 1.0f);
        else
        {
            bar (level, b, 0.42f);                                                   // the peak, dim; the RMS in the middle of it, bright
            bar (rmsLevel, isVertical ? b.reduced (b.getWidth() * 0.27f, 0.0f) : b.reduced (0.0f, b.getHeight() * 0.27f), 1.0f);
        }

        if (isVertical && b.getHeight() > 40.0f)                                    // fine segment lines: the bar reads like an LED ladder
        {
            g.setColour (theme::wellDark.withAlpha (0.85f));
            for (float y = b.getBottom() - 2.0f; y > b.getY(); y -= 3.0f) g.fillRect (b.getX(), y, b.getWidth(), 1.0f);
        }
        g.setColour (juce::Colours::black.withAlpha (0.35f));                      // scale ticks across the bar
        for (int db : (faderScale ? faderScaleDbs() : scaleDbs()))
        {
            const float t = pos ((float) db);
            if (isVertical) g.fillRect (b.getX(), b.getBottom() - b.getHeight() * t - 0.5f, b.getWidth(), 1.0f);
            else            g.fillRect (b.getX() + b.getWidth() * t - 0.5f, b.getY(), 1.0f, b.getHeight());
        }
        if (hold > 0.0f)
        {
            g.setColour (latched ? juce::Colour (0xffff6a5a) : juce::Colours::white);
            if (isVertical) g.fillRect (b.getX(), b.getBottom() - b.getHeight() * h - 1.0f, b.getWidth(), 2.0f);
            else            g.fillRect (b.getX() + b.getWidth() * h - 1.0f, b.getY(), 2.0f, b.getHeight());
        }
    }

private:
    bool isVertical;
    bool faderScale = false;
    float level = 0.0f, rmsLevel = 0.0f, hold = 0.0f;
    bool latched = false;
    juce::uint32 holdTime = 0;
};

/** The dB numbers next to a vertical meter. Give it the same top and bottom as the meter. */
class MeterScale : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setFont (9.0f);
        for (int db : LevelMeter::scaleDbs())
        {
            const float y = b.getBottom() - b.getHeight() * LevelMeter::frac ((float) db);
            g.setColour (theme::dimText.brighter (0.3f));
            g.drawText (juce::String (db), juce::Rectangle<float> (b.getX(), y - 6.0f, b.getWidth() - 1.0f, 12.0f), juce::Justification::centredRight, false);
        }
    }
};

inline juce::String formatTime (double seconds)
{
    const auto ms = (juce::int64) (seconds * 1000.0);
    return juce::String::formatted ("%02d:%02d:%02d.%03d", (int) (ms / 3600000), (int) ((ms / 60000) % 60), (int) ((ms / 1000) % 60), (int) (ms % 1000));
}

inline void showError (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message);
}

/** Asks "are you sure?" without blocking; runs onYes only if the person presses the first button. */
inline void confirmAsync (const juce::String& title, const juce::String& message, const juce::String& yesText, std::function<void()> onYes)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle (title).withMessage (message)
                                      .withButton (yesText).withButton ("Cancel"),
                                  [onYes = std::move (onYes)] (int r) { if (r == 1 && onYes) onYes(); });
}

/** Switches the whole program's look (set by the application: see ThemeSwitch.h). */
inline std::function<void (bool dark)>& themeSwitchHook() { static std::function<void (bool)> f; return f; }

/** The little sun / moon button that sits in the top-right corner of every window. */
class ThemeToggleButton : public juce::Button
{
public:
    ThemeToggleButton() : juce::Button ({})
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
        onClick = [] { if (themeSwitchHook()) themeSwitchHook() (! theme::isDark()); };
        setSize (24, 22);
    }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour ((down ? theme::grid : over ? theme::button.brighter (0.15f) : theme::button).withAlpha (0.92f)); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (theme::border); g.drawRoundedRectangle (b, 4.0f, 1.0f);
        g.setColour (theme::text);
        auto c = b.getCentre();
        if (theme::isDark())                                         // dark look on: a moon (click for light)
        {
            juce::Path moon; moon.addEllipse (c.x - 5.5f, c.y - 5.5f, 11.0f, 11.0f);
            juce::Path bite; bite.addEllipse (c.x - 2.0f, c.y - 7.5f, 10.0f, 10.0f);
            moon.setUsingNonZeroWinding (false); moon.addPath (bite);
            g.fillPath (moon);
        }
        else                                                         // light look on: a sun (click for dark)
        {
            g.fillEllipse (c.x - 3.4f, c.y - 3.4f, 6.8f, 6.8f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                g.drawLine (c.x + std::cos (a) * 5.2f, c.y + std::sin (a) * 5.2f, c.x + std::cos (a) * 7.4f, c.y + std::sin (a) * 7.4f, 1.2f);
            }
        }
    }
    void mouseEnter (const juce::MouseEvent& e) override { setTooltip (theme::isDark() ? "Dark look is on. Click for the light look." : "Light look is on. Click for the dark look."); juce::Button::mouseEnter (e); }
};

/** Keeps a ThemeToggleButton in the top-right corner of a window's content area. Call from the window's resized(). */
inline void placeThemeToggle (juce::DocumentWindow& w, ThemeToggleButton& b)
{
    if (b.getParentComponent() != &w) w.addAndMakeVisible (b);
    const auto area = w.getContentComponentBorder().subtractedFrom (w.getLocalBounds());
    b.setTopRightPosition (area.getRight() - 6, area.getY() + 4);
    b.toFront (false);
}

/** A floating window that removes itself when closed. */
/** Keys that work in every window (M = processing mixer, B = meter bridge). Set by the application. */
inline std::function<bool (const juce::KeyPress&)>& globalKeyHook() { static std::function<bool (const juce::KeyPress&)> f; return f; }

/** The windows that were minimised: they are hidden and shown as title bars along the bottom of the main window. */
class DockHub
{
public:
    static DockHub& get() { static DockHub h; return h; }
    void add (juce::DocumentWindow* w)
    {
        remove (w);
        items.push_back (juce::Component::SafePointer<juce::DocumentWindow> (w));
        changes.sendChangeMessage();
    }
    void remove (juce::DocumentWindow* w)
    {
        items.erase (std::remove_if (items.begin(), items.end(), [w] (const auto& p) { return p.getComponent() == nullptr || p.getComponent() == w; }), items.end());
        changes.sendChangeMessage();
    }
    bool contains (juce::DocumentWindow* w) const { for (auto& p : items) if (p.getComponent() == w) return true; return false; }
    /** Brings a window back from the bar (or just to the front if it was never minimised). */
    void show (juce::DocumentWindow* w)
    {
        if (w == nullptr) return;
        remove (w);
        w->setVisible (true);
        w->toFront (true);
    }
    std::vector<juce::DocumentWindow*> windows() const
    {
        std::vector<juce::DocumentWindow*> out;
        for (auto& p : items) if (p.getComponent() != nullptr) out.push_back (p.getComponent());
        return out;
    }
    juce::ChangeBroadcaster changes;
private:
    std::vector<juce::Component::SafePointer<juce::DocumentWindow>> items;
};

/** The area (screen coordinates) of the floating menu bar of the main window, or an empty rectangle when there is none. Tool windows keep their title bar out of it. */
inline std::function<juce::Rectangle<int>()>& menuBarAvoidArea() { static std::function<juce::Rectangle<int>()> f; return f; }
/** Brings the menu bar strip back in front of the other Fermata windows (not in front of other programs). Set by the main window. */
inline std::function<void()>& menuBarRaise() { static std::function<void()> f; return f; }

class ToolWindow : public juce::DocumentWindow
{
public:
    ToolWindow (const juce::String& title, std::function<void (ToolWindow*)> onClose)
        : juce::DocumentWindow (title, theme::window, juce::DocumentWindow::allButtons), closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
    }
    ~ToolWindow() override { DockHub::get().remove (this); }
    void resized() override { juce::DocumentWindow::resized(); placeThemeToggle (*this, themeToggle); keepTitleBarClear(); reportBounds(); }
    void moved() override { juce::DocumentWindow::moved(); keepTitleBarClear(); reportBounds(); }
    /** Set by the application once the window is in place: told the window's position and size whenever the user changes them (kept in the project). */
    std::function<void (const juce::String&)> onBoundsChanged;
    void reportBounds()
    {
        if (onBoundsChanged && ! adjusting && isShowing() && getPeer() != nullptr && ! isMinimised() && ! isFullScreen()) onBoundsChanged (getBounds().toString());
    }
    /** The menu bar floats above every window: if this window's title bar (close / minimise buttons) would end up under it, the window is moved down below the bar.
        A maximised window is un-maximised and fitted to the screen below the bar instead. */
    void keepTitleBarClear()
    {
        if (adjusting || ! isShowing() || getPeer() == nullptr || ! menuBarAvoidArea()) return;
        const auto avoid = menuBarAvoidArea()();
        if (avoid.isEmpty()) return;
        const auto frame = getPeer()->getFrameSizeIfPresent();
        const int titleH = juce::jmax (30, frame ? (*frame).getTop() : 0);
        const auto b = getBounds();
        const juce::Rectangle<int> title (b.getX(), b.getY() - titleH, b.getWidth(), titleH);
        if (! title.intersects (avoid)) return;
        juce::Component::SafePointer<ToolWindow> safe (this);
        juce::MessageManager::callAsync ([safe, avoid, titleH]
        {
            auto* w = safe.getComponent();
            if (w == nullptr) return;
            w->adjusting = true;
            const int top = avoid.getBottom() + titleH;
            if (w->isFullScreen())
            {
                w->setFullScreen (false);
                auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (avoid)->userArea;
                w->setBounds (area.getX(), top, area.getWidth(), juce::jmax (200, area.getBottom() - top));
            }
            else w->setTopLeftPosition (w->getX(), top);
            w->adjusting = false;
        });
    }
    ThemeToggleButton themeToggle;
    bool adjusting = false;
    /** When this window was last the active one (a counter): used to put the windows back in the same order in front of the main window. */
    int lastActive = 0;
    void activeWindowStatusChanged() override
    {
        juce::DocumentWindow::activeWindowStatusChanged();
        static int counter = 0;
        if (isActiveWindow())
        {
            lastActive = ++counter; grabContentFocus();
            if (isTransportWindow) lastTransportWindow() = this;
            if (menuBarRaise()) juce::MessageManager::callAsync ([] { if (menuBarRaise()) menuBarRaise()(); });     // the menu bar stays in front of the Fermata windows
        }
    }
    /** So the keyboard shortcuts (arrow-key zoom, C, Space ...) work as soon as the window is in front, without clicking in it first.
        A text box that already has the keyboard keeps it. */
    void grabContentFocus()
    {
        juce::Component::SafePointer<ToolWindow> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            auto* w = safe.getComponent();
            if (w == nullptr || ! w->isShowing() || ! w->isActiveWindow()) return;      // never pull a window forward just to give it the keyboard
            auto* cur = juce::Component::getCurrentlyFocusedComponent();
            if (cur != nullptr && w->isParentOf (cur) && dynamic_cast<juce::TextEditor*> (cur) != nullptr) return;
            if (auto* c = w->getContentComponent()) if (c->getWantsKeyboardFocus()) c->grabKeyboardFocus();
        });
    }
    void closeButtonPressed() override { if (closeCallback) closeCallback (this); }
    /** Minimising does not send the window to the taskbar: it is hidden and appears as a title bar at the bottom of the main window. */
    void minimisationStateChanged (bool nowMinimised) override
    {
        if (! nowMinimised) return;
        juce::Component::SafePointer<ToolWindow> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* w = safe.getComponent()) { w->setMinimised (false); w->setVisible (false); DockHub::get().add (w); }
        });
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (globalKeyHook() && globalKeyHook() (k)) return true;
        if (forwardKeys)                                  // e.g. a mixer: the keys (Space, R, 1, 2 ...) go on to the take / edit window that was last in use
            if (auto* t = lastTransportWindow().getComponent())
                if (t != this && t->isVisible())
                    if (auto* c = t->getContentComponent()) if (c->keyPressed (k)) return true;
        return juce::DocumentWindow::keyPressed (k);
    }
    bool isTransportWindow = false;                       // a take window or an edit window: it becomes the target of the keys of the windows that forward them
    bool forwardKeys = false;                             // a mixer, meter bridge ...: keys that it does not use itself are passed to the last take / edit window
    static juce::Component::SafePointer<ToolWindow>& lastTransportWindow() { static juce::Component::SafePointer<ToolWindow> p; return p; }
private:
    std::function<void (ToolWindow*)> closeCallback;
};

/** The square "Next Take 4" / "This Take 4" box: green while idle, red while recording. Shown in the take window and on the main page. */
class NextTakeBox : public juce::Component
{
public:
    NextTakeBox() { setInterceptsMouseClicks (false, false); }
    void set (bool isRecording, int takeNumber)
    {
        if (isRecording == recording && takeNumber == number) return;
        recording = isRecording; number = takeNumber; repaint();
    }
    /** Compact: just the colour and the number (for the menu bar). */
    void setCompact (bool c) { compact = c; repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (recording ? juce::Colour (0xffc62828) : juce::Colour (0xff1f8f4e)); g.fillRoundedRectangle (r, compact ? 4.0f : 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.55f)); g.drawRoundedRectangle (r, compact ? 4.0f : 5.0f, 1.0f);
        g.setColour (juce::Colours::white);
        if (compact)
        {
            g.setFont (juce::FontOptions ((float) getHeight() * 0.62f, juce::Font::bold));
            g.drawText (juce::String (number), getLocalBounds(), juce::Justification::centred, false);
            return;
        }
        auto top = getLocalBounds().removeFromTop (juce::jmax (14, getHeight() / 3));
        g.setFont (juce::FontOptions ((float) top.getHeight() * 0.62f, juce::Font::bold));
        g.drawText (recording ? "This Take" : "Next Take", top.withTrimmedTop (3), juce::Justification::centred, false);
        g.setFont (juce::FontOptions ((float) getHeight() * 0.50f, juce::Font::bold));
        g.drawText (juce::String (number), getLocalBounds().withTrimmedTop (top.getHeight()), juce::Justification::centred, false);
    }
private:
    bool recording = false, compact = false; int number = 1;
};

/** The playhead-mode button: a play triangle with a line to its left. Lit = the playhead moves with the time (stays where you stop). */
class PlayheadModeButton : public juce::Button
{
public:
    PlayheadModeButton() : juce::Button ({}) { setClickingTogglesState (false); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? theme::accent : down ? theme::button.brighter (0.2f) : over ? theme::button.brighter (0.1f) : theme::button); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? theme::accent.brighter (0.4f) : theme::border); g.drawRoundedRectangle (b, 4.0f, 1.0f);
        const auto c = b.getCentre();
        g.setColour (on ? juce::Colours::white : theme::text);
        g.fillRect (c.x - 8.0f, c.y - 6.0f, 2.0f, 12.0f);                                        // the line (where playback began)
        juce::Path tri; tri.addTriangle (c.x - 3.5f, c.y - 6.0f, c.x - 3.5f, c.y + 6.0f, c.x + 7.0f, c.y);
        g.fillPath (tri);
    }
};

/** The small blue "i": click it and the explanation of the window or section appears in a box (click anywhere else to close it).
    setText() takes the text, so it can replace a Label that used to show the explanation all the time. */
class InfoNote : public juce::Button
{
public:
    InfoNote() : juce::Button ({}) { setClickingTogglesState (false); setMouseCursor (juce::MouseCursor::PointingHandCursor); setWantsKeyboardFocus (false); setSize (20, 20); setTooltip ("Information"); }
    void setText (const juce::String& t, juce::NotificationType = juce::dontSendNotification) { help = t; setVisible (help.isNotEmpty()); }
    template <typename... A> void setJustificationType (A&&...) {}
    template <typename... A> void setFont (A&&...) {}
    template <typename... A> void setColour (A&&...) {}
    /** Puts the button (20 x 20) at the right-hand end of 'area', vertically centred. */
    void placeRightOf (juce::Rectangle<int> area) { setBounds (area.removeFromRight (22).withSizeKeepingCentre (20, 20)); }
    void placeTopRight (juce::Component& parent, int margin = 4) { setBounds (parent.getWidth() - 20 - margin, margin, 20, 20); toFront (false); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? juce::Colour (0xff1c5cc0) : over ? juce::Colour (0xff4a8af0) : juce::Colour (0xff2f74e0)); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (juce::Colour (0xff9cc4ff)); g.drawRoundedRectangle (b, 4.0f, 1.0f);
        g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("i", getLocalBounds(), juce::Justification::centred, false);
    }
    void clicked() override
    {
        if (help.isEmpty()) return;
        struct Text : juce::Component
        {
            juce::TextLayout layout;
            explicit Text (const juce::String& t)
            {
                juce::AttributedString as; as.setJustification (juce::Justification::topLeft);
                as.append (t, juce::FontOptions (13.5f), juce::Colours::white);
                layout.createLayout (as, 420.0f);
                setSize (452, (int) std::ceil (layout.getHeight()) + 28);
            }
            void paint (juce::Graphics& g) override { layout.draw (g, getLocalBounds().reduced (16, 14).toFloat()); }
        };
        auto box = std::make_unique<Text> (help);
        juce::CallOutBox::launchAsynchronously (std::move (box), getScreenBounds(), nullptr);
    }
private:
    juce::String help;
};

/** The WaveColour button: a small rainbow. Dim = off; click and it brightens and the waveforms are coloured by their sound. */
class RainbowButton : public juce::Button
{
public:
    RainbowButton() : juce::Button ({}) { setClickingTogglesState (false); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? theme::button.brighter (0.35f) : down ? theme::button.brighter (0.2f) : over ? theme::button.brighter (0.1f) : theme::button); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? theme::accent : theme::border); g.drawRoundedRectangle (b, 4.0f, on ? 1.6f : 1.0f);
        const auto c = juce::Point<float> (b.getCentreX(), b.getCentreY() + 5.0f);
        static const juce::uint32 cols[] = { 0xffe53935, 0xfffb8c00, 0xfffdd835, 0xff43a047, 0xff1e88e5, 0xff8e24aa };
        for (int i = 0; i < 6; ++i)
        {
            const float r = 11.5f - 1.7f * (float) i;
            juce::Path arc; arc.addCentredArc (c.x, c.y, r, r, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
            arc.clear(); arc.addCentredArc (c.x, c.y, r, r, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
            g.setColour (juce::Colour (cols[i]).withAlpha (on ? 1.0f : 0.38f));
            g.strokePath (arc, juce::PathStrokeType (1.7f));
        }
    }
};

/** The scroll bars (and the viewport) must never take the keyboard: after using one, the arrow keys still control the zoom / track heights of the window,
    not the scroll bar. Whenever a bar is moved, the keyboard goes back to the timeline. */
struct KeepKeysOnTimeline : private juce::ScrollBar::Listener
{
    KeepKeysOnTimeline (juce::Viewport& v, juce::Component& t) : vp (v), timeline (t)
    {
        vp.setWantsKeyboardFocus (false);
        vp.getHorizontalScrollBar().setWantsKeyboardFocus (false);
        vp.getVerticalScrollBar().setWantsKeyboardFocus (false);
        vp.getHorizontalScrollBar().addListener (this);
        vp.getVerticalScrollBar().addListener (this);
    }
    ~KeepKeysOnTimeline() override { vp.getHorizontalScrollBar().removeListener (this); vp.getVerticalScrollBar().removeListener (this); }
private:
    void scrollBarMoved (juce::ScrollBar*, double) override
    {
        if (timeline.hasKeyboardFocus (false)) return;
        // Only when this window is the one being worked in. A bar that moves by itself (the timeline following the playhead while a take plays) must NEVER take the
        // keyboard: with two windows both scrolling, each would activate itself in turn and they would fight for the front.
        if (juce::TopLevelWindow::getActiveTopLevelWindow() != timeline.getTopLevelComponent()) return;
        if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr) return;     // somebody is typing: leave them
        timeline.grabKeyboardFocus();
    }
    juce::Viewport& vp; juce::Component& timeline;
};
} // namespace td
