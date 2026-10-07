#pragma once
#include "AppContext.h"
#include "GridEditor.h"

namespace td
{
/** The Project Designer: the audio driver's I/O, audio tracks, Int Buses, Ext Buses and mixers. */
class DesignComponent : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit DesignComponent (AppContext&);
    ~DesignComponent() override;
    void resized() override;

private:
    struct Page : public juce::Component
    {
        Page() { addAndMakeVisible (grid); addAndMakeVisible (note); }      // (without this the list and its note never appeared)
        GridEditor grid;
        juce::OwnedArray<juce::TextButton> buttons;
        juce::Label note;
        juce::TextButton* addButton (const juce::String& text, std::function<void()> fn);
        void resized() override;
    };
    /** Driver inputs on the left, driver outputs on the right. */
    struct IoPage : public juce::Component
    {
        GridEditor ins, outs;
        juce::Label inCaption, outCaption, note;
        void resized() override;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void buildIoPage();
    void buildTracksPage();
    void buildBusPage (Page&, bool external);
    void buildMixersPage();
    bool locked() const;
    void refreshAll();
    int nextFreeInput() const;
    juce::StringArray inputItems (bool withNone) const;
    static int parseInputItem (const juce::String&);

    AppContext& app;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    IoPage ioPage;
    Page tracksPage, intPage, extPage, mixersPage;
    juce::Label lockNote;
};
} // namespace td
