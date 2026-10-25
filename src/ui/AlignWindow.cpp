#include "AlignWindow.h"
#include "FermataLookAndFeel.h"
#include "../core/AudioOps.h"
#include "../core/AudioFormats.h"

namespace td
{
namespace
{
juce::String panText (float p)
{
    if (std::abs (p) < 0.02f) return "C";
    return juce::String (p < 0.0f ? "L " : "R ") + juce::String (juce::roundToInt (std::abs (p) * 100.0f));
}
juce::String msText (double ms) { return juce::String (ms, 2) + " ms"; }
juce::String metres (double ms) { return juce::String (std::abs (ms) * 0.343, 1) + " m"; }
}

// ======================================================================================================================== the analysis, off the message thread
struct AlignComponent::Job : public juce::Thread
{
    Job (Source s, std::vector<timealign::TrackIn> t, std::vector<juce::Uuid> i)
        : juce::Thread ("Align spot mics"), src (std::move (s)), tins (std::move (t)), ids (std::move (i)) { td::registerAudioFormats (formats); }
    ~Job() override { stopThread (4000); }

    void run() override
    {
        auto reader = [this] (int t, int c, juce::int64 start, int n, float* dst) -> bool
        {
            std::fill (dst, dst + n, 0.0f);
            const auto id = ids[(size_t) t].toString();
            if (! src.isEdit)
            {
                auto it = src.takeFiles.find (id);
                if (it == src.takeFiles.end()) return true;                      // not recorded in this take: silence
                return readFile (it->second.file, it->second.offset + start, n, c, dst);
            }
            // an edit: piece by piece (the topmost piece wherever two overlap), ignoring fades and level changes (timing is all that matters here)
            const juce::int64 a = src.from + start, b = a + n;
            std::vector<juce::int64> cuts { a, b };
            for (auto& r : src.regions) { if (r.startSample > a && r.startSample < b) cuts.push_back (r.startSample); if (r.endSample() > a && r.endSample() < b) cuts.push_back (r.endSample()); }
            std::sort (cuts.begin(), cuts.end());
            std::vector<float> tmp;
            for (size_t k = 0; k + 1 < cuts.size(); ++k)
            {
                const auto t0 = cuts[k], t1 = cuts[k + 1];
                if (t1 <= t0) continue;
                const EditRegion* hit = nullptr;
                for (auto& r : src.regions) if (t0 >= r.startSample && t0 < r.endSample()) hit = &r;
                if (hit == nullptr) continue;
                for (auto& f : hit->files)
                {
                    if (f.trackId.toString() != id) continue;
                    tmp.assign ((size_t) (t1 - t0), 0.0f);
                    if (! readFile (f.file, t0 + (hit->srcIn - hit->startSample) - f.fileStart, (int) (t1 - t0), c, tmp.data())) return false;
                    std::copy (tmp.begin(), tmp.end(), dst + (t0 - a));
                    break;
                }
            }
            return true;
        };
        try { result = timealign::analyse (tins, src.length, src.sampleRate, reader, [this] (float p) { progress.store (p); return ! threadShouldExit(); }); }
        catch (const std::bad_alloc&) { result = {}; result.error = "Not enough memory to measure this. Try a shorter take or fewer tracks."; }
        catch (const std::exception& e) { result = {}; result.error = juce::String ("The measurement failed: ") + e.what(); }
        done.store (true);
    }

    bool readFile (const juce::File& f, juce::int64 start, int n, int c, float* dst)
    {
        auto& r = readers[f.getFullPathName()];
        if (r == nullptr) r = audioops::openReader (formats, f);
        if (r == nullptr) return true;                                           // a missing file reads as silence (and the track says "silent")
        std::vector<std::vector<float>> chans;
        if (! audioops::readRange (*r, start, n, chans)) return false;
        if (c < (int) chans.size()) std::copy (chans[(size_t) c].begin(), chans[(size_t) c].end(), dst);
        return true;
    }

    Source src;
    std::vector<timealign::TrackIn> tins;
    std::vector<juce::Uuid> ids;
    juce::AudioFormatManager formats;
    std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> readers;
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> done { false };
    timealign::Analysis result;
    juce::String measuredOnText;
};

// ======================================================================================================================== one track's row
struct AlignComponent::Row : public juce::Component
{
    Row (AlignComponent& o, const juce::Uuid& t) : owner (o), trackId (t)
    {
        name.setFont (juce::FontOptions (13.0f, juce::Font::bold)); name.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (name);
        for (int r = 0; r < timealign::NumRoles; ++r) role.addItem (timealign::roleName (r), r + 1);
        role.setTooltip ("Main: the main pair(s) - every other mic is measured against them. Spot: delayed to line up with the main pair. "
                         "Distance: room, hall and ambience mics - measured and checked, never delayed. Not used: left alone (talkback, LTC ...). "
                         "The role is the same in every piece.");
        role.onChange = [this]
        {
            auto* tk = track();
            if (tk == nullptr) return;
            const int r = juce::jlimit (0, timealign::NumRoles - 1, role.getSelectedId() - 1);
            if (r == tk->alignRole) return;
            tk->alignRole = r;
            if (r != timealign::Spot)                                          // only spots are delayed (in this piece; the next Analyse does the others)
                if (auto* at = owner.editableSet().find (trackId)) at->on = false;
            owner.tracksChanged();
        };
        addAndMakeVisible (role);
        result.setFont (juce::FontOptions (12.0f)); result.setJustificationType (juce::Justification::centredLeft); result.setMinimumHorizontalScale (0.75f);
        addAndMakeVisible (result);
        delay.setJustification (juce::Justification::centredRight);
        delay.setInputRestrictions (7, "0123456789.");
        delay.setTooltip ("The delay (milliseconds) this spot is given in this piece, in every mixer. Analyse sets it; you can type another.");
        auto commit = [this]
        {
            const double v = juce::jlimit (0.0, 200.0, delay.getText().getDoubleValue());
            const auto* shown = owner.shownSet();
            const auto* cur = shown != nullptr ? shown->find (trackId) : nullptr;
            if (cur != nullptr && std::abs (v - cur->ms) < 1.0e-6) return;
            auto& at = owner.editableSet().tracks[trackId.toString()];
            at.ms = v; at.on = v > 0.0;
            owner.tracksChanged();
        };
        delay.onReturnKey = commit; delay.onFocusLost = commit;
        addAndMakeVisible (delay);
        samples.setFont (juce::FontOptions (11.0f)); samples.setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (samples);
        on.setButtonText ("On");
        on.setTooltip ("This spot's delay on or off in this piece (Alignment at the bottom switches every delay of every piece)");
        on.onClick = [this] { owner.editableSet().tracks[trackId.toString()].on = on.getToggleState(); owner.tracksChanged(); };
        addAndMakeVisible (on);
        pan.setFont (juce::FontOptions (12.0f)); pan.setColour (juce::Label::textColourId, theme::text); pan.setJustificationType (juce::Justification::centredRight);
        pan.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (pan);
        usePan.setButtonText ("Use");
        usePan.setTooltip ("Pan this spot to where its instrument sits in the main pair's image (in the processing mixer, or the edit's own mixer); its delay follows the new pan");
        usePan.onClick = [this] { owner.useSuggestedPan (trackId); };
        addAndMakeVisible (usePan);
        for (auto* c : std::initializer_list<juce::Component*> { &role, &delay, &on, &usePan }) c->setWantsKeyboardFocus (c == &delay);
    }
    TrackDef* track() const { return owner.app.project.findTrack (trackId); }

    void refresh (double sr)
    {
        auto* t = track();
        if (t == nullptr) return;
        name.setText (t->name, juce::dontSendNotification);
        role.setSelectedId (t->alignRole + 1, juce::dontSendNotification);
        const auto* set = owner.shownSet();
        const auto* at = set != nullptr ? set->find (trackId) : nullptr;
        const AlignResult ar = at != nullptr ? at->result : AlignResult();
        juce::String text, tip; juce::Colour col = theme::dimText;
        const bool spot = t->alignRole == timealign::Spot, dist = t->alignRole == timealign::Distance;
        if (ar.measured && spot)
        {
            const double lead = ar.hasLeft && ar.hasRight ? 0.5 * (ar.leftMs + ar.rightMs) : ar.hasLeft ? ar.leftMs : ar.rightMs;
            const bool later = lead < 0.0;                                        // (a "spot" farther away than the main pair: flagged below)
            text << msText (std::abs (lead)) << (later ? " later than the main pair" : " earlier than the main pair");
            if (ar.hasLeft && ar.hasRight) text << " (L " << juce::String (std::abs (ar.leftMs), 2) << " / R " << juce::String (std::abs (ar.rightMs), 2) << ")";
            text << "  -  " << timealign::clarityName (ar.clarity);
            tip << "About " << metres (lead) << (later ? " further from its instrument than the main pair" : " nearer its instrument than the main pair")
                << (ar.hasLeft && ar.hasRight ? " (L, R: how much earlier than the left and the right main mics)" : "") << ".";
            col = theme::text;
        }
        else if (ar.measured && dist)
        {
            text << msText (ar.arrivalMs) << " after the main pair (about " << metres (ar.arrivalMs) << " further)";
            if (ar.spreadMs > 0.05) text << ", its two channels " << juce::String (ar.spreadMs, 1) << " ms apart";
            text << "  -  " << timealign::clarityName (ar.clarity) << "  -  not delayed";
            col = theme::text;
        }
        else if (spot || dist) text = set != nullptr && set->measured() ? "Not measured in this piece (its role was different): press Analyse" : "Not measured yet: press Analyse";
        if (ar.message.isNotEmpty()) text << (text.isEmpty() ? juce::String() : juce::String ("  -  ")) << ar.message;
        if (ar.severity >= 2) col = juce::Colour (0xffff6b5e); else if (ar.severity == 1) col = theme::warn;
        if (t->alignRole == timealign::NotUsed) text = {};
        result.setText (text, juce::dontSendNotification); result.setColour (juce::Label::textColourId, col);
        result.setTooltip (tip.isNotEmpty() ? text + "\n" + tip : text);
        delay.setVisible (spot); samples.setVisible (spot); on.setVisible (spot);
        if (spot)
        {
            const double ms = at != nullptr ? at->ms : 0.0;
            if (! delay.hasKeyboardFocus (true)) delay.setText (juce::String (ms, 2), juce::dontSendNotification);
            samples.setText ("ms  = " + juce::String (juce::roundToInt (ms * sr / 1000.0)) + " smp", juce::dontSendNotification);
            on.setToggleState (at != nullptr && at->on, juce::dontSendNotification);
        }
        const bool showPan = spot && ar.measured && ar.hasLeft && ar.hasRight;
        pan.setVisible (showPan); usePan.setVisible (showPan);
        if (showPan)
        {
            juce::String now, sugg;
            const bool pair = ar.suggestedStereo && t->channelCount() == 2;
            if (auto* st = owner.stripOf (trackId))
                now = pair ? panText (st->panL.get()) + " / " + panText (st->panR.get())
                           : t->channelCount() >= 2 ? panText (0.5f * (st->panL.get() + st->panR.get())) : panText (st->pan.get());
            sugg = pair ? panText (ar.suggestedPan) + " / " + panText (ar.suggestedPanR) : panText (ar.suggestedPan);
            pan.setText (sugg + (now.isNotEmpty() ? "  (now " + now + ")" : juce::String()), juce::dontSendNotification);
            pan.setTooltip (pair ? "Where the sound of each of the pair's two channels sits in the main pair's image (left channel / right channel), and their pans now"
                                 : "Where its instrument sits in the main pair's image, and its pan now");
        }
    }
    // the columns (the window's headings use them too)
    static constexpr int kName = 150, kRole = 104, kUse = 46, kPan = 210, kOn = 50, kSmp = 90, kDelay = 60;
    static constexpr int kRight = kUse + 4 + kPan + 10 + kOn + 4 + kSmp + kDelay;
    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 4);
        name.setBounds (r.removeFromLeft (kName)); r.removeFromLeft (4);
        role.setBounds (r.removeFromLeft (kRole)); r.removeFromLeft (10);
        auto right = r.removeFromRight (kRight);
        usePan.setBounds (right.removeFromRight (kUse)); right.removeFromRight (4);
        pan.setBounds (right.removeFromRight (kPan)); right.removeFromRight (10);
        on.setBounds (right.removeFromRight (kOn)); right.removeFromRight (4);
        samples.setBounds (right.removeFromRight (kSmp));
        delay.setBounds (right.removeFromRight (kDelay));
        result.setBounds (r);
    }
    void paint (juce::Graphics& g) override { g.setColour (theme::border.withAlpha (0.6f)); g.fillRect (0, getHeight() - 1, getWidth(), 1); }

    AlignComponent& owner;
    juce::Uuid trackId;
    juce::Label name, result, samples, pan;
    juce::ComboBox role;
    juce::TextEditor delay;
    juce::ToggleButton on;
    juce::TextButton usePan;
};

// ======================================================================================================================== the window
AlignComponent::AlignComponent (AppContext& a, bool edit, const juce::Uuid& id) : app (a), isEdit (edit), sourceId (id)
{
    for (auto* l : { &sourceLabel, &pieceLabel, &measuredLabel, &mainNote, &status, &guessNote })
    {
        l->setFont (juce::FontOptions (12.5f)); l->setColour (juce::Label::textColourId, theme::dimText); l->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (l);
    }
    pieceLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold)); pieceLabel.setColour (juce::Label::textColourId, theme::text);
    mainNote.setColour (juce::Label::textColourId, theme::warn); mainNote.setJustificationType (juce::Justification::topLeft);
    guessNote.setColour (juce::Label::textColourId, theme::warn);
    analyseButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    analyseButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    analyseButton.setTooltip ("Measure every Spot and Distance track against the Main tracks in this take (or edit): the whole of it, or the part between marks 1 and 2");
    analyseButton.onClick = [this] { analyse(); };
    stopButton.onClick = [this] { stopJob(); status.setText ("Stopped.", juce::dontSendNotification); };
    usePansButton.setTooltip ("Pan every measured spot to where its instrument sits in the main pair's image");
    usePansButton.onClick = [this]
    {
        std::vector<juce::Uuid> ids;
        if (const auto* set = shownSet()) for (auto& [tid, at] : set->tracks) if (at.result.measured && at.result.hasLeft && at.result.hasRight) ids.push_back (juce::Uuid (tid));
        for (auto& tid : ids) if (auto* t = app.project.findTrack (tid); t != nullptr && t->alignRole == timealign::Spot) useSuggestedPan (tid);
    };
    recalcButton.setTooltip ("A spot is lined up with the main mic on the side it is panned to: after you change a spot's pan, this gives every spot of this piece the delay for its present pan (no new measurement)");
    recalcButton.onClick = [this] { recalcForPans(); };
    clearButton.setTooltip ("No spot is delayed in this piece any more (the measurements are kept)");
    clearButton.onClick = [this] { for (auto& kv : editableSet().tracks) { kv.second.on = false; kv.second.ms = 0.0; } tracksChanged(); };
    guessButton.setTooltip ("Main for ORTF / omni / spaced / Decca / outrigger tracks, Distance for hall / room / ambience / surround, Not used for LTC / talkback / conductor, Spot for the rest");
    guessButton.onClick = [this]
    {
        for (auto& t : app.project.tracks) t.alignRole = t.isLtc ? (int) timealign::NotUsed : timealign::guessRole (t.name);
        guessNote.setText ("Roles guessed from the track names: check them.", juce::dontSendNotification);
        tracksChanged();
    };
    copyButton.setTooltip ("This piece uses the same set-up as another one: take its measurements and delays instead of measuring again");
    copyButton.onClick = [this] { showCopyMenu(); };
    abButton.setClickingTogglesState (false);
    abButton.setTooltip ("Alignment on or off for every track of every piece at once: switch it while listening to hear the difference");
    abButton.onClick = [this] { app.project.alignEnabled = ! app.project.alignEnabled; tracksChanged(); };
    for (auto* b : { &analyseButton, &stopButton, &usePansButton, &recalcButton, &clearButton, &guessButton, &copyButton, &abButton }) { b->setWantsKeyboardFocus (false); addAndMakeVisible (b); }
    addAndMakeVisible (bar);
    bar.setVisible (false); stopButton.setVisible (false);
    viewport.setViewedComponent (&rowsHolder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    info.setText ("Spot mics are nearer their instruments than the main pair, so they hear every note a few milliseconds earlier: mixed in as they are, the "
                  "instrument is smeared and coloured (comb filtering). Each spot is delayed by exactly its lead, in every mixer; the recorded files are never changed.\n\n"
                  "1. Give every track its role: Main (one or two main pairs, used together), Spot, Distance (room, hall, ambience: checked, never delayed) or "
                  "Not used. Guess the roles from the names sets them from the track names. The roles are the same in every piece.\n"
                  "2. Record a run-through (the one for levels is ideal), then press Analyse: Fermata measures the whole take, or just the part between marks 1 and 2. "
                  "No distances are needed: it measures the time of arrival itself (which also catches a device with more latency than another), and for each "
                  "spot it uses the moments its instrument is playing, so nothing has to be marked per instrument.\n"
                  "3. Listen, switching Alignment on and off.\n\n"
                  "Every piece keeps its own measurements and delays (players, mics, distances and the temperature change from piece to piece): the mixers use "
                  "those of the piece you are recording, playing or working on. A piece set up like another can Copy from another piece.\n\n"
                  "Pan: a spot panned left is lined up with the left main mic, one in the centre with the middle of the pair. Fermata suggests a pan for each spot "
                  "that matches where its instrument sits in the main pair's image (Use). After changing a pan, Delays for the present pans brings the delays up to date.\n\n"
                  "Warnings (red): a spot that hears the music later than the main pair (it cannot be lined up by delaying it), a reversed polarity, a distance "
                  "mic that hears it before the main pair, a distance pair whose channels are far apart in time (patching), two main pairs that are not in line.\n\n"
                  "In post: open this from an edit window and Analyse there, any time: the edit then has its own (until then it plays with the piece most of its "
                  "audio comes from). Bounces and the Mastering window's files use each piece's own.");
    addAndMakeVisible (info);
    rebuildRows();
    startTimer (150);
    setSize (1180, 660);
}

AlignComponent::~AlignComponent() { stopJob(); }

void AlignComponent::setSource (bool edit, const juce::Uuid& id)
{
    // a measurement still running belongs to the piece it was started on: it is stopped, so its results are never saved into the piece shown now
    const bool stopped = job != nullptr && (edit != isEdit || id != sourceId);
    if (stopped) stopJob();
    isEdit = edit; sourceId = id;
    mainNote.setText ({}, juce::dontSendNotification);
    status.setText (stopped ? juce::String ("The measurement of the piece before was stopped: press Analyse to measure this one.") : juce::String(), juce::dontSendNotification);
    refreshRows();
}

juce::String AlignComponent::pieceName (const juce::String& owner) const
{
    const juce::Uuid id (owner);
    if (auto* w = app.project.findTakeWindow (id)) return "piece \"" + w->name + "\"";
    if (auto* e = app.project.findEdit (id)) return "edit \"" + e->name + "\"";
    return "a piece";
}

AlignSet& AlignComponent::editableSet()
{
    auto& sets = app.project.alignSets;
    const auto own = ownOwner();
    if (sets.find (own) == sets.end())
    {
        AlignSet fresh;
        if (const auto* now = shownSet()) fresh = *now;                         // an edit that played with its piece's set starts from it
        sets[own] = fresh;
    }
    return sets[own];
}

MixerState* AlignComponent::sourceMixer() const
{
    if (isEdit) if (auto* em = app.project.mixerOfEdit (sourceId)) return em;
    return app.project.mixers.empty() ? nullptr : app.project.mixers.front().get();
}

StripState* AlignComponent::stripOf (const juce::Uuid& trackId) const
{
    auto* m = sourceMixer();
    return m != nullptr ? m->stripFor (trackId) : nullptr;
}

AlignComponent::Source AlignComponent::makeSource() const
{
    Source s; s.isEdit = isEdit;
    auto& P = app.project;
    if (! isEdit)
    {
        auto* w = P.findTakeWindow (sourceId);
        if (w == nullptr || w->groups.empty()) { s.error = "Record a take first (the run-through for levels is ideal), then press Analyse."; s.text = "no take yet"; return s; }
        const TakeGroup* g = nullptr; double in = 0.0, out = -1.0; juce::String how;
        auto mins = [] (double sec) { return juce::String ((int) (sec / 60)) + ":" + juce::String ((int) sec % 60).paddedLeft ('0', 2); };
        if (auto* m = w->findGroup (w->editTake); m != nullptr && w->editIn >= 0.0 && w->editOut > w->editIn)
        { g = m; in = w->editIn; out = juce::jmin (w->editOut, m->lengthSeconds()); how = "between marks 1 and 2, " + juce::String (out - in, 1) + " s"; }
        else if (auto* c = w->findGroup (w->cursorTake)) { g = c; how = "the whole take, " + mins (c->lengthSeconds()); }
        else
        {
            for (auto& x : w->groups) if (g == nullptr || x.number > g->number) g = &x;
            how = "the whole take, " + mins (g->lengthSeconds()) + " (the latest)";
        }
        if (out < 0.0) out = g->lengthSeconds();
        s.sampleRate = g->sampleRate > 0.0 ? g->sampleRate : 48000.0;
        s.length = (juce::int64) std::llround ((out - in) * s.sampleRate);
        for (auto& f : g->files) s.takeFiles[f.trackId.toString()] = { f.file, (juce::int64) std::llround (in * s.sampleRate) };
        s.text = "take " + w->displayName (*g) + ", " + how;
        s.ok = s.length > 0;
        if (! s.ok) s.error = "The marked part is empty.";
        return s;
    }
    auto* e = P.findEdit (sourceId);
    if (e == nullptr || e->regions.empty()) { s.error = "The edit is empty."; s.text = "nothing in this edit"; return s; }
    s.sampleRate = e->sampleRate > 0.0 ? e->sampleRate : 48000.0;
    juce::int64 last = 0; for (auto& r : e->regions) last = juce::jmax (last, r.endSample());
    juce::int64 a = 0, b = last; juce::String how = "the whole edit";
    if (e->fixIn >= 0.0 && e->fixOut > e->fixIn) { a = (juce::int64) std::llround (e->fixIn * s.sampleRate); b = juce::jmin (last, (juce::int64) std::llround (e->fixOut * s.sampleRate)); how = "between marks 1 and 2"; }
    s.from = a; s.length = b - a; s.regions = e->regions;
    s.text = "the edit, " + how + ", " + juce::String ((double) s.length / s.sampleRate, 1) + " s";
    s.ok = s.length > 0;
    if (! s.ok) s.error = "There is no audio between the marks.";
    return s;
}

std::vector<timealign::TrackIn> AlignComponent::trackInputs (std::vector<juce::Uuid>& ids) const
{
    std::vector<timealign::TrackIn> v;
    for (auto& t : app.project.tracks)
    {
        if (t.isLtc) continue;
        timealign::TrackIn ti;
        ti.role = t.alignRole;
        ti.numChannels = t.channelCount();
        if (auto* st = stripOf (t.id))
        {
            if (ti.numChannels >= 2) { ti.pan[0] = st->panL.get(); ti.pan[1] = st->panR.get(); }
            else ti.pan[0] = st->pan.get();
        }
        else if (ti.numChannels >= 2) { ti.pan[0] = -1.0f; ti.pan[1] = 1.0f; }
        v.push_back (ti); ids.push_back (t.id);
    }
    return v;
}

void AlignComponent::analyse()
{
    if (job != nullptr) return;
    auto src = makeSource();
    if (! src.ok) { status.setText (src.error, juce::dontSendNotification); status.setColour (juce::Label::textColourId, theme::warn); return; }
    std::vector<juce::Uuid> ids;
    auto tins = trackInputs (ids);
    bool anyMain = false, anyOther = false;
    for (auto& t : tins) { anyMain = anyMain || t.role == timealign::Main; anyOther = anyOther || t.role == timealign::Spot || t.role == timealign::Distance; }
    if (! anyMain) { status.setText ("Set the main pair's tracks to Main first (or press Guess the roles from the names).", juce::dontSendNotification); status.setColour (juce::Label::textColourId, theme::warn); return; }
    if (! anyOther) { status.setText ("Set the spot mics to Spot (and any room or hall mics to Distance).", juce::dontSendNotification); status.setColour (juce::Label::textColourId, theme::warn); return; }
    const auto measuredOn = src.text + ", " + juce::Time::getCurrentTime().formatted ("%d %b %Y %H:%M");
    job = std::make_unique<Job> (std::move (src), std::move (tins), std::move (ids));
    job->measuredOnText = measuredOn;
    progress = 0.0;
    bar.setVisible (true); stopButton.setVisible (true); analyseButton.setEnabled (false);
    status.setText ("Measuring...", juce::dontSendNotification); status.setColour (juce::Label::textColourId, theme::dimText);
    job->startThread();
}

void AlignComponent::stopJob()
{
    if (job == nullptr) return;
    job->signalThreadShouldExit();
    job.reset();
    bar.setVisible (false); stopButton.setVisible (false); analyseButton.setEnabled (true);
}

void AlignComponent::applyResults (const timealign::Analysis& an, const std::vector<juce::Uuid>& ids, const juce::String& measuredOn)
{
    if (! an.ok) { status.setText (an.error, juce::dontSendNotification); status.setColour (juce::Label::textColourId, theme::warn); return; }
    AlignSet set;                                                                // this piece's new set: what was measured now
    set.measuredOn = measuredOn;
    int spots = 0, warnings = 0;
    for (size_t i = 0; i < ids.size() && i < an.tracks.size(); ++i)
    {
        auto* t = app.project.findTrack (ids[i]);
        if (t == nullptr || (t->alignRole != timealign::Spot && t->alignRole != timealign::Distance)) continue;
        const auto& o = an.tracks[i];
        AlignTrack at;
        auto& ar = at.result;
        ar.measured = o.measured; ar.severity = o.severity; ar.message = o.message;
        ar.clarity = o.clarity; ar.inverted = o.inverted; ar.suggestedPan = o.suggestedPan; ar.suggestedPanR = o.suggestedPanR; ar.suggestedStereo = o.suggestedStereo;
        ar.hasLeft = timealign::known (o.leftMs); ar.leftMs = ar.hasLeft ? o.leftMs : 0.0;
        ar.hasRight = timealign::known (o.rightMs); ar.rightMs = ar.hasRight ? o.rightMs : 0.0;
        ar.arrivalMs = timealign::known (o.arrivalMs) ? o.arrivalMs : 0.0; ar.spreadMs = o.spreadMs;
        if (o.severity >= 2) ++warnings;
        if (t->alignRole == timealign::Spot)
        {
            at.ms = o.measured ? std::round (o.delayMs * 100.0) / 100.0 : 0.0;
            at.on = o.measured && at.ms > 0.0;
            if (at.on) ++spots;
        }
        set.tracks[ids[i].toString()] = at;
    }
    // the main pairs against each other
    juce::StringArray notes;
    for (auto& mc : an.mainChecks)
    {
        auto* a = mc.track >= 0 && mc.track < (int) ids.size() ? app.project.findTrack (ids[(size_t) mc.track]) : nullptr;
        auto* b = mc.against >= 0 && mc.against < (int) ids.size() ? app.project.findTrack (ids[(size_t) mc.against]) : nullptr;
        if (a == nullptr || b == nullptr) continue;
        if (std::abs (mc.ms) > 2.0)
            notes.add (a->name + " hears the music " + juce::String (std::abs (mc.ms), 2) + " ms " + (mc.ms > 0.0 ? "later" : "earlier") + " than " + b->name
                       + " (about " + metres (mc.ms) + "): the main pairs are not in line, so the spots are lined up with their average.");
        if (mc.inverted) notes.add (a->name + "'s polarity looks reversed against " + b->name + ".");
        AlignTrack at;
        at.result.severity = mc.drastic() ? 2 : 0;
        at.result.message = mc.drastic() ? juce::String ("not in line with ") + b->name + " (" + juce::String (mc.ms, 2) + " ms)" + (mc.inverted ? ", polarity reversed" : "")
                                         : "in line with " + b->name + " (" + juce::String (mc.ms, 2) + " ms)";
        set.tracks[a->id.toString()] = at;
    }
    int mainCount = 0;
    for (auto& id : ids) if (auto* t = app.project.findTrack (id); t != nullptr && t->alignRole == timealign::Main) ++mainCount;
    for (auto& id : ids)                                                         // the main pair itself (or the first of two): what the others are measured against
        if (auto* t = app.project.findTrack (id); t != nullptr && t->alignRole == timealign::Main && set.tracks.find (id.toString()) == set.tracks.end())
        {
            AlignTrack at;
            at.result.message = mainCount > 1 ? "a main pair: the spots and distance mics are measured against the main pairs together"
                                               : "the main pair: the spots and distance mics are measured against it";
            set.tracks[id.toString()] = at;
        }
    app.project.alignSets[ownOwner()] = set;
    mainNote.setText (notes.joinIntoString ("\n"), juce::dontSendNotification);
    tracksChanged();
    status.setText (juce::String (spots) + (spots == 1 ? " spot lined up" : " spots lined up") + " in " + pieceName (ownOwner())
                    + (warnings > 0 ? "  -  " + juce::String (warnings) + (warnings == 1 ? " warning (in red)" : " warnings (in red)") : juce::String())
                    + ". Listen, switching Alignment on and off.", juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, warnings > 0 ? theme::warn : theme::text);
}

void AlignComponent::tracksChanged()
{
    app.project.markDirty();
    app.setAlignOwner (app.project.alignOwnerFor (isEdit, sourceId));          // you hear this piece's alignment while you work on it here
    app.engine.rebuildPlan();
    app.project.sendChangeMessage();
    refreshRows();
}

void AlignComponent::useSuggestedPan (const juce::Uuid& trackId)
{
    auto* t = app.project.findTrack (trackId);
    auto* st = stripOf (trackId);
    const auto* set = shownSet();
    const auto* at = set != nullptr ? set->find (trackId) : nullptr;
    if (t == nullptr || st == nullptr || at == nullptr || ! at->result.measured) return;
    const float p = at->result.suggestedPan;
    if (t->channelCount() == 2 && at->result.suggestedStereo)                 // a pair: each channel where its own sound sits
    {
        st->panL.set (juce::jlimit (-1.0f, 1.0f, at->result.suggestedPan)); st->panR.set (juce::jlimit (-1.0f, 1.0f, at->result.suggestedPanR));
    }
    else if (t->channelCount() >= 2)
    {
        const float l = st->panL.get(), r = st->panR.get(), mid = 0.5f * (l + r), shift = p - mid;
        st->panL.set (juce::jlimit (-1.0f, 1.0f, l + shift)); st->panR.set (juce::jlimit (-1.0f, 1.0f, r + shift));
    }
    else st->pan.set (p);
    recalcForPans();
}

void AlignComponent::recalcForPans()
{
    std::vector<juce::Uuid> ids;
    const auto tins = trackInputs (ids);
    auto& set = editableSet();
    for (size_t i = 0; i < ids.size(); ++i)
    {
        auto* t = app.project.findTrack (ids[i]);
        auto* at = set.find (ids[i]);
        if (t == nullptr || at == nullptr || t->alignRole != timealign::Spot || ! at->result.measured) continue;
        timealign::TrackOut o;
        o.leftMs = at->result.hasLeft ? at->result.leftMs : timealign::kNaN;
        o.rightMs = at->result.hasRight ? at->result.rightMs : timealign::kNaN;
        at->ms = std::round (timealign::delayForPan (o, tins[i]) * 100.0) / 100.0;
        at->on = at->ms > 0.0;
    }
    tracksChanged();
}

void AlignComponent::showCopyMenu()
{
    juce::PopupMenu m;
    std::vector<juce::String> owners;
    for (auto& [owner, set] : app.project.alignSets)
    {
        if (owner == ownOwner() || ! (set.measured() || set.hasDelays())) continue;
        const juce::Uuid id (owner);
        if (app.project.findTakeWindow (id) == nullptr && app.project.findEdit (id) == nullptr) continue;
        owners.push_back (owner);
        m.addItem ((int) owners.size(), juce::String (pieceName (owner)).replaceFirstOccurrenceOf ("piece ", "").replaceFirstOccurrenceOf ("edit ", "Edit ")
                                         + (set.measuredOn.isNotEmpty() ? "   (" + set.measuredOn.fromLastOccurrenceOf (", ", false, false) + ")" : juce::String()));
    }
    if (owners.empty()) { m.addItem (-1, "No other piece has been measured yet", false); }
    juce::Component::SafePointer<AlignComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&copyButton), [safe, owners] (int r)
    {
        if (safe == nullptr || r < 1 || r > (int) owners.size()) return;
        auto& P = safe->app.project;
        auto copy = P.alignSets[owners[(size_t) r - 1]];
        copy.measuredOn = "copied from " + safe->pieceName (owners[(size_t) r - 1]) + (copy.measuredOn.isNotEmpty() ? " (" + copy.measuredOn + ")" : juce::String());
        P.alignSets[safe->ownOwner()] = copy;
        safe->tracksChanged();
        safe->status.setText ("Copied: this piece now has the same delays.", juce::dontSendNotification);
    });
}

void AlignComponent::rebuildRows()
{
    rows.clear();
    juce::String sig;
    for (auto& t : app.project.tracks)
    {
        if (t.isLtc) continue;
        sig << t.id.toString();
        auto* r = rows.add (new Row (*this, t.id));
        rowsHolder.addAndMakeVisible (r);
    }
    rowsSig = sig;
    bool anyRole = false; for (auto& t : app.project.tracks) anyRole = anyRole || t.alignRole != 0;
    if (! anyRole && ! app.project.tracks.empty())                    // the first time: a guess from the names, to be checked
    {
        for (auto& t : app.project.tracks) t.alignRole = t.isLtc ? (int) timealign::NotUsed : timealign::guessRole (t.name);
        guessNote.setText ("Roles guessed from the track names: check them.", juce::dontSendNotification);
        app.project.markDirty();
    }
    resized();
    refreshRows();
}

void AlignComponent::refreshRows()
{
    juce::String sig;
    for (auto& t : app.project.tracks) if (! t.isLtc) sig << t.id.toString();
    if (sig != rowsSig) { rebuildRows(); return; }
    const double sr = app.engine.getSampleRate() > 0.0 ? app.engine.getSampleRate() : 48000.0;
    for (auto* r : rows) r->refresh (sr);
    // which piece, what is measured, and whose delays it plays with
    {
        juce::String piece;
        if (! isEdit) { if (auto* w = app.project.findTakeWindow (sourceId)) piece = "Piece: " + w->name; }
        else if (auto* e = app.project.findEdit (sourceId)) piece = "Edit: " + e->name;
        pieceLabel.setText (piece, juce::dontSendNotification);
    }
    const auto src = makeSource();
    sourceLabel.setText ("Analyse measures " + src.text, juce::dontSendNotification);
    const auto using_ = app.project.alignOwnerFor (isEdit, sourceId);
    const auto* set = shownSet();
    juce::String m;
    if (isEdit && using_ != ownOwner()) m = set != nullptr ? "Plays with the delays of " + pieceName (using_) + (set->measuredOn.isNotEmpty() ? " (" + set->measuredOn + ")" : juce::String()) + ". Analyse here to give the edit its own."
                                                         : "Not measured: the piece this edit comes from has no measurements yet. Analyse here, or in its take window.";
    else m = set != nullptr && set->measuredOn.isNotEmpty() ? "Measured on " + set->measuredOn : juce::String ("Not measured yet in this piece.");
    measuredLabel.setText (m, juce::dontSendNotification);
    const bool on = app.project.alignEnabled;
    abButton.setButtonText (on ? "Alignment ON" : "Alignment OFF");
    abButton.setColour (juce::TextButton::buttonColourId, on ? juce::Colour (0xff1f7a46) : juce::Colour (0xff8a3b2e));
    abButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
}

void AlignComponent::timerCallback()
{
    if (job != nullptr)
    {
        progress = job->progress.load();
        if (job->done.load())
        {
            auto finished = std::move (job);
            finished->stopThread (2000);
            bar.setVisible (false); stopButton.setVisible (false); analyseButton.setEnabled (true);
            applyResults (finished->result, finished->ids, finished->measuredOnText);
        }
    }
    if (++tick % 4 == 0) refreshRows();                                  // (pans moved in the mixer, roles changed elsewhere, a new take ...)
}

void AlignComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::text); g.setFont (FermataLookAndFeel::displayFont (17.0f));
    g.drawText ("Align spot mics", 16, 10, 260, 24, juce::Justification::centredLeft, false);
    // the column headings
    g.setColour (theme::dimText); g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    const int x0 = viewport.getX() + 4, y = headY;
    g.drawText ("TRACK", x0, y, Row::kName, 16, juce::Justification::centredLeft, false);
    g.drawText ("ROLE", x0 + Row::kName + 4, y, Row::kRole, 16, juce::Justification::centredLeft, false);
    g.drawText ("MEASURED", x0 + Row::kName + 4 + Row::kRole + 10, y, 200, 16, juce::Justification::centredLeft, false);
    const int right = viewport.getRight() - viewport.getScrollBarThickness() - 4;
    g.drawText ("DELAY (THIS PIECE)", right - Row::kRight, y, 160, 16, juce::Justification::centredLeft, false);
    g.drawText ("SUGGESTED PAN (NOW)", right - Row::kUse - 4 - Row::kPan, y, Row::kPan, 16, juce::Justification::centredRight, false);
    g.setColour (theme::border); g.fillRect (viewport.getX(), viewport.getY() - 1, viewport.getWidth(), 1);
}

void AlignComponent::resized()
{
    auto r = getLocalBounds().reduced (16, 10);
    info.placeTopRight (*this, 8);
    {
        auto title = r.removeFromTop (26);
        title.removeFromLeft (190);
        pieceLabel.setBounds (title.withTrimmedRight (40));
    }
    r.removeFromTop (6);
    {
        auto row = r.removeFromTop (30);
        analyseButton.setBounds (row.removeFromLeft (120)); row.removeFromLeft (8);
        stopButton.setBounds (row.removeFromLeft (70)); row.removeFromLeft (8);
        bar.setBounds (row.removeFromLeft (180).reduced (0, 4)); row.removeFromLeft (12);
        guessButton.setBounds (row.removeFromRight (220)); row.removeFromRight (8);
        copyButton.setBounds (row.removeFromRight (190));
    }
    r.removeFromTop (4);
    sourceLabel.setBounds (r.removeFromTop (20));
    {
        auto row = r.removeFromTop (20);
        guessNote.setBounds (row.removeFromRight (300));
        measuredLabel.setBounds (row);
    }
    r.removeFromTop (6);
    auto bottom = r.removeFromBottom (92);
    {
        auto row = bottom.removeFromTop (32);
        abButton.setBounds (row.removeFromLeft (160)); row.removeFromLeft (14);
        recalcButton.setBounds (row.removeFromLeft (210)); row.removeFromLeft (8);
        usePansButton.setBounds (row.removeFromLeft (190)); row.removeFromLeft (8);
        clearButton.setBounds (row.removeFromLeft (180));
    }
    bottom.removeFromTop (4);
    status.setBounds (bottom.removeFromTop (20));
    mainNote.setBounds (bottom);
    headY = r.getY();
    r.removeFromTop (18);
    viewport.setBounds (r);
    const int w = viewport.getWidth() - viewport.getScrollBarThickness();
    constexpr int rowH = 34;
    rowsHolder.setSize (w, juce::jmax (1, rows.size() * rowH));
    for (int i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, i * rowH, w, rowH);
}
} // namespace td
