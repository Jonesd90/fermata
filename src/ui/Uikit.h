#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../core/AudioEngine.h"
#include "WaveColor.h"
#include <map>
#include <array>
#include <algorithm>

namespace td
{
/** Colours used everywhere: a dark console look (the same greys as the mixer) with the logo's teal as the accent and fine outlines. */
namespace theme
{
// Plain variables (not constants): switching between the dark and the light look changes them, and everything is drawn from them.
inline juce::Colour window   { 0xff566b82 };     // the desk's blue-grey panel (the "dark" look: see setDark)
inline juce::Colour panel    { 0xff4a5e74 };
inline juce::Colour row      { 0xff1d2b3a };
inline juce::Colour rowAlt   { 0xff1a2735 };
inline juce::Colour field    { 0xff05080c };
inline juce::Colour wave     { 0xff0f1821 };     // waveform backgrounds
inline juce::Colour waveAlt  { 0xff131e2a };
inline juce::Colour ruler    { 0xff1b2634 };
inline juce::Colour grid     { 0xff33465a };
inline juce::Colour selected { 0xff2b5f7a };
inline juce::Colour text     { 0xffeef2f6 };
inline juce::Colour dimText  { 0xffc3ccd6 };
inline juce::Colour border   { 0xff2f3e50 };     // the 1-pixel outline around every control: what makes the interface look crisp
inline juce::Colour accent   { 0xff2a9cc0 };     // "on", selected, the value part of sliders
inline juce::Colour button   { 0xff96abc0 };     // a key at rest: lighter than the panel, so it stands out (dark writing on it)
inline juce::Colour warn     { 0xffe0a040 };     // warnings
inline juce::Colour playhead { 0xffff4a4a };
inline juce::Colour wellDark { 0xff0c0e11 };     // the dark wells of the level meters

/** The colour of the desk (Project > Colour theme): 0 blue (the default), 1 red, 2 orange, 3 yellow, 4 green, 5 purple. */
inline int& deskIndex() { static int i = 0; return i; }
/** Moves a blue colour of the desk to the chosen colour (meters, channel colours and other colours are left alone). */
inline juce::Colour deskTint (juce::Colour c, int t)
{
    if (t <= 0) return c;
    static const float hue[] { 0.0f, 0.985f, 0.07f, 0.125f, 0.385f, 0.75f };
    static const float satMul[] { 1.0f, 1.5f, 1.6f, 1.6f, 0.95f, 1.3f };
    t = juce::jlimit (0, 5, t);
    const float h = c.getHue(), s = c.getSaturationHSL(), l = c.getLightness();
    if (h > 0.52f && h < 0.68f && s > 0.08f)
        return juce::Colour::fromHSL (hue[t], juce::jmin (0.6f, s * satMul[t]), l, c.getFloatAlpha());
    return c;
}
inline juce::Colour desk (juce::uint32 argb) { return deskTint (juce::Colour (argb), deskIndex()); }

inline bool& darkFlag() { static bool d = true; return d; }
inline bool isDark() { return darkFlag(); }

/** All the palette colours in a fixed order (for switching and for re-colouring what was coloured by hand). */
inline std::array<juce::Colour*, 18> all()
{
    return { &window, &panel, &row, &rowAlt, &field, &wave, &waveAlt, &ruler, &grid, &selected, &text, &dimText, &border, &accent, &button, &warn, &playhead, &wellDark };
}
inline std::array<juce::uint32, 18> snapshot() { std::array<juce::uint32, 18> a {}; auto c = all(); for (size_t i = 0; i < a.size(); ++i) a[i] = c[i]->getARGB(); return a; }

/** Sets the palette. The "dark" one is the desk: a blue-grey panel (in the colour chosen in the Project menu) with dark displays and waveform lanes and light keys;
    the light one has the same structure on light greys. */
/** Goes up by one whenever the palette changes (dark / light, or another desk colour), so windows can tell they must take the new colours. */
inline int& generation() { static int g = 0; return g; }

inline void setDark (bool dark)
{
    darkFlag() = dark;
    ++generation();
    static const juce::uint32 d[18] = { 0xff566b82, 0xff4a5e74, 0xff1d2b3a, 0xff1a2735, 0xff05080c, 0xff0f1821, 0xff131e2a, 0xff1b2634, 0xff33465a,
                                        0xff2b5f7a, 0xffeef2f6, 0xffc3ccd6, 0xff2f3e50, 0xff2a9cc0, 0xff96abc0, 0xffe0a040, 0xffff4a4a, 0xff0c0e11 };
    static const juce::uint32 l[18] = { 0xffe6eaf0, 0xfff4f6f9, 0xffedf0f4, 0xffe3e7ed, 0xffffffff, 0xffdfe4ea, 0xffd6dce4, 0xffd2d8e0, 0xffbcc4cf,
                                        0xffb5d6e6, 0xff14181f, 0xff4a5566, 0xff7b8696, 0xff1f7f9f, 0xffdfe4eb, 0xffb36b00, 0xffd62828, 0xff0c0e11 };
    auto c = all();
    for (size_t i = 0; i < c.size(); ++i)
    {
        const bool keep = (i == 4 || i == 10 || i == 13 || i == 15 || i == 16 || i == 17);     // field, text, accent, warn, playhead, meter wells: the same in every desk colour
        *c[i] = dark ? (keep ? juce::Colour (d[i]) : desk (d[i])) : juce::Colour (l[i]);
    }
}
/** Dark or light writing for something drawn on 'fill' (the keys are light in the desk look, so their writing and pictures are dark). */
inline juce::Colour inkOn (juce::Colour fill) { return fill.getPerceivedBrightness() > 0.6f ? juce::Colour (0xff14181f) : text; }

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
    return juce::Colour (k == NodeKind::IntBus ? 0xff7a5bc4 : k == NodeKind::ExtBus ? 0xffc9503f : k == NodeKind::Vca ? 0xffd9a21b : 0xff3f7fc4);
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
    // the Pyramix look: a solid, saturated clip colour with a pale waveform (and a thin dark outline) standing on it
    if (solidLook()) return c.withMultipliedSaturation (0.85f).withBrightness (selected ? 0.60f : 0.82f);
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
    g.setColour (juce::Colour (0xff1b1e23));                                 // always the logo's own dark ground, whatever the look
    g.fillRoundedRectangle (0.0f, 0.0f, (float) size, (float) size, (float) size * 0.2f);
    g.setColour (juce::Colour (0xff59606c).withAlpha (0.9f));
    g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size).reduced ((float) size * 0.01f), (float) size * 0.2f, juce::jmax (1.0f, (float) size * 0.012f));
    drawFermataLogo (g, juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size).reduced ((float) size * 0.12f), juce::Colour (0xffe9edf2), juce::Colour (0xff2a8fb0));
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
        g.setColour (theme::inkOn (theme::button));
        auto c = b.getCentre();
        if (theme::isDark())                                         // dark look on: a moon (click for light)
        {
            juce::Path moon; moon.addEllipse (c.x - 5.5f, c.y - 5.5f, 11.0f, 11.0f);
            juce::Path bite; bite.addEllipse (c.x - 1.0f, c.y - 6.5f, 10.0f, 10.0f);
            juce::Path both (moon); both.setUsingNonZeroWinding (false); both.addPath (bite);
            g.saveState(); g.reduceClipRegion (moon);                // inside the moon only: the moon minus the bite is a crescent
            g.fillPath (both);
            g.restoreState();
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
    void mouseEnter (const juce::MouseEvent& e) override { setTooltip (theme::isDark() ? "Desk look is on. Click for the light look." : "Light look is on. Click for the desk look."); juce::Button::mouseEnter (e); }
};

/** Keeps a ThemeToggleButton in the top-right corner of a window's content area. Call from the window's resized(). */
inline void placeThemeToggle (juce::DocumentWindow& w, ThemeToggleButton& b)
{
    if (b.getParentComponent() != &w) w.addAndMakeVisible (b);
    const auto area = w.getContentComponentBorder().subtractedFrom (w.getLocalBounds());
    b.setTopRightPosition (area.getRight() - 6, area.getY() + 4);
    b.toFront (false);
}

/** Puts a clear strip along the top of a window's content for the pin and sun / moon buttons that sit in its top-right corner, so they never cover anything. */
class CornerStripHost : public juce::Component
{
public:
    static constexpr int kStripH = 30;
    explicit CornerStripHost (juce::Component* inner) : content (inner)
    {
        addAndMakeVisible (*content);
        setWantsKeyboardFocus (content->getWantsKeyboardFocus());
        setSize (content->getWidth(), content->getHeight() + kStripH);
    }
    void resized() override { content->setBounds (getLocalBounds().withTrimmedTop (kStripH)); }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
    void focusGained (FocusChangeType) override { content->grabKeyboardFocus(); }
    std::unique_ptr<juce::Component> content;
};

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

/** The little drawing-pin button next to the sun / moon: a pinned window stays in front of the other windows. Only one window is pinned at a time. */
class PinButton : public juce::Button
{
public:
    PinButton() : juce::Button ({})
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
        setClickingTogglesState (false);
        setTooltip ("Pin: keeps this window in front of the others. Only one window can be pinned - pinning another one unpins this one.");
        setSize (24, 22);
    }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour ((on ? juce::Colour (0xff2f7fd8) : down ? theme::grid : over ? theme::button.brighter (0.15f) : theme::button).withAlpha (0.92f)); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? juce::Colour (0xff9fcbff) : theme::border); g.drawRoundedRectangle (b, 4.0f, 1.0f);
        g.setColour (on ? juce::Colours::white : theme::inkOn (theme::button));
        auto c = b.getCentre();
        juce::Path pin;                                           // a drawing pin seen from the side, tilted
        pin.addRoundedRectangle (-4.0f, -7.0f, 8.0f, 3.5f, 1.0f);   // the head
        pin.addRectangle (-2.2f, -3.6f, 4.4f, 5.0f);                // the body
        pin.addRectangle (-0.6f, 1.4f, 1.2f, 5.0f);                 // the needle
        g.fillPath (pin, juce::AffineTransform::rotation (0.6f).translated (c.x, c.y));
    }
};

/** The wood of the desk look: the side panels of every window and the rail along the bottom of the main window. */
namespace wood
{
constexpr int kCheekW = 20;                       // the width of a wooden side panel
constexpr int kBottomH = 16;                      // the wooden strip along the bottom of a window

inline void paintCheek (juce::Graphics& g, juce::Rectangle<int> r, bool leftSide)
{
    juce::ColourGradient gr (juce::Colour (0xff7a4a28), (float) r.getX(), 0.0f, juce::Colour (0xff6b3f20), (float) r.getRight(), 0.0f, false);
    gr.addColour (0.45, juce::Colour (0xff8c5a32));
    g.setGradientFill (gr); g.fillRect (r);
    g.setColour (juce::Colour (0x33321808));
    for (int x = r.getX() + 2; x < r.getRight(); x += 4) g.fillRect (x, r.getY(), 1, r.getHeight());           // the grain
    g.setColour (juce::Colour (0x4dffe1b4)); g.fillRect (leftSide ? r.getRight() - 1 : r.getX(), r.getY(), 1, r.getHeight());   // a lit edge towards the panel
    g.setColour (juce::Colour (0x59000000)); g.fillRect (leftSide ? r.getX() : r.getRight() - 1, r.getY(), 1, r.getHeight());
    if (r.getHeight() < 60) return;
    auto screw = [&] (float cy)
    {
        const float cx = (float) r.getCentreX();
        g.setColour (juce::Colour (0xff8a6a1f)); g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        g.setColour (juce::Colour (0xfff0dc9a)); g.fillEllipse (cx - 2.6f, cy - 2.6f, 5.2f, 5.2f);
        g.setColour (juce::Colour (0xff8a6a1f)); g.drawLine (cx - 2.0f, cy + 1.0f, cx + 2.0f, cy - 1.0f, 1.0f);
    };
    screw ((float) r.getY() + 14.0f);
    screw ((float) r.getBottom() - 14.0f);
}
/** The wooden strip along the bottom of a window: square along the top, rounded at the bottom corners, the grain running along it. */
inline void paintBottom (juce::Graphics& g, juce::Rectangle<int> r, float corner = 6.0f)
{
    juce::Path shape; shape.addRoundedRectangle ((float) r.getX(), (float) r.getY() - corner - 2.0f, (float) r.getWidth(), (float) r.getHeight() + corner + 2.0f, corner);
    g.saveState();
    g.reduceClipRegion (r);
    g.reduceClipRegion (shape);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8c5a32), 0.0f, (float) r.getY(), juce::Colour (0xff6b3f20), 0.0f, (float) r.getBottom(), false));
    g.fillRect (r);
    g.setColour (juce::Colour (0x33321808));
    for (int y = r.getY() + 3; y < r.getBottom(); y += 4) g.fillRect (r.getX(), y, r.getWidth(), 1);
    g.setColour (juce::Colour (0x61ffe1b4)); g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);                  // a lit top edge
    g.setColour (juce::Colour (0x47000000)); g.fillRect (r.getX(), r.getBottom() - 2, r.getWidth(), 2);
    g.restoreState();
}
} // namespace wood

class ToolWindow : public juce::DocumentWindow
{
public:
    ToolWindow (const juce::String& title, std::function<void (ToolWindow*)> onClose)
        : juce::DocumentWindow (title, theme::window, juce::DocumentWindow::allButtons), closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
    }
    ~ToolWindow() override { DockHub::get().remove (this); if (pinnedWindow().getComponent() == this) pinnedWindow() = nullptr; }
    static juce::Component::SafePointer<ToolWindow>& pinnedWindow() { static juce::Component::SafePointer<ToolWindow> p; return p; }
    /** Pins this window and unpins the one that was pinned before; false unpins it. A pinned window stays in front of the other FERMATA windows only -
        it is not "always on top" of the whole desktop, so other programs can cover it like any window. */
    void setPinned (bool on)
    {
        if (on)
        {
            if (auto* old = pinnedWindow().getComponent()) if (old != this) { old->pinTimer.stopTimer(); old->pinButton.setToggleState (false, juce::dontSendNotification); }
            pinnedWindow() = this;
            pinTimer.fn = [this] { keepInFront(); };
            pinTimer.startTimer (250);
            toFront (false);
        }
        else { if (pinnedWindow().getComponent() == this) pinnedWindow() = nullptr; pinTimer.stopTimer(); }
        pinButton.setToggleState (on, juce::dontSendNotification);
    }
    /** While Fermata is the program in use and one of its own windows is active, the pinned window is brought back above it (without taking the keyboard). */
    void keepInFront()
    {
        if (! isVisible() || ! juce::Process::isForegroundProcess()) return;
        if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0) return;
        auto* active = juce::TopLevelWindow::getActiveTopLevelWindow();
        if (active == nullptr || active == this) return;
        auto* dw = dynamic_cast<juce::DocumentWindow*> (active);
        if (dw == nullptr || ! dw->isUsingNativeTitleBar()) return;                  // an alert, a pop-up or a plug-in's own window stays in front
        toFront (false);
    }
    struct PinTimer : public juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } pinTimer;
    void resized() override
    {
        juce::DocumentWindow::resized(); placeThemeToggle (*this, themeToggle);
        if (pinButton.getParentComponent() != this) { addAndMakeVisible (pinButton); pinButton.onClick = [this] { setPinned (! pinButton.getToggleState()); }; }
        pinButton.setTopLeftPosition (themeToggle.getX() - pinButton.getWidth() - 4, themeToggle.getY()); pinButton.toFront (false);
        keepTitleBarClear(); reportBounds();
    }
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
            else
            {
                // moved down below the bar; on a small screen (a laptop) it is also made shorter, so its bottom edge does not go off the screen
                auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (w->getBounds())->userArea;
                const int frameBottom = w->getPeer() != nullptr && w->getPeer()->getFrameSizeIfPresent() ? (*w->getPeer()->getFrameSizeIfPresent()).getBottom() : 8;
                const int h = juce::jmin (w->getHeight(), juce::jmax (200, area.getBottom() - frameBottom - top));
                w->setBounds (w->getX(), top, w->getWidth(), h);
            }
            w->adjusting = false;
        });
    }
    ThemeToggleButton themeToggle;
    PinButton pinButton;
    bool adjusting = false;
    /** Wooden side panels left and right of the content (the desk look only). */
    bool woodSides = true;
    juce::BorderSize<int> getContentComponentBorder() const override
    {
        auto b = juce::DocumentWindow::getContentComponentBorder();
        if (woodSides && theme::isDark()) { b.setLeft (b.getLeft() + wood::kCheekW); b.setRight (b.getRight() + wood::kCheekW); b.setBottom (b.getBottom() + wood::kBottomH); }
        return b;
    }
    void paint (juce::Graphics& g) override
    {
        g.saveState();
        juce::DocumentWindow::paint (g);                  // (it narrows the drawing to the title bar and does not put that back)
        g.restoreState();
        if (! (woodSides && theme::isDark())) return;
        const auto inner = getContentComponentBorder().subtractedFrom (getLocalBounds());
        wood::paintCheek (g, { inner.getX() - wood::kCheekW, inner.getY(), wood::kCheekW, inner.getHeight() }, true);
        wood::paintCheek (g, { inner.getRight(), inner.getY(), wood::kCheekW, inner.getHeight() }, false);
        wood::paintBottom (g, { inner.getX() - wood::kCheekW, inner.getBottom(), inner.getWidth() + 2 * wood::kCheekW, wood::kBottomH });
    }
    /** When this window was last the active one (a counter): used to put the windows back in the same order in front of the main window. */
    int lastActive = 0;
    void activeWindowStatusChanged() override
    {
        juce::DocumentWindow::activeWindowStatusChanged();
        static int counter = 0;
        if (isActiveWindow())
        {
            lastActive = ++counter; grabContentFocus();
            keepTitleBarClear();                                                        // (also here: a window opened or put back where it covers the menu bar)
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

/** The Loop button: two arrows chasing each other round a circle. Grey when off, bright orange when on. */
class LoopButton : public juce::Button
{
public:
    LoopButton() : juce::Button ({}) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        const auto lit = juce::Colour (0xffffa000);
        g.setColour (on ? lit : down ? theme::button.brighter (0.2f) : over ? theme::button.brighter (0.1f) : theme::button); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? lit.brighter (0.5f) : theme::border); g.drawRoundedRectangle (b, 4.0f, on ? 1.6f : 1.0f);
        const auto ink = on ? juce::Colour (0xff1a1a1a) : theme::text.withAlpha (0.85f);
        const auto c = b.getCentre();
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.27f;
        const float pi = juce::MathConstants<float>::pi;
        g.setColour (ink);
        for (int k = 0; k < 2; ++k)                                   // two arcs, each ending in an arrow head, going round the same way
        {
            const float a0 = (float) k * pi + 0.45f, a1 = a0 + pi - 0.9f;
            juce::Path arc; arc.addCentredArc (c.x, c.y, r, r, 0.0f, a0, a1, true);
            g.strokePath (arc, juce::PathStrokeType (1.9f));
            const float tx = c.x + r * std::sin (a1), ty = c.y - r * std::cos (a1);               // the arc's end point
            const float dx = std::cos (a1), dy = std::sin (a1);                                    // the direction of travel there
            const float nx = -dy, ny = dx;
            juce::Path head; head.addTriangle (tx + dx * 4.2f, ty + dy * 4.2f, tx + nx * 3.2f, ty + ny * 3.2f, tx - nx * 3.2f, ty - ny * 3.2f);
            g.fillPath (head);
        }
    }
};

/** The little pictures on the buttons of the edit and take windows. Each is drawn from a 24 x 24 grid so it stays sharp at any size. */
enum class Icon { Start, Rewind, Play, Stop, Forward, End, Record, Trim, Bounce, Pitch, Repair, Reharm, Tracks, Feedback, Mixer, Earlier, Later, Delete, Undo, Automation, Buses,
                  ExportProc, ImportMulti, Markers, FeedbackMarkers, SlipLeft, SlipRight, Shuffle, InsertFlag, NextAtEnd,
                  Overdub, Mark1, Mark2, ToEdit, ToPlayhead, Flag, EditWin, ArmAll, ArmNone, ImportTakes, Search };

inline void drawIcon (juce::Graphics& g, Icon ic, juce::Rectangle<float> area, juce::Colour ink = theme::text)
{
    using juce::Colour; using P = juce::Path;
    const Colour W = ink, Rd (0xffe5484d), Bl (0xff6fb3ff), Ye (0xffe8b923), Dk (0xff2d3340);
    const float sc = juce::jmin (area.getWidth(), area.getHeight()) / 24.0f;
    g.saveState();
    g.addTransform (juce::AffineTransform::scale (sc).translated (area.getCentreX() - 12.0f * sc, area.getCentreY() - 12.0f * sc));
    const auto round = [] (float w) { return juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); };
    auto fill = [&] (const P& p, Colour c) { g.setColour (c); g.fillPath (p); };
    auto stroke = [&] (const P& p, float w, Colour c) { g.setColour (c); g.strokePath (p, round (w)); };
    auto seg = [&] (float x1, float y1, float x2, float y2, float w, Colour c) { P p; p.startNewSubPath (x1, y1); p.lineTo (x2, y2); stroke (p, w, c); };
    auto tri = [&] (float a, float b, float c2, float d, float e, float f, Colour c) { P p; p.addTriangle (a, b, c2, d, e, f); fill (p, c); };
    auto rect = [&] (float x, float y, float w, float h, float r, Colour c) { P p; p.addRoundedRectangle (x, y, w, h, r); fill (p, c); };
    auto clock = [&] (float cx, float cy, float r)
    {
        g.setColour (Dk); g.fillEllipse (cx - r, cy - r, 2 * r, 2 * r);
        g.setColour (W); g.drawEllipse (cx - r, cy - r, 2 * r, 2 * r, 1.5f);
        P h; h.startNewSubPath (cx, cy - 3.6f); h.lineTo (cx, cy); h.lineTo (cx + 2.6f, cy + 1.6f); stroke (h, 1.3f, W);
        P bl; bl.addCentredArc (cx - 4.8f, cy - 5.8f, 2.6f, 2.6f, 0, -2.6f, 0.2f, true); stroke (bl, 1.6f, W);
        P br; br.addCentredArc (cx + 4.8f, cy - 5.8f, 2.6f, 2.6f, 0, -0.2f, 2.6f, true); stroke (br, 1.6f, W);
        seg (cx - 4.5f, cy + 6.0f, cx - 6.1f, cy + 8.2f, 1.4f, W); seg (cx + 4.5f, cy + 6.0f, cx + 6.1f, cy + 8.2f, 1.4f, W);
    };
    auto shout = [&]
    {
        P h; h.startNewSubPath (10, 3.5f); h.cubicTo (6.7f, 3.5f, 4.6f, 5.8f, 4.6f, 8.7f); h.cubicTo (4.6f, 9.9f, 5, 10.7f, 5.4f, 11.5f); h.cubicTo (4.9f, 12.3f, 5.1f, 13, 5.7f, 13.3f);
            h.cubicTo (5.3f, 14.1f, 5.7f, 14.7f, 6.4f, 14.9f); h.cubicTo (6.4f, 15.9f, 7.1f, 16.5f, 8.1f, 16.6f); h.lineTo (8.1f, 19); h.lineTo (13.5f, 19); h.lineTo (13.5f, 16.5f);
            h.cubicTo (15.5f, 15.8f, 16.5f, 14.5f, 16.5f, 12.5f); h.lineTo (16.5f, 8.7f); h.cubicTo (16.5f, 5.8f, 14.1f, 3.5f, 10, 3.5f); h.closeSubPath();
            fill (h, Colour (0xffe0b894)); g.setColour (Colour (0xff9c7a5c)); g.strokePath (h, juce::PathStrokeType (0.6f));
            const Colour br (0xff6a4a30);
            seg (12.4f, 8.3f, 14.6f, 7.5f, 0.9f, br); seg (12.6f, 9.5f, 14.8f, 9.7f, 0.9f, br); seg (7.6f, 8.2f, 9.6f, 9, 0.9f, br); seg (7.8f, 10.2f, 9.4f, 10.2f, 0.9f, br);
            g.setColour (Colour (0xff3a1f1f)); g.fillEllipse (10.4f, 12.1f, 2.8f, 3.8f);
            seg (17, 9.5f, 21, 8, 1.4f, Ye); seg (17.5f, 12.3f, 21.7f, 12.3f, 1.4f, Ye); seg (17, 15, 21, 16.5f, 1.4f, Ye);
    };
    auto arrowH = [&] (float x1, float x2, float y, Colour c, float sw)
    {
        const float d = x2 > x1 ? 1.0f : -1.0f;
        P p; p.startNewSubPath (x1, y); p.lineTo (x2, y); p.startNewSubPath (x2 - d * 2.8f, y - 2.8f); p.lineTo (x2, y); p.lineTo (x2 - d * 2.8f, y + 2.8f); stroke (p, sw, c);
    };
    auto container = [&] (float x, float y, float w, float h)
    {
        const Colour ed (0xff8f3a24);
        P r; r.addRoundedRectangle (x, y, w, h, 1.0f); fill (r, Colour (0xffd9623f)); g.setColour (ed); g.strokePath (r, juce::PathStrokeType (0.7f));
        for (int i = 1; i <= 4; ++i) { const float xx = x + (float) i * w / 5.0f; seg (xx, y + 1.2f, xx, y + h - 1.2f, 0.9f, ed); }
        g.setColour (ed); g.fillRect (x, y + h - 1.4f, w, 1.4f);
    };
    auto clipboard = [&] (float x, float y, float w, float h)
    {
        P r; r.addRoundedRectangle (x, y + 1.2f, w, h - 1.2f, 1.2f); fill (r, Dk); g.setColour (W); g.strokePath (r, juce::PathStrokeType (1.3f));
        rect (x + w / 2 - 1.9f, y, 3.8f, 2.6f, 0.8f, W);
        seg (x + 1.6f, y + 5.2f, x + w - 1.6f, y + 5.2f, 1.0f, W); seg (x + 1.6f, y + 7.6f, x + w - 1.6f, y + 7.6f, 1.0f, W); seg (x + 1.6f, y + 10.0f, x + w - 3.4f, y + 10.0f, 1.0f, W);
    };
    auto highlighter = [&] (const juce::AffineTransform& tr)
    {
        g.saveState(); g.addTransform (tr);
        g.saveState(); g.addTransform (juce::AffineTransform::rotation (juce::degreesToRadians (40.0f), 12.0f, 11.0f));
        rect (8.6f, 1.5f, 6.8f, 4, 1, W);
        { P b; b.addRectangle (8.6f, 5.5f, 6.8f, 9.0f); fill (b, Ye); }
        { P t; t.addQuadrilateral (8.6f, 14.5f, 15.4f, 14.5f, 13.9f, 18.5f, 10.1f, 18.5f); fill (t, Colour (0xfff4dc6a)); }
        { P n; n.addQuadrilateral (10.6f, 18.5f, 13.4f, 18.5f, 12.9f, 20.5f, 11.1f, 20.5f); fill (n, Colour (0xff1b1e24)); }
        g.restoreState();
        seg (3, 21.6f, 14, 21.6f, 2.6f, Ye.withAlpha (0.85f));
        g.restoreState();
    };
    auto peel = [&] (float ox, float oy)
    {
        const Colour E (0xffa9811a), C (0xfff6e7a8);
        g.saveState(); g.addTransform (juce::AffineTransform::translation (ox, oy));
        auto flap = [&] (const P& pp, Colour c, bool edge) { fill (pp, c); if (edge) { g.setColour (E); g.strokePath (pp, juce::PathStrokeType (0.7f)); } };
        auto cb = [&] (float x0, float y0, float a1, float b1, float a2, float b2, float x1, float y1, float a3, float b3, float a4, float b4, float x2, float y2)
        { P q; q.startNewSubPath (x0, y0); q.cubicTo (a1, b1, a2, b2, x1, y1); q.cubicTo (a3, b3, a4, b4, x2, y2); q.closeSubPath(); return q; };
        flap (cb (7.5f, 7.5f, 4, 6.2f, 0.8f, 8.6f, 0.6f, 14.4f, 3.4f, 12.6f, 5.8f, 12.2f, 7.5f, 11.8f), Ye, true);
        flap (cb (7.5f, 8, 4.5f, 7.4f, 2.4f, 9.4f, 2, 12.6f, 4.2f, 11.4f, 5.9f, 11, 7.5f, 10.8f), C, false);
        flap (cb (7.5f, 7.5f, 11, 6.2f, 14.2f, 8.6f, 14.4f, 14.4f, 11.6f, 12.6f, 9.2f, 12.2f, 7.5f, 11.8f), Ye, true);
        flap (cb (7.5f, 8, 10.5f, 7.4f, 12.6f, 9.4f, 13, 12.6f, 10.8f, 11.4f, 9.1f, 11, 7.5f, 10.8f), C, false);
        flap (cb (5.2f, 11, 5.4f, 15, 6.6f, 18.6f, 7.5f, 19.8f, 8.4f, 18.6f, 9.6f, 15, 9.8f, 11), Ye, true);
        flap (cb (6.6f, 11.4f, 6.8f, 14.6f, 7.2f, 16.6f, 7.5f, 17.4f, 7.8f, 16.6f, 8.2f, 14.6f, 8.4f, 11.4f), C, false);
        seg (7.5f, 7.6f, 7.5f, 3.6f, 1.9f, Colour (0xff6a4a30));
        g.restoreState();
    };
    auto stave = [&] (float x1, float x2, float yTop, float gap)               // five lines of a musical stave
    {
        for (int i = 0; i < 5; ++i) seg (x1, yTop + (float) i * gap, x2, yTop + (float) i * gap, 0.9f, W);
    };
    auto markText = [&] (const juce::String& t, Colour c)                       // "M1" / "M2"
    {
        g.setColour (c); g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        g.drawText (t, juce::Rectangle<float> (0.0f, 2.0f, 24.0f, 20.0f), juce::Justification::centred, false);
    };
    auto fist = [&] (float x, float y, float w, float h, Colour c, Colour dk)   // a clenched fist seen from the side: curled fingers facing left, the thumb across them
    {
        const Colour lt = c.brighter (0.25f);
        P body; body.addRoundedRectangle (x, y, w, h, juce::jmin (w, h) * 0.32f); fill (body, c);
        // the four curled fingers: rounded bumps down the left side, each with a crease
        const float fh = h / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float fy = y + (float) i * fh;
            P f; f.addRoundedRectangle (x - w * 0.10f, fy + 0.15f, w * 0.62f, fh - 0.3f, fh * 0.48f); fill (f, c);
            g.setColour (dk); g.strokePath (f, juce::PathStrokeType (0.55f));
        }
        // the thumb: folded across the front of the fingers, from the wrist side
        P t; t.addRoundedRectangle (x + w * 0.18f, y + h * 0.50f, w * 0.72f, h * 0.30f, h * 0.14f); fill (t, lt);
        g.setColour (dk); g.strokePath (t, juce::PathStrokeType (0.55f));
        g.setColour (dk); g.strokePath (body, juce::PathStrokeType (0.7f));
    };
    auto arm = [&] (bool strong)                                                // an arm bent 90 degrees at the elbow: the upper arm level, the forearm up, a fist on top
    {
        const Colour c = strong ? Rd : Colour (0xff3fbf5f), dk = c.darker (0.45f);
        if (strong)
        {
            P a; a.startNewSubPath (2.0f, 15.5f); a.lineTo (2.0f, 21.0f); a.cubicTo (8.0f, 21.5f, 13.0f, 21.5f, 16.5f, 20.5f);      // under the upper arm, to the elbow
            a.cubicTo (20.5f, 19.5f, 21.0f, 15.5f, 20.0f, 11.0f); a.lineTo (19.0f, 7.0f);                                             // the forearm up the outside
            a.lineTo (13.8f, 7.8f); a.lineTo (15.2f, 12.5f);                                                                            // the inside of the forearm
            a.cubicTo (13.0f, 9.0f, 7.5f, 8.5f, 6.0f, 12.0f); a.cubicTo (5.0f, 14.0f, 3.5f, 15.0f, 2.0f, 15.5f); a.closeSubPath();    // the bicep: a big curve
            fill (a, c); g.setColour (dk); g.strokePath (a, juce::PathStrokeType (0.8f));
            P bl; bl.startNewSubPath (7.5f, 15.0f); bl.cubicTo (9.5f, 13.0f, 12.0f, 13.3f, 13.5f, 15.0f); stroke (bl, 0.9f, dk);       // the line of the muscle
            fist (13.4f, 0.8f, 7.4f, 7.6f, c, dk);
        }
        else
        {
            P a; a.startNewSubPath (2.5f, 18.0f); a.lineTo (15.0f, 18.0f); a.lineTo (16.0f, 8.8f); stroke (a, 2.4f, c);              // thin as a stick
            g.setColour (c); g.fillEllipse (13.6f, 17.0f, 2.6f, 2.6f);                                                                  // a bony elbow
            fist (14.2f, 3.0f, 4.2f, 5.4f, c, dk);                                                                                      // a small fist
        }
    };
    switch (ic)
    {
        case Icon::Overdub:
            rect (1.5f, 6.5f, 21, 11, 1.5f, Colour (0xff2f6fc0));
            rect (3.5f, 10.2f, 17, 3.6f, 1.0f, Colour (0xff9fd0ff));
            break;
        case Icon::Mark1: markText ("M1", Colour (0xff4a9cff)); break;
        case Icon::Mark2: markText ("M2", Colour (0xffff9a1f)); break;
        case Icon::ToEdit:     arrowH (1.5f, 11.0f, 12, W, 2); stave (12.5f, 22.5f, 7.0f, 2.5f); g.setColour (W); g.fillEllipse (16.0f, 11.0f, 3.2f, 2.4f); break;
        case Icon::ToPlayhead: arrowH (1.5f, 11.0f, 12, W, 2); tri (13.0f, 5.0f, 13.0f, 19.0f, 22.5f, 12.0f, Colour (0xff3fbf5f)); break;
        case Icon::Flag:
            seg (6, 3, 6, 21.5f, 1.8f, W);
            { P f; f.startNewSubPath (7, 3.5f); f.cubicTo (11, 1.8f, 14, 5.6f, 19.5f, 3.8f); f.lineTo (19.5f, 12.0f); f.cubicTo (14, 13.8f, 11, 10.0f, 7, 11.7f); f.closeSubPath(); fill (f, Ye); }
            break;
        case Icon::EditWin:
        {
            stave (2.0f, 22.0f, 6.0f, 3.0f);
            P c;                                                                 // a treble clef: the curl round the G line, up to the top loop, down the stem to the tail
            c.startNewSubPath (11.6f, 15.0f);
            c.cubicTo (9.6f, 15.0f, 9.0f, 12.6f, 10.6f, 11.6f);
            c.cubicTo (12.8f, 10.4f, 15.4f, 12.4f, 14.6f, 15.4f);
            c.cubicTo (13.8f, 18.2f, 8.4f, 18.4f, 7.6f, 14.4f);
            c.cubicTo (7.0f, 11.0f, 11.0f, 9.0f, 12.6f, 6.6f);
            c.cubicTo (13.8f, 4.8f, 13.6f, 2.0f, 12.4f, 1.8f);
            c.cubicTo (11.0f, 1.8f, 10.4f, 4.6f, 11.0f, 8.0f);
            c.lineTo (12.6f, 20.6f);
            c.cubicTo (12.8f, 22.6f, 10.6f, 23.0f, 10.0f, 21.4f);
            stroke (c, 1.6f, Ye);
            g.setColour (Ye); g.fillEllipse (9.3f, 20.3f, 2.4f, 2.4f);
            break;
        }
        case Icon::ArmAll:  arm (true);  break;
        case Icon::ArmNone: arm (false); break;
        case Icon::ImportTakes: container (1, 8, 11, 10); clipboard (14.5f, 4.5f, 8, 15); break;
        case Icon::Search:
            g.setColour (W); g.drawEllipse (3.0f, 3.0f, 12.0f, 12.0f, 2.2f);
            g.setColour (Bl.withAlpha (0.25f)); g.fillEllipse (4.2f, 4.2f, 9.6f, 9.6f);
            seg (13.6f, 13.6f, 20.5f, 20.5f, 3.0f, W);
            break;
        case Icon::Start:   rect (4, 5, 2.6f, 14, 0.3f, W); tri (19, 5, 19, 19, 8.5f, 12, W); break;
        case Icon::Rewind:  tri (12, 5, 12, 19, 2.5f, 12, W); tri (21.5f, 5, 21.5f, 19, 12, 12, W); break;
        case Icon::Play:    tri (7, 4.5f, 7, 19.5f, 20, 12, W); break;
        case Icon::Stop:    rect (6, 6, 12, 12, 1, W); break;
        case Icon::Forward: tri (2.5f, 5, 2.5f, 19, 12, 12, W); tri (12, 5, 12, 19, 21.5f, 12, W); break;
        case Icon::End:     rect (17.4f, 5, 2.6f, 14, 0.3f, W); tri (5, 5, 5, 19, 15.5f, 12, W); break;
        case Icon::Record:  g.setColour (Rd); g.fillEllipse (5, 5, 14, 14); break;
        case Icon::Trim:
        {
            seg (3, 3.5f, 14, 3.5f, 2.2f, W); seg (8.5f, 3.5f, 8.5f, 9.5f, 2.2f, W);
            g.setColour (W); g.drawEllipse (3.6f, 17.6f, 4.0f, 4.0f, 1.3f); g.drawEllipse (9.4f, 17.6f, 4.0f, 4.0f, 1.3f);
            seg (6.6f, 17.9f, 12.5f, 10.5f, 1.3f, W); seg (10.4f, 17.9f, 4.5f, 10.5f, 1.3f, W);
            seg (21, 4, 15.5f, 20.5f, 1.3f, Ye);
            P th; th.startNewSubPath (20, 8.5f); th.cubicTo (17, 10, 22.5f, 11.5f, 19, 13.3f); th.cubicTo (16.5f, 14.8f, 21, 15.8f, 17.6f, 17.7f); stroke (th, 1.0f, Bl);
            break;
        }
        case Icon::Bounce:
        {
            g.setColour (juce::Colours::white.withAlpha (0.13f)); g.fillEllipse (7, 19.7f, 10, 2.6f);
            g.setColour (Rd); g.fillEllipse (5, 4, 14, 14);
            P s1; s1.startNewSubPath (5.4f, 8.8f); s1.cubicTo (9, 9.8f, 15, 9.8f, 18.6f, 8.8f); stroke (s1, 1.0f, juce::Colours::white.withAlpha (0.85f));
            P s2; s2.startNewSubPath (5.2f, 13.4f); s2.cubicTo (9, 12.4f, 15, 12.4f, 18.8f, 13.4f); stroke (s2, 1.0f, juce::Colours::white.withAlpha (0.85f));
            seg (3, 4.5f, 5, 6, 1, W); seg (2.5f, 9, 4.7f, 9.3f, 1, W); seg (21, 4.5f, 19, 6, 1, W);
            break;
        }
        case Icon::Pitch:
        {
            seg (2, 12, 22, 12, 1.6f, W);
            P w; w.startNewSubPath (2, 15); w.cubicTo (4.5f, 7, 6.5f, 7, 8, 13); w.cubicTo (9.5f, 19, 11.5f, 21, 14, 14.5f); w.cubicTo (16.5f, 8, 17, 5.5f, 22, 8.5f); stroke (w, 2.0f, Rd);
            break;
        }
        case Icon::Repair:
        {
            g.saveState(); g.addTransform (juce::AffineTransform::rotation (-0.698f, 12, 12));
            rect (2, 7.5f, 20, 9, 4.5f, Colour (0xffe8c9a0)); g.setColour (Colour (0xfff3e2c7)); g.fillRect (8.0f, 7.5f, 8.0f, 9.0f);
            g.setColour (Colour (0xffc9a577)); for (auto d : { std::pair<float, float> { 10.5f, 10 }, { 13.5f, 10 }, { 12, 12 }, { 10.5f, 14 }, { 13.5f, 14 } }) g.fillEllipse (d.first - 0.7f, d.second - 0.7f, 1.4f, 1.4f);
            g.setColour (Colour (0xffb58d5c)); g.drawRoundedRectangle (2, 7.5f, 20, 9, 4.5f, 0.8f);
            g.restoreState();
            break;
        }
        case Icon::Reharm:
        {
            P a; a.startNewSubPath (3, 5); a.lineTo (9.5f, 5); a.lineTo (13.5f, 12); stroke (a, 1.8f, W);
            seg (3, 12, 13.5f, 12, 1.8f, W);
            P b; b.startNewSubPath (3, 19); b.lineTo (9.5f, 19); b.lineTo (13.5f, 12); stroke (b, 1.8f, W);
            seg (13.5f, 12, 20.5f, 12, 2.6f, Bl); g.setColour (Bl); g.fillEllipse (11.8f, 10.3f, 3.4f, 3.4f);
            break;
        }
        case Icon::Tracks:  seg (12, 4, 12, 20, 3, W); seg (4, 12, 20, 12, 3, W); break;
        case Icon::Feedback: shout(); break;
        case Icon::ExportProc: container (1, 7, 9, 10); arrowH (11, 15.4f, 12, W, 2); clipboard (16.3f, 5, 7, 13); break;
        case Icon::ImportMulti:
            for (float y : { 7.5f, 12.0f, 16.5f }) seg (1.5f, y, 8.5f, y, 2, W);
            arrowH (13.4f, 9.6f, 12, W, 2); container (14.8f, 7, 8.2f, 10); break;
        case Icon::Markers: highlighter ({}); break;
        case Icon::FeedbackMarkers:
            highlighter (juce::AffineTransform::scale (0.82f).translated (-1.5f, 1.0f));
            g.saveState(); g.addTransform (juce::AffineTransform::scale (0.6f).translated (9.6f, 5.4f)); shout(); g.restoreState(); break;
        case Icon::SlipLeft:  arrowH (8, 1.6f, 12, Rd, 2); peel (9, 1.6f); break;
        case Icon::SlipRight: peel (1.4f, 1.6f); arrowH (16.4f, 22.4f, 12, Bl, 2); break;
        case Icon::Shuffle:
        {
            g.saveState(); g.addTransform (juce::AffineTransform::rotation (juce::degreesToRadians (-14.0f), 9, 13));
            { P r; r.addRoundedRectangle (3.0f, 5.0f, 10.0f, 14.0f, 1.6f); fill (r, Colour (0xff3d4a66)); g.setColour (W); g.strokePath (r, juce::PathStrokeType (1.0f)); }
            g.restoreState();
            g.saveState(); g.addTransform (juce::AffineTransform::rotation (juce::degreesToRadians (8.0f), 14, 12));
            { P r; r.addRoundedRectangle (8.5f, 4.0f, 10.0f, 14.5f, 1.6f); fill (r, Colour (0xfff2f4f8)); g.setColour (Colour (0xff8a93a3)); g.strokePath (r, juce::PathStrokeType (0.8f)); }
            { P h; h.startNewSubPath (13.5f, 8.2f); h.cubicTo (12.6f, 6.7f, 10.5f, 7.1f, 10.5f, 8.8f); h.cubicTo (10.5f, 10.3f, 13.5f, 12.4f, 13.5f, 12.4f);
              h.cubicTo (13.5f, 12.4f, 16.5f, 10.3f, 16.5f, 8.8f); h.cubicTo (16.5f, 7.1f, 14.4f, 6.7f, 13.5f, 8.2f); h.closeSubPath(); fill (h, Rd); }
            g.restoreState();
            break;
        }
        case Icon::InsertFlag:
        {
            seg (5.5f, 2.5f, 5.5f, 21.5f, 2, W);
            { P f; f.addTriangle (6.5f, 3.2f, 20, 3.2f, 17, 7.5f); f.addTriangle (6.5f, 3.2f, 17, 7.5f, 6.5f, 11.8f); f.addTriangle (17, 7.5f, 20, 11.8f, 6.5f, 11.8f); fill (f, Rd); }
            { P a; a.startNewSubPath (12.6f, 4.6f); a.lineTo (12.6f, 9.6f); a.startNewSubPath (10.2f, 7.8f); a.lineTo (12.6f, 10.2f); a.lineTo (15, 7.8f); stroke (a, 1.5f, juce::Colours::white); }
            break;
        }
        case Icon::NextAtEnd:
            arrowH (2, 13.5f, 12, Bl, 2);
            for (float y : { 6.0f, 9.0f, 12.0f, 15.0f, 18.0f }) seg (15, y, 22.5f, y, 0.7f, W.withAlpha (0.5f));
            seg (17, 6, 17, 18, 1.3f, W); { P r; r.addRectangle (19.4f, 6, 2.8f, 12); fill (r, W); }
            break;
        case Icon::Mixer:
        {
            for (float x : { 6.0f, 12.0f, 18.0f }) seg (x, 3.5f, x, 20.5f, 1.6f, W.withAlpha (0.55f));
            for (auto k : { std::pair<float, float> { 3.2f, 14 }, { 9.2f, 6.5f }, { 15.2f, 10.5f } }) { rect (k.first, k.second, 5.6f, 3.2f, 0.8f, Bl); g.setColour (Colour (0xff1b1e24)); g.drawRoundedRectangle (k.first, k.second, 5.6f, 3.2f, 0.8f, 0.6f); }
            break;
        }
        case Icon::Earlier: clock (14.5f, 12.5f, 6); seg (8.5f, 17, 2, 17, 2, Rd); { P hd; hd.startNewSubPath (4.8f, 14.3f); hd.lineTo (2, 17); hd.lineTo (4.8f, 19.7f); stroke (hd, 2, Rd); } break;
        case Icon::Later:   clock (9.5f, 12.5f, 6);  seg (15.5f, 17, 22, 17, 2, Bl); { P hd; hd.startNewSubPath (19.2f, 14.3f); hd.lineTo (22, 17); hd.lineTo (19.2f, 19.7f); stroke (hd, 2, Bl); } break;
        case Icon::Delete:  g.setColour (Rd); g.drawEllipse (3.8f, 3.8f, 16.4f, 16.4f, 2.6f); seg (6.2f, 17.8f, 17.8f, 6.2f, 2.6f, Rd); break;
        case Icon::Undo:
        {
            P h; h.startNewSubPath (8.5f, 4.5f); h.lineTo (3.5f, 9.5f); h.lineTo (8.5f, 14.5f); stroke (h, 2.2f, W);
            P b; b.startNewSubPath (3.8f, 9.5f); b.lineTo (13.5f, 9.5f); b.addCentredArc (13.5f, 15, 5.5f, 5.5f, 0, 0, juce::MathConstants<float>::pi, false); b.lineTo (8.5f, 20.5f); stroke (b, 2.2f, W);
            break;
        }
        case Icon::Automation:
        {
            P l; l.startNewSubPath (2.5f, 16); l.lineTo (8, 6.5f); l.lineTo (13, 15.5f); l.lineTo (17, 8.5f); l.lineTo (21.5f, 14.5f); stroke (l, 1.8f, Ye);
            for (auto d : { std::pair<float, float> { 2.8f, 16 }, { 8, 6.5f }, { 13, 15.5f }, { 17, 8.5f }, { 21.2f, 14.5f } })
            { g.setColour (juce::Colours::white); g.fillEllipse (d.first - 1.7f, d.second - 1.7f, 3.4f, 3.4f); g.setColour (Ye); g.drawEllipse (d.first - 1.7f, d.second - 1.7f, 3.4f, 3.4f, 1.3f); }
            break;
        }
        case Icon::Buses:
        {
            rect (1.8f, 5, 20.4f, 12.5f, 2.4f, Ye);
            const Colour k (0xff1b1e24);
            rect (3.6f, 7, 3.6f, 3.6f, 0.5f, k); rect (8.2f, 7, 3.6f, 3.6f, 0.5f, k); rect (12.8f, 7, 3.6f, 3.6f, 0.5f, k); rect (17.4f, 7, 3.0f, 5.8f, 0.5f, k);
            g.setColour (k.withAlpha (0.35f)); g.fillRect (1.8f, 13.2f, 20.4f, 1.2f);
            for (float x : { 7.0f, 17.0f }) { g.setColour (Dk); g.fillEllipse (x - 2.2f, 16.0f, 4.4f, 4.4f); g.setColour (W); g.drawEllipse (x - 2.2f, 16.0f, 4.4f, 4.4f, 1.2f); }
            break;
        }
    }
    g.restoreState();
}

/** A button that shows a picture instead of words. The name of the button (and its longer explanation) appears when the mouse rests on it. Colours: buttonColourId / buttonOnColourId as for any TextButton. */
class IconButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void setIcon (Icon i, const juce::String& name) { icon = i; label = name; repaint(); }
    juce::String getTooltip() override
    {
        const auto t = juce::TextButton::getTooltip();
        return label.isEmpty() ? t : (t.isEmpty() ? label : label + "\n" + t);
    }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        const auto base = findColour (on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        g.setColour (down ? base.brighter (0.2f) : over ? base.brighter (0.1f) : base); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? base.brighter (0.4f) : theme::border); g.drawRoundedRectangle (b, 4.0f, 1.0f);
        auto ic = icon;
        const auto txt = getButtonText();
        if ((ic == Icon::Play || ic == Icon::Record) && (txt.startsWithIgnoreCase ("stop"))) ic = Icon::Stop;
        const bool dim = ! isEnabled();
        if (dim) g.beginTransparencyLayer (0.4f);
        drawIcon (g, ic, b.reduced (3.0f, 2.0f), theme::inkOn (base));          // dark pictures on the light keys, light ones on a lit key
        if (dim) g.endTransparencyLayer();
    }
private:
    Icon icon = Icon::Play; juce::String label;
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
