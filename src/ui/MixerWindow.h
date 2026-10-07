#pragma once
#include "AppContext.h"
#include "MixerLook.h"

namespace td
{
class MixerStripsHolder;

/** A small square button with a drawn icon: the clipboard-with-a-fader (copy mix menu) and the sun / moon (look). */
class MixIconButton : public juce::Button
{
public:
    enum class Kind { CopyMix, Look, AltDeck };
    explicit MixIconButton (Kind k) : juce::Button ({}), kind (k) {}
    void setDarkLook (bool dark) { if (dark != darkLook) { darkLook = dark; repaint(); } }
    void setGlow (bool on) { if (on != glowing) { glowing = on; setToggleState (on, juce::dontSendNotification); repaint(); } }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& t = mixTheme();
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? t.soft : over ? t.field.brighter (0.15f) : t.field); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (t.line); g.drawRoundedRectangle (b, 4.0f, 1.0f);
        const auto ink = t.text;
        g.setColour (ink);
        auto c = b.getCentre();
        if (kind == Kind::AltDeck)                                   // a little key pad (like a Stream Deck): glows when this is the Alt Mixer
        {
            const juce::Colour lit (0xff29d3ff);
            if (glowing)
            {
                g.setColour (lit.withAlpha (0.22f)); g.fillRoundedRectangle (b.expanded (0.0f), 4.0f);
                g.setColour (lit.withAlpha (0.55f)); g.drawRoundedRectangle (b, 4.0f, 2.0f);
            }
            for (int r = 0; r < 2; ++r)
                for (int k = 0; k < 3; ++k)
                {
                    juce::Rectangle<float> key (c.x - 10.0f + (float) k * 7.0f, c.y - 6.5f + (float) r * 7.0f, 6.0f, 6.0f);
                    g.setColour (glowing ? lit : ink.withAlpha (0.65f)); g.fillRoundedRectangle (key, 1.4f);
                }
            return;
        }
        if (kind == Kind::CopyMix)
        {
            const float w = 12.0f, h = 15.0f;
            juce::Rectangle<float> board (c.x - w * 0.5f, c.y - h * 0.5f + 1.0f, w, h);
            g.drawRoundedRectangle (board, 1.5f, 1.2f);
            g.fillRoundedRectangle (juce::Rectangle<float> (c.x - 3.0f, board.getY() - 2.0f, 6.0f, 3.5f), 1.0f);       // the clip
            g.fillRect (c.x - 0.6f, board.getY() + 3.5f, 1.2f, h - 6.5f);                                              // the fader slot
            g.fillRect (c.x - 3.0f, board.getY() + 6.0f, 6.0f, 2.4f);                                                   // the fader cap
            g.fillRect (c.x - 4.2f, board.getY() + 4.2f, 1.5f, 1.0f); g.fillRect (c.x + 2.7f, board.getY() + 4.2f, 1.5f, 1.0f);
        }
        else if (darkLook)                                           // the dark look is on: a moon
        {
            juce::Path moon;
            moon.addEllipse (c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f);
            juce::Path bite; bite.addEllipse (c.x - 2.0f, c.y - 8.0f, 11.0f, 11.0f);
            moon.setUsingNonZeroWinding (false);
            moon.addPath (bite);
            g.fillPath (moon);
        }
        else                                                         // the light look is on: a sun
        {
            g.fillEllipse (c.x - 3.6f, c.y - 3.6f, 7.2f, 7.2f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                g.drawLine (c.x + std::cos (a) * 5.6f, c.y + std::sin (a) * 5.6f, c.x + std::cos (a) * 8.0f, c.y + std::sin (a) * 8.0f, 1.3f);
            }
        }
    }
private:
    Kind kind; bool darkLook = true, glowing = false;
};

/** One mixer. Several can be open at once; each has its own solo/mute/levels/sends/plug-ins and output pair. */
class MixerComponent : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    MixerComponent (AppContext&, const juce::Uuid& mixerId);
    ~MixerComponent() override;
    void resized() override;
    void parentHierarchyChanged() override;
    juce::Uuid getMixerId() const { return mixerId; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuild();
    int naturalContentW() const;
    int naturalContentH() const;
    void updateWindowLimits();
    void updateBars();
    bool inLimits = false;
    int lastMaxW = 0, lastMaxH = 0;
    MixerState* mixer() const;

    AppContext& app;
    juce::Uuid mixerId;
    juce::Label statusLabel, nameCaption;
    juce::TextEditor nameEditor;
    MixIconButton themeButton { MixIconButton::Kind::Look }, copyButton { MixIconButton::Kind::CopyMix }, altButton { MixIconButton::Kind::AltDeck };
    juce::TextButton auditionButton, deleteButton { "Delete this mixer" };
    int slotCount = 1;                    // insert slots shown on every strip: one more than the highest one in use
    int slotsNeeded() const;
    int topBarMinWidth() const;
    void showCopyMenu();
    bool withInsertsToggle = false;
    int statusFlash = 0;
    void updateAuditionButton();
    MixerLookAndFeel mixLook;             // declared before the viewport: it must outlive it
    bool shownDark = false;
    juce::Viewport viewport;
    std::unique_ptr<MixerStripsHolder> holder;
};
} // namespace td
