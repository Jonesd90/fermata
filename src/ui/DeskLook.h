#pragma once
#include "MixerLook.h"
#include "ValueEntry.h"

namespace td
{
/** The "desk" look of the main window: a blue-grey console panel with wooden side panels and a wooden rail along the bottom, digital-desk strips
    (LED-ring gain knobs, black displays, light keys with little pictures). The panel comes in six colours, chosen once in the Project menu. */
namespace desk
{
//==============================================================================================================================
// colours
//==============================================================================================================================
struct Palette
{
    juce::Colour window, strip, stripEdge, key, keyEdge, keyOn, ink, cream, disp, dispEdge, ringOff, menu, menuEdge, cyan, knobEdge;
};

inline const char* themeName (int i) { static const char* n[] { "Blue", "Red", "Orange", "Yellow", "Green", "Purple" }; return n[juce::jlimit (0, 5, i)]; }
constexpr int kThemes = 6;

inline juce::Colour tint (juce::Colour c, int theme) { return theme::deskTint (c, theme); }
inline int& themeIndex() { return theme::deskIndex(); }

inline Palette makePalette (int t, bool dark = theme::isDark())
{
    // the dark look: the same colours a shade darker (the panels well down, the keys a little, so they still stand out)
    auto shadeOf = [] (juce::uint32 argb)
    {
        switch (argb) { case 0xff96abc0: return 0.8f; case 0xff1b2a3a: return 1.0f; case 0xff6ee7ff: return 1.0f; default: break; }
        return 0.65f;
    };
    auto k = [t, dark, shadeOf] (juce::uint32 argb) { return theme::deskShade (tint (juce::Colour (argb), t), dark, shadeOf (argb)); };
    Palette p;
    p.window = k (0xff566b82);   p.strip = k (0xff617890);  p.stripEdge = k (0xff2f3e50); p.key = k (0xff96abc0);  p.keyEdge = k (0xff3d5066);
    p.keyOn = juce::Colour (0xff1a2a32);
    p.ink = k (0xff1b2a3a);      p.cream = juce::Colour (0xffe2d9c0);
    p.disp = juce::Colour (0xff05080c); p.dispEdge = k (0xff33465a); p.ringOff = k (0xff47596e);
    p.menu = k (0xff1b2634);     p.menuEdge = k (0xff3b4e63); p.cyan = k (0xff6ee7ff); p.knobEdge = k (0xff4d6178);
    return p;
}

inline Palette& pal() { static Palette p = makePalette (themeIndex()); return p; }
/** Sets the desk colour (the palette of the whole program follows: see ThemeSwitch.h, refreshTheme). */
inline void setTheme (int t) { themeIndex() = juce::jlimit (0, kThemes - 1, t); pal() = makePalette (themeIndex()); }
inline juce::Colour swatchColour (int t) { return tint (juce::Colour (0xff617890), t); }

/** The colour bar on each strip: the same as the rest of Fermata's channel colours, going round as the inputs go along. */
inline juce::Colour stripColour (int index)
{
    static const juce::uint32 c[] { 0xff4aa3ff, 0xff2fd0e8, 0xff2fe0b0, 0xff6ee05a, 0xffc6e23a, 0xffffd23a, 0xffff9a2e, 0xffff5a4a, 0xffff5fb0, 0xffc27bff, 0xff8f7bff, 0xff6f86ff, 0xff9aa7b8, 0xffc58a5a };
    return juce::Colour (c[(size_t) juce::jmax (0, index) % 14]);
}

//==============================================================================================================================
// the little pictures
//==============================================================================================================================
enum class Ic { None = -1, Mic, Jack, Bolt, Ramp, Pillow, Rocket, Mega, TbMic, Phones };

inline const char* svgBody (Ic ic)
{
    switch (ic)
    {
        case Ic::Mic: return R"svg(<rect x="7.6" y="1.8" width="8.8" height="12.6" rx="4.4" fill="#c4cad4" stroke="OUT" stroke-width="1.2"/><path d="M8 5.6H16M7.8 8.2H16.2M8 10.8H16" stroke="#6f7886" stroke-width="1"/><path d="M10.2 3.4Q11 2.8 12 2.8" stroke="#ffffff" stroke-width="1.1" stroke-linecap="round" fill="none"/><path d="M4.8 10.8V11.5A7.2 7.2 0 0 0 19.2 11.5V10.8" fill="none" stroke="OUT" stroke-width="1.8" stroke-linecap="round"/><path d="M12 18.7V21.2M8.3 21.6H15.7" stroke="OUT" stroke-width="2" stroke-linecap="round"/>)svg";
        case Ic::Jack: return R"svg(<path d="M1 12.2C3 12.2 3.6 8.2 5.6 8.2" fill="none" stroke="OUT" stroke-width="2.2" stroke-linecap="round"/><rect x="5.2" y="7.6" width="7.6" height="9" rx="1.6" fill="#e5484d" stroke="OUT" stroke-width="1.1"/><path d="M7.2 9.4V14.8M9 9.4V14.8M10.8 9.4V14.8" stroke="#ffffff" stroke-opacity=".6" stroke-width=".9"/><rect x="12.8" y="9.9" width="3.4" height="4.4" fill="#d7dbe2" stroke="#7d8591" stroke-width=".8"/><rect x="16.2" y="10.3" width="1.5" height="3.6" fill="#2a231b"/><path d="M17.7 10.3H20.4C22.6 10.3 22.6 14 20.4 14H17.7Z" fill="#d7dbe2" stroke="#7d8591" stroke-width=".8"/>)svg";
        case Ic::Bolt: return R"svg(<path d="M14 1.8L4.8 13.2H10.6L9 22.2L19.2 9.6H13.2Z" fill="#f2c230" stroke="#a9811a" stroke-width="1" stroke-linejoin="round"/><path d="M12.6 5.4L8.4 11.4" stroke="#fff3b0" stroke-width="1.2" stroke-linecap="round"/><path d="M2.6 6.6L5 7.8M3.6 3.4L5.6 5.2M21 4.4L18.8 5.8M21.8 8.4L19.6 8.8" stroke="#c8940a" stroke-width="1.3" stroke-linecap="round"/>)svg";
        case Ic::Ramp: return R"svg(<path d="M2 21.2H22.4M2.4 3.4V21.2" stroke="OUT" stroke-width="1.3" stroke-linecap="round" stroke-opacity=".6"/><path d="M3 20.4C7 20.4 8.4 7.4 12.6 7.4H22" fill="none" stroke="#2a6fd6" stroke-width="2.4" stroke-linecap="round"/><path d="M3 20.4C7 20.4 8.4 7.4 12.6 7.4V20.4Z" fill="#c9302c" fill-opacity=".16"/><path d="M9.2 3.6L11.6 6.2M11.6 3.6L9.2 6.2" stroke="#c9302c" stroke-width="1.6" stroke-linecap="round"/>)svg";
        case Ic::Pillow: return R"svg(<path d="M3 6.6C8 4.8 16 4.8 21 6.6C19.6 10.4 19.6 13.6 21 17.4C16 19.2 8 19.2 3 17.4C4.4 13.6 4.4 10.4 3 6.6Z" fill="#8fbdee" stroke="#3f78b8" stroke-width="1.1" stroke-linejoin="round"/><path d="M3 6.6L6.2 9.2M21 6.6L17.8 9.2M3 17.4L6.2 14.8M21 17.4L17.8 14.8" stroke="#3f78b8" stroke-width="1" stroke-linecap="round"/><path d="M9.6 9.8H13.2L9.6 14.2H13.4" fill="none" stroke="#ffffff" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M15 8.6H17.4L15 11.4H17.5" fill="none" stroke="#ffffff" stroke-opacity=".8" stroke-width="1.1" stroke-linecap="round" stroke-linejoin="round"/>)svg";
        case Ic::Rocket: return R"svg(<g transform="rotate(40 12 12)"><path d="M12 1.8C15.6 5 16.6 10 15.6 15.2H8.4C7.4 10 8.4 5 12 1.8Z" fill="#f6f1e4" stroke="OUT" stroke-width="1.2" stroke-linejoin="round"/><circle cx="12" cy="8.8" r="2" fill="#4f8cff" stroke="OUT" stroke-width="1"/><path d="M8.6 11.6L4.6 17L8.4 15.4Z M15.4 11.6L19.4 17L15.6 15.4Z" fill="#e5484d" stroke="OUT" stroke-width="1" stroke-linejoin="round"/><path d="M9.8 15.6Q12 23 14.2 15.6Z" fill="#ffb82e"/><path d="M11 15.6Q12 19.8 13 15.6Z" fill="#ff5a2e"/></g>)svg";
        case Ic::Mega: return R"svg(<path d="M2.6 9H6.6L15.6 4V20L6.6 15H2.6Z" fill="#e5484d" stroke="OUT" stroke-width="1.2" stroke-linejoin="round"/><ellipse cx="15.6" cy="12" rx="1.8" ry="8" fill="#f7c9c5" stroke="OUT" stroke-width="1.1"/><path d="M7 15.2L8.6 21H11.4L10.2 16.6" fill="#6a4a30" stroke="OUT" stroke-width=".9" stroke-linejoin="round"/><path d="M19.4 8.6Q21 12 19.4 15.4M21.4 5.8Q24 12 21.4 18.2" fill="none" stroke="#c8940a" stroke-width="1.5" stroke-linecap="round"/>)svg";
        case Ic::TbMic: return R"svg(<circle cx="12" cy="7.4" r="6.6" fill="none" stroke="OUT" stroke-width="1" stroke-dasharray="1.6 1.4" stroke-opacity=".7"/><circle cx="12" cy="7.4" r="4.7" fill="#c4cad4" stroke="OUT" stroke-width="1.3"/><path d="M8.3 5.6H15.7M7.8 7.4H16.2M8.3 9.2H15.7M10.4 3.2V11.6M13.6 3.2V11.6" stroke="#6f7886" stroke-width=".7"/><rect x="10.6" y="14" width="2.8" height="2.6" fill="OUT"/><path d="M8.6 17H15.4" stroke="OUT" stroke-width="1.5" stroke-linecap="round"/>)svg";
        case Ic::Phones: return R"svg(<path d="M4.6 15V12.6A7.4 7.4 0 0 1 19.4 12.6V15" fill="none" stroke="OUT" stroke-width="2.4" stroke-linecap="round"/><rect x="2.6" y="13.4" width="5" height="8" rx="2" fill="#4f8cff" stroke="OUT" stroke-width="1.2"/><rect x="16.4" y="13.4" width="5" height="8" rx="2" fill="#4f8cff" stroke="OUT" stroke-width="1.2"/><path d="M4.4 15.6V19.2M18.2 15.6V19.2" stroke="#ffffff" stroke-opacity=".7" stroke-width="1" stroke-linecap="round"/>)svg";
        default: return "";
    }
}

/** The picture as a drawable; 'lightOutline' gives the outlines in white (for a lit key) instead of dark. Made once, then kept. */
inline juce::Drawable* iconDrawable (Ic ic, bool lightOutline)
{
    static std::unique_ptr<juce::Drawable> cache[9][2];
    const int i = (int) ic;
    if (i < 0 || i > 8) return nullptr;
    auto& slot = cache[i][lightOutline ? 1 : 0];
    if (slot == nullptr)
    {
        const juce::String body = juce::String (svgBody (ic)).replace ("OUT", lightOutline ? "#ffffff" : "#1b2a3a");
        const juce::String svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"24\" height=\"24\">" + body + "</svg>";
        if (auto xml = juce::XmlDocument::parse (svg)) slot = juce::Drawable::createFromSVG (*xml);
    }
    return slot.get();
}

/** Draws a picture into 'area' (centred, never stretched). The talkback microphone has its red TALK written under it. */
inline void drawPicture (juce::Graphics& g, Ic ic, juce::Rectangle<float> area, bool lightOutline, float opacity = 1.0f)
{
    if (auto* d = iconDrawable (ic, lightOutline))
        d->drawWithin (g, area, juce::RectanglePlacement::centred, opacity);
    if (ic == Ic::TbMic)
    {
        const float s = juce::jmin (area.getWidth(), area.getHeight());
        auto sq = juce::Rectangle<float> (s, s).withCentre (area.getCentre());
        g.setColour (juce::Colour (0xffe5484d).withMultipliedAlpha (opacity));
        g.setFont (juce::FontOptions (juce::jmax (6.0f, s * 0.30f), juce::Font::bold));
        g.drawText ("TALK", sq.withTop (sq.getY() + s * 0.73f), juce::Justification::centred, false);
    }
}

/** A small picture that just sits there (the headphones beside the audition buttons). */
class PictureBadge : public juce::Component
{
public:
    explicit PictureBadge (Ic i) : ic (i) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override { drawPicture (g, ic, getLocalBounds().toFloat().reduced (1.0f), true); }
private:
    Ic ic;
};

//==============================================================================================================================
// the wood
//==============================================================================================================================
inline void paintCheek (juce::Graphics& g, juce::Rectangle<int> r, bool leftSide) { wood::paintCheek (g, r, leftSide); }

/** The rail along the bottom of the main window, like the arm rest of a real desk: square along the top, rounded at the bottom corners. */
inline void paintRail (juce::Graphics& g, juce::Rectangle<int> r)
{
    juce::ColourGradient gr (juce::Colour (0xff8c5a32), 0.0f, (float) r.getY(), juce::Colour (0xff6b3f20), 0.0f, (float) r.getBottom(), false);
    juce::Path shape; shape.addRoundedRectangle ((float) r.getX(), (float) r.getY() - 12.0f, (float) r.getWidth(), (float) r.getHeight() + 12.0f, 10.0f);
    g.saveState(); g.reduceClipRegion (r);
    g.setGradientFill (gr); g.fillPath (shape);
    g.reduceClipRegion (shape);
    g.setColour (juce::Colour (0x33321808));
    for (int y = r.getY() + 4; y < r.getBottom(); y += 4) g.fillRect (r.getX(), y, r.getWidth(), 1);              // the grain runs along the rail
    g.setColour (juce::Colour (0x61ffe1b4)); g.fillRect (r.getX(), r.getY(), r.getWidth(), 2);                      // a lit top edge
    g.setColour (juce::Colour (0x2e000000)); g.fillRect (r.getX(), r.getY() + 2, r.getWidth(), 1);
    g.setColour (juce::Colour (0x47000000)); g.fillRect (r.getX(), r.getBottom() - 3, r.getWidth(), 3);
    g.restoreState();
}

/** "fermata" and the mark, in one line, pressed into the wood of the rail like a maker's name. Returns the width it took. */
inline int paintNameplate (juce::Graphics& g, juce::Rectangle<int> area)
{
    const float h = (float) area.getHeight();
    const juce::Font f (juce::FontOptions ("Georgia", h * 0.78f, juce::Font::bold));
    const int textW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, "fermata")) + 6;
    const int markW = juce::roundToInt (h * 1.5f);
    const int total = markW + 6 + textW;
    auto r = area.withLeft (area.getRight() - total);
    auto mark = r.removeFromLeft (markW).toFloat().reduced (0.0f, h * 0.08f);
    r.removeFromLeft (6);
    const juce::Colour brass (0xffe6cf8e), shadow (0x99301a08);
    drawFermataLogo (g, mark.translated (0.0f, 1.0f), shadow, shadow);
    drawFermataLogo (g, mark, brass, brass.brighter (0.2f));
    g.setFont (f);
    g.setColour (shadow); g.drawText ("fermata", r.translated (0, 1), juce::Justification::centredLeft, false);
    g.setColour (brass);  g.drawText ("fermata", r, juce::Justification::centredLeft, false);
    return total;
}

//==============================================================================================================================
// controls
//==============================================================================================================================
/** A key of the desk: light when off, dark with a coloured line along the bottom when on. It can carry a little picture, over a small caption (tall keys),
    beside the text (wide, short keys) or alone. */
class DeskKey : public juce::Button
{
public:
    explicit DeskKey (const juce::String& text = {}, Ic i = Ic::None, float cornerRadius = 10.0f) : juce::Button (text), icon (i), radius (cornerRadius)
    {
        setClickingTogglesState (true);
        setWantsKeyboardFocus (false);
    }
    void setIcon (Ic i) { if (i != icon) { icon = i; repaint(); } }
    void setLed (juce::Colour c) { led = c; repaint(); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& p = pal();
        const bool on = getToggleState();
        const float alpha = isEnabled() ? 1.0f : 0.5f;
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const float rad = juce::jmin (radius, r.getHeight() * 0.5f);
        auto fill = on ? p.keyOn : p.key;
        if (down) fill = fill.darker (0.2f); else if (over) fill = fill.brighter (0.12f);
        juce::Path shape; shape.addRoundedRectangle (r, rad);
        g.setColour (fill.withMultipliedAlpha (alpha)); g.fillPath (shape);
        if (on)
        {
            g.saveState();
            g.reduceClipRegion (shape);
            g.setColour (led.withMultipliedAlpha (alpha));
            g.fillRect (r.withTop (r.getBottom() - 3.0f));
            g.restoreState();
        }
        g.setColour ((on ? led.darker (0.6f) : p.keyEdge).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, rad, 1.0f);

        const auto ink = (on ? juce::Colours::white : p.ink).withMultipliedAlpha (alpha);
        const auto text = getButtonText();
        auto area = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
        if (icon != Ic::None && text.isNotEmpty() && area.getHeight() >= 18.0f && area.getWidth() < 3.0f * area.getHeight())   // tall key: the picture over a small caption
        {
            const float capH = juce::jlimit (7.5f, 10.0f, area.getHeight() * 0.36f);         // (smaller on the compact strips of a short screen)
            auto cap = area.removeFromBottom (capH);
            drawPicture (g, icon, area.reduced (0.0f, 0.5f), on, alpha);
            g.setColour (ink); g.setFont (juce::FontOptions (capH - 1.5f + (capH < 9.0f ? 0.5f : 0.0f), juce::Font::bold));
            g.drawText (text, cap.toNearestInt(), juce::Justification::centred, false);
        }
        else if (icon != Ic::None && text.isNotEmpty())                                      // wide, short key: the picture beside the text
        {
            auto pic = area.removeFromLeft (area.getHeight() + 2.0f);
            drawPicture (g, icon, pic.reduced (2.0f), on, alpha);
            g.setColour (ink); g.setFont (juce::FontOptions (juce::jmin (11.0f, area.getHeight() * 0.55f), juce::Font::bold));
            g.drawFittedText (text, area.toNearestInt(), juce::Justification::centred, 1);
        }
        else if (icon != Ic::None)                                                           // picture alone
        {
            drawPicture (g, icon, area.reduced (2.0f), on, alpha);
        }
        else
        {
            g.setColour (ink); g.setFont (juce::FontOptions (juce::jmin (11.5f, area.getHeight() * 0.55f), juce::Font::bold));
            g.drawFittedText (text, area.toNearestInt(), juce::Justification::centred, 1);
        }
    }

private:
    Ic icon;
    float radius;
    juce::Colour led { 0xffffb02e };
};

/** The gain knob: a ring of 21 lit segments round a dark knob with a white pointer. Drag up / down; the mouse wheel never moves it. */
class DeskEncoder : public valueentry::TypedSlider
{
public:
    DeskEncoder()
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
        setMouseDragSensitivity (220);
        setScrollWheelEnabled (false);
    }
    void setRing (juce::Colour c) { ring = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = pal();
        auto b = getLocalBounds().toFloat();
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float f = juce::jlimit (0.0f, 1.0f, (float) valueToProportionOfLength (getValue()));
        constexpr int n = 21;
        const int lit = juce::jmax (1, (int) std::round (f * (float) n));
        const float R = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 3.0f;
        const float pitch = 270.0f / (float) n, seg = pitch * 0.72f;
        for (int i = 0; i < n; ++i)
        {
            const float a0 = -135.0f + (float) i * pitch + (pitch - seg) * 0.5f, a1 = a0 + seg;
            juce::Path arc;
            arc.addCentredArc (cx, cy, R, R, 0.0f, juce::degreesToRadians (a0), juce::degreesToRadians (a1), true);
            g.setColour (isEnabled() && i < lit ? ring : p.ringOff);
            g.strokePath (arc, juce::PathStrokeType (4.0f));
        }
        const float kr = R * 0.58f;
        juce::ColourGradient gr (juce::Colour (0xff363d45), cx, cy - kr * 0.45f, juce::Colour (0xff14171a), cx, cy + kr, true);
        g.setGradientFill (gr); g.fillEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
        g.setColour (p.knobEdge); g.drawEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);
        juce::Path pointer; pointer.addRoundedRectangle (-1.5f, -(kr - 3.0f), 3.0f, kr * 0.5f, 1.0f);
        g.setColour (juce::Colours::white);
        g.fillPath (pointer, juce::AffineTransform::rotation (juce::degreesToRadians (-135.0f + f * 270.0f)).translated (cx, cy));
    }

private:
    juce::Colour ring { 0xff2fd0e8 };
};

/** A small knob ringed with lit segments: the level and pan knobs of the stage speaker boxes. A pan knob lights from the middle outwards. */
class RingPot : public valueentry::TypedSlider
{
public:
    explicit RingPot (bool isBipolar = false) : bipolar (isBipolar)
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
        setMouseDragSensitivity (160);
    }
    void setRing (juce::Colour c) { ring = c; customRing = true; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = pal();
        const auto ringCol = customRing ? ring : p.cyan;                                // unless told otherwise, the desk's own light colour
        auto b = getLocalBounds().toFloat();
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float f = juce::jlimit (0.0f, 1.0f, (float) valueToProportionOfLength (getValue()));
        const float R = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 2.5f;
        const int n = juce::jlimit (9, 21, (int) (R * 0.6f) | 1);                        // an odd number, so a pan knob has a middle segment
        const float pitch = 270.0f / (float) n, seg = pitch * 0.7f;
        const int on = (int) std::round (f * (float) (n - 1)), mid = n / 2;
        for (int i = 0; i < n; ++i)
        {
            const bool lit = bipolar ? (on >= mid ? (i >= mid && i <= on) : (i <= mid && i >= on)) : (i <= on && f > 0.001f);
            const float a0 = -135.0f + (float) i * pitch + (pitch - seg) * 0.5f;
            juce::Path arc;
            arc.addCentredArc (cx, cy, R, R, 0.0f, juce::degreesToRadians (a0), juce::degreesToRadians (a0 + seg), true);
            g.setColour (lit ? ringCol : p.ringOff);
            g.strokePath (arc, juce::PathStrokeType (3.0f));
        }
        const float kr = R * 0.6f;
        juce::ColourGradient gr (juce::Colour (0xff363d45), cx, cy - kr * 0.45f, juce::Colour (0xff14171a), cx, cy + kr, true);
        g.setGradientFill (gr); g.fillEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
        g.setColour (p.knobEdge); g.drawEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);
        juce::Path pointer; pointer.addRoundedRectangle (-1.0f, -(kr - 2.0f), 2.0f, kr * 0.55f, 1.0f);
        g.setColour (juce::Colours::white);
        g.fillPath (pointer, juce::AffineTransform::rotation (juce::degreesToRadians (-135.0f + f * 270.0f)).translated (cx, cy));
    }
private:
    bool bipolar, customRing = false;
    juce::Colour ring { 0xff2fd0e8 };
};

/** The talkback key at the bottom of each input strip: a red outline round the light key with TALK in black; when the input is a talkback mic it is all red with
    TALK in white. */
class TalkKey : public juce::Button
{
public:
    TalkKey() : juce::Button ("TALK") { setClickingTogglesState (true); setWantsKeyboardFocus (false); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool on = getToggleState();
        const juce::Colour red (0xffe0302a);
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        auto fill = on ? red : pal().key;
        if (down) fill = fill.darker (0.2f); else if (over) fill = fill.brighter (0.1f);
        g.setColour (fill.withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f)); g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? red.darker (0.35f) : red); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, on ? 1.0f : 2.0f);
        g.setColour (on ? juce::Colours::white : juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jmin (12.5f, r.getHeight() * 0.6f), juce::Font::bold));
        g.drawText ("TALK", getLocalBounds(), juce::Justification::centred, false);
    }
};

/** A small black display with a value in it ("38.5 dB"). Double-click it to type the value. */
class DeskReadout : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onDouble;
    void setValueText (const juce::String& t) { if (t != text) { text = t; repaint(); } }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onDouble) onDouble(); }
    void paint (juce::Graphics& g) override
    {
        const auto& p = pal();
        auto b = getLocalBounds();
        g.setColour (p.disp); g.fillRect (b);
        g.setColour (p.dispEdge); g.drawRect (b, 1);
        g.setColour (p.cyan);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold));
        const int vw = juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold)), text));
        const int total = vw + 4 + 14;
        const int x = (b.getWidth() - total) / 2;
        g.drawText (text, x, 0, vw + 2, b.getHeight(), juce::Justification::centredLeft, false);
        g.setFont (juce::FontOptions (9.0f)); g.setColour (p.cyan.withMultipliedBrightness (0.6f));
        g.drawText ("dB", x + vw + 4, 0, 14, b.getHeight(), juce::Justification::centredLeft, false);
    }
private:
    juce::String text;
};

/** The dB numbers beside a strip's meter: 0, -9, -20 and -40 (the meter's own scale, so they line up). */
class DeskScale : public juce::Component
{
public:
    DeskScale() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (pal().cream); g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 8.0f, juce::Font::plain));
        for (int db : { 0, -9, -20, -40 })
        {
            const float y = b.getBottom() - b.getHeight() * LevelMeter::frac ((float) db);
            g.drawText (juce::String (db), juce::Rectangle<float> (b.getX(), juce::jlimit (b.getY(), b.getBottom() - 10.0f, y - 5.0f), b.getWidth(), 10.0f), juce::Justification::centredLeft, false);
        }
    }
};

/** The row of colour dots in the Project menu. Clicking one picks that colour. */
class SwatchRow : public juce::PopupMenu::CustomComponent
{
public:
    SwatchRow (int current, std::shared_ptr<int> pickedOut) : CustomComponent (false), cur (current), picked (std::move (pickedOut)) {}
    void getIdealSize (int& w, int& h) override { w = 230; h = 58; }
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::dimText); g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText ("COLOUR THEME", 14, 5, getWidth() - 28, 16, juce::Justification::centredLeft, false);
        for (int i = 0; i < kThemes; ++i)
        {
            auto c = dot (i);
            g.setColour (swatchColour (i)); g.fillEllipse (c);
            g.setColour (juce::Colours::black.withAlpha (0.45f)); g.drawEllipse (c, 2.0f);
            if (i == cur) { g.setColour (juce::Colour (0xffe2d9c0)); g.drawEllipse (c.expanded (3.0f), 2.0f); }
            else if (i == hover) { g.setColour (juce::Colours::white.withAlpha (0.5f)); g.drawEllipse (c.expanded (3.0f), 1.0f); }
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = hit (e.position); if (h != hover) { hover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h < 0) return;
        *picked = h;
        triggerMenuItem();
    }
private:
    juce::Rectangle<float> dot (int i) const { return { 16.0f + (float) i * 33.0f, 26.0f, 22.0f, 22.0f }; }
    int hit (juce::Point<float> p) const { for (int i = 0; i < kThemes; ++i) if (dot (i).expanded (5.0f).contains (p)) return i; return -1; }
    int cur, hover = -1;
    std::shared_ptr<int> picked;
};
} // namespace desk
} // namespace td
