#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MarkerStrip.h"
#include "../core/Edit.h"
#include "../core/Feedback.h"

namespace td
{
/** Listeners' comments drawn on an edit window: a flag in the top strip for every moment someone commented (several comments at one moment are one flag, "+2" for the replies).
    Click a flag to read the thread; tick comments off as done; the flag goes grey when all of them are. */
namespace feedbackui
{
inline juce::Font flagFont() { return juce::Font (juce::FontOptions (11.0f, juce::Font::bold)); }

inline juce::String flagText (const std::vector<FeedbackNote>& list, const std::vector<int>& thread)
{
    const auto& first = list[(size_t) thread.front()];
    juce::String t = first.bar.isNotEmpty() ? first.bar : feedback::timeText (first.seconds);
    if (thread.size() > 1) t += "  +" + juce::String ((int) thread.size() - 1);
    return t;
}
inline int flagWidth (const juce::String& text) { return juce::jlimit (26, 150, juce::GlyphArrangement::getStringWidthInt (flagFont(), text) + 16); }

inline bool allDone (const std::vector<FeedbackNote>& list, const std::vector<int>& thread)
{
    for (int i : thread) if (! list[(size_t) i].done) return false;
    return true;
}

inline void paintFlags (juce::Graphics& g, const std::vector<FeedbackNote>& list, const std::function<int (double)>& xOf, int stripY, int leftLimit, int totalHeight, const juce::String& selectedKey)
{
    const int h = markerui::kStripH;
    const auto teal = juce::Colour (0xff35b7c9);                                      // feedback flags are teal (the user's own markers are amber); the colour the website gave is a stripe on the flag
    g.setFont (flagFont());
    for (auto& th : feedback::threads (list))
    {
        const auto& first = list[(size_t) th.front()];
        const int x = xOf (first.seconds);
        if (x < leftLimit) continue;
        const bool done = allDone (list, th), sel = first.key == selectedKey;
        const auto body = done ? juce::Colour (0xff6d7078) : teal;
        g.setColour (body.withAlpha (sel ? 0.9f : 0.4f));
        g.fillRect (x, stripY + h, sel ? 2 : 1, juce::jmax (0, totalHeight - stripY - h));
        const auto text = flagText (list, th);
        auto f = juce::Rectangle<int> (x, stripY, flagWidth (text), h - 1);
        g.setColour (body); g.fillRoundedRectangle (f.toFloat(), 3.0f);
        if (first.colour.isNotEmpty() && ! done) { g.setColour (juce::Colour (feedback::colourArgb (first.colour))); g.fillRect (f.getX(), f.getY(), 4, f.getHeight()); }
        if (sel) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (f.toFloat().reduced (0.5f), 3.0f, 1.5f); }
        g.setColour (done ? juce::Colour (0xffd0d2d8) : juce::Colour (0xff0b1b1e));
        g.drawText (text, f.reduced (8, 0), juce::Justification::centredLeft, true);
        if (done) g.fillRect (f.getX() + 3, f.getCentreY(), f.getWidth() - 6, 1);
    }
}

/** The thread whose flag is at x (in the strip), as indices into 'list' - empty when none. */
inline std::vector<int> threadAt (const std::vector<FeedbackNote>& list, int x, const std::function<int (double)>& xOf)
{
    std::vector<int> hit;
    for (auto& th : feedback::threads (list))
    {
        const int fx = xOf (list[(size_t) th.front()].seconds), w = flagWidth (flagText (list, th));
        if ((x >= fx && x < fx + w) || std::abs (x - fx) <= 3) hit = th;               // (the last one drawn is on top)
    }
    return hit;
}

/** The pop-up that opens on a flag: every comment of the thread, with its bar, who wrote it, a Done tick and a Delete. */
class ThreadBox : public juce::Component
{
public:
    ThreadBox (std::vector<FeedbackNote> notes_, std::function<void (const juce::String&, bool)> onDone, std::function<void (const juce::String&)> onDelete, std::function<void()> onGo)
        : notes (std::move (notes_)), doneCb (std::move (onDone)), delCb (std::move (onDelete)), goCb (std::move (onGo))
    {
        addAndMakeVisible (goBtn);
        goBtn.setButtonText ("Go here"); goBtn.onClick = [this] { if (goCb) goCb(); };
        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto* t = ticks.add (new juce::ToggleButton ("Done")); t->setToggleState (notes[i].done, juce::dontSendNotification);
            const auto key = notes[i].key;
            t->onClick = [this, i, key] { if (doneCb) doneCb (key, ticks[(int) i]->getToggleState()); notes[i].done = ticks[(int) i]->getToggleState(); repaint(); };
            addAndMakeVisible (t);
            auto* d = dels.add (new juce::TextButton ("Delete"));
            d->onClick = [this, key] { if (delCb) delCb (key); if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
            addAndMakeVisible (d);
        }
        layoutNow();
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff23262b));
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i]; const auto r = blocks[i];
            g.setColour (juce::Colour (feedback::colourArgb (n.colour))); g.fillRect (r.getX(), r.getY(), 4, r.getHeight() - 6);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            auto head = (n.who.isNotEmpty() ? n.who : juce::String ("Someone")) + juce::String::fromUTF8 ("  \xC2\xB7  ") + feedback::timeText (n.seconds);
            if (n.bar.isNotEmpty()) head += juce::String::fromUTF8 ("  \xC2\xB7  ") + n.bar;
            g.drawText (head, r.getX() + 12, r.getY(), r.getWidth() - 150, 20, juce::Justification::centredLeft, true);
            juce::AttributedString as; as.setJustification (juce::Justification::topLeft);
            as.append (n.text, juce::Font (juce::FontOptions (13.0f)), n.done ? juce::Colour (0xff9a9da6) : juce::Colour (0xffe8e8e8));
            juce::TextLayout tl; tl.createLayout (as, (float) (r.getWidth() - 16));
            tl.draw (g, juce::Rectangle<float> ((float) r.getX() + 12, (float) r.getY() + 22, (float) r.getWidth() - 16, tl.getHeight() + 2.0f));
        }
    }
    void resized() override { layoutNow(); }

private:
    void layoutNow()
    {
        constexpr int w = 360;
        blocks.clear();
        int y = 6;
        goBtn.setBounds (w - 76, 6, 66, 22);
        y = 34;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            juce::AttributedString as; as.append (notes[i].text, juce::Font (juce::FontOptions (13.0f)), juce::Colours::white);
            juce::TextLayout tl; tl.createLayout (as, (float) (w - 36));
            const int h = 22 + (int) std::ceil (tl.getHeight()) + 10;
            blocks.push_back (juce::Rectangle<int> (10, y, w - 20, h));
            ticks[(int) i]->setBounds (w - 150, y, 60, 20); dels[(int) i]->setBounds (w - 84, y, 60, 20);
            y += h + 4;
        }
        setSize (w, juce::jmin (y + 6, 420));
    }
    std::vector<FeedbackNote> notes;
    std::function<void (const juce::String&, bool)> doneCb; std::function<void (const juce::String&)> delCb; std::function<void()> goCb;
    juce::TextButton goBtn;
    juce::OwnedArray<juce::ToggleButton> ticks; juce::OwnedArray<juce::TextButton> dels;
    std::vector<juce::Rectangle<int>> blocks;
};

/** Opens the thread under 'flagArea' (screen coordinates). 'changed' is called after a tick / delete; 'goTo' puts the playhead at the thread's time. */
inline void showThread (std::function<EditDef*()> getEdit, const std::vector<int>& thread, juce::Rectangle<int> flagArea, std::function<void()> changed, std::function<void (double)> goTo)
{
    auto* e = getEdit(); if (e == nullptr) return;
    std::vector<FeedbackNote> notes; for (int i : thread) if (i < (int) e->feedback.size()) notes.push_back (e->feedback[(size_t) i]);
    if (notes.empty()) return;
    const double at = notes.front().seconds;
    auto box = std::make_unique<ThreadBox> (notes,
        [getEdit, changed] (const juce::String& key, bool done) { if (auto* ed = getEdit()) { for (auto& n : ed->feedback) if (n.key == key) n.done = done; if (changed) changed(); } },
        [getEdit, changed] (const juce::String& key)
        {
            if (auto* ed = getEdit())
            {
                ed->feedback.erase (std::remove_if (ed->feedback.begin(), ed->feedback.end(), [&] (const FeedbackNote& n) { return n.key == key; }), ed->feedback.end());
                if (changed) changed();
            }
        },
        [goTo, at] { if (goTo) goTo (at); });
    juce::CallOutBox::launchAsynchronously (std::move (box), flagArea, nullptr);
}
} // namespace feedbackui
} // namespace td
