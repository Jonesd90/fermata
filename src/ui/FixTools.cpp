// The fix tools: imports, ReHarmoniSer, copying the project, Export for Processing, the room tone, AAF import; and the few functions every tool
// uses (say, launchDialog, isAudioFile). 3.18.6: split - the shared parts are in FixToolsCommon.h, the pitch / repair / Noise mode / pitch curve
// tools in FixToolsEdits.cpp (with their windows in FixToolsDialogs.h).
#include "FixToolsCommon.h"

namespace td { namespace fixtools
{
// ======================================================================================================== helpers every tool uses
void say (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle (title).withMessage (text).withButton ("OK"), nullptr);
}

void launchDialog (std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* around, bool resizable, std::function<bool()> mayClose)
{
    juce::DialogWindow* dw = nullptr;
    if (mayClose != nullptr)
    {
        auto* g = new GuardedDialogWindow (title, theme::window, around != nullptr ? juce::Component::getApproximateScaleFactorForComponent (around) : 1.0f);
        g->mayClose = std::move (mayClose);
        g->setContentOwned (content.release(), true);
        g->centreAroundComponent (around, g->getWidth(), g->getHeight());
        g->setResizable (resizable, false);
        g->setUsingNativeTitleBar (true);
        g->setAlwaysOnTop (juce::WindowUtils::areThereAnyAlwaysOnTopWindows());
        g->enterModalState (true, nullptr, true);
        dw = g;
    }
    else
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (content.release());
        o.dialogTitle = title;
        o.dialogBackgroundColour = theme::window;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = resizable;
        o.componentToCentreAround = around;
        dw = o.launchAsync();
    }
    if (dw != nullptr && resizable)
    {
        const auto key = title.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- ");
        auto saved = juce::Rectangle<int>::fromString (uiPrefs().getValue ("win." + key));
        if (saved.getWidth() >= 300 && saved.getHeight() >= 200)
        {
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto area = displays.getDisplayForRect (saved)->userArea;
            saved.setSize (juce::jmin (saved.getWidth(), area.getWidth()), juce::jmin (saved.getHeight(), area.getHeight()));
            // keep it on a screen (the screens may have changed since): if less than 120 pixels of it would show, it is centred instead
            if (! area.getIntersection (saved).expanded (0).contains (saved.getX() + 60, saved.getY() + 20) && ! area.contains (saved.getCentre())) saved.setCentre (area.getCentre());
            saved.setPosition (juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - saved.getWidth()), saved.getX()), juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - saved.getHeight()), saved.getY()));
            dw->setBounds (saved);
        }
        auto* k = new BoundsKeeper(); k->key = key; k->last = dw->getBounds(); dw->addComponentListener (k);
    }
}

bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;wave;bwf;w64;aif;aiff;aifc;flac"); }

// ======================================================================================================== the order of the imported tracks
class TrackOrderDialog : public juce::Component, private juce::ListBoxModel
{
public:
    TrackOrderDialog (std::vector<std::pair<juce::String, juce::String>> keysAndNames, std::function<void (const std::vector<juce::String>&)> onOk)
        : items (std::move (keysAndNames)), done (std::move (onOk))
    {
        setSize (380, 520);
        list.setModel (this); list.setRowHeight (24); addAndMakeVisible (list);
        intro.setText ("Put the tracks in the order you want them, top to bottom (in the take windows, edit windows and mixers). Select a track and use Move up / Move down.", juce::dontSendNotification);
        intro.setJustificationType (juce::Justification::topLeft); intro.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (intro);
        for (auto* b : { &up, &down, &ok, &cancel }) addAndMakeVisible (b);
        ok.setColour (juce::TextButton::buttonColourId, theme::accent);
        up.onClick = [this] { move (-1); }; down.onClick = [this] { move (1); };
        ok.onClick = [this] { std::vector<juce::String> k; for (auto& i : items) k.push_back (i.first); if (done) done (k); closeDialogOf (this); };
        cancel.onClick = [this] { closeDialogOf (this); };
        list.selectRow (0);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        intro.setBounds (r.removeFromTop (54)); r.removeFromTop (4);
        auto bottom = r.removeFromBottom (30); r.removeFromBottom (6);
        cancel.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (8); ok.setBounds (bottom.removeFromRight (90));
        auto mv = r.removeFromBottom (28); up.setBounds (mv.removeFromLeft (120)); mv.removeFromLeft (8); down.setBounds (mv.removeFromLeft (120)); r.removeFromBottom (6);
        list.setBounds (r);
    }
private:
    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (row < 0 || row >= (int) items.size()) return;
        if (sel) g.fillAll (theme::accent.withAlpha (0.35f));
        g.setColour (theme::text); g.setFont (juce::FontOptions (14.0f));
        g.drawText (juce::String (row + 1) + "   " + items[(size_t) row].second, 8, 0, w - 12, h, juce::Justification::centredLeft);
    }
    void move (int d)
    {
        const int r = list.getSelectedRow(), t = r + d;
        if (r < 0 || t < 0 || t >= (int) items.size()) return;
        std::swap (items[(size_t) r], items[(size_t) t]);
        list.updateContent(); list.selectRow (t); list.repaint();
    }
    std::vector<std::pair<juce::String, juce::String>> items; std::function<void (const std::vector<juce::String>&)> done;
    juce::ListBox list; juce::Label intro; juce::TextButton up { "Move up" }, down { "Move down" }, ok { "OK" }, cancel { "Cancel" };
};

// ======================================================================================================== the import dialog
class ImportDialog : public JobDialogBase
{
public:
    ImportDialog (AppContext& a, const juce::Uuid& wid, const juce::Uuid& eid, std::vector<ImportSource> srcs, juce::StringArray problemNotes)
        : sources (std::move (srcs)), problems (std::move (problemNotes)), app (a), windowId (wid), editId (eid)
    {
        setSize (680, 680);
        for (auto* x : std::initializer_list<juce::Component*> { &summary, &doIt, &cancel }) addAndMakeVisible (x);
        summary.setMultiLine (true); summary.setReadOnly (true); summary.setCaretVisible (false); summary.setScrollbarsShown (true);
        summary.setColour (juce::TextEditor::backgroundColourId, theme::field); summary.setFont (juce::FontOptions (13.0f));
        if (auto* st = app.props.getUserSettings())
        {
            juce::StringArray ks; ks.addTokens (st->getValue ("importTrackOrder", {}), "|", "");
            for (auto& k : ks) if (k.isNotEmpty()) trackOrder.push_back (k);
        }
        // how the file names are read: Automatic, or the program that recorded them (when Automatic gets it wrong)
        namingCap.setText ("Recorded with:", juce::dontSendNotification); namingCap.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (namingCap);
        namingBox.addItem ("Automatic", 1); namingBox.addItem ("Pyramix  (piece 001_mic-L / -R / -C)", 2); namingBox.addItem ("Pro Tools  (piece track 01, .L / .R)", 3);
        namingBox.addItem ("SADiE  (piece-Mic01)", 4); namingBox.addItem ("Other: by file length (every file of a take is the same length)", 5);
        namingBox.setSelectedId (1, juce::dontSendNotification);
        namingBox.setTooltip ("How Fermata reads the file names to find the takes and the tracks. Automatic tries every way it knows and says which it used. "
                              "If the takes or tracks come out wrong, choose the program that recorded the files: the files are read again that way at once.");
        namingBox.onChange = [this] { setBase(); };
        addAndMakeVisible (namingBox);
        pieceBox.onClick = [this] { refreshSummary(); };
        addAndMakeVisible (orderButton);
        orderButton.setTooltip ("Choose the order of the tracks (top to bottom). Fermata remembers it for the next import.");
        orderButton.onClick = [this] { openOrder(); };
        joinBox.setButtonText ("Join left / right pairs into stereo tracks (one stereo strip and one stereo track each). Tick the ones you want stereo, untick the ones to keep as mono tracks:");
        joinBox.onClick = [this] { for (auto* b : pairBoxes) b->setToggleState (joinBox.getToggleState(), juce::dontSendNotification); rebuildPlan(); };
        pairView.setViewedComponent (&pairHolder, false); pairView.setScrollBarsShown (true, false);
        numBox.setButtonText ("Also join names ending in 1 and 2 (Mic 1 + Mic 2) into stereo tracks");
        numBox.onClick = [this] { buildPairList(); rebuildPlan(); };
        setBase();
        // the files are one set of tracks: each goes on the track named in its file name, whatever else the name says (numbers, the piece)
        addAndMakeVisible (byNameBox);
        byNameBox.setButtonText ("One set of tracks: put each file on the track named in its file name (AB, SPOT1 ...) - numbers and the rest of the name are ignored");
        byNameBox.setTooltip ("For files whose names carry the piece and the take numbers (\"Rorate Caeli SPOT4 19 18\"), which would otherwise each make new tracks. "
                              "A file goes on the project's track whose name its name ends with, once the numbers at the end are taken off; a file with no such "
                              "track gets a track of its own (named without the numbers, shared by the takes). The takes are still told apart by their lengths.");
        if (auto* st = app.props.getUserSettings()) byNameBox.setToggleState (st->getBoolValue ("importByTrackName", false), juce::dontSendNotification);
        byNameBox.onClick = [this] { if (auto* st = app.props.getUserSettings()) st->setValue ("importByTrackName", byNameBox.getToggleState()); refreshSummary(); };
        addAndMakeVisible (copyBox); addAndMakeVisible (linkBox);
        copyBox.setButtonText ("Copy the audio into this project's folder (recommended)"); linkBox.setButtonText ("Use the audio files where they are (nothing is copied)");
        copyBox.setRadioGroupId (4711); linkBox.setRadioGroupId (4711);
        copyBox.setToggleState (true, juce::dontSendNotification);
        copyBox.onClick = [this] { refreshSummary(); }; linkBox.onClick = [this] { refreshSummary(); };
        doIt.setColour (juce::TextButton::buttonColourId, theme::accent);
        doIt.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
    }
    /** (Re)reads the files the way the "Recorded with" box says and sets up everything that depends on it. */
    void setBase()
    {
        static const ImportNaming ways[] = { ImportNaming::Auto, ImportNaming::Pyramix, ImportNaming::ProTools, ImportNaming::Sadie, ImportNaming::ByLength };
        basePlan = buildImportPlan (sources, ways[juce::jlimit (0, 4, namingBox.getSelectedId() - 1)]);
        for (auto& pr : problems) basePlan.notes.add (pr);
        plan = basePlan;
        std::set<juce::String> pieceNames; for (auto& t : basePlan.takes) if (t.piece.isNotEmpty()) pieceNames.insert (t.piece);
        const bool pieces = editId.isNull() && pieceNames.size() > 1;
        pieceBox.setVisible (pieces);
        if (pieces)
        {
            pieceBox.setButtonText ("The file names name " + juce::String ((int) pieceNames.size()) + " different pieces: put each piece in its own take window (named after the piece)");
            pieceBox.setToggleState (true, juce::dontSendNotification);
        }
        pairBoxes.clear(); pairCands.clear();
        const bool pairs = hasStereoPairs (basePlan);
        joinBox.setVisible (pairs); pairView.setVisible (pairs);
        numBox.setVisible (pairs && joinStereoPairs (basePlan, true).tracks.size() < joinStereoPairs (basePlan, false).tracks.size());    // there are names ending 1 and 2 as well
        if (pairs)
        {
            joinBox.setToggleState (true, juce::dontSendNotification); numBox.setToggleState (false, juce::dontSendNotification);
            buildPairList();
            rebuildPlan();
        }
        else refreshSummary();
        resized();
    }
    bool numbersOn() const { return numBox.isVisible() && numBox.getToggleState(); }
    /** One tick box per left / right pair that could be a stereo track. What was kept mono last time stays unticked. */
    void buildPairList()
    {
        std::set<juce::String> keepMono;
        if (pairBoxes.isEmpty())
        {
            if (auto* st = app.props.getUserSettings())
            {
                juce::StringArray ks; ks.addTokens (st->getValue ("importMonoPairs", {}), "|", "");
                for (auto& k : ks) if (k.isNotEmpty()) keepMono.insert (k);
            }
        }
        else for (int i = 0; i < pairBoxes.size(); ++i) if (! pairBoxes[i]->getToggleState()) keepMono.insert (pairCands[(size_t) i].key);
        pairBoxes.clear(); pairCands = stereoPairCandidates (basePlan, numbersOn());
        for (auto& c : pairCands)
        {
            auto* b = pairBoxes.add (new juce::ToggleButton());
            b->setButtonText (c.name + "     (" + c.leftName + " + " + c.rightName + ")  =  one stereo track");
            b->setToggleState (keepMono.count (c.key) == 0, juce::dontSendNotification);
            b->onClick = [this] { rebuildPlan(); };
            pairHolder.addAndMakeVisible (b);
        }
        pairHolder.setSize (juce::jmax (100, pairView.getWidth() - 20), juce::jmax (1, pairBoxes.size()) * 22);
        for (int i = 0; i < pairBoxes.size(); ++i) pairBoxes[i]->setBounds (0, i * 22, pairHolder.getWidth(), 22);
        resized();
    }
    void rebuildPlan()
    {
        if (pairBoxes.isEmpty() && pairCands.empty() && ! joinBox.isVisible()) { plan = basePlan; refreshSummary(); return; }
        std::set<juce::String> only; int ticked = 0;
        for (int i = 0; i < pairBoxes.size(); ++i) if (pairBoxes[i]->getToggleState()) { only.insert (pairCands[(size_t) i].key); ++ticked; }
        plan = ticked > 0 ? joinStereoPairs (basePlan, numbersOn(), &only) : basePlan;
        joinBox.setToggleState (ticked == pairBoxes.size() && ticked > 0, juce::dontSendNotification);
        refreshSummary();
    }
    bool linking() const { return linkBox.getToggleState(); }
    bool byName() const { return byNameBox.getToggleState(); }
    bool splitting() const { return pieceBox.isVisible() && pieceBox.getToggleState(); }
    /** The tracks in the order the person chose (anything not in that list keeps its place after the others). */
    void applyOrder (ImportPlan& pl) const
    {
        if (trackOrder.empty()) return;
        auto rank = [&] (const juce::String& k) { for (size_t i = 0; i < trackOrder.size(); ++i) if (trackOrder[i] == k) return (int) i; return 1000000; };
        std::stable_sort (pl.tracks.begin(), pl.tracks.end(), [&] (const ImportTrack& x, const ImportTrack& y) { return rank (x.key) < rank (y.key); });
    }
    void openOrder()
    {
        applyOrder (plan);
        std::vector<std::pair<juce::String, juce::String>> items; for (auto& t : plan.tracks) items.push_back ({ t.key, t.name });
        juce::Component::SafePointer<ImportDialog> self (this);
        launchDialog (std::make_unique<TrackOrderDialog> (std::move (items), [self] (const std::vector<juce::String>& keys)
        {
            if (self == nullptr) return;
            self->trackOrder = keys;
            self->refreshSummary();
        }), "Track order", this);
    }
    void refreshSummary()
    {
        applyOrder (plan);
        juce::String t;
        t << juce::String ((int) plan.takes.size()) << " take(s), " << juce::String ((int) plan.tracks.size()) << " track(s)."
          << (plan.readAs.isNotEmpty() ? "   (The names were read as " + plan.readAs + (namingBox.getSelectedId() == 1 ? " - automatically; if this is wrong, choose the program in \"Recorded with\" below" : juce::String()) + ".)" : juce::String())
          << "\n\n";
        juce::String lastPiece; int n = 0;
        for (size_t i = 0; i < plan.takes.size(); ++i)
        {
            const auto& tk = plan.takes[i];
            if (splitting() && tk.piece != lastPiece) { lastPiece = tk.piece; n = 0; t << (i > 0 ? "\n" : "") << "Take window  \"" << tk.piece << "\":\n"; }
            ++n;
            t << "Take " << juce::String (splitting() ? n : (int) i + 1) << (tk.number > 0 ? "  (number " + juce::String (tk.number) + " in the file names)" : juce::String())
              << ":  " << juce::String ((int) tk.parts.size()) << " track(s),  " << formatTimeShort ((double) tk.length / juce::jmax (1.0, tk.sampleRate), 2)
              << ",  " << juce::String (tk.sampleRate, 0) << " Hz\n";
        }
        if (byName())
        {
            // what the tick box does: each file's track, by the name in the file name
            t << "\nTracks (each file on the track named in its file name; new ones are added):\n";
            std::map<juce::String, juce::StringArray> onto; std::vector<juce::String> order;
            for (auto& tr : plan.tracks)
            {
                const auto* m = app.project.trackNamedIn (tr.name, tr.channels);
                const auto target = m != nullptr ? m->name : Project::withoutEndNumbers (tr.name) + "  (new track)";
                if (! onto.count (target)) order.push_back (target);
                onto[target].add (tr.name);
            }
            for (auto& o : order)
                t << "   " << o << "   <   " << onto[o][0] << (onto[o].size() > 1 ? " and " + juce::String (onto[o].size() - 1) + " more" : juce::String()) << "\n";
        }
        else
        {
            t << "\nTracks, in this order (matched to the tracks you already have by name; new ones are added):\n";
            for (auto& tr : plan.tracks) t << "   " << tr.name << (tr.channels == 2 ? "  (stereo)" : "") << "\n";
        }
        if (plan.notes.size() > 0) { t << "\nNotes:\n"; for (auto& note : plan.notes) t << "   " << note << "\n"; }
        if (! editId.isNull()) t << "\nEach take is placed in the edit, one after the other, with every track on its own row of the edit window (ready for mixing).\n";
        if (linking())
            t << "\nThe files are used where they are, exactly as they are (nothing is copied; if you move or delete them the project cannot find the audio). Left / right pairs joined into a stereo track and the channels of multichannel files still have to be written as new files in this project's folder.";
        else
            t << "\nThe audio is copied into this project's folder as 24-bit WAV files; your original files are not touched.";
        summary.setText (t, false);
        doIt.setButtonText (editId.isNull() ? "Import " + juce::String ((int) plan.takes.size()) + " take(s)" : "Bring into the edit");
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bottom = r.removeFromBottom (30);
        orderButton.setBounds (bottom.removeFromLeft (150));
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); doIt.setBounds (bottom.removeFromRight (180));
        linkBox.setBounds (r.removeFromBottom (24)); copyBox.setBounds (r.removeFromBottom (24)); r.removeFromBottom (4);
        byNameBox.setBounds (r.removeFromBottom (24)); r.removeFromBottom (4);
        { auto row = r.removeFromBottom (26); namingCap.setBounds (row.removeFromLeft (110)); namingBox.setBounds (row.removeFromLeft (juce::jmin (440, row.getWidth())).reduced (0, 1)); r.removeFromBottom (4); }
        if (pieceBox.isVisible()) pieceBox.setBounds (r.removeFromBottom (24));
        if (numBox.isVisible()) numBox.setBounds (r.removeFromBottom (24));
        if (joinBox.isVisible())
        {
            pairView.setBounds (r.removeFromBottom (juce::jmin (130, juce::jmax (24, pairBoxes.size() * 22 + 4))));
            pairHolder.setSize (juce::jmax (100, pairView.getWidth() - 20), juce::jmax (1, pairBoxes.size()) * 22);
            for (int i = 0; i < pairBoxes.size(); ++i) pairBoxes[i]->setBounds (0, i * 22, pairHolder.getWidth(), 22);
            joinBox.setBounds (r.removeFromBottom (24)); r.removeFromBottom (4);
        }
        r.removeFromBottom (6); bar.setBounds (r.removeFromBottom (22)); status.setBounds (r.removeFromBottom (22));
        summary.setBounds (r);
    }
private:
    std::vector<ImportSource> sources; juce::StringArray problems;     // the files as read (header information), and what could not be read
    ImportPlan basePlan; std::vector<juce::String> trackOrder;
    juce::Label namingCap; juce::ComboBox namingBox;
    juce::ToggleButton joinBox, numBox, copyBox, linkBox, pieceBox, byNameBox; juce::TextButton orderButton { "Track order..." };
    std::vector<StereoPairCandidate> pairCands; juce::OwnedArray<juce::ToggleButton> pairBoxes; juce::Component pairHolder; juce::Viewport pairView;
    void start()
    {
        auto* w = app.project.findTakeWindow (windowId);
        auto* ed = app.project.findEdit (editId);
        if (editId.isNull() ? w == nullptr : ed == nullptr) return;
        if (ed != nullptr && ! ed->isEmpty() && ! plan.takes.empty() && std::abs (ed->sampleRate - plan.takes.front().sampleRate) > 0.5)
        {
            status.setText ("This audio is " + juce::String (plan.takes.front().sampleRate, 0) + " Hz but the edit is " + juce::String (ed->sampleRate, 0) + " Hz.", juce::dontSendNotification);
            return;
        }
        if (! pairBoxes.isEmpty())
            if (auto* st = app.props.getUserSettings())
            {
                juce::StringArray keep; for (int i = 0; i < pairBoxes.size(); ++i) if (! pairBoxes[i]->getToggleState()) keep.add (pairCands[(size_t) i].key);
                st->setValue ("importMonoPairs", keep.joinIntoString ("|"));
            }
        doIt.setEnabled (false); status.setText (linking() ? "Reading the files..." : "Copying the audio...", juce::dontSendNotification);
        const bool link = linking(), named = byName();
        applyOrder (plan);
        const bool reorder = ! trackOrder.empty();
        if (reorder) if (auto* st = app.props.getUserSettings()) { juce::StringArray ks; for (auto& k : trackOrder) ks.add (k); st->setValue ("importTrackOrder", ks.joinIntoString ("|")); }
        if (editId.isNull() && splitting())
        {
            // one take window per piece (an existing window of the same name is used), each numbering its own takes
            struct Part { juce::Uuid wid; ImportPlan plan; juce::File folder; juce::String label; std::vector<int> numbers; std::vector<std::vector<juce::File>> files; bool created = false; };
            auto parts = std::make_shared<std::vector<Part>>();
            std::vector<juce::String> pieces;
            for (auto& t : plan.takes) if (std::find (pieces.begin(), pieces.end(), t.piece) == pieces.end()) pieces.push_back (t.piece);
            for (auto& pc : pieces)
            {
                Part part; part.plan = plan; part.plan.takes.clear(); part.plan.tracks.clear();
                std::set<juce::String> keys;
                for (auto& t : plan.takes) if (t.piece == pc) { part.plan.takes.push_back (t); for (auto& pt : t.parts) keys.insert (pt.trackKey); }
                for (auto& tr : plan.tracks) if (keys.count (tr.key)) part.plan.tracks.push_back (tr);
                TakeWindowDef* tw = nullptr;
                for (auto& x : app.project.takeWindows) if (x->name.equalsIgnoreCase (pc.isEmpty() ? w->name : pc)) { tw = x.get(); break; }
                if (tw == nullptr) { tw = &app.project.addTakeWindow (pc.isEmpty() ? w->name : pc); part.created = true; }
                part.wid = tw->id; part.folder = app.project.takeFolder (*tw); part.label = tw->labelFor (TakeGroup());
                int next = tw->nextNumber;
                for (size_t i = 0; i < part.plan.takes.size(); ++i) part.numbers.push_back (next++);
                parts->push_back (std::move (part));
            }
            juce::Component::SafePointer<ImportDialog> safe (this); auto& appRef = app;
            runJob ([parts, link] (AudioJob& j) -> juce::String
                    {
                        for (size_t k = 0; k < parts->size(); ++k)
                        {
                            auto& pt = (*parts)[k];
                            const auto err = copyImportFiles (formats(), pt.plan, pt.folder, pt.label, pt.numbers, pt.files, &j, link);
                            if (err.isEmpty()) continue;
                            // The pieces copied before this one used to be left on the disk, in no take (and a second try copied them again under "(2)"
                            // names). Their new files are taken away again here: copies made a moment ago, never put in a take - the originals are not
                            // touched, and a file that is an original (linked, not copied) is left alone.
                            for (size_t m = 0; m < k; ++m)
                            {
                                auto& done = (*parts)[m];
                                auto isOriginal = [&] (const juce::File& f) { for (auto& src : done.plan.sources) if (src.file == f) return true; return false; };
                                for (auto& take : done.files) for (auto& f : take) if (f != juce::File() && ! isOriginal (f)) f.deleteFile();
                                done.files.clear();
                            }
                            return err;
                        }
                        return {};
                    },
                    [safe, parts, &appRef, reorder, named] (const juce::String& err)
                    {
                        if (err.isNotEmpty())                 // the take windows made for this import, still empty: taken away again (a window of the project's own, used by name, stays)
                            for (auto& pt : *parts)
                                if (pt.created) if (auto* tw = appRef.project.findTakeWindow (pt.wid); tw != nullptr && tw->groups.empty()) appRef.deleteWindowAndFile (false, pt.wid);
                        if (safe == nullptr) return;
                        if (err.isNotEmpty()) { safe->status.setText (err + "  Nothing was added; the files copied so far were taken away again.", juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                        busy::begin ("Adding the takes", "Putting the imported takes into their take windows");
                        for (auto& pt : *parts) finishImport (appRef, pt.wid, pt.plan, pt.files, reorder, named);
                        busy::endAfterPendingMessages();
                        closeDialogOf (safe.getComponent());
                    });
            return;
        }
        std::vector<int> numbers; for (size_t i = 0; i < plan.takes.size(); ++i) numbers.push_back ((w != nullptr ? w->nextNumber : 1) + (int) i);
        const auto folder = w != nullptr ? app.project.takeFolder (*w) : app.project.audioFolder().getChildFile ("Imported into " + sanitiseForFile (ed->name));
        const auto label = w != nullptr ? w->labelFor (TakeGroup()) : juce::String ("Imported");
        auto result = std::make_shared<std::vector<std::vector<juce::File>>>();
        auto planCopy = std::make_shared<ImportPlan> (plan);
        auto& appRef = app; const auto wid = windowId; const auto eid = editId;
        juce::Component::SafePointer<ImportDialog> safe (this);
        runJob ([planCopy, folder, label, numbers, result, link] (AudioJob& j) { return copyImportFiles (formats(), *planCopy, folder, label, numbers, *result, &j, link); },
                [safe, planCopy, result, &appRef, wid, eid, reorder, named] (const juce::String& err)
                {
                    if (safe == nullptr) return;
                    if (err.isNotEmpty()) { safe->status.setText (err, juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                    busy::begin ("Adding the takes", eid.isNull() ? "Putting the imported takes into the take window" : "Putting the imported audio into the edit");
                    if (eid.isNull()) finishImport (appRef, wid, *planCopy, *result, reorder, named); else finishImportToEdit (appRef, eid, *planCopy, *result, reorder, named);
                    busy::endAfterPendingMessages();
                    closeDialogOf (safe.getComponent());
                });
    }

    /** The imported tracks take the places they already have in the project's list, in the order of plan.tracks (other tracks stay where they are). */
    static void orderProjectTracks (AppContext& app, const ImportPlan& plan, const std::map<juce::String, juce::Uuid>& trackFor)
    {
        auto& p = app.project;
        std::vector<juce::Uuid> ids;
        for (auto& tr : plan.tracks) { auto it = trackFor.find (tr.key); if (it != trackFor.end() && std::find (ids.begin(), ids.end(), it->second) == ids.end()) ids.push_back (it->second); }
        std::vector<size_t> slots;
        for (size_t i = 0; i < p.tracks.size(); ++i) if (std::find (ids.begin(), ids.end(), p.tracks[i].id) != ids.end()) slots.push_back (i);
        if (slots.size() != ids.size() || ids.size() < 2) return;
        std::vector<TrackDef> picked;
        for (auto& id : ids) for (auto& t : p.tracks) if (t.id == id) { picked.push_back (t); break; }
        if (picked.size() != ids.size()) return;
        for (size_t k = 0; k < slots.size(); ++k) p.tracks[slots[k]] = picked[k];
    }

    /** Finds (by name and channel count) or creates the project track of each import track. 'byName' (the "one set of tracks" box): a name that is
        not a track's goes on the track named in it (Project::trackNamedIn: "Rorate Caeli SPOT4 19 18" -> SPOT4), else on a new track named without
        the numbers at its end, shared by every import track with that name; and two files of one take never go on the same track. */
    static std::map<juce::String, juce::Uuid> mapTracks (AppContext& app, const ImportPlan& plan, TakeWindowDef* w, bool byName = false)
    {
        auto& p = app.project;
        std::map<juce::String, juce::Uuid> trackFor;
        std::set<int> usedInputs; for (auto& t : p.tracks) for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) >= 0) usedInputs.insert (t.inputOf (c));
        auto makeTrack = [&] (const juce::String& trackName, int channels) -> juce::Uuid
        {
            int first = -1;
            for (int i = 0; i + channels <= (int) p.inputs.size() && first < 0; ++i)
            {
                bool freeSlot = true; for (int c = 0; c < channels; ++c) freeSlot = freeSlot && ! usedInputs.count (i + c);
                if (freeSlot) first = i;
            }
            auto& nt = p.addTrack (trackName, channels == 2 ? TrackFormat::Stereo : TrackFormat::Mono, first);
            if (first < 0) nt.inputs.fill (-1); else for (int c = 0; c < channels; ++c) usedInputs.insert (first + c);
            return nt.id;
        };
        std::map<juce::String, juce::Uuid> made;                              // byName: the new track of each name without its numbers (+ channels)
        for (auto& tr : plan.tracks)
        {
            const TrackDef* found = nullptr;
            for (auto& t : p.tracks) if (t.name.equalsIgnoreCase (tr.name) && t.channelCount() == tr.channels) { found = &t; break; }
            if (found == nullptr && byName) found = p.trackNamedIn (tr.name, tr.channels);
            juce::Uuid id;
            if (found != nullptr) id = found->id;
            else if (byName)
            {
                const auto base = Project::withoutEndNumbers (tr.name);
                const auto k = base.toLowerCase() + "#" + juce::String (tr.channels);
                if (auto it = made.find (k); it != made.end()) id = it->second;
                else id = made[k] = makeTrack (base.isNotEmpty() ? base : tr.name, tr.channels);
            }
            else id = makeTrack (tr.name, tr.channels);
            trackFor[tr.key] = id;
            if (w != nullptr) w->unhide (id);
        }
        if (byName)                                                         // a second file of a take on the same track gets a track of its own
            for (auto& tk : plan.takes)
            {
                std::set<juce::Uuid> used;
                for (auto& pt : tk.parts)
                {
                    if (used.insert (trackFor[pt.trackKey]).second) continue;
                    const auto id = makeTrack (pt.trackName, pt.trackChannels);
                    trackFor[pt.trackKey] = id; used.insert (id);
                    if (w != nullptr) w->unhide (id);
                }
            }
        return trackFor;
    }

    /** The window the audio was being imported into was deleted while the files were copied (it used to end without a word): the copies stay where they are, and it is said. */
    static void importTargetGone (const char* what, const std::vector<std::vector<juce::File>>& files)
    {
        juce::File folder;
        for (auto& take : files) for (auto& f : take) if (f != juce::File()) { folder = f.getParentDirectory(); break; }
        showError ("Import", "The " + juce::String (what) + " the audio was being imported into has gone meanwhile, so nothing was added."
                             + (folder != juce::File() ? "\n\nThe files copied for it are still in " + folder.getFullPathName() + " (nothing was deleted); import them again into another window if you want them." : juce::String()));
    }
    /** Edit window import: one piece of the edit per imported take (placed one after the other), each file on its own track. */
    static void finishImportToEdit (AppContext& app, const juce::Uuid& eid, const ImportPlan& plan, const std::vector<std::vector<juce::File>>& files, bool reorder = false, bool byName = false)
    {
        auto* e = app.project.findEdit (eid);
        if (e == nullptr) { importTargetGone ("edit", files); return; }
        auto trackFor = mapTracks (app, plan, nullptr, byName);
        if (reorder) orderProjectTracks (app, plan, trackFor);
        for (size_t ti = 0; ti < plan.takes.size() && ti < files.size(); ++ti)
        {
            const auto& tk = plan.takes[ti];
            EditRegion r;
            r.id = juce::Uuid();
            r.takeName = files[ti].empty() ? juce::String ("Imported") : files[ti].front().getFileNameWithoutExtension();
            r.sampleRate = tk.sampleRate; r.sourceLength = tk.length; r.srcIn = 0; r.srcOut = tk.length;
            for (size_t pi = 0; pi < tk.parts.size() && pi < files[ti].size(); ++pi)
            {
                RegionFile f; f.trackId = trackFor[tk.parts[pi].trackKey]; f.trackName = tk.parts[pi].trackName;
                if (auto* tr = app.project.findTrack (f.trackId)) f.trackName = tr->name;
                f.file = files[ti][pi]; f.numChannels = tk.parts[pi].trackChannels;
                r.files.push_back (f);
            }
            if (r.files.empty() || r.length() <= 0) continue;
            if (! e->trackIds.empty())
                for (auto& f : r.files) if (std::find (e->trackIds.begin(), e->trackIds.end(), f.trackId) == e->trackIds.end()) e->trackIds.push_back (f.trackId);
            e->insertIndex = -1;
            e->insertRegion (r);
        }
        app.project.structureChanged();
        app.project.changed();
    }

    static void finishImport (AppContext& app, const juce::Uuid& wid, const ImportPlan& plan, const std::vector<std::vector<juce::File>>& files, bool reorder = false, bool byName = false)
    {
        auto* w = app.project.findTakeWindow (wid);
        if (w == nullptr) { importTargetGone ("take window", files); return; }
        auto& p = app.project;
        auto trackFor = mapTracks (app, plan, w, byName);
        if (reorder) orderProjectTracks (app, plan, trackFor);
        for (size_t ti = 0; ti < plan.takes.size() && ti < files.size(); ++ti)
        {
            const auto& tk = plan.takes[ti];
            TakeGroup g; g.id = juce::Uuid(); g.number = w->nextNumber++;
            g.startSeconds = w->groups.empty() ? 0.0 : std::ceil (w->endSeconds() + 2.0);
            g.lengthSamples = tk.length; g.sampleRate = tk.sampleRate; g.recordedAt = tk.modified;
            for (size_t pi = 0; pi < tk.parts.size() && pi < files[ti].size(); ++pi)
            {
                TakeFile f; f.trackId = trackFor[tk.parts[pi].trackKey]; f.trackName = tk.parts[pi].trackName;
                f.file = files[ti][pi]; f.numChannels = tk.parts[pi].trackChannels;
                if (tk.parts[pi].source >= 0 && tk.parts[pi].source < (int) plan.sources.size()) f.sourceName = plan.sources[(size_t) tk.parts[pi].source].file.getFileName();
                if (tk.parts[pi].source2 >= 0 && tk.parts[pi].source2 < (int) plan.sources.size()) f.sourceName2 = plan.sources[(size_t) tk.parts[pi].source2].file.getFileName();
                if (auto* tr = p.findTrack (f.trackId)) f.trackName = tr->name;
                g.files.push_back (f);
            }
            w->groups.push_back (std::move (g));
        }
        p.structureChanged();
    }

    AppContext& app; juce::Uuid windowId, editId; ImportPlan plan;
    juce::TextEditor summary; juce::TextButton doIt { "Import" }, cancel { "Cancel" };
};

static void openImportDialog (AppContext& app, const juce::Uuid& wid, const juce::Uuid& eid, const juce::Array<juce::File>& chosen, juce::Component* parent)
{
    juce::Array<juce::File> files;
    for (auto& f : chosen)
    {
        if (f.isDirectory()) { for (auto& c : f.findChildFiles (juce::File::findFiles, false)) if (isAudioFile (c)) files.add (c); }
        else if (isAudioFile (f)) files.add (f);
    }
    if (files.isEmpty()) { say ("Import takes", "No audio files were found. Fermata imports WAV (including polyphonic / multichannel WAV), AIFF and FLAC files."); return; }
    // Every file is opened to read its length, rate and channels. With hundreds of files (or a slow or network drive) that takes a while, so it is
    // done on another thread, with the "please wait" window up saying how far it has got (and a Cancel button). The import window follows.
    struct Shared
    {
        juce::Array<juce::File> files;
        std::atomic<int> done { 0 };
        std::atomic<bool> cancel { false };
        std::vector<ImportSource> sources;
        juce::StringArray problems;
    };
    struct Ticker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } };
    auto sh = std::make_shared<Shared>();
    sh->files = files;
    const int total = files.size();
    busy::begin ("Reading the audio files", juce::String (total) + " files");
    busy::setCancel ([sh] { sh->cancel = true; });
    auto* ticker = new Ticker();                                           // (made and deleted on the message thread only)
    ticker->fn = [sh, total] { busy::step ("File " + juce::String (sh->done.load()) + " of " + juce::String (total), (double) sh->done.load() / (double) juce::jmax (1, total)); };
    ticker->startTimer (100);
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    auto* appPtr = &app;
    const bool launched = juce::Thread::launch ([sh, ticker, appPtr, wid, eid, safeParent]
    {
        juce::AudioFormatManager fm; td::registerAudioFormats (fm);            // (its own, so nothing is shared with the message thread)
        sh->sources = inspectFiles (fm, sh->files, sh->problems, [sh] (int d, int) { sh->done = d; return ! sh->cancel.load(); });
        juce::MessageManager::callAsync ([sh, ticker, appPtr, wid, eid, safeParent]
        {
            delete ticker;
            busy::setCancel (nullptr);
            busy::end();
            if (sh->cancel.load()) return;
            if (sh->sources.empty()) { say ("Import takes", "None of those files could be read.\n\n" + sh->problems.joinIntoString ("\n")); return; }
            launchDialog (std::make_unique<ImportDialog> (*appPtr, wid, eid, std::move (sh->sources), sh->problems), eid.isNull() ? "Import takes" : "Import into the edit", safeParent.getComponent());
        });
    });
    if (! launched) { delete ticker; busy::setCancel (nullptr); busy::end(); say ("Import takes", "Could not start reading the files. Please try again."); }
}

void importTakes (AppContext& app, const juce::Uuid& wid, const juce::Array<juce::File>& files, juce::Component* parent, const juce::Uuid& intoEdit)
{
    if (heldByRec (app, intoEdit.isNull() ? "Importing takes" : "Importing into the edit", parent,
                   [ap = &app, wid, files, intoEdit] (juce::Component* p) { importTakes (*ap, wid, files, p, intoEdit); })) return;
    if (! files.isEmpty()) { openImportDialog (app, wid, intoEdit, files, parent); return; }
    auto chooser = std::make_shared<juce::FileChooser> ("Choose the audio files of the session (select them all)", juce::File(), "*.wav;*.wave;*.bwf;*.aif;*.aiff;*.flac");
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems | juce::FileBrowserComponent::canSelectFiles,
                          [chooser, &app, wid, intoEdit, safeParent] (const juce::FileChooser& fc)
                          { if (fc.getResults().size() > 0) openImportDialog (app, wid, intoEdit, fc.getResults(), safeParent.getComponent()); });
}

void showImportMenu (AppContext& app, const juce::Uuid& wid, juce::Component* source, const juce::Uuid& intoEdit)
{
    juce::PopupMenu m;
    m.addItem (1, "Choose audio files...");
    m.addItem (2, "Choose a folder (every audio file in it)...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (source), [&app, wid, intoEdit, source = juce::Component::SafePointer<juce::Component> (source)] (int r)
    {
        if (r == 1) importTakes (app, wid, {}, source.getComponent(), intoEdit);
        else if (r == 2)
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Choose the folder with the session's audio files");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [chooser, &app, wid, intoEdit, source] (const juce::FileChooser& fc)
                                  { if (fc.getResult() != juce::File()) openImportDialog (app, wid, intoEdit, { fc.getResult() }, source.getComponent()); });
        }
    });
}

// ======================================================================================================== Re-HarmoniSer (edit window)
class ReHarmoniserDialog : public JobDialogBase
{
public:
    ReHarmoniserDialog()
    {
        addAndMakeVisible (editor);
        int w = 1280, h = 800;
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) { w = juce::jmin (w, disp->userArea.getWidth() - 40); h = juce::jmin (h, disp->userArea.getHeight() - 90); }
        setSize (juce::jmax (900, w), juce::jmax (620, h));
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ReHarmoniserDialog> (this)] { if (safe != nullptr) safe->editor.grabKeyboardFocus(); });
    }
    ~ReHarmoniserDialog() override
    {
        if (onClosing) onClosing();
    }
    void resized() override { editor.setBounds (getLocalBounds()); }
    void start (AudioJob::Work w, std::function<void (const juce::String&)> d) { runJob (std::move (w), std::move (d)); }
    void closeMe() { closeDialogOf (this); }
    /** Closing (Cancel, the X or Escape) with corrected notes that have not been written back asks first, as Spectral Repair does (3.0.2: Escape threw them
        away without a word). False when it has asked: the answer closes the window. */
    bool mayClose()
    {
        if (isWrittenBack != nullptr && isWrittenBack()) return true;
        const int n = (int) editor.editList().size();
        if (n == 0) return true;
        juce::Component::SafePointer<ReHarmoniserDialog> self (this);
        confirmAsync ("ReHarmoniSer", juce::String (n) + (n == 1 ? " corrected note has" : " corrected notes have") + " not been written back yet. Close the window and throw "
                      + (n == 1 ? "it" : "them") + " away? (Write back to clip keeps " + (n == 1 ? "it" : "them") + ".)",
                      n == 1 ? "Throw it away" : "Throw them away", [self] { if (self != nullptr) self->closeMe(); });
        return false;
    }
    std::function<bool()> isWrittenBack;                                // set by openReharmoniser: the corrections went into the edit (or this stream is done)
    juce::String jobText = "Working";                                   // what the progress bar in the editor says while a job runs
    void timerCallback() override { JobDialogBase::timerCallback(); editor.setProgress ((double) job.progress(), jobText); }
    void jobEnded (const juce::String&) override { editor.setProgress (-2.0); }
    rehar::ReHarmoniserEditor editor;
    std::function<void()> onClosing;
};

static void openReharmoniser (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot);

void reharmoniserEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "The ReHarmoniSer", parent, [ap = &app, eid] (juce::Component* p) { reharmoniserEdit (*ap, eid, p); })) return;
    const juce::String nm = "ReHarmoniSer";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (app.engine.isRecording()) { say (nm, "Stop recording first."); return; }
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (nm, "First mark the area with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key. Mark the whole stretch that has the notes to correct, with a little music round it (at least 2 seconds is best)."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 lastEnd = 0; int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) { ++pieces; lastEnd = juce::jmax (lastEnd, r.endSample()); }
    if (pieces == 0) { say (nm, "There is no audio between the marks."); return; }
    const auto from = tIn, to = juce::jmin (tOut, lastEnd);
    if (to - from < (juce::int64) (0.5 * rate)) { say (nm, "The marked area is too short (at least about half a second, and at least 2 seconds is best)."); return; }
    StreamSel outS, inS;
    const int sm = detectStreams (*e, from, to, outS, inS, nm);
    if (sm < 0) return;
    if (sm == 0) { openReharmoniser (app, eid, parent, from, to, StreamSel(), nullptr, -1); return; }
    auto pair = makePair (app, eid, nm, "reharmoniser");
    openReharmoniser (app, eid, parent, from, to, outS, pair, 0);
    openReharmoniser (app, eid, parent, from, to, inS, pair, 1);
    pair->focus (0);
}

static void openReharmoniser (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot)
{
    const juce::String nm = "ReHarmoniSer";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto only = e->fixTracks;
    struct Part { juce::File file; juce::int64 fileStart = 0; size_t reg = 0; };       // a file of a region that is heard between the marks
    std::vector<juce::Uuid> trackIds; std::vector<std::vector<Part>> parts;
    auto regs = std::make_shared<std::vector<EditRegion>>();                              // the regions heard between the marks (their fades can reach a little past their ends)
    {
        for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && (! ss.on || ss.has (r.id)))
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                if (std::find (trackIds.begin(), trackIds.end(), f.trackId) == trackIds.end()) { trackIds.push_back (f.trackId); parts.emplace_back(); }
            }
        if (ss.on) { for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && ss.has (r.id)) regs->push_back (r); }          // a stream: only its own pieces
        else for (auto& r : e->regions) if (r.fadeOutFinish() > from && r.fadeInBegin() < to) regs->push_back (r);
        for (size_t ri = 0; ri < regs->size(); ++ri)
            for (auto& f : (*regs)[ri].files)
            {
                const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                if (ti == trackIds.end()) continue;
                parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, ri });
            }
    }
    if (trackIds.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in the marked area."); if (pair != nullptr) pair->closeAll(); return; }

    // what the window and the glue share
    struct State
    {
        std::vector<EditRegion> base;                       // the edit as it was when the window opened
        std::vector<RegionJob> jobs; PreviewApplied applied; bool accepted = false;
        std::vector<int> chBase, chCount;                   // for each track: where its channels start in the block, and how many it has
        std::vector<juce::Uuid> trackIds;
    };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    st->base = e->regions; st->trackIds = trackIds;
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto total = to - from;
    auto dlg = std::make_unique<ReHarmoniserDialog>();
    auto* d = dlg.get();
    juce::Component::SafePointer<ReHarmoniserDialog> self (d);
    app.undoHold += 1;
    auto discardFiles = [st] { for (auto& rj : st->jobs) for (auto& r : rj.results) r.file.deleteFile(); st->jobs.clear(); };
    auto unapply = [&app, eid, st, discardFiles, pair, slot]
    {
        if (st->applied) { if (pair != nullptr) pair->put (slot, false); else if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->base; app.project.changed(); } }
        st->applied = false; discardFiles();
    };
    // The corrected audio is only put into the edit while it is being played (so it goes through the edit's mixer); it is taken out again when playback stops,
    // and it stays in the edit for good only when Write back to clip is pressed.
    auto putIn = [&app, eid, st, pair, slot]() -> bool
    {
        if (st->jobs.empty() || st->applied) return true;
        if (pair != nullptr) { st->applied = true; if (pair->put (slot, true)) return true; st->applied = false; return false; }
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr) return false;
        if (! applyRegionJobs (*e2, st->base, st->jobs, "reharmonised")) { e2->regions = st->base; app.project.changed(); return false; }
        st->applied = true; app.project.changed();
        return true;
    };
    auto takeOut = [&app, eid, st, pair, slot]
    {
        if (! st->applied) return;
        if (pair != nullptr) { st->applied = false; pair->put (slot, false); return; }
        if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->base; app.project.changed(); }
        st->applied = false;
    };
    d->onClosing = [&app, st, unapply]
    {
        app.stopPlayback();
        if (! st->accepted) unapply();
        app.undoHold -= 1;
    };
    d->setWantsKeyboardFocus (false);

    // ---- the editor's host
    auto& h = d->editor.host;
    h.log = [] (const juce::String& s) { juce::Logger::writeToLog (s); };
    h.spaceIsTaken = [] { return spaceIsForRecording(); };           // (as in every other window: the Space that stops a take does nothing else)
    h.loadSetting = [] (const juce::String& k) { return uiPrefs().getValue ("reharmoniser." + k); };
    h.saveSetting = [] (const juce::String& k, const juce::String& v) { uiPrefs().setValue ("reharmoniser." + k, v); };
    d->editor.restoreViewSettings();
    h.play = [&app, eid, from, rate, nm, putIn] (double x, double y, bool loop)
    {
        if (! putIn()) { say (nm, "The corrected audio does not fit the edit any more (" + gApplyWhy + ")."); return; }
        const auto err = app.playEdit (eid, (double) from / rate + x, (double) from / rate + y, loop);
        if (err.isNotEmpty()) say (nm, err);
        app.engine.setPlaybackLooping (loop);
    };
    h.stop = [&app, takeOut] { app.stopPlayback(); takeOut(); };
    h.playPosition = [&app, eid, from, rate, takeOut]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) { takeOut(); return -1.0; }
        return juce::jmax (0.0, app.playheadSeconds() - (double) from / rate);
    };
    h.playTone = [&app, eid, nm] (double hz, double levelDb)         // a piano key: a short tone, to the outputs the edit's mixer uses
    {
        const double sr = app.engine.getSampleRate();
        if (sr < 8000.0 || hz < 20.0 || hz > 8000.0) return;
        const int n = (int) (sr * 1.3);
        std::vector<float> ch ((size_t) n);
        const double amp = std::pow (10.0, juce::jlimit (-60.0, 0.0, levelDb) / 20.0) / 1.3;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double env = juce::jmin (1.0, t / 0.02, juce::jmax (0.0, (1.3 - t) / 0.25));
            ch[(size_t) i] = (float) (amp * env * (std::sin (juce::MathConstants<double>::twoPi * hz * t) + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 2.0 * hz * t)));
        }
        static int toneCounter = 0;
        toneCounter = (toneCounter + 1) % 12;
        const auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto file = tmpDir.getChildFile ("fermata-key-tone-" + juce::String (toneCounter) + ".wav");
        app.stopPlayback();
        file.deleteFile();                                           // (a WAV writer adds to a file that is already there, so an old one must go first)
        if (! audioops::writeWav (file, { ch, ch }, sr)) return;
        int outFirst = 0;
        if (auto* mx = app.project.mixerOfEdit (eid))
        {
            auto outOf = [&] (const juce::Uuid& busId) { for (auto& b : mx->busPool) if (b->busId == busId) return b->outFirst.load(); return -1; };
            int f = -1;
            for (auto& b : app.project.buses) if (b.external && mx->busFor (b.id)->isMain.get()) { f = outOf (b.id); if (f >= 0) break; }     // a main output first
            if (f < 0) for (auto& b : app.project.buses) if (b.external) { f = outOf (b.id); if (f >= 0) break; }
            if (f >= 0) outFirst = f;
        }
        const auto err = app.playRenders ({ { file, 0.0, sr, (juce::int64) n } }, 0.0, -1.0, juce::Uuid::null(), outFirst);
        if (err.isNotEmpty()) say (nm, err);
    };
    h.cancel = [self] { if (self != nullptr && self->mayClose()) self->closeMe(); };         // (asks first when corrected notes would be thrown away)
    d->isWrittenBack = [st] { return st->accepted; };
    h.leaveUnchangedOk = pair != nullptr;
    h.writeBack = [&app, eid, st, self, putIn, pair, slot]
    {
        if (self == nullptr) return;
        if (pair != nullptr)
        {
            // an OUT / IN stream window: this stream is ready (or left unchanged when nothing was corrected); both go into the edit together when both are ready
            app.stopPlayback();
            if (st->applied) { st->applied = false; pair->put (slot, false); }
            st->accepted = true;                                     // (the corrected files wait in the pair; this window goes away and the other stream is next)
            pair->wrote (slot, [] { return true; });
            return;
        }
        if (st->jobs.empty()) { self->editor.setStatus ("Nothing to write back.", true); return; }
        app.stopPlayback();
        if (! putIn()) { self->editor.setStatus ("The corrected audio does not fit the edit any more (" + gApplyWhy + ").", true); return; }
        for (auto& rj : st->jobs) rj.results.clear();              // the new files are kept now
        st->jobs.clear(); st->applied = false; st->accepted = true;
        setUndoForEdit (app, eid, st->base, "reharmoniser");
        app.project.changed();
        self->closeMe();
    };
    h.setCorrected = [&app, eid, st, self, unapply, fixFolder, from, to, rate, ss, pair] (std::shared_ptr<const rehar::Correction> corr, std::function<void (const juce::String&)> done)
    {
        if (self == nullptr) return;
        app.stopPlayback();
        unapply();
        if (corr == nullptr) { done ({}); return; }
        // Each piece of the edit that the marks touch is made again from its OWN files: every track of that piece is loaded together with up to 1 s of
        // untouched sound either side (the part of the take that its crossfades use), the corrections are applied to that, and the new files are written.
        // So the takes on either side of an edit point each get the pitch change for their own sound, and the edit points and their fades stay as they were.
        struct Piece { RegionJob rj; juce::int64 editStart = 0, srcIn = 0, A = 0, B = 0; bool rawBefore = false, rawAfter = false; std::vector<int> base, cnt; };
        auto pieces = std::make_shared<std::vector<Piece>>();
        const auto h1 = (juce::int64) (1.0 * rate);
        FixSpec spec; spec.kind = FixSpec::Kind::Reharmonise;
        int nch = 0; for (int c : st->chCount) nch += c;
        juce::int64 prevHi = from;
        for (auto& r : st->base)
        {
            if (r.endSample() <= from || r.startSample >= to) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
            if (r1 - r0 < 2) continue;
            juce::int64 lo = r0, hi = r1;                                     // a stream also corrects the part of its take past the edit point (its crossfade uses it)
            if (ss.on) { ss.span (r, from, to, lo, hi); lo = juce::jmax (lo, prevHi); if (hi > lo) prevHi = hi; }
            Piece p; p.editStart = r.startSample; p.srcIn = r.srcIn;
            const auto a = r.srcIn + (r0 - r.startSample), b = r.srcIn + (r1 - r.startSample);
            const auto fa = juce::jmin (a, r.srcIn + (lo - r.startSample)), fb = juce::jmax (b, r.srcIn + (hi - r.startSample));
            if (! prepRegionJob (fixFolder, r, p.rj, a, b, fa - h1, fb + h1, a, b, spec)) continue;
            // the stretch [A, B) that every file of the piece can give. A file can be a little shorter than the region says (the recording stopped a few
            // samples early): the piece then ends where the files end, and what is beyond stays exactly as it was.
            juce::int64 A = fa - h1, B = fb + h1; bool ok = true;
            for (size_t k = 0; k < p.rj.src.size(); ++k)
            {
                auto rd = audioops::openReader (formats(), p.rj.src[k]);
                if (rd == nullptr) { ok = false; break; }
                A = juce::jmax (A, p.rj.fileStart[k]); B = juce::jmin (B, p.rj.fileStart[k] + (juce::int64) rd->lengthInSamples);
            }
            const auto a2 = juce::jmax (a, A), b2 = juce::jmin (b, B);
            if (! ok || b2 - a2 < 2) continue;
            // a piece the corrections cannot reach is left exactly as it is (no new files, no new joins)
            if (! rehar::correctionReaches (*corr, (double) ((r.startSample + (A - r.srcIn)) - from) / rate, (double) ((r.startSample + (B - r.srcIn)) - from) / rate)) continue;
            p.rj.a = a2; p.rj.b = b2; p.rj.fixFrom = a2; p.rj.fixTo = b2; p.rj.outFrom = A; p.rj.outTo = B; p.A = A; p.B = B;
            p.rawBefore = a2 > r.srcIn; p.rawAfter = b2 < r.srcOut;           // a join made here (not an edit point): outside the marks it must be exactly the original
            for (size_t k = 0; k < p.rj.idx.size(); ++k)
            {
                const auto& f = r.files[p.rj.idx[k]];
                int bi = -1, ci = 1;
                for (size_t t = 0; t < st->trackIds.size(); ++t) if (st->trackIds[t] == f.trackId) { bi = st->chBase[t]; ci = st->chCount[t]; break; }
                p.base.push_back (bi); p.cnt.push_back (ci);
            }
            pieces->push_back (std::move (p));
        }
        if (pieces->empty()) { done ("Nothing to write in this area."); return; }
        self->jobText = "Making the corrected audio";
        self->start ([pieces, corr, from, nch, rate, fixFolder] (AudioJob& j) -> juce::String
        {
            fixFolder.createDirectory();
            auto cleanup = [&] { for (auto& p : *pieces) { for (auto& r : p.rj.results) r.file.deleteFile(); p.rj.results.clear(); } };
            for (size_t pi = 0; pi < pieces->size(); ++pi)
            {
                auto& p = (*pieces)[pi]; auto& rj = p.rj; rj.results.clear();
                const auto n = p.B - p.A;
                if (n < 16) { cleanup(); return "Nothing to process in this piece."; }
                if (j.cancelled()) { cleanup(); return "Cancelled."; }
                // the piece's own audio, every track together (what the corrections work on)
                rehar::Block blk ((size_t) nch, std::vector<float> ((size_t) n, 0.0f));
                std::vector<std::vector<std::vector<float>>> raw (rj.src.size());
                std::vector<double> fileRate (rj.src.size(), rate);
                for (size_t k = 0; k < rj.src.size(); ++k)
                {
                    auto rd = audioops::openReader (formats(), rj.src[k]);
                    if (rd == nullptr) { cleanup(); return "Cannot read " + rj.src[k].getFileName(); }
                    fileRate[k] = rd->sampleRate;
                    if (! audioops::readRange (*rd, p.A - rj.fileStart[k], n, raw[k])) { cleanup(); return "Cannot read " + rj.src[k].getFileName(); }
                    if (p.base[k] >= 0)
                        for (size_t c = 0; c < raw[k].size(); ++c)
                        {
                            auto& dstv = blk[(size_t) p.base[k] + juce::jmin (c, (size_t) p.cnt[k] - 1)];
                            for (size_t q = 0; q < raw[k][c].size() && q < dstv.size(); ++q) dstv[q] = raw[k][c][q];
                        }
                    if (p.base[k] >= 0) raw[k].clear();                       // (the corrected channels are written, not these)
                }
                const size_t nBefore = (size_t) (p.rj.a - p.A), nAfter = (size_t) (p.B - p.rj.b);
                rehar::Block keepBefore, keepAfter;
                if (p.rawBefore) { keepBefore.assign (blk.size(), {}); for (size_t c = 0; c < blk.size(); ++c) keepBefore[c].assign (blk[c].begin(), blk[c].begin() + (long) nBefore); }
                if (p.rawAfter)  { keepAfter.assign (blk.size(), {});  for (size_t c = 0; c < blk.size(); ++c) keepAfter[c].assign (blk[c].end() - (long) nAfter, blk[c].end()); }
                rehar::Hooks hk; hk.cancelled = [&j] { return j.cancelled(); };
                hk.progress = [&j, pi, np = pieces->size()] (const char*, double f) { j.setProgress ((float) (((double) pi + 0.9 * f) / (double) np)); };
                try { rehar::renderCorrection (*corr, blk, (int) std::lround (rate), (double) ((p.editStart + (p.A - p.srcIn)) - from) / rate, hk); }
                catch (const rehar::Cancelled&) { cleanup(); return "Cancelled."; }
                for (auto& chn : blk) if ((juce::int64) chn.size() != n) { cleanup(); return "The corrected audio is not exactly the same length as the original, so it was not used."; }
                if (p.rawBefore) for (size_t c = 0; c < blk.size(); ++c) std::copy (keepBefore[c].begin(), keepBefore[c].end(), blk[c].begin());
                if (p.rawAfter)  for (size_t c = 0; c < blk.size(); ++c) std::copy (keepAfter[c].begin(), keepAfter[c].end(), blk[c].end() - (long) nAfter);
                for (size_t k = 0; k < rj.src.size(); ++k)
                {
                    std::vector<std::vector<float>> ch = std::move (raw[k]);
                    if (p.base[k] >= 0)
                    {
                        auto rd = audioops::openReader (formats(), rj.src[k]);
                        const size_t nc = rd != nullptr ? (size_t) rd->numChannels : (size_t) 1;
                        ch.assign (nc, {});
                        for (size_t c = 0; c < nc; ++c) ch[c] = blk[(size_t) p.base[k] + juce::jmin (c, (size_t) p.cnt[k] - 1)];
                    }
                    if (! audioops::writeWav (rj.dest[k], ch, fileRate[k])) { cleanup(); return "Cannot write " + rj.dest[k].getFileName(); }
                    audioops::PieceResult res; res.file = rj.dest[k]; res.from = p.A; res.to = p.B; res.ok = true;
                    rj.results.push_back (res);
                }
            }
            return {};
        },
        [&app, eid, st, self, pieces, done, pair] (const juce::String& err)
        {
            if (self == nullptr) return;
            if (err.isNotEmpty())
            {
                for (auto& p : *pieces) for (auto& r : p.rj.results) r.file.deleteFile();
                done (err == "Cancelled." ? err : "Could not write the corrected audio: " + err); return;
            }
            auto* e2 = app.project.findEdit (eid);
            if (e2 == nullptr) { done ("The edit is gone."); return; }
            st->jobs.clear(); for (auto& p : *pieces) st->jobs.push_back (p.rj);
            if (pair == nullptr) e2->regions = st->base;                 // (the edit itself is not changed here: the corrected audio goes in when it is played, and for good at Write back to clip)
            st->applied = false;
            done ({});
        });
    };

    // ---- load the audio (every channel of every track, between the marks), then give it to the editor
    auto loaded = std::make_shared<rehar::Block>();
    st->chBase.assign (parts.size(), 0); st->chCount.assign (parts.size(), 1);
    d->editor.setStatus ("Reading the audio...");
    d->jobText = "Reading the audio";
    d->start ([parts, regs, from, to, total, st, loaded, ss] (AudioJob& j) -> juce::String
    {
        int nch = 0;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            int cnt = 1;
            for (auto& pt : parts[i]) { auto r = audioops::openReader (formats(), pt.file); if (r != nullptr) { cnt = (int) r->numChannels; break; } }
            st->chBase[i] = nch; st->chCount[i] = juce::jmax (1, cnt); nch += st->chCount[i];
        }
        loaded->assign ((size_t) nch, std::vector<float> ((size_t) total, 0.0f));
        // what is loaded is what is heard: each region's own fades (the crossfades between the takes) are applied, so the picture and the pitch
        // measurements see the join as it sounds. (Volume changes are not applied: the files themselves are corrected, and those play on top.)
        std::vector<std::vector<float>> gains (regs->size());
        std::vector<juce::int64> gainFrom (regs->size(), 0);
        juce::int64 prevHi = from;
        for (size_t ri = 0; ri < regs->size(); ++ri)
        {
            const auto& r = (*regs)[ri];
            if (ss.on)                                     // a stream: its own take as it is in the files (no fades, nothing of the other take), the out stream carrying on past the edit, the in stream starting before it
            {
                juce::int64 lo = 0, hi = 0; ss.span (r, from, to, lo, hi); lo = juce::jmax (lo, prevHi);
                if (hi <= lo) continue;
                prevHi = hi; gainFrom[ri] = lo; gains[ri].assign ((size_t) (hi - lo), 1.0f);
                continue;
            }
            const auto t0 = juce::jmax (from, r.fadeInBegin()), t1 = juce::jmin (to, r.fadeOutFinish());
            if (t1 <= t0) continue;
            gainFrom[ri] = t0; gains[ri].resize ((size_t) (t1 - t0));
            for (juce::int64 t = t0; t < t1; ++t) gains[ri][(size_t) (t - t0)] = r.gainAt (t);
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (j.cancelled()) return "Cancelled.";
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, parts.size()));
            for (auto& pt : parts[i])
            {
                const auto& r = (*regs)[pt.reg]; const auto& g = gains[pt.reg];
                if (g.empty()) continue;
                auto rd = audioops::openReader (formats(), pt.file); if (rd == nullptr) continue;
                const auto t0 = gainFrom[pt.reg];
                juce::int64 rel = (r.srcIn + (t0 - r.startSample)) - pt.fileStart, n = (juce::int64) g.size(), dst = t0 - from, gi = 0;
                if (rel < 0) { n += rel; dst -= rel; gi -= rel; rel = 0; }                       // before the start of the file: silence
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*rd, rel, n, ch);
                for (size_t c = 0; c < ch.size(); ++c)
                {
                    auto& out = (*loaded)[(size_t) st->chBase[i] + juce::jmin (c, (size_t) st->chCount[i] - 1)];
                    for (size_t k = 0; k < ch[c].size() && dst + (juce::int64) k < total; ++k) out[(size_t) dst + k] += ch[c][k] * g[(size_t) (gi + (juce::int64) k)];
                }
            }
        }
        return {};
    },
    [&app, st, self, loaded, rate] (const juce::String& err)
    {
        if (self == nullptr) return;
        if (err.isNotEmpty()) { self->editor.setStatus (err, true); return; }
        juce::StringArray names;                                    // for the See mixer: the track names (L / R when a track has two channels)
        for (size_t i = 0; i < st->trackIds.size(); ++i)
        {
            const auto* tr = app.project.findTrack (st->trackIds[i]);
            const auto tn = tr != nullptr ? tr->name : juce::String ("Track " + juce::String ((int) i + 1));
            const int cnt = i < st->chCount.size() ? st->chCount[i] : 1;
            for (int c = 0; c < cnt; ++c) names.add (cnt > 1 ? tn + (cnt == 2 ? (c == 0 ? " L" : " R") : " " + juce::String (c + 1)) : tn);
        }
        self->editor.setChannelNames (names);
        self->editor.setAudio (loaded, (int) std::lround (rate));
    });
    if (pair != nullptr)
    {
        pair->jobsOf[slot] = [st] { return &st->jobs; };
        pair->suffix[slot] = "reharmonised";
        launchStream (pair, slot, std::move (dlg), nm + " - " + ss.label, parent);
    }
    else launchDialog (std::move (dlg), nm, parent, true, [self] { return self == nullptr || self->mayClose(); });      // Escape and the X ask first too
}

// ======================================================================================================== copy the whole project / save as
/** Shows the progress of copying a project (every file is copied, then read back and compared). */
class CopyProjectDialog : public JobDialogBase
{
public:
    CopyProjectDialog (AppContext& a, juce::File dest, juce::String name, bool audio, bool switchWhenDone)
        : app (a), destFolder (std::move (dest)), newName (std::move (name)), copyAudio (audio), saveAs (switchWhenDone)
    {
        setSize (560, 190);
        title.setText (juce::String (saveAs ? "Save as: " : "Copy of the whole project: ") + destFolder.getFullPathName(), juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, theme::text); title.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (title);
        addAndMakeVisible (cancel); addChildComponent (reveal);
        cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        reveal.onClick = [this] { report.projectFile.getParentDirectory().revealToUser(); };
        status.setColour (juce::Label::textColourId, theme::text);
        status.setText ("Starting...", juce::dontSendNotification);
        app.saveNow();
        auto root = app.project.toVar();                       // a snapshot: what is copied is the project as it is now
        const auto src = app.project.projectFolder();
        juce::Component::SafePointer<CopyProjectDialog> self (this);
        auto text = std::make_shared<std::pair<juce::CriticalSection, juce::String>>();
        shared = text;
        auto rep = std::make_shared<projectcopy::Report>();
        runJob ([root, src, dest = destFolder, nm = newName, audio = copyAudio, text, rep] (AudioJob& j) -> juce::String
                {
                    const auto err = projectcopy::run (root, src, dest, nm, audio, [&] (float p, const juce::String& t)
                                                       { j.setProgress (p); { const juce::ScopedLock sl (text->first); text->second = t; } return ! j.cancelled(); }, *rep);
                    return err;
                },
                [self, rep] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->report = *rep;
                    self->finished (err);
                });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        title.setBounds (r.removeFromTop (44));
        status.setBounds (r.removeFromTop (22)); r.removeFromTop (4);
        bar.setBounds (r.removeFromTop (22)); r.removeFromTop (12);
        auto row = r.removeFromBottom (30);
        cancel.setBounds (row.removeFromRight (110)); row.removeFromRight (8); reveal.setBounds (row.removeFromRight (150));
    }
private:
    void timerCallback() override
    {
        JobDialogBase::timerCallback();
        if (auto s = shared.lock()) { const juce::ScopedLock sl (s->first); if (running) status.setText (s->second, juce::dontSendNotification); }
    }
    void finished (const juce::String& err)
    {
        cancel.setButtonText ("Close");
        if (err.isNotEmpty())
        {
            status.setColour (juce::Label::textColourId, theme::warn);
            status.setText (err == "Cancelled." ? juce::String ("Cancelled. The original project was not touched; the unfinished copy can be deleted.") : err, juce::dontSendNotification);
            status.setBounds (status.getBounds().withHeight (60));
            return;
        }
        juce::String t = "Done: " + juce::String (report.files) + " files, " + juce::File::descriptionOfSizeInBytes (report.bytes) + ", each read back and checked.";
        if (report.missing > 0) t << "  " << juce::String (report.missing) << " file(s) the project refers to could not be found and were not copied.";
        status.setText (t, juce::dontSendNotification);
        status.setBounds (status.getBounds().withHeight (60));
        reveal.setVisible (true);
        if (saveAs) { const auto f = report.projectFile; juce::MessageManager::callAsync ([&a = app, f] { a.openProject (f); }); }     // (the application, not this dialog: it may be gone by then)
    }
    AppContext& app; juce::File destFolder; juce::String newName; bool copyAudio, saveAs;
    projectcopy::Report report;
    std::weak_ptr<std::pair<juce::CriticalSection, juce::String>> shared;
    std::shared_ptr<std::pair<juce::CriticalSection, juce::String>> keep;
    juce::Label title;
    juce::TextButton cancel { "Cancel" }, reveal { "Show the folder" };
};

static std::unique_ptr<juce::FileChooser> copyChooser;

void copyProject (AppContext& app, juce::Component* parent, bool saveAs)
{
    if (heldByRec (app, saveAs ? "Save as (it copies the audio)" : "Making a copy of the project", parent, [ap = &app, saveAs] (juce::Component* p) { copyProject (*ap, p, saveAs); })) return;
    if (app.engine.isRecording()) { say ("Recording", "Stop recording first."); return; }
    auto start = [&]
    {
        juce::File last (app.props.getUserSettings()->getValue ("copyFolder"));
        return last.isDirectory() ? last : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    };
    auto choose = [&app, parent, saveAs, start] (bool copyAudio)
    {
        copyChooser = std::make_unique<juce::FileChooser> (saveAs ? "Save as: choose where, and name the project (a folder with this name is created)"
                                                                  : "Copy the whole project: choose where (an external drive, say), and name the copy (a folder with this name is created)",
                                                           start().getChildFile (app.project.name + (saveAs ? " 2" : " copy") + ".fermata"), "*.fermata");
        copyChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [&app, parent, saveAs, copyAudio] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            const auto name = f.getFileNameWithoutExtension();
            auto folder = f.getParentDirectory().getChildFile (name);
            app.props.getUserSettings()->setValue ("copyFolder", f.getParentDirectory().getFullPathName()); app.props.saveIfNeeded();
            launchDialog (std::make_unique<CopyProjectDialog> (app, folder, name, copyAudio, saveAs), saveAs ? "Save project as" : "Copy the whole project", parent);
        });
    };
    if (! saveAs) { choose (true); return; }
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Save as")
                                      .withMessage ("Save this project under a new name and in a new folder.\n\n"
                                                    "Copy everything: all the recordings and bounces are copied too, so the new folder is complete on its own.\n"
                                                    "Project file only: just the project (the edits, mixes and so on) is saved; the audio stays where it is.")
                                      .withButton ("Copy everything").withButton ("Project file only").withButton ("Cancel"),
                                  [choose] (int r) { if (r == 1) choose (true); else if (r == 2) choose (false); });
}

// ======================================================================================================== Export for Processing
/** A small window with a progress bar for a background job; it closes itself when the job has finished fine (after 'onSuccess'), and shows the problem otherwise. */
class SimpleJobDialog : public JobDialogBase
{
public:
    SimpleJobDialog (const juce::String& text, AudioJob::Work work, std::function<void()> onSuccess, std::function<void()> onFail)
    {
        setSize (480, 120);
        status.setColour (juce::Label::textColourId, theme::text);
        status.setText (text, juce::dontSendNotification);
        addAndMakeVisible (cancel);
        cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        runJob (std::move (work), [this, onSuccess, onFail] (const juce::String& err)
        {
            if (err.isEmpty())
            {
                if (onSuccess) onSuccess();
                juce::Component::SafePointer<SimpleJobDialog> sp (this);
                juce::MessageManager::callAsync ([sp] { if (sp != nullptr) closeDialogOf (sp.getComponent()); });
                return;
            }
            if (onFail) onFail();
            status.setColour (juce::Label::textColourId, theme::warn);
            status.setText (err == "Cancelled." ? juce::String ("Cancelled. Nothing was changed.") : err, juce::dontSendNotification);
            cancel.setButtonText ("Close");
        });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        status.setBounds (r.removeFromTop (24)); r.removeFromTop (6);
        bar.setBounds (r.removeFromTop (20)); r.removeFromTop (10);
        cancel.setBounds (r.removeFromBottom (28).removeFromRight (110));
    }
private:
    juce::TextButton cancel { "Cancel" };
};

// ======================================================================================================== the room tone (edit window button)
/** Learns the project's room tone from one source on the job thread: per track, a profile per channel of its file(s) and one for the mono mix. */
static juce::String learnRoomTone (const std::vector<std::pair<juce::Uuid, juce::String>>& tracks, const std::function<void (size_t trackIndex, juce::int64 start, int n, std::vector<std::vector<float>>& out)>& readTrack,
                                   juce::int64 lengthSamples, double rate, const juce::String& source, Project::RoomTone& out, AudioJob& j)
{
    out = Project::RoomTone(); out.source = source;
    const juce::int64 len = juce::jmin (lengthSamples, (juce::int64) (60.0 * rate));          // (a minute says everything)
    if (len < (juce::int64) (0.5 * rate)) return "The room tone is too short (at least half a second, better a few seconds).";
    denoise::Params p;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (j.cancelled()) return "Cancelled.";
        j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, tracks.size()));
        std::vector<std::vector<float>> ch;
        readTrack (i, 0, (int) len, ch);
        if (ch.empty()) continue;
        Project::RoomToneTrack t; t.trackId = tracks[i].first; t.trackName = tracks[i].second;
        std::vector<float> mono (ch[0].size(), 0.0f);
        for (auto& c : ch) { t.perChannel.push_back (denoise::learnProfile ({ c }, rate, p)); for (size_t k = 0; k < c.size() && k < mono.size(); ++k) mono[k] += c[k] / (float) ch.size(); }
        t.mono = denoise::learnProfile ({ mono }, rate, p);
        for (auto& pr : t.perChannel) if (! pr.valid()) return "The room tone is too short for the analysis window.";
        if (t.mono.valid()) out.tracks.push_back (std::move (t));
    }
    if (out.tracks.empty()) return "No audio was found in the room tone.";
    return {};
}

/** A take marked as the room tone, anywhere in the project. */
struct RoomToneCandidate { juce::Uuid windowId, takeId; juce::String text; double in = 0.0, out = 0.0; };
static std::vector<RoomToneCandidate> roomToneCandidates (const Project& project)
{
    std::vector<RoomToneCandidate> v;
    for (auto& w : project.takeWindows)
        for (auto& g : w->groups)
            if (g.roomTone && g.lengthSamples > 0)
            {
                RoomToneCandidate c; c.windowId = w->id; c.takeId = g.id;
                c.in = g.roomToneIn >= 0.0 ? g.roomToneIn : 0.0; c.out = g.roomToneIn >= 0.0 ? juce::jmin (g.lengthSeconds(), g.roomToneOut) : g.lengthSeconds();
                c.text = formatTimeShort (c.in) + " - " + formatTimeShort (c.out) + " of take " + juce::String (g.number) + " of " + w->name + (g.roomToneIn < 0.0 ? " (the whole take)" : "");
                v.push_back (c);
            }
    return v;
}

class RoomToneDialog : public juce::Component
{
public:
    RoomToneDialog (AppContext& a, const juce::Uuid& editId) : app (a), eid (editId)
    {
        setSize (640, 250);
        const auto& rt = app.project.roomTone;
        head.setText (rt.valid() ? "The room tone in use: " + rt.source + " (" + juce::String ((int) rt.tracks.size()) + " tracks). Choose another to replace it."
                                 : "No room tone has been learned yet. The ReTouch and Noise mode windows denoise with it: every track's own noise, learned once from a part of the recording that holds nothing but the room.", juce::dontSendNotification);
        head.setColour (juce::Label::textColourId, theme::text); head.setJustificationType (juce::Justification::topLeft); addAndMakeVisible (head);
        cands = roomToneCandidates (app.project);
        for (size_t i = 0; i < cands.size(); ++i) takeBox.addItem (cands[i].text, (int) i + 1);
        if (! cands.empty()) takeBox.setSelectedId (1, juce::dontSendNotification);
        takeBtn.setButtonText (cands.empty() ? "Use the take marked as the room tone  (none is marked yet)" : "Use the take marked as the room tone:");
        takeBtn.setEnabled (! cands.empty()); takeBox.setEnabled (cands.size() > 1);
        takeBtn.setTooltip ("In a take window, right-click the take that was recorded with nothing but the room in it (or set marks 1 / 2 on the silent part first) and choose \"This whole take is the room tone\" or \"Marks 1 - 2 of this take are the room tone\". One take per take window; it can be in any take window.");
        auto* e = app.project.findEdit (eid);
        const bool marks = e != nullptr && e->fixIn >= 0.0 && e->fixOut > e->fixIn + 0.5;
        marksBtn.setButtonText (marks ? "Use marks 1 - 2 of this edit:  " + formatTimeShort (e->fixIn) + " - " + formatTimeShort (e->fixOut) : "Use marks 1 - 2 of this edit  (set them first)");
        marksBtn.setEnabled (marks);
        marksBtn.setTooltip ("Marks 1 and 2 (keys 1 / 2 at the playhead) round a part of the edit that holds nothing but the room, on every track.");
        laterBtn.setTooltip ("Closes this. Go and mark the room tone - in a take window (right-click a take) or with marks 1 / 2 here - and press the Room tone button again.");
        for (auto* b : { &takeBtn, &marksBtn, &laterBtn }) addAndMakeVisible (b);
        addChildComponent (takeBox); takeBox.setVisible (cands.size() > 1);
        takeBtn.onClick = [this] { chooseTake(); };
        marksBtn.onClick = [this] { chooseMarks(); };
        laterBtn.onClick = [this] { closeDialogOf (this); };
        takeBtn.setColour (juce::TextButton::buttonColourId, theme::accent); marksBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        head.setBounds (r.removeFromTop (70)); r.removeFromTop (10);
        takeBtn.setBounds (r.removeFromTop (34)); r.removeFromTop (4);
        if (takeBox.isVisible()) { takeBox.setBounds (r.removeFromTop (26).withTrimmedLeft (30)); r.removeFromTop (8); } else r.removeFromTop (6);
        marksBtn.setBounds (r.removeFromTop (34)); r.removeFromTop (14);
        laterBtn.setBounds (r.removeFromTop (34));
    }
private:
    void chooseTake()
    {
        const int i = juce::jlimit (0, (int) cands.size() - 1, takeBox.getSelectedId() - 1);
        if (cands.empty()) return;
        const auto c = cands[(size_t) i];
        juce::Component::SafePointer<RoomToneDialog> self (this);
        confirmAsync ("Room tone", "You have selected " + c.text + " as the room tone. Is this correct?", "Yes, learn it", [self, c]
        {
            if (self == nullptr) return;
            auto* w = self->app.project.findTakeWindow (c.windowId); if (w == nullptr) return;
            auto* g = w->findGroup (c.takeId); if (g == nullptr) return;
            std::vector<std::pair<juce::Uuid, juce::String>> tracks; std::vector<juce::File> files;
            for (auto& f : g->files) { tracks.push_back ({ f.trackId, f.trackName }); files.push_back (f.file); }
            const double rate = g->sampleRate > 0 ? g->sampleRate : 48000.0;
            const juce::int64 s0 = (juce::int64) std::llround (c.in * rate), len = (juce::int64) std::llround ((c.out - c.in) * rate);
            self->learn (tracks, [files, s0] (size_t k, juce::int64 start, int n, std::vector<std::vector<float>>& out)
                         { auto r = audioops::openReader (formats(), files[k]); if (r != nullptr) audioops::readRange (*r, s0 + start, n, out); }, len, rate, c.text);
        });
    }
    void chooseMarks()
    {
        auto* e = app.project.findEdit (eid); if (e == nullptr || e->fixIn < 0.0 || e->fixOut <= e->fixIn) return;
        const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
        const auto from = (juce::int64) std::llround (e->fixIn * rate), to = (juce::int64) std::llround (e->fixOut * rate);
        const auto text = formatTimeShort (e->fixIn) + " - " + formatTimeShort (e->fixOut) + " of " + e->name;
        auto tp = std::make_shared<TrackParts> (buildTrackParts (*e, from, to, e->fixTracks));
        if (tp->ids.empty()) { say ("Room tone", "There is no audio between the marks."); return; }
        juce::Component::SafePointer<RoomToneDialog> self (this);
        confirmAsync ("Room tone", "You have selected " + text + " (" + juce::String ((int) tp->ids.size()) + " tracks) as the room tone. Is this correct?", "Yes, learn it", [self, tp, from, to, rate, text]
        {
            if (self == nullptr) return;
            std::vector<std::pair<juce::Uuid, juce::String>> tracks;
            for (size_t i = 0; i < tp->ids.size(); ++i) tracks.push_back ({ tp->ids[i], tp->names[i] });
            // each track's channels over the stretch: the pieces heard there, file by file (a track is one file per piece; the first piece's channel count rules)
            self->learn (tracks, [tp, rate] (size_t i, juce::int64 start, int n, std::vector<std::vector<float>>& out)
            {
                out.clear();
                for (auto& pt : tp->parts[i])
                {
                    const auto p0 = juce::jmax (start, pt.dst), p1 = juce::jmin (start + n, pt.dst + pt.n);
                    if (p1 <= p0) continue;
                    juce::int64 rel = pt.srcA - pt.fileStart + (p0 - pt.dst), cnt = p1 - p0, o = p0 - start;
                    if (rel < 0) { cnt += rel; o -= rel; rel = 0; }
                    if (cnt <= 0) continue;
                    auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue;
                    std::vector<std::vector<float>> ch; audioops::readRange (*r, rel, cnt, ch);
                    if (out.empty()) out.assign (ch.size(), std::vector<float> ((size_t) n, 0.0f));
                    for (size_t c = 0; c < ch.size() && c < out.size(); ++c) for (size_t k = 0; k < ch[c].size() && o + (juce::int64) k < n; ++k) out[c][(size_t) o + k] = ch[c][k];
                }
            }, to - from, rate, text);
        });
    }
    void learn (std::vector<std::pair<juce::Uuid, juce::String>> tracks, std::function<void (size_t, juce::int64, int, std::vector<std::vector<float>>&)> read, juce::int64 len, double rate, const juce::String& text)
    {
        auto out = std::make_shared<Project::RoomTone>();
        auto& a = app;
        auto dlg = std::make_unique<SimpleJobDialog> ("Learning the room tone of " + juce::String ((int) tracks.size()) + " tracks...",
                                                     [tracks, read, len, rate, text, out] (AudioJob& j) { return learnRoomTone (tracks, read, len, rate, text, *out, j); },
                                                     [&a, out] { a.project.roomTone = *out; a.project.changed(); say ("Room tone", "The room tone is learned: " + out->source + ", " + juce::String ((int) out->tracks.size()) + " tracks. The ReTouch and Noise mode windows will denoise with it (draw a box and press Denoise). It is kept with the project."); },
                                                     std::function<void()>());
        auto* parent = getParentComponent();
        closeDialogOf (this);
        launchDialog (std::move (dlg), "Room tone", parent);
    }
    AppContext& app; juce::Uuid eid;
    juce::Label head; juce::TextButton takeBtn, marksBtn, laterBtn { "Not now - I'll go and select it" }; juce::ComboBox takeBox;
    std::vector<RoomToneCandidate> cands;
};

void roomToneEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (editOf (app, eid) == nullptr) return;
    launchDialog (std::make_unique<RoomToneDialog> (app, eid), "Room tone", parent);
}

/** Asks for the name of a processing job; 'done' gets the (non-empty) name. */
static void askProcessingName (const juce::String& message, const juce::String& suggestion, std::function<void (const juce::String&)> done)
{
    auto* aw = new juce::AlertWindow ("Export for Processing", message, juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("n", suggestion);
    aw->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, done] (int res)
    {
        const auto n = aw->getTextEditorContents ("n").trim();
        delete aw;
        if (res == 1 && n.isNotEmpty() && done) done (n);
    }));
}

/** A new folder for the job inside Processing Media, named after the job. */
static juce::File processingJobFolder (AppContext& app, const juce::String& name)
{
    const auto base = app.project.processingFolder();
    base.createDirectory();
    auto f = base.getChildFile (sanitiseForFile (name));
    for (int i = 2; f.exists() && i < 1000; ++i) f = base.getChildFile (sanitiseForFile (name) + " (" + juce::String (i) + ")");
    return f;
}

struct ExportJob { std::vector<juce::File> src, dest; std::vector<juce::int64> fileStart; juce::int64 a = 0, b = 0; juce::File folder; };

static AudioJob::Work exportWork (std::shared_ptr<ExportJob> j)
{
    return [j] (AudioJob& job) -> juce::String
    {
        j->folder.createDirectory();
        for (size_t i = 0; i < j->src.size(); ++i)
        {
            if (job.cancelled()) { j->folder.deleteRecursively(); return "Cancelled."; }
            job.setProgress ((float) i / (float) juce::jmax ((size_t) 1, j->src.size()));
            juce::String err;
            if (! audioops::exportRange (formats(), j->src[i], j->fileStart[i], j->a, j->b, j->dest[i], err)) { j->folder.deleteRecursively(); return err; }
        }
        return {};
    };
}

static std::shared_ptr<ExportJob> makeExportJob (AppContext& app, const juce::String& name, const std::vector<juce::File>& src, const std::vector<juce::String>& trackNames,
                                                 const std::vector<juce::int64>& fileStart, juce::int64 a, juce::int64 b)
{
    auto j = std::make_shared<ExportJob>();
    j->a = a; j->b = b; j->src = src; j->fileStart = fileStart;
    j->folder = processingJobFolder (app, name);
    for (size_t i = 0; i < src.size(); ++i)
    {
        auto d = audioops::uniqueFile (j->folder.getChildFile (sanitiseForFile (name + " - " + trackNames[i]) + ".wav"));
        for (auto& o : j->dest) if (o == d) d = d.getSiblingFile (d.getFileNameWithoutExtension() + " b.wav");
        j->dest.push_back (d);
    }
    return j;
}

static juce::String exportedMessage (const juce::String& name, const juce::File& folder)
{
    return "'" + name + "' is in the folder\n" + folder.getFullPathName() + "\n\nOpen the files in your other software (e.g. iZotope RX), correct them and save OVER the same files, "
           "keeping the same length, sample rate and channels. Then right-click the 'Waiting for corrected audio' block and choose 'Re-link corrected audio'. "
           "Fermata keeps none of these files open, so the other program can overwrite them freely.";
}

/** The piece of region 'rid' that the exported take range [a,b) covers becomes a piece of its own that waits for corrected audio. The exported range may reach past the piece into its handle
    (an OUT / IN stream export): the corrected file then also gives the audio under the crossfade. False if the region is no longer there as it was. */
static bool markWaiting (EditDef& e2, const juce::Uuid& rid, juce::int64 a, juce::int64 b, const juce::String& name, const std::vector<juce::File>& dest, const std::vector<juce::Uuid>& trk, bool subset)
{
    int i = e2.indexOf (rid);
    if (i < 0 || e2.regions[(size_t) i].waiting.active()) return false;
    const auto pa = juce::jmax (a, e2.regions[(size_t) i].srcIn), pb = juce::jmin (b, e2.regions[(size_t) i].srcOut);
    if (pb <= pa) return false;
    const bool cutAfter = pb < e2.regions[(size_t) i].srcOut, cutBefore = pa > e2.regions[(size_t) i].srcIn;
    if (cutAfter)  e2.splitRegion (i, pb - e2.regions[(size_t) i].srcIn);
    if (cutBefore) i = e2.splitRegion (i, pa - e2.regions[(size_t) i].srcIn);
    auto& mid = e2.regions[(size_t) i];
    mid.waiting.name = name; mid.waiting.from = a; mid.waiting.to = b; mid.waiting.files = dest;
    mid.waiting.tracks = subset ? trk : std::vector<juce::Uuid>();
    if (cutBefore) { mid.setInCurve (FadeCurve::Linear); if (i > 0) e2.regions[(size_t) i - 1].setOutCurve (FadeCurve::Linear); }
    if (cutAfter)  { mid.setOutCurve (FadeCurve::Linear); if (i + 1 < (int) e2.regions.size()) e2.regions[(size_t) i + 1].setInCurve (FadeCurve::Linear); }
    if (cutBefore) { mid.fixIn = true; if (i > 0) e2.regions[(size_t) i - 1].fixOut = true; }                 // not edit points
    if (cutAfter)  { mid.fixOut = true; if (i + 1 < (int) e2.regions.size()) e2.regions[(size_t) i + 1].fixIn = true; }
    return true;
}

void exportForProcessingEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "Export for Processing", parent, [ap = &app, eid] (juce::Component* p) { exportForProcessingEdit (*ap, eid, p); })) return;
    const juce::String title = "Export for Processing";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (title, "First mark the part to send out with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    // an edit point between the marks: the two takes are sent out separately (the OUT stream and / or the IN stream), each as it is in the file: full volume, no fade
    {
        juce::int64 lastEnd = 0;
        for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) lastEnd = juce::jmax (lastEnd, r.endSample());
        StreamSel outS, inS;
        const int sm = lastEnd > tIn ? detectStreams (*e, tIn, juce::jmin (tOut, lastEnd), outS, inS, title) : 0;
        if (sm < 0) return;
        if (sm == 1)
        {
            struct Stream { juce::Uuid rid; juce::int64 a = 0, b = 0; juce::String label; std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart; std::vector<juce::Uuid> trk; };
            auto plan = std::make_shared<std::vector<Stream>>();      // [0] = out, [1] = in (a stream that cannot be sent has an empty 'src')
            plan->resize (2);
            const bool subset = ! e->fixTracks.empty();
            juce::String notes;
            auto fill = [&] (Stream& st, const EditRegion& r, juce::int64 lo, juce::int64 hi, const juce::String& label)
            {
                st.label = label;
                lo = juce::jmax (lo, r.startSample - r.srcIn); if (r.sourceLength > 0) hi = juce::jmin (hi, r.startSample + (r.sourceLength - r.srcIn));
                if (hi - lo < (juce::int64) (0.01 * rate) || r.waiting.active()) return;
                st.rid = r.id; st.a = r.srcIn + (lo - r.startSample); st.b = r.srcIn + (hi - r.startSample);
                for (auto& f : r.files) { if (! e->fixUses (f.trackId)) continue; st.src.push_back (f.file); st.names.push_back (f.trackName); st.fstart.push_back (f.fileStart); st.trk.push_back (f.trackId); }
            };
            const EditRegion* A = nullptr; const EditRegion* B = nullptr;
            for (auto& r : e->regions) { if (r.id == outS.ids.back()) A = &r; if (r.id == inS.ids.front()) B = &r; }
            if (A == nullptr || B == nullptr) return;
            // OUT: from mark 1 to mark 2, or to where it has faded out completely, whichever comes first.  IN: from where its fade-in begins (or mark 1) to mark 2.  No fades are applied to what is sent.
            fill ((*plan)[0], *A, juce::jmax (tIn, A->startSample), juce::jmin (tOut, A->fadeOutFinish()), "Out Stream");
            fill ((*plan)[1], *B, juce::jmax (tIn, B->fadeInBegin()), juce::jmin (tOut, B->endSample()), "In Stream");
            if (outS.ids.size() > 1 && tIn < A->startSample) notes << "\n\nNOTE: the OUT stream before " << formatTimeShort ((double) A->startSample / rate, 3) << " is made of an earlier fix, so only the part from there on is sent.";
            if (inS.ids.size() > 1 && tOut > B->endSample()) notes << "\n\nNOTE: the IN stream is sent up to the end of its piece (" << formatTimeShort ((double) B->endSample() / rate, 3) << "); what follows is made of an earlier fix.";
            const bool haveOut = ! (*plan)[0].src.empty(), haveIn = ! (*plan)[1].src.empty();
            if (! haveOut && ! haveIn) { say (title, "There is nothing to send: the marked part is too short on both sides of the edit point, or the tracks you chose with Ctrl + Alt + drag are not there."); return; }
            auto ask = [&app, eid, plan, notes, parent = juce::Component::SafePointer<juce::Component> (parent), subset, title] (bool wantOut, bool wantIn)
            {
                if (wantOut && (*plan)[0].src.empty()) { say (title, "The OUT stream has nothing to send between the marks."); return; }
                if (wantIn && (*plan)[1].src.empty()) { say (title, "The IN stream has nothing to send between the marks."); return; }
                askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". 'Out Stream' / 'In Stream' is added to the end of it. Each becomes a folder in Processing Media with one file per track, "
                                   "in the format they were recorded, at full volume with no fade applied.", "", [&app, eid, plan, notes, parent, subset, wantOut, wantIn] (const juce::String& name)
                {
                    struct Made { std::shared_ptr<ExportJob> job; juce::Uuid rid; juce::int64 a, b; juce::String full; std::vector<juce::Uuid> trk; };
                    auto made = std::make_shared<std::vector<Made>>();
                    for (int q = 0; q < 2; ++q)
                    {
                        if (! (q == 0 ? wantOut : wantIn)) continue;
                        auto& st = (*plan)[(size_t) q]; const auto full = name + " - " + st.label;
                        made->push_back ({ makeExportJob (app, full, st.src, st.names, st.fstart, st.a, st.b), st.rid, st.a, st.b, full, st.trk });
                    }
                    AudioJob::Work work = [made] (AudioJob& job) -> juce::String { for (auto& m : *made) { const auto err = exportWork (m.job) (job); if (err.isNotEmpty()) { for (auto& o : *made) o.job->folder.deleteRecursively(); return err; } } return {}; };
                    auto onSuccess = [&app, eid, made, subset, notes]
                    {
                        auto* e2 = app.project.findEdit (eid);
                        const auto old = e2 != nullptr ? e2->regions : std::vector<EditRegion>();
                        bool ok = e2 != nullptr;
                        if (ok) for (auto& m : *made) ok = markWaiting (*e2, m.rid, m.a, m.b, m.full, m.job->dest, m.trk, subset) && ok;
                        if (! ok)
                        {
                            if (e2 != nullptr) e2->regions = old;
                            for (auto& m : *made) m.job->folder.deleteRecursively();
                            say ("Export for Processing", "The edit changed while the audio was being exported, so nothing was changed. Please try again."); return;
                        }
                        setUndoForEdit (app, eid, old, "export for processing");
                        app.project.changed();
                        juce::String msg;
                        for (auto& m : *made) msg << "'" << m.full << "' is in the folder\n" << m.job->folder.getFullPathName() << "\n\n";
                        msg << "Open the files in your other software (e.g. iZotope RX), correct them and save OVER the same files, keeping the same length, sample rate and channels. "
                               "Then right-click each 'Waiting for corrected audio' block and choose 'Re-link corrected audio'. Fermata keeps none of these files open, so the other program can overwrite them freely." << notes;
                        say ("Export for Processing", msg);
                    };
                    launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", work, onSuccess, nullptr), "Export for Processing", parent.getComponent());
                });
            };
            // (built by hand: JUCE's message box only shows up to THREE buttons, so with four none of them appeared)
            auto* w = new juce::AlertWindow (title,
                                             "The marked part has an edit point in it. Which take do you want to send out?\n\n"
                                             "OUT stream: the take that plays up to the edit point, from mark 1 until it is completely faded out (or mark 2, whichever comes first).\n"
                                             "IN stream: the take that plays from the edit point, from where its fade-in begins (or mark 1) to mark 2.\n\n"
                                             "Each is sent at full volume with no fade applied, so the edit and its crossfade stay in place and each take can be corrected on its own. 'Both' sends two separate sets of files.",
                                             juce::MessageBoxIconType::QuestionIcon);
            w->addButton ("Out stream", 1, juce::KeyPress ('o'));
            w->addButton ("In stream", 2, juce::KeyPress ('i'));
            w->addButton ("Both", 3, juce::KeyPress ('b'));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([ask] (int r)
            {
                if (r == 1) ask (true, false); else if (r == 2) ask (false, true); else if (r == 3) ask (true, true);
            }), true);
            return;
        }
    }
    const EditRegion* region = nullptr;
    for (auto& r : e->regions) if (tIn >= r.startSample && tIn < r.endSample()) region = &r;
    if (region == nullptr) { say (title, "Mark 1 is not on any piece of audio."); return; }
    if (region->waiting.active()) { say (title, "This piece is already waiting for corrected audio. Re-link it first (right-click the 'Waiting for corrected audio' block)."); return; }
    const auto from = tIn, to = juce::jmin (tOut, region->endSample());
    if (to - from < (juce::int64) (0.01 * rate)) { say (title, "The marked part is too short (it must be inside one piece, at least about 0.01 s)."); return; }
    const auto a = region->srcIn + (from - region->startSample), b = a + (to - from);
    const auto rid = region->id;
    std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart; std::vector<juce::Uuid> trk;
    for (auto& f : region->files) { if (! e->fixUses (f.trackId)) continue; src.push_back (f.file); names.push_back (f.trackName); fstart.push_back (f.fileStart); trk.push_back (f.trackId); }
    if (src.empty()) { say (title, "None of the tracks you marked with Ctrl + Alt + drag are in this piece."); return; }
    const bool subset = ! e->fixTracks.empty();
    askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". It becomes a folder in Processing Media with " + juce::String ((int) src.size()) + " file(s), one per track, "
                       "in the format they were recorded.", "", [&app, eid, rid, a, b, src, names, fstart, trk, subset, parent = juce::Component::SafePointer<juce::Component> (parent)] (const juce::String& name)
    {
        auto j = makeExportJob (app, name, src, names, fstart, a, b);
        auto onSuccess = [&app, eid, rid, a, b, j, name, trk, subset]
        {
            auto* e2 = app.project.findEdit (eid);
            const auto old = e2 != nullptr ? e2->regions : std::vector<EditRegion>();
            if (e2 == nullptr || ! markWaiting (*e2, rid, a, b, name, j->dest, trk, subset))
            {
                if (e2 != nullptr) e2->regions = old;
                j->folder.deleteRecursively(); say ("Export for Processing", "The edit changed while the audio was being exported, so nothing was changed. Please try again."); return;
            }
            setUndoForEdit (app, eid, old, "export for processing");
            app.project.changed();
            say ("Export for Processing", exportedMessage (name, j->folder));
        };
        launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", exportWork (j), onSuccess, nullptr), "Export for Processing", parent.getComponent());
    });
}

void exportForProcessingTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "Export for Processing", parent, [ap = &app, wid] (juce::Component* p) { exportForProcessingTake (*ap, wid, p); })) return;
    const juce::String title = "Export for Processing";
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (*w, in, out, title); if (g == nullptr) return;
    if (app.engine.isRecording()) { say (title, "Stop recording first."); return; }
    const double rate = g->sampleRate > 0 ? g->sampleRate : 48000.0;
    const auto a = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (in * rate)), b = juce::jmin (g->lengthSamples, (juce::int64) std::llround (out * rate));
    if (b - a < (juce::int64) (0.01 * rate)) { say (title, "The marked part is too short."); return; }
    for (auto& wp : g->waiting) if (wp.active() && a < wp.to && b > wp.from) { say (title, "Part of this is already waiting for corrected audio ('" + wp.name + "'). Re-link it first (right-click the block)."); return; }
    const auto gid = g->id;
    std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart;
    std::vector<juce::Uuid> trk;
    for (auto& f : g->files) { if (! w->editUses (f.trackId)) continue; src.push_back (f.file); names.push_back (f.trackName); fstart.push_back (0); trk.push_back (f.trackId); }
    if (src.empty()) { say (title, "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    const bool subset = ! w->editTracks.empty();
    askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". It becomes a folder in Processing Media with " + juce::String ((int) src.size()) + " file(s), one per track, "
                       "in the format they were recorded.", "", [&app, wid, gid, a, b, src, names, fstart, trk, subset, parent = juce::Component::SafePointer<juce::Component> (parent)] (const juce::String& name)
    {
        auto j = makeExportJob (app, name, src, names, fstart, a, b);
        auto onSuccess = [&app, wid, gid, a, b, j, name, trk, subset]
        {
            auto* w2 = app.project.findTakeWindow (wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
            if (g2 == nullptr) { j->folder.deleteRecursively(); return; }
            const auto old = g2->waiting;
            WaitingPiece wp; wp.name = name; wp.from = a; wp.to = b; wp.files = j->dest; if (subset) wp.tracks = trk;
            g2->waiting.push_back (wp);
            app.armFixUndo ("export for processing", [&app, wid, gid, old]
            {
                if (auto* w3 = app.project.findTakeWindow (wid)) if (auto* g3 = w3->findGroup (gid)) { g3->waiting = old; app.project.changed(); }
            }, false, wid, gid);
            app.project.changed();
            say ("Export for Processing", exportedMessage (name, j->folder));
        };
        launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", exportWork (j), onSuccess, nullptr), "Export for Processing", parent.getComponent());
    });
}

/** "" if the corrected files can be used, else the sentence(s) that say what is wrong. */
static juce::String checkWaitingFiles (const WaitingPiece& wp, const std::vector<int>& channels, double rate)
{
    juce::String problems;
    for (size_t i = 0; i < wp.files.size(); ++i)
    {
        const auto p = audioops::checkReplacement (formats(), wp.files[i], wp.to - wp.from, i < channels.size() ? channels[i] : 1, rate);
        if (p.isNotEmpty()) problems << p << "\n";
    }
    return problems;
}

void relinkEdit (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid, juce::Component*)
{
    const juce::String title = "Re-link corrected audio";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const int i = e->indexOf (rid); if (i < 0) return;
    const auto& r = e->regions[(size_t) i];
    if (! r.waiting.active()) return;
    // which file of the piece each exported file belongs to
    std::vector<size_t> where;
    for (size_t k = 0; k < r.waiting.files.size(); ++k)
    {
        size_t fi = r.files.size();
        if (r.waiting.tracks.empty()) fi = k;
        else for (size_t q = 0; q < r.files.size(); ++q) if (r.files[q].trackId == r.waiting.tracks[k]) fi = q;
        where.push_back (fi);
    }
    bool fits = r.srcIn >= r.waiting.from && r.srcOut <= r.waiting.to && (! r.waiting.tracks.empty() || r.waiting.files.size() == r.files.size());          // (an OUT / IN stream file reaches past the piece, into its handle)
    for (auto fi : where) if (fi >= r.files.size()) fits = false;
    if (! fits) { say (title, "This piece was changed after it was sent out, so the corrected files no longer fit it. Use 'Stop waiting: keep the original audio'."); return; }
    std::vector<int> ch; for (auto fi : where) ch.push_back (r.files[fi].numChannels);
    const auto problems = checkWaitingFiles (r.waiting, ch, r.sampleRate);
    if (problems.isNotEmpty()) { say (title, problems + "\nNothing was changed. Fix the files in your other software (same length, sample rate and channels) and try again."); return; }
    const auto old = e->regions;
    auto& m = e->regions[(size_t) i];
    for (size_t k = 0; k < where.size(); ++k) { m.files[where[k]].file = m.waiting.files[k]; m.files[where[k]].fileStart = m.waiting.from; }
    m.takeName = m.takeName.upToFirstOccurrenceOf (" (", false, false) + " (" + m.waiting.name + ")";
    const auto wf = m.waiting.from, wt = m.waiting.to;
    m.waiting = WaitingPiece();
    e->keepFixFadesInside (i, wf, wt);          // the crossfade must not start before the corrected audio does
    setUndoForEdit (app, eid, old, "re-link corrected audio");
    app.project.changed();
}

void relinkTake (AppContext& app, const juce::Uuid& wid, const juce::Uuid& gid, juce::int64 waitingFrom, juce::Component* parent)
{
    const juce::String title = "Re-link corrected audio";
    auto* w = takeWin (app, wid); auto* g = w ? w->findGroup (gid) : nullptr; if (g == nullptr) return;
    const WaitingPiece* wp = nullptr; for (auto& x : g->waiting) if (x.active() && x.from == waitingFrom) wp = &x;
    if (wp == nullptr) return;
    std::vector<size_t> where;                                   // which file of the take each exported file belongs to
    for (size_t k = 0; k < wp->files.size(); ++k)
    {
        size_t fi = g->files.size();
        if (wp->tracks.empty()) fi = k;
        else for (size_t q = 0; q < g->files.size(); ++q) if (g->files[q].trackId == wp->tracks[k]) fi = q;
        where.push_back (fi);
    }
    bool fits = wp->tracks.empty() ? wp->files.size() == g->files.size() : true;
    for (auto fi : where) if (fi >= g->files.size()) fits = false;
    if (! fits) { say (title, "The take has changed since this was sent out. Use 'Stop waiting: keep the original audio'."); return; }
    std::vector<int> ch; for (auto fi : where) ch.push_back (g->files[fi].numChannels);
    const auto problems = checkWaitingFiles (*wp, ch, g->sampleRate);
    if (problems.isNotEmpty()) { say (title, problems + "\nNothing was changed. Fix the files in your other software (same length, sample rate and channels) and try again."); return; }
    struct Splice { std::vector<juce::File> src, repl, dest; std::vector<size_t> where; juce::int64 from, to; juce::String name; };
    auto sp = std::make_shared<Splice>();
    sp->from = wp->from; sp->to = wp->to; sp->name = wp->name; sp->repl = wp->files; sp->where = where;
    for (auto fi : where)
    {
        auto& f = g->files[fi];
        sp->src.push_back (f.file);
        auto d = audioops::uniqueFile (f.file.getSiblingFile (f.file.getFileNameWithoutExtension() + " (" + sanitiseForFile (wp->name) + ").wav"));
        for (auto& o : sp->dest) if (o == d) d = d.getSiblingFile (d.getFileNameWithoutExtension() + " b.wav");
        sp->dest.push_back (d);
    }
    auto work = [sp] (AudioJob& job) -> juce::String
    {
        for (size_t i = 0; i < sp->src.size(); ++i)
        {
            if (job.cancelled()) { for (auto& d : sp->dest) d.deleteFile(); return "Cancelled."; }
            job.setProgress ((float) i / (float) juce::jmax ((size_t) 1, sp->src.size()));
            juce::String err;
            if (! audioops::spliceFile (formats(), sp->src[i], sp->repl[i], sp->from, sp->to, sp->dest[i], 0.005, err)) { for (auto& d : sp->dest) d.deleteFile(); return err; }
        }
        return {};
    };
    auto onSuccess = [&app, wid, gid, sp]
    {
        auto* w2 = app.project.findTakeWindow (wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        bool okIdx = g2 != nullptr; if (okIdx) for (auto fi : sp->where) if (fi >= g2->files.size()) okIdx = false;
        if (! okIdx) { for (auto& d : sp->dest) d.deleteFile(); return; }
        const auto old = g2->files;
        for (size_t k = 0; k < sp->where.size(); ++k) g2->files[sp->where[k]].file = sp->dest[k];
        g2->waiting.erase (std::remove_if (g2->waiting.begin(), g2->waiting.end(), [sp] (const WaitingPiece& x) { return x.from == sp->from; }), g2->waiting.end());
        setUndoForTake (app, wid, gid, old, "re-link corrected audio");
        app.project.changed();
    };
    launchDialog (std::make_unique<SimpleJobDialog> ("Putting the corrected audio into the take...", work, onSuccess, nullptr), title, parent);
}

// ======================================================================================================== Project > Import AAF as edit windows
struct AafFound { juce::File file; int channels = 1; juce::int64 length = 0; double rate = 0.0; bool hasTrack = false; juce::Uuid trackId; juce::String trackName; };
struct AafCand { juce::String name; juce::File file; bool hasTrack = false; juce::Uuid trackId; juce::String trackName; };

class AafImportDialog : public JobDialogBase
{
public:
    AafImportDialog (AppContext& a, const juce::File& f, AafResult first) : app (a), aafFile (f), res (std::move (first))
    {
        setSize (700, 560);
        for (auto* l : { &fileLabel, &gapCap, &folderLabel }) { addAndMakeVisible (l); l->setColour (juce::Label::textColourId, theme::text); }
        fileLabel.setText ("AAF:  " + f.getFullPathName(), juce::dontSendNotification);
        gapCap.setText ("A new piece starts after this many seconds of silence:", juce::dontSendNotification);
        gap.setText ("10", juce::dontSendNotification); gap.setInputRestrictions (6, "0123456789.");
        gap.setTooltip ("Nothing playing on any track for longer than this = the next piece (its own edit window). Press Enter to apply.");
        gap.onReturnKey = [this] { reread(); gap.giveAwayKeyboardFocus(); }; gap.onFocusLost = [this] { reread(); };
        addAndMakeVisible (gap);
        folderLabel.setText ("Audio is found in the takes you imported into Fermata. Not there?  Also look in a folder:  (none)", juce::dontSendNotification);
        folderButton.onClick = [this]
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Choose the folder with the session's audio files (e.g. the Pyramix audio folder)");
            juce::Component::SafePointer<AafImportDialog> self (this);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [chooser, self] (const juce::FileChooser& fc)
            {
                if (self == nullptr || fc.getResult() == juce::File()) return;
                self->extraFolder = fc.getResult();
                self->folderLabel.setText ("Audio is found in the takes you imported into Fermata, and in the folder:  " + self->extraFolder.getFullPathName() + "  (used where it is, not copied)", juce::dontSendNotification);
            });
        };
        addAndMakeVisible (folderButton);
        addAndMakeVisible (summary); addAndMakeVisible (doIt); addAndMakeVisible (cancel);
        summary.setMultiLine (true); summary.setReadOnly (true); summary.setCaretVisible (false); summary.setScrollbarsShown (true);
        summary.setColour (juce::TextEditor::backgroundColourId, theme::field); summary.setFont (juce::FontOptions (13.0f));
        doIt.setColour (juce::TextButton::buttonColourId, theme::accent);
        doIt.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        refreshSummary();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        fileLabel.setBounds (r.removeFromTop (22)); r.removeFromTop (4);
        { auto x = r.removeFromTop (26); gapCap.setBounds (x.removeFromLeft (360)); gap.setBounds (x.removeFromLeft (60)); } r.removeFromTop (6);
        { auto x = r.removeFromTop (26); folderButton.setBounds (x.removeFromLeft (150)); } folderLabel.setBounds (r.removeFromTop (40)); r.removeFromTop (4);
        auto bottom = r.removeFromBottom (30);
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); doIt.setBounds (bottom.removeFromRight (230));
        r.removeFromBottom (6); bar.setBounds (r.removeFromBottom (22)); status.setBounds (r.removeFromBottom (22));
        summary.setBounds (r);
    }
private:
    void reread()
    {
        if (running) return;
        const double g = juce::jlimit (0.5, 3600.0, gap.getText().getDoubleValue() > 0.0 ? gap.getText().getDoubleValue() : 10.0);
        if (std::abs (g - lastGap) < 1.0e-9) return;
        lastGap = g;
        auto r = readAaf (aafFile, g);
        if (r.error.isNotEmpty()) { status.setText (r.error, juce::dontSendNotification); return; }
        res = std::move (r); status.setText ({}, juce::dontSendNotification); refreshSummary();
    }
    void refreshSummary()
    {
        juce::String t;
        t << juce::String ((int) res.pieces.size()) << " piece(s) found on the AAF's timeline, " << juce::String ((int) res.trackNames.size()) << " track(s), "
          << juce::String (res.clips) << " clips, " << juce::String (res.sampleRate, 0) << " Hz.\n\n";
        for (size_t i = 0; i < res.pieces.size(); ++i)
        {
            const auto& pc = res.pieces[i];
            t << juce::String ((int) i + 1) << ".  " << pc.name << "   -   " << juce::String (pc.regions.size()) << " pieces of takes, " << juce::String (pc.lengthSeconds / 60.0, 1) << " min, at "
              << formatTimeShort (pc.startSeconds, 2) << " in the AAF\n";
        }
        t << "\nTracks:  " << res.trackNames.size() << (res.trackNames.empty() ? juce::String() : juce::String ("  (") + res.trackNames.front() + " ... " + res.trackNames.back() + ")") << "\n";
        if (res.notes.size() > 0) { t << "\nNotes:\n"; for (auto& n : res.notes) t << "   " << n << "\n"; }
        t << "\nEach piece becomes its own edit window, with the takes laid out where they sat in the AAF, the track names kept and the cross-fades kept. "
             "The audio files are NOT copied: the edits use the files you already imported (matched by their original names), or the ones in the extra folder.";
        summary.setText (t, false);
        doIt.setButtonText ("Make " + juce::String ((int) res.pieces.size()) + " edit window(s)");
        doIt.setEnabled (! res.pieces.empty());
    }
    void start()
    {
        reread();
        if (res.pieces.empty()) return;
        doIt.setEnabled (false); status.setText ("Finding the audio files...", juce::dontSendNotification);
        std::vector<AafCand> cands;
        for (auto& w : app.project.takeWindows)
            for (auto& g : w->groups)
                for (auto& f : g.files)
                {
                    for (auto& nm : { f.sourceName, f.sourceName2, f.file.getFileName() })
                        if (nm.isNotEmpty()) cands.push_back ({ nm.toLowerCase(), f.file, true, f.trackId, f.trackName });      // (a joined stereo file answers to both of its mono names)
                }
        auto resCopy = std::make_shared<AafResult> (res);
        auto found = std::make_shared<std::map<juce::String, AafFound>>();
        auto missing = std::make_shared<juce::StringArray>();
        const auto extra = extraFolder;
        juce::Component::SafePointer<AafImportDialog> safe (this);
        auto& appRef = app;
        runJob ([cands, resCopy, found, missing, extra] (AudioJob& j) -> juce::String
                {
                    std::map<juce::String, AafCand> index;
                    for (auto& c : cands) index.emplace (c.name, c);                          // Fermata's own files first
                    if (extra.isDirectory())
                        for (auto& f : extra.findChildFiles (juce::File::findFiles, true, "*"))
                        {
                            if (j.cancelled()) return "Cancelled.";
                            if (isAudioFile (f)) index.emplace (f.getFileName().toLowerCase(), AafCand { f.getFileName().toLowerCase(), f, false, {}, {} });
                        }
                    std::set<juce::String> needed; std::map<juce::String, juce::String> shown;
                    for (auto& pc : resCopy->pieces) for (auto& r : pc.regions) for (auto& f : r.files) { needed.insert (f.fileName.toLowerCase()); shown[f.fileName.toLowerCase()] = f.fileName; }
                    size_t n = 0;
                    for (auto& name : needed)
                    {
                        if (j.cancelled()) return "Cancelled.";
                        j.setProgress ((float) ++n / (float) needed.size());
                        auto it = index.find (name);
                        if (it == index.end() || ! it->second.file.existsAsFile()) { missing->add (shown[name]); continue; }
                        std::unique_ptr<juce::AudioFormatReader> rd (formats().createReaderFor (it->second.file));
                        if (rd == nullptr) { missing->add (shown[name] + " (cannot be read)"); continue; }
                        AafFound af; af.file = it->second.file; af.channels = (int) rd->numChannels; af.length = rd->lengthInSamples; af.rate = rd->sampleRate;
                        af.hasTrack = it->second.hasTrack; af.trackId = it->second.trackId; af.trackName = it->second.trackName;
                        (*found)[name] = af;
                    }
                    return {};
                },
                [safe, resCopy, found, missing, &appRef] (const juce::String& err)
                {
                    if (safe == nullptr) return;
                    if (err.isNotEmpty()) { safe->status.setText (err, juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                    const auto msg = build (appRef, *resCopy, *found, *missing);
                    closeDialogOf (safe.getComponent());
                    say ("Import AAF", msg);
                });
    }

    /** Message thread: makes the project tracks and one edit per piece. Returns the text to tell the person. */
    static juce::String build (AppContext& app, const AafResult& res, const std::map<juce::String, AafFound>& found, const juce::StringArray& missing)
    {
        auto& p = app.project;
        std::set<int> usedInputs; for (auto& t : p.tracks) for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) >= 0) usedInputs.insert (t.inputOf (c));
        int newTracks = 0;
        std::map<juce::String, juce::Uuid> madeTrack;                                       // tracks made here for files that are not in a take window (by AAF track name)
        // a file that is already in a take window keeps that window's track (a stereo track made from Omni-L and Omni-R is one track); any other file gets a track of its own name
        auto trackFor = [&] (const AafRegionFile& af, const AafFound& fd, juce::Uuid& id, juce::String& name) -> bool
        {
            if (fd.hasTrack && p.findTrack (fd.trackId) != nullptr) { id = fd.trackId; name = p.findTrack (fd.trackId)->name; return true; }
            if (madeTrack.count (af.trackName)) { id = madeTrack[af.trackName]; name = af.trackName; return true; }
            const int ch = juce::jlimit (1, 2, fd.channels);
            const TrackDef* tf = nullptr;
            for (auto& t : p.tracks) if (t.name.equalsIgnoreCase (af.trackName) && t.channelCount() == ch) { tf = &t; break; }
            if (tf == nullptr)
            {
                int first = -1;
                for (int i = 0; i + ch <= (int) p.inputs.size() && first < 0; ++i)
                {
                    bool freeSlot = true; for (int c = 0; c < ch; ++c) freeSlot = freeSlot && ! usedInputs.count (i + c);
                    if (freeSlot) first = i;
                }
                auto& nt = p.addTrack (af.trackName, ch == 2 ? TrackFormat::Stereo : TrackFormat::Mono, first);
                if (first < 0) nt.inputs.fill (-1); else for (int c = 0; c < ch; ++c) usedInputs.insert (first + c);
                tf = &nt; ++newTracks;
            }
            madeTrack[af.trackName] = tf->id; id = tf->id; name = tf->name;
            return true;
        };
        std::map<juce::String, juce::Uuid> aafTrackId;
        int edits = 0, regionsMade = 0, regionsLost = 0, rateSkipped = 0, shortened = 0; juce::Uuid firstEdit;
        for (auto& pc : res.pieces)
        {
            std::vector<EditRegion> regs;
            for (auto& ar : pc.regions)
            {
                EditRegion r; r.id = juce::Uuid(); r.takeName = ar.takeName; r.sampleRate = res.sampleRate;
                r.srcIn = ar.srcIn; r.srcOut = ar.srcOut; r.startSample = ar.startSample;
                r.inStart = ar.inStart; r.inEnd = ar.inEnd; r.outStart = ar.outStart; r.outEnd = ar.outEnd;
                juce::int64 minLen = (std::numeric_limits<juce::int64>::max)();
                std::set<juce::String> seenFiles;
                for (auto& af : ar.files)
                {
                    auto it = found.find (af.fileName.toLowerCase());
                    if (it == found.end()) continue;
                    if (std::abs (it->second.rate - res.sampleRate) > 0.5) { ++rateSkipped; continue; }
                    juce::Uuid tid; juce::String tname;
                    trackFor (af, it->second, tid, tname);
                    aafTrackId[af.trackName] = tid;
                    if (! seenFiles.insert (it->second.file.getFullPathName()).second) continue;          // the left and right halves of one stereo file: one entry
                    RegionFile rf; rf.trackId = tid; rf.trackName = tname; rf.file = it->second.file; rf.numChannels = juce::jlimit (1, 2, it->second.channels);
                    r.files.push_back (rf); minLen = juce::jmin (minLen, it->second.length);
                }
                if (r.files.empty()) { ++regionsLost; continue; }
                r.sourceLength = minLen;
                if (r.srcOut > minLen) { r.srcOut = minLen; ++shortened; }
                if (r.srcIn < 0 || r.srcIn >= r.srcOut) { ++regionsLost; continue; }
                regs.push_back (std::move (r));
            }
            if (regs.empty()) continue;
            auto& e = p.addEdit (pc.name, juce::Uuid::null());             // (from no take window: null, not a made-up id)
            e.sampleRate = res.sampleRate;
            for (auto& tn : pc.trackNames)
            {
                auto ti = aafTrackId.find (tn);
                if (ti != aafTrackId.end() && std::find (e.trackIds.begin(), e.trackIds.end(), ti->second) == e.trackIds.end()) e.trackIds.push_back (ti->second);
            }
            e.regions = std::move (regs); regionsMade += (int) e.regions.size();
            e.insertIndex = -1; e.hardenEnds(); e.clampAll();
            if (edits++ == 0) firstEdit = e.id;
        }
        p.structureChanged(); p.changed();
        if (! firstEdit.isNull() && app.showEdit) app.showEdit (firstEdit);
        juce::String t;
        if (edits == 0) t << "No edit windows were made: none of the audio files named in the AAF could be found.\n\n";
        else t << juce::String (edits) << " edit window(s) made, with " << juce::String (regionsMade) << " pieces of takes in all" << (newTracks > 0 ? ", and " + juce::String (newTracks) + " new track(s)" : juce::String())
               << ". The first is open; the others are in the Edits menu.\n\n";
        if (! missing.isEmpty())
        {
            t << juce::String (missing.size()) << " audio file(s) named in the AAF were not found (import those takes first, or choose the folder they are in):\n";
            for (int i = 0; i < juce::jmin (8, missing.size()); ++i) t << "   " << missing[i] << "\n";
            if (missing.size() > 8) t << "   ...\n";
            t << "\n";
        }
        if (regionsLost > 0) t << juce::String (regionsLost) << " piece(s) were left out because none of their files were found.\n";
        if (rateSkipped > 0) t << juce::String (rateSkipped) << " file(s) were left out because their sample rate is not the AAF's (" << juce::String (res.sampleRate, 0) << " Hz).\n";
        if (shortened > 0) t << juce::String (shortened) << " piece(s) were shortened to the length of their file.\n";
        return t.trimEnd();
    }

    AppContext& app; juce::File aafFile; AafResult res; juce::File extraFolder; double lastGap = 10.0;
    juce::Label fileLabel, gapCap, folderLabel; juce::TextEditor gap, summary;
    juce::TextButton folderButton { "Also look in a folder..." }, doIt { "Make edit windows" }, cancel { "Cancel" };
};

void importAaf (AppContext& app, juce::Component* parent)
{
    if (heldByRec (app, "Importing an AAF", parent, [ap = &app] (juce::Component* p) { importAaf (*ap, p); })) return;
    auto chooser = std::make_shared<juce::FileChooser> ("Choose the AAF file", juce::File(), "*.aaf");
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [chooser, &app, safeParent] (const juce::FileChooser& fc)
    {
        if (fc.getResult() == juce::File()) return;
        auto r = readAaf (fc.getResult(), 10.0);
        if (r.error.isNotEmpty()) { say ("Import AAF", r.error); return; }
        launchDialog (std::make_unique<AafImportDialog> (app, fc.getResult(), std::move (r)), "Import AAF as edit windows", safeParent.getComponent(), true);
    });
}

}} // namespace td::fixtools
