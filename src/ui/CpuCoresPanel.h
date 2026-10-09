#pragma once
#include "AppContext.h"
#include "../core/CoreReservation.h"

namespace td
{
/** The reserved cores are a property of this PC, not of a project: they are kept in the program's own settings file. */
inline CoreReservationSettings loadCoreReservationSettings (juce::PropertiesFile& f)
{
    CoreReservationSettings s;
    s.enabled = f.getBoolValue ("cpuReserveOn", false);
    s.keepOthersOff = f.getBoolValue ("cpuReserveOthers", false);
    s.setReservedText (f.getValue ("cpuReserveCores", {}));
    return s;
}

inline void saveCoreReservationSettings (juce::PropertiesFile& f, const CoreReservationSettings& s)
{
    f.setValue ("cpuReserveOn", s.enabled);
    f.setValue ("cpuReserveOthers", s.keepOthersOff);
    f.setValue ("cpuReserveCores", s.reservedText());
    f.saveIfNeeded();
}

/** "CPU cores": choose which cores are kept for the audio engine (see CoreReservation.h for what that does and does not do). */
class CpuCoresPanel : public juce::Component
{
public:
    explicit CpuCoresPanel (AppContext& a) : app (a)
    {
        auto& cr = CoreReservation::get();
        intro.setText ("Keeps some CPU cores for Fermata's audio only (the driver's audio callback, the disk writer and the playback reader), and keeps the rest of Fermata - and, if you tick the last box, "
                       "the other programs too - on the remaining cores. That stops ordinary programs from taking the processor away in the middle of a take.\n\n"
                       "It cannot stop Windows' own driver work (DPCs and interrupts from the network, USB, graphics or Wi-Fi) on a core that the driver has chosen for it. "
                       "So pick the QUIETEST cores: 'Find the quietest cores' measures how much interrupt work each core is doing for three seconds and ticks the best ones. "
                       "Also close the programs you do not need, and switch off Wi-Fi and Bluetooth during a recording on a laptop.", juce::dontSendNotification);
        intro.setFont (juce::FontOptions (12.5f)); intro.setColour (juce::Label::textColourId, theme::dimText); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro);

        const auto cur = loadCoreReservationSettings (*app.props.getUserSettings());
        enableBox.setButtonText ("Keep CPU cores for Fermata's audio");
        enableBox.setToggleState (cur.enabled, juce::dontSendNotification);
        othersBox.setButtonText ("Also ask other programs to stay off these cores (like Process Lasso)");
        othersBox.setToggleState (cur.keepOthersOff, juce::dontSendNotification);
        othersBox.setTooltip ("Every few seconds Fermata gives the programs that have no core choice of their own (only those, and only ones it is allowed to change) the other cores to run on. "
                              "They are set free again when you untick this or close Fermata. If Fermata ever crashes, restart Fermata and close it again, or restart those programs.");
        for (auto* b : { static_cast<juce::Button*> (&enableBox), static_cast<juce::Button*> (&othersBox) }) { b->onClick = [this] { commit(); }; addAndMakeVisible (b); }

        for (auto& c : cr.cores())
        {
            auto* t = coreButtons.add (new juce::ToggleButton ("Core " + juce::String (c.number)));
            t->setToggleState (std::find (cur.reserved.begin(), cur.reserved.end(), c.number) != cur.reserved.end(), juce::dontSendNotification);
            t->onClick = [this] { commit(); };
            t->setTooltip ("Core " + juce::String (c.number) + (c.logical.size() > 1 ? " (hyper-threads " : " (thread ") + threadsText (c) + ")"
                           + (cr.cores().size() > 0 && c.efficiency < maxEfficiency() ? ". A small, slower core of this CPU: better left to Windows." : juce::String()));
            addAndMakeVisible (t);
            auto* l = loadLabels.add (new juce::Label());
            l->setFont (juce::FontOptions (11.0f)); l->setColour (juce::Label::textColourId, theme::dimText);
            addAndMakeVisible (l);
        }
        measureButton.setButtonText ("Find the quietest cores (3 s)");
        measureButton.setTooltip ("Measures, for three seconds, how much of each core's time Windows spends on driver interrupts and DPCs, then ticks the quietest cores. Do it with the audio interface connected and the PC otherwise idle.");
        measureButton.onClick = [this] { measure(); };
        closeButton.setButtonText ("Close");
        closeButton.onClick = [this] { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); };
        addAndMakeVisible (measureButton); addAndMakeVisible (closeButton);
        status.setFont (juce::FontOptions (12.5f)); status.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (status);
        refreshStatus();
        setSize (620, 150 + 40 + rowsNeeded() * 46 + 150 + 56);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        intro.setBounds (r.removeFromTop (150));
        enableBox.setBounds (r.removeFromTop (28));
        r.removeFromTop (4);
        const int perRow = 8, w = r.getWidth() / perRow;
        for (int i = 0; i < coreButtons.size(); ++i)
        {
            const int row = i / perRow, col = i % perRow;
            juce::Rectangle<int> cell (r.getX() + col * w, r.getY() + row * 46, w, 46);
            coreButtons[i]->setBounds (cell.removeFromTop (26));
            loadLabels[i]->setBounds (cell.reduced (24, 0).withTrimmedLeft (-2));
        }
        r.removeFromTop (rowsNeeded() * 46 + 4);
        othersBox.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (grid::rowH);
        closeButton.setBounds (bottom.removeFromRight (grid::btnW));
        measureButton.setBounds (bottom.removeFromLeft (grid::btnW + 80));
        status.setBounds (r);
    }

private:
    int maxEfficiency() const { int m = 0; for (auto& c : CoreReservation::get().cores()) m = std::max (m, c.efficiency); return m; }
    int rowsNeeded() const { return std::max (1, ((int) coreButtons.size() + 7) / 8); }
    static juce::String threadsText (const CpuCore& c) { juce::StringArray a; for (int l : c.logical) a.add (juce::String (l)); return a.joinIntoString (", "); }

    CoreReservationSettings fromControls() const
    {
        CoreReservationSettings s;
        s.enabled = enableBox.getToggleState();
        s.keepOthersOff = othersBox.getToggleState();
        for (int i = 0; i < coreButtons.size(); ++i) if (coreButtons[i]->getToggleState()) s.reserved.push_back (CoreReservation::get().cores()[(size_t) i].number);
        return s;
    }

    void commit()
    {
        const auto s = fromControls();
        saveCoreReservationSettings (*app.props.getUserSettings(), s);
        CoreReservation::get().configure (s);
        refreshStatus();
    }

    void refreshStatus()
    {
        auto& cr = CoreReservation::get();
        status.setText (cr.statusText(), juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, cr.active() ? theme::text : theme::dimText);
        const bool ok = cr.supported();
        enableBox.setEnabled (ok); othersBox.setEnabled (ok); measureButton.setEnabled (ok);
        for (auto* c : coreButtons) c->setEnabled (ok);
    }

    void measure()
    {
        measureButton.setEnabled (false);
        status.setText ("Measuring for three seconds ...", juce::dontSendNotification);
        juce::Component::SafePointer<CpuCoresPanel> safe (this);
        juce::Thread::launch ([safe]
        {
            auto load = CoreReservation::get().measureInterruptLoad (3000);
            juce::MessageManager::callAsync ([safe, load]
            {
                if (safe != nullptr) safe->measured (load);
            });
        });
    }

    void measured (const std::vector<double>& load)
    {
        measureButton.setEnabled (CoreReservation::get().supported());
        auto& cores = CoreReservation::get().cores();
        if (load.empty()) { status.setText ("Windows would not report the interrupt load of the cores here. Tick the cores by hand (the higher-numbered ones are usually the quieter).", juce::dontSendNotification); return; }
        for (int i = 0; i < coreButtons.size() && i < loadLabels.size(); ++i)
        {
            double worst = 0.0;
            for (int l : cores[(size_t) i].logical) if (l >= 0 && (size_t) l < load.size()) worst = std::max (worst, load[(size_t) l]);
            loadLabels[i]->setText (juce::String (worst, 2) + " %", juce::dontSendNotification);
        }
        const auto pick = pickQuietCores (cores, load, suggestedReserveCount ((int) cores.size()));
        for (int i = 0; i < coreButtons.size(); ++i)
            coreButtons[i]->setToggleState (std::find (pick.begin(), pick.end(), cores[(size_t) i].number) != pick.end(), juce::dontSendNotification);
        enableBox.setToggleState (true, juce::dontSendNotification);
        commit();
    }

    AppContext& app;
    juce::Label intro, status;
    juce::ToggleButton enableBox, othersBox;
    juce::OwnedArray<juce::ToggleButton> coreButtons;
    juce::OwnedArray<juce::Label> loadLabels;
    juce::TextButton measureButton, closeButton;
};

inline void showCpuCoresPanel (AppContext& app)
{
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "CPU cores for the audio";
    o.content.setOwned (new CpuCoresPanel (app));
    o.dialogBackgroundColour = theme::window;
    o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = false;
    o.launchAsync();
}
} // namespace td
