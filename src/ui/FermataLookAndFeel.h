#pragma once
#include "Uikit.h"

namespace td { namespace fontdata { extern const unsigned char interRegular[]; extern const unsigned long interRegularSize; extern const unsigned char interSemiBold[]; extern const unsigned long interSemiBoldSize; } }

namespace td
{
/** Flat, square, 1-pixel-outlined controls with strong contrast: no rounded blobs, no gradients. */
class FermataLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static FermataLookAndFeel*& instance() { static FermataLookAndFeel* i = nullptr; return i; }
    ~FermataLookAndFeel() override { if (instance() == this) instance() = nullptr; }

    /** (Re)sets every colour from the current palette: called when the dark / light look is switched. */
    void applyTheme()
    {
        setColourScheme (theme::scheme());
        setColour (juce::ResizableWindow::backgroundColourId, theme::window);
        setColour (juce::TextButton::buttonColourId, theme::button);
        setColour (juce::TextButton::buttonOnColourId, theme::accent);
        setColour (juce::TextButton::textColourOffId, theme::text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ComboBox::backgroundColourId, theme::field);
        setColour (juce::ComboBox::outlineColourId, theme::border);
        setColour (juce::ComboBox::textColourId, theme::text);
        setColour (juce::ComboBox::arrowColourId, theme::text);
        setColour (juce::TextEditor::backgroundColourId, theme::field);
        setColour (juce::TextEditor::textColourId, theme::text);
        setColour (juce::TextEditor::highlightColourId, theme::selected);
        setColour (juce::TextEditor::highlightedTextColourId, theme::text);
        setColour (juce::TextEditor::outlineColourId, theme::border);
        setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
        setColour (juce::CaretComponent::caretColourId, theme::text);
        setColour (juce::Label::textColourId, theme::text);
        setColour (juce::Slider::textBoxTextColourId, theme::text);
        setColour (juce::Slider::textBoxBackgroundColourId, theme::field);
        setColour (juce::Slider::textBoxOutlineColourId, theme::border);
        setColour (juce::ToggleButton::textColourId, theme::text);
        setColour (juce::ToggleButton::tickColourId, theme::accent);
        setColour (juce::ToggleButton::tickDisabledColourId, theme::grid);
        setColour (juce::PopupMenu::backgroundColourId, theme::panel);
        setColour (juce::PopupMenu::textColourId, theme::text);
        setColour (juce::PopupMenu::headerTextColourId, theme::dimText);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TableHeaderComponent::backgroundColourId, theme::ruler);
        setColour (juce::TableHeaderComponent::textColourId, theme::text);
        setColour (juce::TableHeaderComponent::outlineColourId, theme::border);
        setColour (juce::ListBox::backgroundColourId, theme::window);
        setColour (juce::ListBox::textColourId, theme::text);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff59606c));
        setColour (juce::TooltipWindow::backgroundColourId, theme::panel);
        setColour (juce::TooltipWindow::textColourId, theme::text);
        setColour (juce::TooltipWindow::outlineColourId, theme::border);
        setColour (juce::AlertWindow::backgroundColourId, theme::window);
        setColour (juce::AlertWindow::textColourId, theme::text);
        setColour (juce::AlertWindow::outlineColourId, theme::border);
        setColour (juce::DocumentWindow::textColourId, theme::text);
        setColour (juce::ScrollBar::thumbColourId, theme::border);
    }

    FermataLookAndFeel() : juce::LookAndFeel_V4 (theme::scheme())
    {
        if (instance() == nullptr) instance() = this;
        regular  = juce::Typeface::createSystemTypefaceFor (fontdata::interRegular,  (size_t) fontdata::interRegularSize);
        semibold = juce::Typeface::createSystemTypefaceFor (fontdata::interSemiBold, (size_t) fontdata::interSemiBoldSize);
        applyTheme();
    }

    /** Everything that does not name a font uses Inter (regular, and semibold for bold text), bundled inside the program. */
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override
    {
        const auto n = font.getTypefaceName();
        if ((n == juce::Font::getDefaultSansSerifFontName() || n.isEmpty()) && regular != nullptr)
            return font.isBold() && semibold != nullptr ? semibold : regular;
        return juce::LookAndFeel_V4::getTypefaceForFont (font);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (13.0f, (float) buttonHeight * 0.52f), juce::Font::bold));
    }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.0f)); }

    /** Text is black on light buttons and white on dark ones, whatever colour the button was given. */
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        const auto fill = b.findColour (b.getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        g.setColour ((fill.getPerceivedBrightness() > 0.6f ? juce::Colour (0xff14181f) : juce::Colours::white).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds();
        auto c = bg;
        if (! b.isEnabled()) c = c.withMultipliedAlpha (0.5f);
        else if (down) c = c.darker (0.25f);
        else if (highlighted) c = c.brighter (0.18f);
        g.setColour (c);
        g.fillRect (r);
        g.setColour (b.getToggleState() ? c.brighter (0.4f) : theme::border);
        g.drawRect (r, 1);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool) override
    {
        const int s = 16;
        auto box = juce::Rectangle<int> (2, (b.getHeight() - s) / 2, s, s);
        g.setColour (theme::field);
        g.fillRect (box);
        g.setColour (highlighted ? theme::text : theme::border);
        g.drawRect (box, 1);
        if (b.getToggleState())
        {
            g.setColour (theme::accent);
            g.fillRect (box.reduced (4));
        }
        g.setColour (b.isEnabled() ? theme::text : theme::grid);
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (s + 8), juce::Justification::centredLeft, true);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<int> (0, 0, width, height);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRect (r);
        g.setColour (box.hasKeyboardFocus (true) ? theme::accent : box.findColour (juce::ComboBox::outlineColourId));
        g.drawRect (r, 1);
        const float cx = (float) width - 12.0f, cy = (float) height * 0.5f;
        juce::Path p;
        p.addTriangle (cx - 4.5f, cy - 2.5f, cx + 4.5f, cy - 2.5f, cx, cy + 3.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.4f));
        g.fillPath (p);
    }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (4, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
        g.fillRect (0, 0, w, h);
    }
    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        g.setColour (e.hasKeyboardFocus (true) ? e.findColour (juce::TextEditor::focusedOutlineColourId) : e.findColour (juce::TextEditor::outlineColourId));
        g.drawRect (0, 0, w, h, 1);
    }

    // ---- sliders: thin crisp tracks, square thumbs
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, const juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }
        const bool horiz = style == juce::Slider::LinearHorizontal;
        const auto fill = s.findColour (juce::Slider::trackColourId).isTransparent() ? theme::accent : theme::accent;
        const int p = (int) std::round (pos);
        if (horiz)
        {
            const int cy = y + h / 2;
            g.setColour (juce::Colour (0xff0c0e11)); g.fillRect (x, cy - 2, w, 4);
            g.setColour (fill);                      g.fillRect (x, cy - 2, juce::jmax (0, p - x), 4);
            g.setColour (theme::border);             g.drawRect (x, cy - 2, w, 4, 1);
            auto th = juce::Rectangle<int> (p - 4, cy - 9, 9, 18);
            g.setColour (juce::Colour (0xffb7bfca)); g.fillRect (th);
            g.setColour (juce::Colour (0xff10141a));               g.drawRect (th, 1);
        }
        else
        {
            const int cx = x + w / 2;
            g.setColour (juce::Colour (0xff0c0e11)); g.fillRect (cx - 3, y, 6, h);
            g.setColour (theme::border);             g.drawRect (cx - 3, y, 6, h, 1);
            // dB ticks beside the groove
            g.setColour (theme::border.withAlpha (0.6f));
            for (int i = 0; i <= 8; ++i) { const int ty = y + (int) ((double) h * i / 8.0); g.fillRect (cx + 6, ty, 5, 1); g.fillRect (cx - 10, ty, 5, 1); }
            auto th = juce::Rectangle<int> (cx - 12, p - 6, 24, 12);
            g.setColour (juce::Colour (0xffb7bfca)); g.fillRect (th);
            g.setColour (juce::Colour (0xff10141a));               g.drawRect (th, 1);
            g.fillRect (th.getX() + 2, th.getCentreY(), th.getWidth() - 4, 1);
        }
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startA, float endA, juce::Slider&) override
    {
        const float size = (float) juce::jmin (w, h) - 6.0f, r = size * 0.5f;
        const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
        const float a = startA + pos * (endA - startA);
        juce::Path track, value;
        track.addCentredArc (cx, cy, r - 1.0f, r - 1.0f, 0.0f, startA, endA, true);
        value.addCentredArc (cx, cy, r - 1.0f, r - 1.0f, 0.0f, startA, a, true);
        g.setColour (juce::Colour (0xff0c0e11)); g.strokePath (track, juce::PathStrokeType (4.0f));
        g.setColour (theme::accent);             g.strokePath (value, juce::PathStrokeType (4.0f));
        g.setColour (juce::Colour (0xff3d424b)); g.fillEllipse (cx - r * 0.55f, cy - r * 0.55f, r * 1.1f, r * 1.1f);
        g.setColour (theme::text);
        g.drawEllipse (cx - r * 0.55f, cy - r * 0.55f, r * 1.1f, r * 1.1f, 1.0f);
        juce::Path pointer;
        pointer.addRectangle (-1.0f, -r * 0.55f, 2.0f, r * 0.5f);
        g.fillPath (pointer, juce::AffineTransform::rotation (a).translated (cx, cy));
    }

    // ---- scroll bars: flat
    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& sb, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override
    {
        g.setColour (juce::Colour (0xff0c0e11));
        g.fillRect (x, y, w, h);
        if (thumbSize <= 0) return;
        auto t = vertical ? juce::Rectangle<int> (x + 1, thumbStart, w - 2, thumbSize) : juce::Rectangle<int> (thumbStart, y + 1, thumbSize, h - 2);
        g.setColour (sb.findColour (juce::ScrollBar::thumbColourId).withMultipliedBrightness (down ? 0.8f : over ? 1.0f : 0.85f));
        g.fillRect (t);
    }
    int getDefaultScrollbarWidth() override { return 14; }

    // ---- tabs (Project Designer)
    void drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool down) override
    {
        auto r = b.getActiveArea();
        const bool front = b.isFrontTab();
        g.setColour (front ? juce::Colour (0xff3a404a) : theme::panel.darker (over || down ? 0.08f : 0.0f));
        g.fillRect (r);
        g.setColour (theme::border); g.drawRect (r, 1);
        if (front) { g.setColour (theme::accent); g.fillRect (r.getX(), r.getY(), r.getWidth(), 3); }
        g.setColour (theme::text);
        g.setFont (juce::Font (juce::FontOptions (14.5f, front ? juce::Font::bold : juce::Font::plain)));
        g.drawText (b.getButtonText(), r.reduced (4, 0), juce::Justification::centred, true);
    }
    int getTabButtonBestWidth (juce::TabBarButton& b, int) override { return juce::jmax (110, b.getButtonText().length() * 9 + 28); }
private:
    juce::Typeface::Ptr regular, semibold;
};
} // namespace td
