#pragma once
#include "AppContext.h"
#include "../core/Ravenna.h"

namespace td
{
/** The window where you say which Merging devices have preamps that Fermata should control: a name, the IP address, and which driver input
    the device's first preamp channel feeds (its other channels follow in order). Nothing is sent to a device until you move a control. */
class PreampSetupPanel : public juce::Component, private juce::Timer
{
public:
    explicit PreampSetupPanel (AppContext& a) : app (a)
    {
        rows = app.project.preampDevices;
        intro.setText ("Each device is reached over the network at its IP address. Its preamp channels feed consecutive driver inputs, starting at 'First input'. "
                       "Fermata only reads the device when it connects: it never changes a preamp until you do.", juce::dontSendNotification);
        intro.setFont (juce::FontOptions (12.5f)); intro.setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (intro);
        addButton.setButtonText ("Add device");        addButton.onClick = [this] { commitEditors(); rows.push_back ({ "Device " + juce::String ((int) rows.size() + 1), "165.165.1.", 0 }); build(); };
        usualButton.setButtonText ("Add my usual four"); usualButton.setTooltip ("Anubis .20, MT48 .30, THAP .40, BHAP .50 - change the addresses if they differ");
        usualButton.onClick = [this]
        {
            commitEditors();
            rows = { { "Anubis", "165.165.1.20", 0 }, { "MT48", "165.165.1.30", 0 }, { "THAP", "165.165.1.40", 0 }, { "BHAP", "165.165.1.50", 0 } };
            build();
        };
        stackButton.setButtonText ("Stack in order"); stackButton.setTooltip ("Sets 'First input' so that the devices follow each other in the list order, using the channel counts they report");
        stackButton.onClick = [this] { stack(); };
        applyButton.setButtonText ("Apply and connect"); applyButton.setColour (juce::TextButton::buttonColourId, theme::accent);
        applyButton.onClick = [this] { commitEditors(); app.setPreampDevices (rows); };
        closeButton.setButtonText ("Close"); closeButton.onClick = [] { if (auto* w = juce::Component::getCurrentlyModalComponent()) w->exitModalState (0); };
        findButton.setButtonText ("Find automatically");
        findButton.setTooltip ("Asks ANEMAN (the Merging network manager on this PC) which devices exist and which are patched to which ASIO inputs, then sets the list and connects");
        findButton.onClick = [this] { findAutomatically(); };
        found.setFont (juce::FontOptions (12.0f)); found.setColour (juce::Label::textColourId, theme::dimText); found.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (found);
        deviceNote.setFont (juce::FontOptions (12.0f)); deviceNote.setColour (juce::Label::textColourId, theme::warn); deviceNote.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (deviceNote);
        for (auto* b : { &addButton, &usualButton, &stackButton, &applyButton, &closeButton, &findButton }) addAndMakeVisible (b);
        build();
        startTimerHz (2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        { auto t = r.removeFromTop (22); intro.placeRightOf (t); }
        auto bottom = r.removeFromBottom (grid::rowH);
        findButton.setBounds (bottom.removeFromLeft (grid::btnW + 40));
        closeButton.setBounds (bottom.removeFromRight (grid::btnW));
        bottom.removeFromRight (grid::gap);
        applyButton.setBounds (bottom.removeFromRight (grid::btnW + 20));
        found.setBounds (r.removeFromBottom (96));
        auto top = r.removeFromBottom (grid::rowH);
        deviceNote.setBounds (r.removeFromBottom (40));
        addButton.setBounds (grid::cell (top, 0));
        usualButton.setBounds (grid::cell (top, 1).withWidth (grid::btnW + 24));
        stackButton.setBounds (grid::cell (top, 1).withWidth (grid::btnW).translated (grid::btnW + grid::gap + 30, 0));
        layoutRows (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::window);
        g.setColour (theme::dimText); g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        const int y = 12 + 22 + 4;                      // the column titles sit between the intro line and the first row (layoutRows leaves this room)
        g.drawText ("NAME", 12, y, 120, 16, juce::Justification::centredLeft);
        g.drawText ("IP ADDRESS", 12 + 130, y, 140, 16, juce::Justification::centredLeft);
        g.drawText ("FIRST INPUT", 12 + 280, y, 90, 16, juce::Justification::centredLeft);
        g.drawText ("STATUS", 12 + 380, y, 200, 16, juce::Justification::centredLeft);
    }

private:
    struct Row
    {
        juce::TextEditor name, host, first; juce::Label status; juce::TextButton remove { "Remove" };
    };

    void build()
    {
        ui.clear();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto* r = ui.add (new Row());
            r->name.setText (rows[i].name, false); r->host.setText (rows[i].host, false); r->first.setText (juce::String (rows[i].firstInput + 1), false);
            r->first.setInputRestrictions (4, "0123456789");
            r->status.setFont (juce::FontOptions (12.0f));
            r->remove.onClick = [this, i]
            {
                // Rebuilding destroys this very button, so it must wait until its click has finished (doing it here crashed).
                commitEditors();
                juce::Component::SafePointer<juce::Component> self (this);
                juce::MessageManager::callAsync ([self, this, i]
                {
                    if (self == nullptr || i >= rows.size()) return;
                    rows.erase (rows.begin() + (long) i);
                    build();
                });
            };
            for (auto* c : std::initializer_list<juce::Component*> { &r->name, &r->host, &r->first, &r->status, &r->remove }) addAndMakeVisible (c);
        }
        setSize (700, 204 + 36 * (int) juce::jmax (4, (int) rows.size()) + 2 * grid::rowH + 20);
        resized(); refreshStatus();
    }

    void layoutRows (juce::Rectangle<int> area)
    {
        area.removeFromTop (24);                        // room for the column titles drawn in paint()
        for (auto* r : ui)
        {
            auto row = area.removeFromTop (34).withSizeKeepingCentre (area.getWidth(), grid::btnH);
            r->name.setBounds (row.removeFromLeft (120)); row.removeFromLeft (10);
            r->host.setBounds (row.removeFromLeft (140)); row.removeFromLeft (10);
            r->first.setBounds (row.removeFromLeft (70)); row.removeFromLeft (20);
            r->remove.setBounds (row.removeFromRight (80));
            r->status.setBounds (row);
        }
    }

    void commitEditors()
    {
        for (size_t i = 0; i < rows.size() && i < (size_t) ui.size(); ++i)
        {
            rows[i].name = ui[(int) i]->name.getText().trim();
            rows[i].host = ui[(int) i]->host.getText().trim();
            const int typed = juce::jmax (1, ui[(int) i]->first.getText().getIntValue()) - 1;
            if (typed != rows[i].firstInput) { rows[i].firstInput = typed; rows[i].map.clear(); }      // typing a number means "consecutive from here", not ANEMAN's patch
        }
    }

    void refreshStatus()
    {
        auto* d = dynamic_cast<RavennaPreampDriver*> (app.project.preampDriver.get());
        const auto info = d ? d->info() : std::vector<RavennaPreampDriver::Info>();
        juce::StringArray notes;
        for (int i = 0; i < ui.size(); ++i)
        {
            juce::String s = "Not applied yet";
            juce::Colour c = theme::dimText;
            for (auto& inf : info)
                if (inf.host == (i < (int) rows.size() ? rows[(size_t) i].host : juce::String()) && inf.name == rows[(size_t) i].name)
                {
                    s = inf.status; c = inf.connected ? juce::Colour (0xff5ec27e) : theme::warn;
                    if (inf.connected) s += "  (inputs " + juce::String (inf.firstInput + 1) + " - " + juce::String (inf.firstInput + inf.channels) + ")";
                    if (inf.connected && inf.patched > inf.channels)
                        notes.add (inf.name + ": ANEMAN sends " + juce::String (inf.patched) + " of its inputs to ASIO, but the device itself lists preamp controls for only " + juce::String (inf.channels)
                                   + ". The others are still counted in the input numbers, but have no gain / 48V controls here (see the lines with MODULE in fermata-log.txt).");
                }
            ui[i]->status.setText (s, juce::dontSendNotification); ui[i]->status.setColour (juce::Label::textColourId, c);
        }
        deviceNote.setText (notes.joinIntoString ("\n"), juce::dontSendNotification);
    }

    void findAutomatically()
    {
        findButton.setEnabled (false);
        found.setColour (juce::Label::textColourId, theme::dimText);
        found.setText ("Asking ANEMAN...", juce::dontSendNotification);
        juce::Component::SafePointer<PreampSetupPanel> self (this);
        auto preferred = app.props.getUserSettings()->getValue ("anemanHost");
        juce::Thread::launch ([self, preferred]
        {
            auto plan = fetchAnemanPlan (preferred);
            juce::MessageManager::callAsync ([self, plan]
            {
                if (self == nullptr) return;
                self->findButton.setEnabled (true);
                if (! plan.ok)
                {
                    self->found.setColour (juce::Label::textColourId, theme::warn);
                    self->found.setText (plan.error, juce::dontSendNotification);
                    return;
                }
                self->app.props.getUserSettings()->setValue ("anemanHost", plan.source); self->app.props.saveIfNeeded();
                self->rows = plan.devices;
                self->build();
                self->app.setPreampDevices (self->rows);
                auto txt = plan.summary.joinIntoString ("\n");
                if (plan.warnings.size() > 0) txt += "\nNote: " + plan.warnings.joinIntoString (" ");
                self->found.setColour (juce::Label::textColourId, plan.warnings.isEmpty() ? juce::Colour (0xff5ec27e) : theme::warn);
                self->found.setText (txt, juce::dontSendNotification);
            });
        });
    }

    void stack()
    {
        commitEditors();
        auto* d = dynamic_cast<RavennaPreampDriver*> (app.project.preampDriver.get());
        if (d == nullptr) { showError ("Stack in order", "Press 'Apply and connect' first, so the channel counts can be read from the devices."); return; }
        const auto info = d->info();
        int next = 0;
        for (auto& r : rows)
        {
            r.firstInput = next;
            for (auto& inf : info) if (inf.host == r.host && inf.name == r.name) next += inf.channels;
        }
        build();
    }

    void timerCallback() override { refreshStatus(); }

    AppContext& app;
    std::vector<PreampDeviceCfg> rows;
    juce::OwnedArray<Row> ui;
    InfoNote intro; juce::Label found, deviceNote;
    juce::TextButton findButton, addButton, usualButton, stackButton, applyButton, closeButton;
};

inline void showPreampSetup (AppContext& app)
{
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Preamps";
    o.content.setOwned (new PreampSetupPanel (app));
    o.dialogBackgroundColour = theme::window;
    o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = false;
    o.launchAsync();
}
} // namespace td
