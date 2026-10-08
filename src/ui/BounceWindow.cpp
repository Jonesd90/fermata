#include "BounceWindow.h"

namespace td
{
/** A scrolling list of tick boxes: every Ext Bus, Int Bus and audio track. */
class BounceSourceList : public juce::Component
{
public:
    struct Item { juce::Uuid id; bool isMain = false; juce::ToggleButton* box = nullptr; NodeKind kind = NodeKind::None; };
    explicit BounceSourceList (const Project& p)
    {
        heading ("Ext buses (the mixer's outputs, after their faders)");
        for (auto& b : p.buses) if (b.external) addEntry (b.id, false, NodeKind::ExtBus, b.name, true);
        heading ("Int buses");
        for (auto& b : p.buses) if (! b.external) addEntry (b.id, false, NodeKind::IntBus, b.name, false);
        heading ("Audio tracks (after their faders)");
        for (auto& t : p.tracks) addEntry (t.id, false, NodeKind::Track, t.name, false);
        setSize (400, rows * kRow);
    }
    void resized() override
    {
        int y = 0;
        for (auto* c : order) { c->setBounds (0, y, getWidth(), kRow); y += kRow; }
        setSize (getWidth(), juce::jmax (getHeight(), rows * kRow));
    }
    void select (std::function<bool (const Item&)> pred) { for (auto& it : items) it.box->setToggleState (pred (it), juce::dontSendNotification); }
    std::vector<juce::Uuid> chosen() const
    {
        std::vector<juce::Uuid> v;
        for (auto& it : items) if (! it.isMain && it.box->getToggleState()) v.push_back (it.id);
        return v;
    }
    static constexpr int kRow = 22;
private:
    void heading (const juce::String& t)
    {
        auto* l = labels.add (new juce::Label ({}, t));
        l->setFont (juce::FontOptions (12.0f, juce::Font::bold)); l->setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (l); order.add (l); ++rows;
    }
    void addEntry (const juce::Uuid& id, bool isMain, NodeKind k, const juce::String& text, bool on)
    {
        auto* b = boxes.add (new juce::ToggleButton (text));
        b->setToggleState (on, juce::dontSendNotification);
        addAndMakeVisible (b); order.add (b); ++rows;
        items.push_back ({ id, isMain, b, k });
    }
    int rows = 0;
    std::vector<Item> items;
    juce::OwnedArray<juce::ToggleButton> boxes; juce::OwnedArray<juce::Label> labels;
    juce::Array<juce::Component*> order;
};

BounceComponent::BounceComponent (AppContext& a, const BounceContext& c)
    : app (a), ctx (c), isEdit (c.kind == BounceContext::Kind::Edit), progressBar (progressValue)
{
    auto cap = [this] (juce::Label& l, const juce::String& t, bool bold = false)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (juce::FontOptions (13.0f, bold ? juce::Font::bold : juce::Font::plain));
        addAndMakeVisible (l);
    };
    cap (title, "Bounce Out - " + ctx.defaultName, true);
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    cap (rangeCaption, "What to bounce:", true);
    fullButton.setButtonText (isEdit ? "Full Track (first to last audio of the edit)" : "Whole take");
    ioButton.setButtonText ("Between the I/O flags (set with I and O, or with 1 and 2)");
    selectedButton.setButtonText (isEdit ? "Selected pieces (click a piece in the edit, Ctrl + click for more)"
                                         : "Selected takes (click a take, Ctrl + click for more)");
    allTakesButton.setButtonText ("All takes as individual files (every take of this window, one file each - e.g. to send for listening)");
    for (auto* b : { &fullButton, &ioButton, &selectedButton, &allTakesButton }) { b->setRadioGroupId (8123); b->setClickingTogglesState (true); b->onClick = [this] { updateEnabled(); }; }
    for (auto* b : { &fullButton, &ioButton, &selectedButton }) addAndMakeVisible (b);
    if (! isEdit) addAndMakeVisible (allTakesButton);
    cap (leadCaption, "Add before:"); cap (tailCaption, "Add after:");
    for (auto* b : { &leadBox, &tailBox })
    {
        b->addItem ("0 s", 1);
        for (int s = 1; s <= 9; ++s) b->addItem (juce::String (s) + " s", s + 1);
        b->setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (b);
    }
    leadBox.setTooltip ("Seconds added before the audio (the file becomes this much longer). Silence goes through the mixer so plug-ins can settle.");
    tailBox.setTooltip ("Seconds added after the audio (the file becomes this much longer). Silence goes through the mixer so reverb tails and plug-ins can ring out.");

    cap (mixerCaption, "Mixer whose settings are used:", true);
    int i = 0, preset = 1;
    const int ce = app.project.cueEnd();
    for (auto& m : app.project.mixers)
    {
        mixerBox.addItem ((i == 0 ? "Processing mixer: " : i < ce ? "Cue mixer: " : "Edit mixer: ") + m->name, i + 1);
        if (isEdit && m->editId == ctx.id) preset = i + 1;                // bouncing an Edit: its own mixer is the one to use
        ++i;
    }
    mixerBox.setSelectedId (preset, juce::dontSendNotification);
    addAndMakeVisible (mixerBox);

    cap (sourcesCaption, "Outputs to bounce (every ticked output becomes its own stereo file):", true);
    sources = std::make_unique<BounceSourceList> (app.project);
    sourcesViewport.setViewedComponent (sources.get(), false);
    sourcesViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (sourcesViewport);
    for (auto* b : { &allExt, &allInt, &allTracks, &clearAll }) addAndMakeVisible (b);
    allTracks.onClick = [this] { sources->select ([] (const BounceSourceList::Item& it) { return it.kind == NodeKind::Track; }); };
    allInt.onClick    = [this] { sources->select ([] (const BounceSourceList::Item& it) { return it.kind == NodeKind::IntBus; }); };
    allExt.onClick    = [this] { sources->select ([] (const BounceSourceList::Item& it) { return it.kind == NodeKind::ExtBus; }); };
    clearAll.onClick  = [this] { sources->select ([] (const BounceSourceList::Item&) { return false; }); };

    auto* st = app.props.getUserSettings();
    normToggle.setToggleState (st != nullptr && st->getBoolValue ("bounceNormalise", false), juce::dontSendNotification);
    addAndMakeVisible (normToggle);
    togetherToggle.setToggleState (st != nullptr && st->getBoolValue ("bounceTogether", false), juce::dontSendNotification);
    togetherToggle.setTooltip ("Off: every file is normalised on its own. On: one gain for all the files of a piece, so the loudest reaches the level and the others keep their balance.");
    outNameToggle.setToggleState (st == nullptr || st->getBoolValue ("bounceOutName", true), juce::dontSendNotification);
    outNameToggle.setTooltip ("With several outputs ticked the name is always added, e.g. 'Symphony 2 - Out', 'Symphony 2 - violin'.");
    addAndMakeVisible (togetherToggle); addAndMakeVisible (outNameToggle);
    peakEditor.setText (st != nullptr ? st->getValue ("bouncePeak1", "-1") : "-1", false);
    peakEditor.setInputRestrictions (8, "-0123456789.");
    addAndMakeVisible (peakEditor);
    cap (normCaption, "dBFS");
    cap (nameCaption, "File name:", true); cap (folderCaption, "Folder:", true);
    nameEditor.setText (ctx.defaultName, false); addAndMakeVisible (nameEditor);
    folderEditor.setText (st != nullptr ? st->getValue ("bounceFolder|" + app.project.projectFile.getFullPathName(), app.project.bouncedFolder().getFullPathName())
                                        : app.project.audioFolder().getSiblingFile ("Bounces").getFullPathName(), false);
    addAndMakeVisible (folderEditor);
    chooseButton.onClick = [this] { chooseFolder(); };
    addAndMakeVisible (chooseButton);
    status.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (status);
    addAndMakeVisible (progressBar);
    bounceButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    bounceButton.onClick = [this] { start(); };
    closeButton.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed(); };
    showButton.onClick = [this] { if (lastFile.exists()) lastFile.revealToUser(); };
    showButton.setVisible (false);
    for (auto* b : { &bounceButton, &closeButton, &showButton }) addAndMakeVisible (b);

    // which range options make sense right now
    bool hasIO = false;
    if (isEdit)
    {
        if (auto* e = app.project.findEdit (ctx.id)) hasIO = (e->markIn >= 0 && e->markOut > e->markIn) || (e->fixIn >= 0 && e->fixOut > e->fixIn);
    }
    else if (auto* w = app.project.findTakeWindow (ctx.id)) hasIO = (w->findGroup (w->editTake) != nullptr && w->editIn >= 0 && w->editOut > w->editIn);
    ioButton.setEnabled (hasIO);
    selectedButton.setEnabled (! ctx.selected.empty());
    selectedButton.setTooltip (ctx.selected.empty() ? (isEdit ? "Click a piece in the edit window first (Ctrl + click for several)" : "Click a take in the take window first (Ctrl + click for several)") : "");
    ioButton.setTooltip (hasIO ? "" : isEdit ? "Set an IN and an OUT flag first (keys I and O, or 1 and 2)" : "Set the IN and OUT flags first (keys 1 and 2, or P for a whole take)");
    if (! ctx.selected.empty()) selectedButton.setToggleState (true, juce::dontSendNotification);
    else fullButton.setToggleState (true, juce::dontSendNotification);
    if (! isEdit && ctx.selected.empty())
        if (auto* w = app.project.findTakeWindow (ctx.id))
            if (w->findGroup (w->editTake) == nullptr && w->findGroup (w->cursorTake) == nullptr && ! w->groups.empty())
                allTakesButton.setToggleState (true, juce::dontSendNotification);     // no take picked: offer every take
    updateEnabled();
    startTimerHz (15);
    setSize (700, isEdit ? 740 : 766);
}

BounceComponent::~BounceComponent() { job.reset(); }

void BounceComponent::paint (juce::Graphics& g) { g.fillAll (theme::window); }

void BounceComponent::resized()
{
    auto r = getLocalBounds().reduced (12);
    title.setBounds (r.removeFromTop (26));
    r.removeFromTop (6);
    rangeCaption.setBounds (r.removeFromTop (20));
    fullButton.setBounds (r.removeFromTop (22));
    {
        auto row = r.removeFromTop (28);
        row.removeFromLeft (24);
        leadCaption.setBounds (row.removeFromLeft (84));
        leadBox.setBounds (row.removeFromLeft (70).reduced (0, 2));
        row.removeFromLeft (16);
        tailCaption.setBounds (row.removeFromLeft (80));
        tailBox.setBounds (row.removeFromLeft (70).reduced (0, 2));
    }
    ioButton.setBounds (r.removeFromTop (22));
    selectedButton.setBounds (r.removeFromTop (22));
    if (! isEdit) allTakesButton.setBounds (r.removeFromTop (22));
    r.removeFromTop (8);
    mixerCaption.setBounds (r.removeFromTop (20));
    mixerBox.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    sourcesCaption.setBounds (r.removeFromTop (20));
    auto quick = r.removeFromTop (28);
    for (auto* b : { &allExt, &allInt, &allTracks, &clearAll }) b->setBounds (quick.removeFromLeft (juce::jmax (80, b->getBestWidthForHeight (24) + 8)).reduced (1, 2));
    auto lower = r.removeFromBottom (266);
    sourcesViewport.setBounds (r.reduced (0, 2));
    sources->setSize (sourcesViewport.getWidth() - sourcesViewport.getScrollBarThickness(), sources->getHeight());
    sources->resized();

    lower.removeFromTop (6);
    {
        auto row = lower.removeFromTop (26);
        normToggle.setBounds (row.removeFromLeft (260));
        peakEditor.setBounds (row.removeFromLeft (70).reduced (0, 1));
        normCaption.setBounds (row.removeFromLeft (50).withTrimmedLeft (6));
    }
    togetherToggle.setBounds (lower.removeFromTop (24).withTrimmedLeft (24));
    outNameToggle.setBounds (lower.removeFromTop (24));
    lower.removeFromTop (6);
    nameCaption.setBounds (lower.removeFromTop (18));
    nameEditor.setBounds (lower.removeFromTop (26));
    lower.removeFromTop (6);
    folderCaption.setBounds (lower.removeFromTop (18));
    {
        auto row = lower.removeFromTop (26);
        chooseButton.setBounds (row.removeFromRight (130).reduced (2, 0));
        folderEditor.setBounds (row);
    }
    lower.removeFromTop (10);
    progressBar.setBounds (lower.removeFromTop (18));
    status.setBounds (lower.removeFromTop (38));
    auto btn = lower.removeFromTop (32);
    closeButton.setBounds (btn.removeFromRight (100).reduced (2));
    bounceButton.setBounds (btn.removeFromRight (140).reduced (2));
    showButton.setBounds (btn.removeFromLeft (140).reduced (2));
}

void BounceComponent::updateEnabled()
{
    leadBox.setEnabled (job == nullptr); tailBox.setEnabled (job == nullptr);       // padding applies to every range
}

void BounceComponent::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose the folder for the bounced file(s)", juce::File (folderEditor.getText()));
    auto safe = juce::Component::SafePointer<BounceComponent> (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult() != juce::File()) safe->folderEditor.setText (fc.getResult().getFullPathName(), false);
    });
}

bool BounceComponent::buildSettings (BounceSettings& bs, juce::String& error) const
{
    auto& p = app.project;
    const auto base = nameEditor.getText().trim().isEmpty() ? juce::String ("Bounce") : nameEditor.getText().trim();
    bs.folder = juce::File (folderEditor.getText().trim());
    if (folderEditor.getText().trim().isEmpty()) { error = "Choose a folder to bounce into."; return false; }
    const int mi = mixerBox.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (mi, (int) p.mixers.size())) { error = "Choose a mixer."; return false; }
    bs.mixerId = p.mixers[(size_t) mi]->id;
    bs.sources = sources->chosen();
    if (bs.sources.empty()) { error = p.buses.empty() ? "There is nothing to bounce yet: add an Ext bus (and route tracks to it) in the Project Designer." : "Tick at least one output to bounce (an Ext bus, an Int bus or an audio track)."; return false; }
    bs.leadSeconds = leadBox.getSelectedId() - 1; bs.tailSeconds = tailBox.getSelectedId() - 1;
    bs.addOutputName = outNameToggle.getToggleState();
    bs.normaliseTogether = togetherToggle.getToggleState();
    bs.normalise = normToggle.getToggleState();
    bs.peakDb = juce::jlimit (-60.0f, 0.0f, peakEditor.getText().getFloatValue());
    if (bs.normalise && peakEditor.getText().trim().isEmpty()) { error = "Type the peak level to normalise to, for example -1"; return false; }

    if (isEdit)
    {
        auto* e = p.findEdit (ctx.id);
        if (e == nullptr || e->isEmpty()) { error = "The edit is empty."; return false; }
        bs.sampleRate = e->sampleRate;
        bs.segments = segmentsForEdit (p, *e);
        if (e->automationOn) bs.automationEdit = e->id;                  // the edit's automation is part of what is bounced
        if (fullButton.getToggleState())
        {
            bs.items.push_back ({ base, e->firstSample(), e->lengthSamples() });
        }
        else if (ioButton.getToggleState())
        {
            const bool io = e->markIn >= 0 && e->markOut > e->markIn, fix = e->fixIn >= 0 && e->fixOut > e->fixIn;
            if (! io && ! fix) { error = "Set an IN and an OUT flag first (keys I and O, or 1 and 2, in the edit window)."; return false; }
            const double a = io ? e->markIn : e->fixIn, b = io ? e->markOut : e->fixOut;           // the I / O flags; if they are not set, the 1 / 2 flags
            bs.items.push_back ({ base, (juce::int64) std::llround (a * e->sampleRate), (juce::int64) std::llround (b * e->sampleRate) });
        }
        else
        {
            int n = 0;
            std::vector<const EditRegion*> chosen;
            for (auto& r : e->regions)  if (std::find (ctx.selected.begin(), ctx.selected.end(), r.id) != ctx.selected.end()) chosen.push_back (&r);
            for (auto& r : e->overdubs) if (std::find (ctx.selected.begin(), ctx.selected.end(), r.id) != ctx.selected.end()) chosen.push_back (&r);
            if (chosen.empty()) { error = "No pieces are selected in the edit window."; return false; }
            for (auto* r : chosen)
                bs.items.push_back ({ chosen.size() > 1 ? base + " - " + juce::String (++n).paddedLeft ('0', 2) : base, r->startSample, r->endSample() });
        }
    }
    else
    {
        auto* w = p.findTakeWindow (ctx.id);
        if (w == nullptr) { error = "The take window no longer exists."; return false; }
        if (selectedButton.getToggleState() || allTakesButton.getToggleState())
        {
            const bool all = allTakesButton.getToggleState();
            std::vector<const TakeGroup*> chosen;
            for (auto& g : w->groups) if (all || std::find (ctx.selected.begin(), ctx.selected.end(), g.id) != ctx.selected.end()) chosen.push_back (&g);
            if (chosen.empty()) { error = all ? "This take window has no takes yet." : "No takes are selected in the take window."; return false; }
            bs.sampleRate = chosen[0]->sampleRate;
            for (auto* g : chosen)
            {
                if (std::abs (g->sampleRate - bs.sampleRate) > 0.5) { error = "The selected takes have different sample rates. Select takes with the same rate (or bounce them separately)."; return false; }
                BounceItem it { (chosen.size() > 1 || all) ? base + " - take " + pad3 (g->number) : base, 0, (juce::int64) g->lengthSamples, segmentsForTake (p, *g) };
                if (it.segments.empty()) { error = "Take " + pad3 (g->number) + " has no audio."; return false; }
                bs.items.push_back (std::move (it));
            }
            return true;
        }
        const bool ioSet = false;                                                          // (the separate Bounce I / O flags are gone: the 1 and 2 flags are used)
        const bool fixSet = w->findGroup (w->editTake) != nullptr && w->editIn >= 0 && w->editOut > w->editIn;
        const bool wantRange = ioButton.getToggleState();
        const auto takeId = (wantRange && ! ioSet && fixSet) ? w->editTake : ((! w->editTake.isNull()) ? w->editTake : w->cursorTake);
        auto* g = w->findGroup (takeId);
        if (g == nullptr) { error = "Click in a take first (or set the 1 / 2 flags in one), then press Bounce Out."; return false; }
        bs.sampleRate = g->sampleRate;
        bs.segments = segmentsForTake (p, *g);
        if (fullButton.getToggleState())
            bs.items.push_back ({ base, (juce::int64) 0, (juce::int64) g->lengthSamples });
        else
        {
            const double a = w->editIn, b = w->editOut;
            if (! ioSet && ! fixSet) { error = "Set the IN and OUT flags in the take first (keys 1 and 2, or P for a whole take)."; return false; }
            bs.items.push_back ({ base, (juce::int64) std::llround (a * g->sampleRate), (juce::int64) std::llround (b * g->sampleRate) });
        }
    }
    if (bs.segments.empty() && bs.items.empty()) { error = "There is no audio to bounce."; return false; }
    if (bs.segments.empty()) for (auto& it : bs.items) if (it.segments.empty()) { error = "There is no audio to bounce."; return false; }
    return true;
}

void BounceComponent::setBusy (bool busy)
{
    for (juce::Component* c : std::initializer_list<juce::Component*> { &fullButton, &ioButton, &selectedButton, &allTakesButton, &mixerBox, &allExt, &allInt, &allTracks, &clearAll,
                                                                         &normToggle, &togetherToggle, &outNameToggle, &peakEditor, &nameEditor, &folderEditor, &chooseButton })
        c->setEnabled (! busy);
    bounceButton.setButtonText (busy ? "Cancel" : "Bounce");
    if (! busy)
    {
        ioButton.setEnabled (ioButton.isEnabled());
        updateEnabled();
    }
}

void BounceComponent::start()
{
    if (job != nullptr) { job->cancel(); status.setText ("Cancelling...", juce::dontSendNotification); return; }
    if (app.engine.isRecording()) { showError ("Bounce Out", "Stop recording first."); return; }
    BounceSettings bs;
    juce::String err;
    if (! buildSettings (bs, err)) { status.setText (err, juce::dontSendNotification); status.setColour (juce::Label::textColourId, theme::warn); return; }
    if (auto* st = app.props.getUserSettings())
    {
        st->setValue ("bounceFolder|" + app.project.projectFile.getFullPathName(), folderEditor.getText()); st->setValue ("bounceNormalise", normToggle.getToggleState());
        st->setValue ("bouncePeak1", peakEditor.getText());
        st->setValue ("bounceTogether", togetherToggle.getToggleState()); st->setValue ("bounceOutName", outNameToggle.getToggleState()); app.props.saveIfNeeded();
    }
    status.setColour (juce::Label::textColourId, theme::text);
    status.setText ("Bouncing...", juce::dontSendNotification);
    showButton.setVisible (false);
    progressValue = 0.0;
    setBusy (true);
    auto safe = juce::Component::SafePointer<BounceComponent> (this);
    job = std::make_unique<BounceJob> (app.project, bs, [safe] (BounceResult r) { if (safe != nullptr) safe->finished (r); });
}

void BounceComponent::finished (const BounceResult& r)
{
    job.reset();
    setBusy (false);
    progressValue = r.error.isEmpty() && ! r.cancelled ? 1.0 : 0.0;
    if (r.cancelled) status.setText ("Cancelled.", juce::dontSendNotification);
    else if (r.error.isNotEmpty()) { status.setColour (juce::Label::textColourId, theme::warn); status.setText (r.error, juce::dontSendNotification); }
    else
    {
        status.setColour (juce::Label::textColourId, r.clipped ? theme::warn : theme::text);
        status.setText ("Done: " + juce::String (r.files.size()) + " file(s), loudest peak " + juce::String (r.peakDb, 1) + " dBFS"
                        + (r.clipped ? "  -  THIS CLIPPED (over 0 dBFS). Bounce again with Normalise ticked, or lower the faders." : "") + "\n"
                        + juce::File (r.files[0]).getFileName() + (r.files.size() > 1 ? "  ..." : ""), juce::dontSendNotification);
        lastFile = juce::File (r.files[0]);
        showButton.setVisible (true);
    }
}

void BounceComponent::timerCallback()
{
    if (job != nullptr) progressValue = job->getProgress();
}
} // namespace td
