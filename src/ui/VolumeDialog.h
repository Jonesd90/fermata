#pragma once
#include "AppContext.h"

namespace td
{
/** The little mixer for changing the volume inside a grouped piece (like Pyramix's and SADiE's clip volume): one fader per file of the piece.
    The change starts at the point you right-clicked (or at the start of the piece), and can glide over 0.1 - 1.0 s instead of jumping. */
class VolumeChangePanel : public juce::Component
{
public:
    VolumeChangePanel (AppContext& a, const juce::Uuid& edit, const juce::Uuid& region, juce::int64 relSample, std::function<void()> close)
        : app (a), editId (edit), regionId (region), rel (relSample), closeFn (std::move (close))
    {
        auto* e = app.project.findEdit (editId);
        auto* r = e ? e->findAny (regionId) : nullptr;
        if (r == nullptr) { setSize (10, 10); return; }
        const double rate = juce::jmax (1.0, r->sampleRate);
        existing = false;
        const GainChange* ex = nullptr;
        for (auto& g : r->gains) if (std::abs (g.at - rel) <= (juce::int64) (0.001 * rate)) { existing = true; ex = &g; rel = g.at; initialRamp = g.ramp; }

        const int n = (int) r->files.size();
        for (int i = 0; i < n; ++i)
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow);
            setupFader (*s);
            s->setValue (juce::jlimit (-100.0f, 12.0f, ex != nullptr && (size_t) i < ex->db.size() ? ex->db[(size_t) i] : r->levelDbAt ((size_t) i, rel)), juce::dontSendNotification);
            addAndMakeVisible (*s);
            auto l = std::make_unique<juce::Label>();
            l->setText (r->files[(size_t) i].trackName, juce::dontSendNotification);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, chan::of (app.project, r->files[(size_t) i].trackId));
            l->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            addAndMakeVisible (*l);
            faders.push_back (std::move (s)); labels.push_back (std::move (l));
        }
        allFader.reset (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow));
        setupFader (*allFader);
        allFader->setRange (-24.0, 24.0, 0.1); allFader->setSkewFactor (1.0);
        allFader->setValue (0.0, juce::dontSendNotification);
        allFader->setTooltip ("Moves every fader up or down together, keeping the balance between them");
        allFader->onValueChange = [this]
        {
            const double d = allFader->getValue() - lastAll; lastAll = allFader->getValue();
            for (auto& f : faders) f->setValue (juce::jlimit (-100.0, 12.0, f->getValue() + d), juce::dontSendNotification);
        };
        addAndMakeVisible (*allFader);
        allLabel.setText ("All", juce::dontSendNotification); allLabel.setJustificationType (juce::Justification::centred);
        allLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold)); addAndMakeVisible (allLabel);

        title.setText ("Volume change at " + formatTime ((double) (r->startSample + rel) / rate).substring (3, 11) + "  -  " + r->takeName, juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold)); addAndMakeVisible (title);

        fadeBox.setButtonText ("Fade the change");
        fadeBox.setTooltip ("Off: the level jumps at once. On: it glides over the time on the right (a short fade sounds more natural)");
        fadeBox.setToggleState (existing ? initialRamp > 0.0 : true, juce::dontSendNotification);
        addAndMakeVisible (fadeBox);
        for (int i = 1; i <= 10; ++i) fadeLen.addItem (juce::String (i / 10.0, 1) + " s", i);
        fadeLen.setSelectedId (juce::jlimit (1, 10, (int) std::lround ((existing && initialRamp > 0.0 ? initialRamp : 0.3) * 10.0)), juce::dontSendNotification);
        addAndMakeVisible (fadeLen);
        wholeBox.setButtonText ("Whole piece (from its start)");
        wholeBox.setTooltip ("Set the level for the whole piece, instead of starting the change at the point you clicked");
        wholeBox.setToggleState (false, juce::dontSendNotification);
        wholeBox.onClick = [this] { fadeBox.setEnabled (! wholeBox.getToggleState()); fadeLen.setEnabled (! wholeBox.getToggleState()); };
        if (rel <= 0) { wholeBox.setToggleState (true, juce::dontSendNotification); wholeBox.onClick(); }
        addAndMakeVisible (wholeBox);

        apply.setButtonText ("Apply"); cancel.setButtonText ("Cancel"); remove.setButtonText ("Remove this change");
        remove.setEnabled (existing);
        apply.onClick  = [this] { doApply(); };
        cancel.onClick = [this] { if (closeFn) closeFn(); };
        remove.onClick = [this] { doRemove(); };
        for (auto* b : { &apply, &cancel, &remove }) addAndMakeVisible (b);
        apply.setColour (juce::TextButton::buttonColourId, theme::accent);

        setSize (juce::jmax (400, 70 * (n + 1) + 30), 420);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (20));
        auto bottom = r.removeFromBottom (32);
        const int bw = bottom.getWidth() / 3;
        apply.setBounds (bottom.removeFromLeft (bw).reduced (2)); cancel.setBounds (bottom.removeFromLeft (bw).reduced (2)); remove.setBounds (bottom.reduced (2));
        r.removeFromBottom (6);
        auto opts = r.removeFromBottom (54);
        auto row1 = opts.removeFromTop (26);
        fadeLen.setBounds (row1.removeFromRight (80).reduced (0, 2));
        fadeBox.setBounds (row1);
        wholeBox.setBounds (opts);
        r.removeFromBottom (4);
        const int n = (int) faders.size() + 1;
        const int w = r.getWidth() / juce::jmax (1, n);
        for (size_t i = 0; i < faders.size(); ++i)
        {
            auto col = r.removeFromLeft (w);
            labels[i]->setBounds (col.removeFromTop (18)); faders[i]->setBounds (col.reduced (6, 0));
        }
        allLabel.setBounds (r.removeFromTop (18)); allFader->setBounds (r.reduced (6, 0));
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

private:
    static void setupFader (juce::Slider& s)
    {
        s.setRange (-100.0, 12.0, 0.1);
        s.setSkewFactorFromMidPoint (-20.0);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 20);
        s.setTextValueSuffix (" dB");
        s.textFromValueFunction = [] (double v) { return v <= -99.9 ? juce::String ("-inf") : juce::String (v, 1); };
        s.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -100.0 : t.getDoubleValue(); };
    }

    void doApply()
    {
        auto* e = app.project.findEdit (editId);
        auto* r = e ? e->findAny (regionId) : nullptr;
        if (r == nullptr) { if (closeFn) closeFn(); return; }
        std::vector<float> db;
        for (auto& f : faders) db.push_back ((float) f->getValue());
        const bool whole = wholeBox.getToggleState();
        if (whole)                                                     // the whole piece: one level from its start, earlier changes in it are dropped
        {
            r->gains.clear();
            r->setGainChange (0, 0.0, db);
        }
        else r->setGainChange (rel, fadeBox.getToggleState() ? fadeLen.getSelectedId() / 10.0 : 0.0, db);
        app.project.changed();
        if (closeFn) closeFn();
    }

    void doRemove()
    {
        auto* e = app.project.findEdit (editId);
        auto* r = e ? e->findAny (regionId) : nullptr;
        if (r != nullptr && r->removeGainChangeAt (rel)) app.project.changed();
        if (closeFn) closeFn();
    }

    AppContext& app; juce::Uuid editId, regionId; juce::int64 rel; std::function<void()> closeFn;
    bool existing = false; double initialRamp = 0.0, lastAll = 0.0;
    std::vector<std::unique_ptr<juce::Slider>> faders; std::vector<std::unique_ptr<juce::Label>> labels;
    std::unique_ptr<juce::Slider> allFader; juce::Label allLabel, title;
    juce::ToggleButton fadeBox, wholeBox; juce::ComboBox fadeLen;
    juce::TextButton apply, cancel, remove;
};

/** Opens the volume mixer for a piece in its own small window. */
inline void showVolumeChange (AppContext& app, const juce::Uuid& editId, const juce::Uuid& regionId, juce::int64 relSample)
{
    struct Holder { juce::DialogWindow* w = nullptr; };
    auto holder = std::make_shared<Holder>();
    auto panel = std::make_unique<VolumeChangePanel> (app, editId, regionId, relSample, [holder]
    {
        if (holder->w) { holder->w->exitModalState (0); }
    });
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Volume change";
    o.content.setOwned (panel.release());
    o.componentToCentreAround = nullptr;
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = false; o.useBottomRightCornerResizer = false;
    holder->w = o.launchAsync();
}
} // namespace td
