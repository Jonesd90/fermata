#pragma once
#include "AppContext.h"

namespace td
{
/** "Plug-ins" (Audio settings > Plug-ins..., or the menu of a mixer's insert slot): every plug-in file Fermata knows of, with the plug-ins it holds.
    - Ignore one: it is never scanned, loaded or offered again (a project that uses it keeps the slot, with its settings, without loading it).
    - Use again: an ignored one comes back (its file is scanned again).
    - Try again: a file that was left out because it crashed Fermata while it was being scanned is scanned once more.
    It works on files: a file that holds several plug-ins (a shell) is ignored with all of them. */
class PluginManagerPanel : public juce::Component, private juce::Timer
{
public:
    explicit PluginManagerPanel (AppContext& a) : app (a)
    {
        intro.setText ("Every VST3 plug-in file Fermata has found. Ignore a plug-in you never want: it is not scanned, loaded or offered any more (a project that "
                       "uses it keeps it in its slot, with its settings, without loading it). A file marked CRASHED was left out because it crashed Fermata while "
                       "it was being scanned: Try again scans it once more.", juce::dontSendNotification);
        intro.setFont (juce::FontOptions (12.5f)); intro.setColour (juce::Label::textColourId, theme::dimText); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro);
        filter.setTextToShowWhenEmpty ("Search by name or maker", theme::dimText);
        filter.onTextChange = [this] { rebuild(); };
        filter.onEscapeKey = [this] { filter.clear(); rebuild(); };
        addAndMakeVisible (filter);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        status.setFont (juce::FontOptions (12.5f)); status.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (status);
        scanButton.setButtonText ("Scan for new plug-ins");
        scanButton.setTooltip ("Look for VST3 plug-ins that have been installed since the last scan (ignored and crashed ones are left alone)");
        scanButton.onClick = [this]
        {
            if (app.plugins.isScanning()) return;
            juce::Component::SafePointer<juce::Button> again (&scanButton);
            if (app.recModeHolds ("A plug-in scan", [again] { if (again != nullptr) again->triggerClick(); })) return;     // (3.18.0: asks first while Rec is on)
            status.setText ("Scanning for VST3 plug-ins...", juce::dontSendNotification);
            juce::Component::SafePointer<PluginManagerPanel> safe (this);
            app.plugins.startScan ([safe] { if (safe != nullptr) { safe->status.setText ("Scan finished: " + juce::String (safe->app.plugins.getKnownPlugins().getNumTypes()) + " plug-ins in use.", juce::dontSendNotification); safe->rebuild(); } });
            rebuild();
        };
        addAndMakeVisible (scanButton);
        closeButton.setButtonText ("Close");
        closeButton.onClick = [this] { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); };
        addAndMakeVisible (closeButton);
        rebuild();
        startTimerHz (4);                                         // (a scan started elsewhere: the list waits for it, then shows what it found)
        setSize (760, 620);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        intro.setBounds (r.removeFromTop (54));
        r.removeFromTop (4);
        filter.setBounds (r.removeFromTop (26).withWidth (juce::jmin (320, r.getWidth())));
        r.removeFromTop (8);
        auto bottom = r.removeFromBottom (32);
        closeButton.setBounds (bottom.removeFromRight (100).reduced (2));
        scanButton.setBounds (bottom.removeFromLeft (180).reduced (2));
        status.setBounds (bottom.reduced (8, 0));
        r.removeFromBottom (8);
        viewport.setBounds (r);
        layoutRows();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }

private:
    using Entry = PluginHost::FileEntry;
    static constexpr int kRowH = 44;

    struct Row : public juce::Component, public juce::SettableTooltipClient
    {
        Row (PluginManagerPanel& o, Entry e) : owner (o), entry (std::move (e))
        {
            const auto st = entry.state;
            button.setButtonText (st == Entry::State::found ? "Ignore" : st == Entry::State::ignored ? "Use again" : "Try again");
            button.setTooltip (st == Entry::State::found   ? "Never scan, load or offer this plug-in again (Use again brings it back)"
                             : st == Entry::State::ignored ? "Take it off the ignored list: its file is scanned again and its plug-ins are offered again"
                                                           : "Scan this file once more. If it crashes Fermata again, it is left out again at the next scan.");
            button.onClick = [this] { const auto e = entry; owner.act (e); };      // (a copy: the row may be rebuilt)
            addAndMakeVisible (button);
            setTooltip (entry.file);
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.setColour (theme::row.withAlpha (index % 2 == 0 ? 1.0f : 0.6f)); g.fillRect (r);
            g.setColour (theme::border.withAlpha (0.5f)); g.fillRect (r.removeFromBottom (1));
            r = getLocalBounds().reduced (8, 4).withTrimmedRight (112);
            const auto chip = r.removeFromLeft (78).withSizeKeepingCentre (72, 18);
            const auto st = entry.state;
            const juce::Colour c = st == Entry::State::crashed ? juce::Colour (0xffe0312b) : st == Entry::State::ignored ? juce::Colour (0xff8a909a) : juce::Colour (0xff2b9e6a);
            g.setColour (c.withAlpha (0.25f)); g.fillRoundedRectangle (chip.toFloat(), 4.0f);
            g.setColour (c); g.drawRoundedRectangle (chip.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawText (st == Entry::State::crashed ? "CRASHED" : st == Entry::State::ignored ? "IGNORED" : "IN USE", chip, juce::Justification::centred, false);
            r.removeFromLeft (6);
            g.setColour (st == Entry::State::found ? theme::text : theme::dimText);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (entry.names, r.removeFromTop (r.getHeight() / 2 + 2), juce::Justification::centredLeft, true);
            g.setColour (theme::dimText); g.setFont (juce::FontOptions (11.0f));
            g.drawText (entry.file, r, juce::Justification::centredLeft, true);
        }
        void resized() override { button.setBounds (getLocalBounds().removeFromRight (108).reduced (6, 9)); }
        PluginManagerPanel& owner; Entry entry; juce::TextButton button; int index = 0;
    };

    void act (const Entry& e)
    {
        if (app.plugins.isScanning()) { status.setText ("A scan is running: wait for it to finish.", juce::dontSendNotification); return; }
        juce::Component::SafePointer<PluginManagerPanel> safe (this);
        auto after = [safe] (juce::String msg) { return [safe, msg] { if (safe != nullptr) { safe->status.setText (msg, juce::dontSendNotification); safe->rebuild(); } }; };
        if (e.state == Entry::State::found)
        {
            app.plugins.setIgnored (e.file, true);
            status.setText ("Ignored: " + e.names + ". (A slot that has it loaded now keeps it until the project is opened again.)", juce::dontSendNotification);
            rebuildLater();
        }
        else if (e.state == Entry::State::ignored)
        {
            status.setText ("Scanning " + e.names + " again...", juce::dontSendNotification);
            if (! app.plugins.setIgnored (e.file, false, after ("Back in use: " + e.names + ".")))
                status.setText ("Taken off the ignored list, but the scan could not start: press Scan for new plug-ins.", juce::dontSendNotification);
            rebuildLater();
        }
        else
        {
            const auto file = e.file, names = e.names;
            confirmAsync ("Try a plug-in again", names + " crashed Fermata the last time it was scanned.\n\nScan it again now? If it crashes again, Fermata closes "
                          "(the project is saved first) and the plug-in is left out again at the next scan.", "Try again",
                          [safe, file, names, after]
                          {
                              if (safe == nullptr) return;
                              if (safe->app.hasProject()) safe->app.saveNow();
                              safe->status.setText ("Trying " + names + " again...", juce::dontSendNotification);
                              if (! safe->app.plugins.tryAgain (file, after ("Tried " + names + " again: see the list."))) safe->status.setText ("Could not start: a scan is running.", juce::dontSendNotification);
                              safe->rebuildLater();
                          });
        }
    }

    /** After a row's button: the rows are made again once that click is over (not while the button that was clicked is still busy). */
    void rebuildLater() { juce::Component::SafePointer<PluginManagerPanel> safe (this); juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuild(); }); }
    void rebuild()
    {
        rows.clear();
        if (app.plugins.isScanning()) { shownWhileScanning = true; layoutRows(); return; }      // (the lists change under a scan: shown again when it ends)
        shownWhileScanning = false;
        const auto words = juce::StringArray::fromTokens (filter.getText().toLowerCase(), true);
        int i = 0;
        for (auto& e : app.plugins.listFiles())
        {
            const auto hay = (e.names + " " + e.file).toLowerCase();
            bool ok = true;
            for (auto& w : words) ok = ok && hay.contains (w);
            if (! ok) continue;
            auto* r = rows.add (new Row (*this, e));
            r->index = i++;
            content.addAndMakeVisible (r);
        }
        layoutRows();
    }
    void layoutRows()
    {
        const int w = juce::jmax (100, viewport.getWidth() - viewport.getScrollBarThickness());
        content.setSize (w, juce::jmax (kRowH, rows.size() * kRowH));
        for (int i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, i * kRowH, w, kRowH);
        repaint();
    }
    void timerCallback() override
    {
        scanButton.setEnabled (! app.plugins.isScanning());
        if (app.plugins.isScanning()) status.setText (app.plugins.scanStatus(), juce::dontSendNotification);
        if (shownWhileScanning && ! app.plugins.isScanning()) rebuild();
    }

    AppContext& app;
    juce::Label intro, status;
    juce::TextEditor filter;
    juce::Viewport viewport; juce::Component content;
    juce::OwnedArray<Row> rows;                                    // (after content: removed from it first)
    juce::TextButton scanButton, closeButton;
    bool shownWhileScanning = false;
};

inline void showPluginManager (AppContext& app)
{
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Plug-ins";
    o.content.setOwned (new PluginManagerPanel (app));
    o.dialogBackgroundColour = theme::window;
    o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = true;
    o.launchAsync();
}
} // namespace td
