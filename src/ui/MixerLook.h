#pragma once
#include "FermataLookAndFeel.h"

namespace td
{
/** The colours of the mixer. Light follows the rest of Fermata; Dark is the classic console look. Both are drawn with 1-pixel lines and small type. */
struct MixTheme
{
    bool dark = false;
    juce::Colour bg, strip, well, line, soft, text, dim, field, insertOn, insertByp, groove;
};

inline MixTheme makeMixTheme (bool dark)
{
    MixTheme t;
    t.dark = dark;
    if (dark)
    {
        t.bg = juce::Colour (0xff121418);    t.strip = juce::Colour (0xff2b2f36); t.well = juce::Colour (0xff1d2025);
        t.line = juce::Colour (0xff59606c);  t.soft = juce::Colour (0xff3b414b);  t.text = juce::Colour (0xffe9edf2);
        t.dim = juce::Colour (0xff9ba5b4);   t.field = juce::Colour (0xff15171b); t.insertOn = juce::Colour (0xff2b6a4c);
        t.insertByp = juce::Colour (0xff7a6420); t.groove = juce::Colour (0xff0c0e11);
    }
    else
    {
        t.bg = juce::Colour (0xffd9dee5);    t.strip = juce::Colour (0xfff4f6f9); t.well = juce::Colour (0xffe6eaf0);
        t.line = juce::Colour (0xff7b8696);  t.soft = juce::Colour (0xffbcc4cf);  t.text = juce::Colour (0xff14181f);
        t.dim = juce::Colour (0xff4a5566);   t.field = juce::Colours::white;      t.insertOn = juce::Colour (0xffc4e4d1);
        t.insertByp = juce::Colour (0xffefdc9c); t.groove = juce::Colour (0xff8e98a6);
    }
    return t;
}

inline MixTheme& mixTheme() { static MixTheme t = makeMixTheme (true); return t; }   // the dark console look is the default
inline void setMixDark (bool dark) { mixTheme() = makeMixTheme (dark); }

/** The fine, dense widget drawing of the mixer: hairline outlines, small square buttons, slim faders with a finely divided scale. */
class MixerLookAndFeel : public FermataLookAndFeel
{
public:
    MixerLookAndFeel() { apply(); }

    /** Call after setMixDark(). */
    void apply()
    {
        applyTheme();
        const auto& t = mixTheme();
        setColour (juce::ComboBox::backgroundColourId, t.field);
        setColour (juce::ComboBox::outlineColourId, t.line);
        setColour (juce::ComboBox::textColourId, t.text);
        setColour (juce::ComboBox::arrowColourId, t.dim);
        setColour (juce::TextButton::buttonColourId, t.well);
        setColour (juce::TextButton::buttonOnColourId, theme::accent);
        setColour (juce::TextButton::textColourOffId, t.text);
        setColour (juce::Label::textColourId, t.text);
        setColour (juce::ScrollBar::thumbColourId, t.line);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (10.5f, (float) h * 0.6f), juce::Font::bold));
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (10.5f)); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool highlighted, bool down) override
    {
        const auto& t = mixTheme();
        auto r = b.getLocalBounds();
        auto c = bg;
        if (! b.isEnabled()) c = c.withMultipliedAlpha (0.5f);
        else if (down) c = c.darker (0.2f);
        else if (highlighted) c = c.brighter (t.dark ? 0.2f : 0.12f);
        g.setColour (c);                                       g.fillRect (r);
        g.setColour (juce::Colours::white.withAlpha (t.dark ? 0.09f : 0.5f)); g.fillRect (r.getX() + 1, r.getY() + 1, r.getWidth() - 2, 1);    // a fine bevel along the top
        g.setColour (b.getToggleState() ? c.darker (0.55f) : t.line); g.drawRect (r, 1);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto& t = mixTheme();
        auto r = juce::Rectangle<int> (0, 0, w, h);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId)); g.fillRect (r);
        g.setColour (juce::Colours::black.withAlpha (t.dark ? 0.35f : 0.08f)); g.fillRect (1, 1, w - 2, 1);        // recessed: a shadow along the top
        g.setColour (box.hasKeyboardFocus (true) ? theme::accent : t.line); g.drawRect (r, 1);
        const float cx = (float) w - 8.0f, cy = (float) h * 0.5f;
        juce::Path p; p.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.0f);
        g.setColour (t.dim.withAlpha (box.isEnabled() ? 1.0f : 0.4f)); g.fillPath (p);
    }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (3, 0, box.getWidth() - 14, box.getHeight());
        label.setFont (getComboBoxFont (box));
    }

    juce::Slider::SliderLayout getSliderLayout (juce::Slider& s) override
    {
        auto l = juce::LookAndFeel_V4::getSliderLayout (s);
        if (isFader (s)) l.sliderBounds = l.sliderBounds.reduced (0, kCapHalf);   // room for the cap at both ends of the travel
        return l;
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, const juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        const auto& t = mixTheme();
        if (isFader (s) && style == juce::Slider::LinearVertical)
        {
            const int cx = x + w / 2, p = (int) std::round (pos);
            g.setColour (t.groove);   g.fillRect (cx - 2, y - 4, 5, h + 8);                       // the slot the cap runs in
            g.setColour (t.line);     g.drawRect (cx - 2, y - 4, 5, h + 8, 1);
            g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRect (cx, y - 3, 1, h + 6);
            const juce::Colour cap = juce::Colour ((juce::uint32) (juce::int64) s.getProperties().getWithDefault ("cap", (juce::int64) 0xff9aa2ad));
            auto r = juce::Rectangle<int> (cx - 14, p - kCapHalf, 28, 2 * kCapHalf);
            const float rad = 6.0f;
            g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRoundedRectangle (r.toFloat().translated (0.0f, 1.5f), rad);    // a soft shadow under it
            g.setColour (cap);                                    g.fillRoundedRectangle (r.toFloat(), rad);
            g.setColour (cap.brighter (0.5f).withAlpha (0.8f));   g.fillRoundedRectangle (r.toFloat().reduced (2.0f, 1.0f).withHeight (6.0f), 3.0f);          // lit top
            g.setColour (cap.darker (0.5f).withAlpha (0.8f));     g.fillRoundedRectangle (r.toFloat().reduced (2.0f, 1.0f).withTrimmedTop ((float) r.getHeight() - 8.0f), 3.0f);   // shaded bottom
            g.setColour (cap.darker (0.45f).withAlpha (0.75f));
            for (int k : { -15, -12, -9, 9, 12, 15 }) g.fillRect (r.getX() + 5, p + k, r.getWidth() - 10, 1);                                               // fine grip lines
            g.setColour (chan::textOn (cap));                     g.fillRoundedRectangle ((float) r.getX() + 3.0f, (float) p - 1.0f, (float) r.getWidth() - 6.0f, 3.0f, 1.5f);   // the marker line
            g.setColour (juce::Colour (0xff10141a).withAlpha (0.9f)); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), rad, 1.0f);
            return;
        }
        if (isPan (s) && style == juce::Slider::LinearHorizontal)
        {
            const int cy = y + h / 2, cx = x + w / 2, p = (int) std::round (pos);
            const juce::Colour arc = juce::Colour ((juce::uint32) (juce::int64) s.getProperties().getWithDefault ("arc", (juce::int64) theme::accent.getARGB()));
            g.setColour (t.groove.withAlpha (t.dark ? 1.0f : 0.55f)); g.fillRect (x, cy - 1, w, 3);
            g.setColour (t.line);                                      g.drawRect (x, cy - 1, w, 3, 1);
            g.setColour (arc);                                         g.fillRect (juce::jmin (cx, p), cy, std::abs (p - cx) + 1, 1);
            g.setColour (t.dim);                                       g.fillRect (cx, cy - 5, 1, 4); g.fillRect (cx, cy + 3, 1, 4);              // centre mark
            for (int i = 1; i < 4; ++i) { g.setColour (t.soft); g.fillRect (x + (w - 1) * i / 4, cy - 3, 1, 2); g.fillRect (x + (w - 1) * i / 4, cy + 2, 1, 2); }
            auto th = juce::Rectangle<int> (p - 3, cy - 7, 7, 15);
            g.setColour (t.dark ? juce::Colour (0xffb7bfca) : juce::Colours::white); g.fillRect (th);
            g.setColour (t.text);                                                    g.drawRect (th, 1);
            g.fillRect (th.getCentreX(), th.getY() + 3, 1, th.getHeight() - 6);
            return;
        }
        FermataLookAndFeel::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startA, float endA, juce::Slider& s) override
    {
        const auto& t = mixTheme();
        const float size = (float) juce::jmin (w, h) - 2.0f, r = size * 0.5f;
        const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
        const float a = startA + pos * (endA - startA);
        const juce::Colour arc = juce::Colour ((juce::uint32) (juce::int64) s.getProperties().getWithDefault ("arc", (juce::int64) theme::accent.getARGB()));
        juce::Path track, value;
        track.addCentredArc (cx, cy, r - 0.5f, r - 0.5f, 0.0f, startA, endA, true);
        value.addCentredArc (cx, cy, r - 0.5f, r - 0.5f, 0.0f, startA, a, true);
        g.setColour (t.soft);  g.strokePath (track, juce::PathStrokeType (2.0f));
        g.setColour (arc);     g.strokePath (value, juce::PathStrokeType (2.0f));
        const float br = r * 0.66f;
        g.setColour (t.dark ? juce::Colour (0xff3d424b) : juce::Colours::white); g.fillEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f);
        g.setColour (t.line);                                                    g.drawEllipse (cx - br, cy - br, br * 2.0f, br * 2.0f, 1.0f);
        juce::Path pointer; pointer.addRectangle (-0.5f, -br + 1.5f, 1.0f, br * 0.6f);
        g.setColour (t.text); g.fillPath (pointer, juce::AffineTransform::rotation (a).translated (cx, cy));
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& sb, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override
    {
        const auto& t = mixTheme();
        g.setColour (t.dark ? juce::Colour (0xff0c0e11) : juce::Colour (0xffc9d0da)); g.fillRect (x, y, w, h);
        if (thumbSize <= 0) return;
        auto th = vertical ? juce::Rectangle<int> (x + 1, thumbStart, w - 2, thumbSize) : juce::Rectangle<int> (thumbStart, y + 1, thumbSize, h - 2);
        g.setColour ((t.dark ? juce::Colour (0xff59606c) : juce::Colour (0xff8a95a5)).withMultipliedBrightness (down ? 0.8f : over ? 1.15f : 1.0f));
        g.fillRect (th);
    }

    static constexpr int kCapHalf = 22;                  // the fader cap is 44 pixels tall
    static bool isFader (const juce::Slider& s) { return (bool) s.getProperties().getWithDefault ("fader", false); }
    static bool isPan (const juce::Slider& s)   { return (bool) s.getProperties().getWithDefault ("pan", false); }
};
} // namespace td
