#pragma once
#include "AppContext.h"

namespace td
{
/** Where a sound goes (the pitch pipe, the click): the chosen driver outputs written out ("7 - Stage L, 8 - Stage R"), and Choose... for the list of
    every output of the project with a tick box each - any number may be ticked. One line however many outputs the interface has (a MADI or RAVENNA
    system can have 64 or more); the list scrolls. It follows the Project Designer: outputs added, removed or renamed appear at once. */
class OutputPicker : public juce::Component, private juce::Timer
{
public:
    /** list: the project's list of chosen outputs (the picker changes it); onChange: after a change (save, tell the engine). */
    OutputPicker (AppContext& a, std::function<std::vector<int>&()> list, std::function<void()> onChange)
        : app (a), getList (std::move (list)), changed (std::move (onChange))
    {
        chooseButton.setTooltip ("Choose the outputs it plays to (any number)");
        chooseButton.setWantsKeyboardFocus (false);
        chooseButton.onClick = [this] { openList(); };
        addAndMakeVisible (chooseButton);
        summary.setFont (juce::FontOptions (12.5f));
        summary.setJustificationType (juce::Justification::centredLeft);
        summary.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (summary);
        refresh();
        startTimer (700);
    }

    /** The height it needs (one line, whatever the width). */
    int heightFor (int) const { return kRowH; }
    void resized() override
    {
        auto r = getLocalBounds().withHeight (kRowH);
        chooseButton.setBounds (r.removeFromLeft (96)); r.removeFromLeft (10);
        summary.setBounds (r);
    }
    /** Shows the outputs now chosen (and drops any that no longer exist). */
    void refresh()
    {
        auto& v = getList();
        const int n = (int) app.project.outputs.size();
        juce::StringArray names;
        for (int i : v) if (i >= 0 && i < n) names.add (app.project.outputLabel (i));
        juce::String text;
        if (n == 0) text = "No outputs yet: add them in the Project Designer.";
        else if (names.isEmpty()) text = "Nowhere yet: press Choose... and tick the outputs.";
        else if (names.size() <= 4) text = names.joinIntoString (",   ");
        else
        {
            juce::StringArray first; for (int k = 0; k < 3; ++k) first.add (names[k]);
            text = first.joinIntoString (",   ") + "   and " + juce::String (names.size() - 3) + " more";
        }
        summary.setText (text, juce::dontSendNotification);
        summary.setTooltip (names.joinIntoString ("\n"));
        summary.setColour (juce::Label::textColourId, names.isEmpty() ? theme::warn : theme::text);
        sig = signature();
    }

private:
    static constexpr int kRowH = 26;

    /** The list in the call-out: a tick box per output, and None. */
    struct OutputList : public juce::Component
    {
        explicit OutputList (OutputPicker& o) : owner (&o)
        {
            const int n = (int) o.app.project.outputs.size();
            const auto& v = o.getList();
            for (int i = 0; i < n; ++i)
            {
                auto* t = toggles.add (new juce::ToggleButton (o.app.project.outputLabel (i)));
                t->setToggleState (std::find (v.begin(), v.end(), i) != v.end(), juce::dontSendNotification);
                t->setColour (juce::ToggleButton::textColourId, theme::text);
                t->onClick = [this, i, t] { if (owner != nullptr) owner->set (i, t->getToggleState()); };
                holder.addAndMakeVisible (t);
            }
            none.setButtonText ("None");
            none.onClick = [this]
            {
                if (owner == nullptr) return;
                for (int i = 0; i < toggles.size(); ++i) if (toggles[i]->getToggleState()) { toggles[i]->setToggleState (false, juce::dontSendNotification); owner->set (i, false); }
            };
            addAndMakeVisible (none);
            title.setText ("Plays to:", juce::dontSendNotification);
            title.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            title.setColour (juce::Label::textColourId, theme::text);
            addAndMakeVisible (title);
            viewport.setViewedComponent (&holder, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);
            int w = 220;
            for (auto* t : toggles) w = juce::jmax (w, juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (14.0f)), t->getButtonText())) + 60);
            setSize (juce::jmin (w, 420), juce::jlimit (90, 460, 40 + toggles.size() * kLine + 10));
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto top = r.removeFromTop (26);
            none.setBounds (top.removeFromRight (70)); title.setBounds (top);
            r.removeFromTop (6);
            viewport.setBounds (r);
            const int w = r.getWidth() - (toggles.size() * kLine > r.getHeight() ? viewport.getScrollBarThickness() : 0);
            holder.setSize (w, juce::jmax (1, toggles.size() * kLine));
            for (int i = 0; i < toggles.size(); ++i) toggles[i]->setBounds (0, i * kLine, w, kLine);
        }
        static constexpr int kLine = 24;
        juce::Component::SafePointer<OutputPicker> owner;
        juce::Label title;
        juce::TextButton none;
        juce::Viewport viewport;
        juce::Component holder;
        juce::OwnedArray<juce::ToggleButton> toggles;
    };

    void set (int output, bool on)
    {
        auto& v = getList();
        v.erase (std::remove (v.begin(), v.end(), output), v.end());
        if (on) { v.push_back (output); std::sort (v.begin(), v.end()); }
        refresh();
        if (changed) changed();
    }
    void openList()
    {
        if (app.project.outputs.empty()) return;
        juce::CallOutBox::launchAsynchronously (std::make_unique<OutputList> (*this), chooseButton.getScreenBounds(), nullptr);
    }
    juce::String signature() const
    {
        juce::String s;
        for (int i = 0; i < (int) app.project.outputs.size(); ++i) s << app.project.outputLabel (i) << "|";
        for (int i : getList()) s << i << ",";
        return s;
    }
    void timerCallback() override { if (signature() != sig) refresh(); }

    AppContext& app;
    std::function<std::vector<int>&()> getList;
    std::function<void()> changed;
    juce::TextButton chooseButton { "Choose..." };
    juce::Label summary;
    juce::String sig;
};
/** The To talkback button of the click and the pitch pipe (3.1.0): lit = the sound also goes to the talkback pair (the TB output that the talkback mic
    and Playback go to), through the Click / Pipe box of the main window's stage speaker mixer - whatever Plays to says. */
inline void setUpTalkbackButton (juce::TextButton& b, const juce::String& sound)
{
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffff8a2e));          // (the orange of the main window's TB keys)
    b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    b.setWantsKeyboardFocus (false);
    b.setTooltip ("To talkback: " + sound + " also goes to the talkback speakers - the TB output that the talkback mic and Playback go to - as well as to the "
                  "outputs under Plays to. Its level there is the Click/Pipe box at the top right of the main window.");
}
} // namespace td
