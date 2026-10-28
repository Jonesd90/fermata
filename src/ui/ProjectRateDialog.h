#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
/** The project's sample rate, chosen on purpose (3.9.0): four buttons - 44.1, 48, 96, 192 kHz - and nothing chosen to begin with, so a project is never
    made at 48 kHz by accident when it was meant to be 96 kHz. The OK button stays greyed until one is picked. Shown when a project is made (after its
    folder and name), when a project from before 3.9.0 with no takes is opened or about to record, and from Project > Project sample rate (while the
    project has no takes). Enter = OK once a rate is picked; Esc / Cancel = nothing chosen. */
class ProjectRateChooser : public juce::Component
{
public:
    ProjectRateChooser (const juce::String& heading, const juce::String& text, double preselect, double deviceRate, const juce::String& okText, bool cancellable)
    {
        title.setText (heading, juce::dontSendNotification);
        title.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, theme::text);
        info.setText (text, juce::dontSendNotification);
        info.setFont (juce::FontOptions (13.5f));
        info.setColour (juce::Label::textColourId, theme::text);
        info.setJustificationType (juce::Justification::topLeft);
        device.setText (deviceRate > 0.0 ? "The audio device is running at " + kHz (deviceRate) + " now." : juce::String ("No audio device is open."), juce::dontSendNotification);
        device.setFont (juce::FontOptions (12.5f));
        device.setColour (juce::Label::textColourId, theme::dimText);
        for (auto* c : std::initializer_list<juce::Component*> { &title, &info, &device, &ok }) addAndMakeVisible (c);
        if (cancellable) addAndMakeVisible (cancel);
        for (int i = 0; i < 4; ++i)
        {
            auto& b = rates[(size_t) i];
            const double r = Project::kProjectRates[i];
            b.setButtonText (kHz (r));
            b.setRadioGroupId (391);
            b.setClickingTogglesState (true);
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff19a08a));
            b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            b.setToggleState (std::abs (r - preselect) < 0.5, juce::dontSendNotification);
            b.onClick = [this] { ok.setEnabled (picked() > 0.0); };
            addAndMakeVisible (b);
        }
        ok.setButtonText (okText);
        ok.setEnabled (picked() > 0.0);
        ok.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
        ok.onClick = [this] { finish (picked()); };
        cancel.onClick = [this] { finish (0.0); };
        setWantsKeyboardFocus (true);
        setSize (560, 270);
    }
    ~ProjectRateChooser() override
    {
        if (! done && onDone) { auto f = onDone; juce::MessageManager::callAsync ([f] { f (0.0); }); }     // (closed with Esc or the title bar: nothing chosen)
    }

    /** Called once: the rate picked, or 0 when nothing was. */
    std::function<void (double)> onDone;

    double picked() const
    {
        for (int i = 0; i < 4; ++i) if (rates[(size_t) i].getToggleState()) return Project::kProjectRates[i];
        return 0.0;
    }
    static juce::String kHz (double r) { return juce::String (r / 1000.0, std::fmod (r, 1000.0) == 0.0 ? 0 : 1) + " kHz"; }

    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        title.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        info.setBounds (r.removeFromTop (58));
        r.removeFromTop (8);
        auto row = r.removeFromTop (48);
        const int bw = (row.getWidth() - 3 * 10) / 4;
        for (auto& b : rates) { b.setBounds (row.removeFromLeft (bw)); row.removeFromLeft (10); }
        r.removeFromTop (8);
        device.setBounds (r.removeFromTop (20));
        auto bottom = r.removeFromBottom (34);
        ok.setBounds (bottom.removeFromRight (170));
        if (cancel.isVisible()) { bottom.removeFromRight (10); cancel.setBounds (bottom.removeFromRight (110)); }
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey) { if (ok.isEnabled()) finish (picked()); return true; }
        if (k == juce::KeyPress::escapeKey && cancel.isVisible()) { finish (0.0); return true; }
        return true;                                                       // (the program's keys do nothing while this is up)
    }

private:
    void finish (double rate)
    {
        if (done) return;
        done = true;
        auto f = onDone;
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);     // (deletes the window, and this with it)
        if (f) juce::MessageManager::callAsync ([f, rate] { f (rate); });
    }
    juce::Label title, info, device;
    std::array<juce::TextButton, 4> rates;
    juce::TextButton ok, cancel { "Cancel" };
    bool done = false;
};

/** Puts the chooser up (modal, in front) and calls 'chosen' with the rate picked, or with 0 when nothing was. */
inline void chooseProjectRate (AppContext& app, const juce::String& heading, const juce::String& text, double preselect, const juce::String& okText, bool cancellable,
                               std::function<void (double)> chosen, juce::Component* around = nullptr)
{
    auto* dev = app.devices.getCurrentAudioDevice();
    auto c = std::make_unique<ProjectRateChooser> (heading, text, preselect, dev != nullptr ? dev->getCurrentSampleRate() : 0.0, okText, cancellable);
    c->onDone = std::move (chosen);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (c.release());
    o.dialogTitle = "Project sample rate";
    o.dialogBackgroundColour = theme::window;
    o.escapeKeyTriggersCloseButton = cancellable;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = around;
    if (auto* dw = o.launchAsync())
    {
        dw->setAlwaysOnTop (true);
        if (auto* content = dw->getContentComponent()) content->grabKeyboardFocus();
    }
}
} // namespace td
