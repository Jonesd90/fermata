// The fix tools opened from the take and edit windows: pitch, repair, Noise mode and the pitch curve (3.18.6: from FixTools.cpp, unchanged).
#include "FixToolsDialogs.h"

namespace td { namespace fixtools
{
// ======================================================================================================== take window: pitch / repair
/** Indices of the files of the take that are marked (all of them, or the tracks chosen with Ctrl + Alt + drag). */
static std::vector<size_t> takeSel (const TakeWindowDef& w, const TakeGroup& g)
{
    std::vector<size_t> v;
    for (size_t i = 0; i < g.files.size(); ++i) if (w.editUses (g.files[i].trackId)) v.push_back (i);
    return v;
}

void pitchTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (! gMakeProcessPage && heldByRec (app, "Pitch correction", parent, [ap = &app, wid] (juce::Component* p) { pitchTake (*ap, wid, p); })) return;
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (*w, in, out, "Pitch correction"); if (g == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch correction", "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    PitchContext c; c.allowPad = false;
    c.what = "Shifts the pitch of the marked part (" + juce::String (out - in, 2) + " s of " + w->displayName (*g) + ") on every track of the take. "
             "New audio files are made and put in the same place; the originals are not touched (Undo fix brings them back).";
    auto jobFiles = std::make_shared<audioops::TakeFixResult>();
    auto spec = std::make_shared<FixSpec>();
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say ("Pitch correction", "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    std::vector<juce::File> srcAll; for (auto i : sel) srcAll.push_back (g->files[i].file);
    c.work = [srcAll, in, out, rate, jobFiles, spec] (double cents, double, AudioJob& j) -> juce::String
    {
        spec->kind = FixSpec::Kind::Pitch; spec->cents = cents;
        std::vector<juce::File> src = srcAll, dst;
        for (auto& f : src) dst.push_back (takeFixDest (f, *spec));
        *jobFiles = audioops::fixTakeFiles (formats(), src, dst, (juce::int64) std::llround (in * rate), (juce::int64) std::llround (out * rate), *spec, &j);
        return jobFiles->error;
    };
    c.finish = [&app, wid, gid, jobFiles, spec, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || jobFiles->newFiles.size() != sel.size()) return;
        auto old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = jobFiles->newFiles[k];
        setUndoForTake (app, wid, gid, old, "pitch correction");
        app.project.changed();
    };
    if (gMakeProcessPage) { gProcessPage = std::make_unique<PitchDialog> (std::move (c)); return; }
    launchDialog (std::make_unique<PitchDialog> (std::move (c)), "Pitch correction", parent);
}

void repairTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "ReTouch, DeClick & Denoise", parent, [ap = &app, wid] (juce::Component* p) { repairTake (*ap, wid, p); })) return;
    const juce::String nm = "Spectral Repair";
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (*w, in, out, nm); if (g == nullptr) return;
    if (app.engine.isRecording()) { say (nm, "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    const juce::int64 from = (juce::int64) std::llround (in * rate), to = (juce::int64) std::llround (out * rate);
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    std::vector<juce::File> src; std::vector<juce::Uuid> srcTracks; for (auto i : sel) { src.push_back (g->files[i].file); srcTracks.push_back (g->files[i].trackId); }
    auto projectRT = std::make_shared<const Project::RoomTone> (app.project.roomTone);
    RepairContext c; c.rate = rate; c.length = to - from;
    c.roomSource = projectRT->source;
    for (auto& id : srcTracks) { const auto* t = projectRT->forTrack (id); c.roomMono.push_back (t != nullptr ? t->mono : denoise::Profile()); }
    c.what = "All " + juce::String ((int) src.size()) + " tracks of " + w->displayName (*g) + " are shown as one picture. Draw a box round the noise (time and pitch), then press ReTouch "
             "(or De-click if it is clicks, or Denoise for background noise with the room tone): the box is rebuilt from the clean sound next to it, on every track. New files are made; the originals stay.";
    for (auto i : sel) c.trackNames.push_back (g->files[i].trackName);
    c.trackIds = srcTracks;
    c.where = w->displayName (*g); c.areaStart = in;
    c.readChannels = [src, from] (juce::int64 a, juce::int64 b, AudioJob& j)
    {
        std::vector<std::vector<std::vector<float>>> out;
        for (auto& f : src)
        {
            if (j.cancelled()) break;
            std::vector<std::vector<float>> ch;
            auto r = audioops::openReader (formats(), f);
            if (r != nullptr) audioops::readRange (*r, from + a, b - a, ch);
            if (ch.empty()) ch.push_back (std::vector<float> ((size_t) juce::jmax ((juce::int64) 0, b - a), 0.0f));
            out.push_back (std::move (ch));
        }
        return out;
    };
    wireConsonants (c, app);
    c.load = [src, from, to] (AudioJob& j)
    {
        std::vector<std::vector<float>> tracks;
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (j.cancelled()) break;
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, src.size()));
            auto r = audioops::openReader (formats(), src[i]); if (r == nullptr) continue;
            std::vector<std::vector<float>> ch; audioops::readRange (*r, from, to - from, ch);
            std::vector<float> mono (ch.empty() ? 0 : ch[0].size(), 0.0f);
            for (auto& x : ch) for (size_t n = 0; n < mono.size(); ++n) mono[n] += x[n] / (float) ch.size();
            tracks.push_back (std::move (mono));
        }
        return tracks;
    };
    struct State { audioops::TakeFixResult res; std::vector<TakeFile> old; PreviewApplied applied; };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    c.prepare = [src, srcTracks, from, st, projectRT, rate] (const std::vector<RepairOp>& ops, AudioJob& j) -> juce::String
    {
        if (ops.empty()) return "Nothing to do.";
        std::vector<audioops::FixOp> fo;
        for (auto& o : ops) fo.push_back (audioops::FixOp { o.spec, from + o.t0, from + o.t1 });
        std::vector<juce::File> srcF = src, dst;
        for (auto& f : srcF) dst.push_back (takeFixDest (f, ops.back().spec));
        // a denoise: each file's room-tone profiles - learned from the box of this take (on the file itself, every channel), or the project's for its track
        auto boxes = std::make_shared<std::map<std::pair<double, double>, std::vector<audioops::DenoiseProfiles>>>();
        for (auto& o : ops)
        {
            if (o.spec.kind != FixSpec::Kind::Denoise || ! (o.spec.dnBoxT1 > o.spec.dnBoxT0)) continue;
            const auto key = std::make_pair (o.spec.dnBoxT0, o.spec.dnBoxT1);
            if (boxes->count (key)) continue;
            std::vector<audioops::DenoiseProfiles> per;
            for (auto& f : srcF)
            {
                juce::String err;
                per.push_back (audioops::learnDenoise (formats(), f, from + (juce::int64) std::llround (o.spec.dnBoxT0 * rate), from + (juce::int64) std::llround (o.spec.dnBoxT1 * rate), o.spec.dn, err));
                if (err.isNotEmpty()) return err;
            }
            (*boxes)[key] = std::move (per);
        }
        auto profilesFor = [boxes, srcTracks, projectRT] (size_t k, const FixSpec& spec) -> std::shared_ptr<const std::vector<denoise::Profile>>
        {
            if (spec.dnBoxT1 > spec.dnBoxT0)
            {
                auto it = boxes->find (std::make_pair (spec.dnBoxT0, spec.dnBoxT1));
                if (it == boxes->end() || k >= it->second.size()) return nullptr;
                return std::make_shared<const std::vector<denoise::Profile>> (it->second[k].perChannel);
            }
            if (k < srcTracks.size()) if (auto* t = projectRT->forTrack (srcTracks[k])) if (! t->perChannel.empty()) return std::make_shared<const std::vector<denoise::Profile>> (t->perChannel);
            return nullptr;
        };
        st->res = audioops::fixTakeFiles (formats(), srcF, dst, fo, &j, profilesFor, srcTracks);     // one new file per track with every change in it (a pasted consonant: each its own track's part)
        return st->res.error;
    };
    auto deleteFiles = [st] { for (auto& f : st->res.newFiles) f.deleteFile(); st->res.newFiles.clear(); };
    c.apply = [&app, wid, gid, st, deleteFiles, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || st->res.newFiles.size() != sel.size()) { deleteFiles(); return false; }
        st->old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = st->res.newFiles[k];
        st->applied = true; app.project.changed();
        return true;
    };
    c.unapply = [&app, wid, gid, st]
    {
        if (! st->applied) return;
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
        st->applied = false;
    };
    c.discard = deleteFiles;
    c.accept = [&app, wid, gid, st] { st->res.newFiles.clear(); st->applied = false; setUndoForTake (app, wid, gid, st->old, "spectral repair"); app.project.changed(); };
    c.play = [&app, wid, gid, in, nm] (double a, double b, bool loop)
    {
        const auto err = app.playTake (wid, gid, in + a, in + b, loop);
        if (err.isNotEmpty()) say (nm, err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, wid, gid, in]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != wid || app.playInfo.id != gid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - in);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    gSpecPrefs = app.props.getUserSettings();
    auto dlg = std::make_unique<RepairDialog> (std::move (c));
    juce::Component::SafePointer<RepairDialog> sp (dlg.get());
    launchDialog (std::move (dlg), nm, parent, true, [sp] { return sp == nullptr || sp->mayClose(); });
}

void pitchCurveTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "The pitch curve", parent, [ap = &app, wid] (juce::Component* p) { pitchCurveTake (*ap, wid, p); })) return;
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (*w, in, out, "Pitch curve"); if (g == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch curve", "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    const juce::int64 from = (juce::int64) std::llround (in * rate), to = (juce::int64) std::llround (out * rate);
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say ("Pitch curve", "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    std::vector<juce::File> src; for (auto i : sel) src.push_back (g->files[i].file);
    CurveContext c; c.rate = rate; c.length = to - from;
    c.what = "Draw how the pitch should change along the marked part (" + juce::String (out - in, 2) + " s of " + w->displayName (*g) + "), on every track. "
             "The middle line is no change; up raises the pitch (+100 cents at the top), down lowers it. Press Preview (key R) to hear it, then Accept or Revert. "
             "The original files are never touched.";
    struct State { audioops::TakeFixResult res; std::vector<TakeFile> old; PreviewApplied applied; };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    c.load = [src, from, to] (AudioJob& j)
    {
        std::vector<float> mix ((size_t) (to - from), 0.0f);
        for (auto& f : src)
        {
            if (j.cancelled()) break;
            auto rd = audioops::openReader (formats(), f); if (rd == nullptr) continue;
            std::vector<std::vector<float>> ch; audioops::readRange (*rd, from, to - from, ch);
            for (auto& x : ch) for (size_t n = 0; n < x.size() && n < mix.size(); ++n) mix[n] += x[n];
        }
        std::vector<std::vector<float>> o; o.push_back (std::move (mix)); return o;
    };
    c.prepare = [src, from, to, st] (const FixSpec& spec, double, AudioJob& j) -> juce::String
    {
        std::vector<juce::File> srcF = src, dst;
        for (auto& f : srcF) dst.push_back (takeFixDest (f, spec));
        st->res = audioops::fixTakeFiles (formats(), srcF, dst, from, to, spec, &j);
        return st->res.error;
    };
    auto deleteFiles = [st] { for (auto& f : st->res.newFiles) f.deleteFile(); st->res.newFiles.clear(); };
    c.apply = [&app, wid, gid, st, deleteFiles, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || st->res.newFiles.size() != sel.size()) { deleteFiles(); return false; }
        st->old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = st->res.newFiles[k];
        st->applied = true; app.project.changed();
        return true;
    };
    c.revert = [&app, wid, gid, st, deleteFiles]
    {
        if (st->applied)
        {
            auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
            if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
            st->applied = false;
        }
        deleteFiles();
    };
    c.unapply = [&app, wid, gid, st]
    {
        if (! st->applied) return;
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
        st->applied = false;                                  // (the corrected files are kept, ready to be put back)
    };
    c.accept = [&app, wid, gid, st] { st->res.newFiles.clear(); st->applied = false; setUndoForTake (app, wid, gid, st->old, "pitch curve"); app.project.changed(); };
    c.play = [&app, wid, gid, in, out] (double fromSec, double toSec, bool loop)
    {
        const auto err = app.playTake (wid, gid, in + fromSec, juce::jmin (out, in + toSec), loop);          // the part in view (inside the marked part), nothing either side
        if (err.isNotEmpty()) say ("Pitch curve", err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, wid, gid, in]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != wid || app.playInfo.id != gid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - in);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    launchDialog (withPitchTabs (std::make_unique<CurveDialog> (std::move (c)), false), "Pitch", parent, true);
}

/** The Pitch button of the take window: Pitch curve first, Pitch process on the second tab. */
void pitchTabsTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "Pitch correction", parent, [ap = &app, wid] (juce::Component* p) { pitchTabsTake (*ap, wid, p); })) return;
    gProcessPage.reset(); gMakeProcessPage = true; pitchTake (app, wid, parent); gMakeProcessPage = false;
    if (gProcessPage == nullptr) return;                                   // (a message was shown: nothing is marked)
    pitchCurveTake (app, wid, parent);
    gProcessPage.reset();
}

// ======================================================================================================== edit window: pitch / repair

void pitchEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (! gMakeProcessPage && heldByRec (app, "Pitch correction", parent, [ap = &app, eid] (juce::Component* p) { pitchEdit (*ap, eid, p); })) return;
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say ("Pitch correction", "First mark the part with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) ++pieces;
    if (pieces == 0) { say ("Pitch correction", "There is no audio between the marks."); return; }
    PitchContext c; c.allowPad = true;
    c.what = "Shifts the pitch of the audio between marks 1 and 2 (" + juce::String (e->fixOut - e->fixIn, 2) + " s, " + juce::String (pieces) + " piece(s)) on " + (e->fixTracks.empty() ? juce::String ("every track") : juce::String ((int) e->fixTracks.size()) + " chosen track(s)") + ". "
             "The audio is replaced in the same place by new files; the originals are not touched (Undo fix brings them back). "
             "'Process extra' also corrects that much more audio each side, so you can move the edit points outward later.";
    auto jobs = std::make_shared<std::vector<RegionJob>>(); auto spec = std::make_shared<FixSpec>();
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    const auto only = e->fixTracks;
    c.work = [fixFolder, regionsCopy, tIn, tOut, rate, jobs, spec, only] (double cents, double padSec, AudioJob& j) -> juce::String
    {
        spec->kind = FixSpec::Kind::Pitch; spec->cents = cents; jobs->clear();
        const auto pad = (juce::int64) std::llround (padSec * rate);
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut) continue;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            RegionJob rj; const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, a - pad, b + pad, a - pad, b + pad, *spec, only);
            jobs->push_back (std::move (rj));
        }
        for (size_t i = 0; i < jobs->size(); ++i)
        {
            const auto err = runRegionJob ((*jobs)[i], j, (float) i / (float) jobs->size(), (float) (i + 1) / (float) jobs->size());
            if (err.isNotEmpty()) { for (auto& done : *jobs) for (auto& r : done.results) r.file.deleteFile(); jobs->clear(); return err; }
        }
        return {};
    };
    c.finish = [&app, eid, jobs, spec]
    {
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || jobs->empty()) return;
        const auto old = e2->regions;
        const bool allOk = applyRegionJobs (*e2, old, *jobs, spec->shortName());
        if (! allOk) { e2->regions = old; say ("Pitch correction", "The edit changed while the audio was being processed, so nothing was changed. Please try again."); return; }
        setUndoForEdit (app, eid, old, "pitch correction");
        app.project.changed();
    };
    if (gMakeProcessPage) { gProcessPage = std::make_unique<PitchDialog> (std::move (c)); return; }
    launchDialog (std::make_unique<PitchDialog> (std::move (c)), "Pitch correction", parent);
}

static void openRepair (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot);

void repairEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "ReTouch, DeClick & Denoise", parent, [ap = &app, eid] (juce::Component* p) { repairEdit (*ap, eid, p); })) return;
    const juce::String nm = "Spectral Repair";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (nm, "First mark the area with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 lastEnd = 0; int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) { ++pieces; lastEnd = juce::jmax (lastEnd, r.endSample()); }
    if (pieces == 0) { say (nm, "There is no audio between the marks."); return; }
    const auto from = tIn, to = juce::jmin (tOut, lastEnd);
    if (to - from < (juce::int64) (0.05 * rate)) { say (nm, "The marked area is too short (at least about 0.05 s)."); return; }
    StreamSel outS, inS;
    const int sm = detectStreams (*e, from, to, outS, inS, nm);
    if (sm < 0) return;
    if (sm == 0) { openRepair (app, eid, parent, from, to, StreamSel(), nullptr, -1); return; }
    auto pair = makePair (app, eid, nm, "spectral repair");
    openRepair (app, eid, parent, from, to, outS, pair, 0);
    openRepair (app, eid, parent, from, to, inS, pair, 1);
    pair->focus (0);                                           // the keyboard (Space ...) starts with the OUT stream
}

static void openRepair (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot)
{
    const juce::String nm = "Spectral Repair";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    // the pieces that are heard in the marked area (in a stream window: only that stream's pieces)
    int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && (! ss.on || ss.has (r.id))) ++pieces;
    const auto only = e->fixTracks;
    // the tracks (one picture row each) and, for each, where its audio comes from along the marked area: whoever owns each moment (the piece that is on top there)
    struct Part { juce::File file; juce::int64 fileStart = 0, srcA = 0, n = 0, dst = 0; };
    std::vector<juce::Uuid> trackIds; std::vector<std::vector<Part>> parts;
    {
        for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && (! ss.on || ss.has (r.id)))
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                if (std::find (trackIds.begin(), trackIds.end(), f.trackId) == trackIds.end()) { trackIds.push_back (f.trackId); parts.emplace_back(); }
            }
        if (ss.on)
        {
            // a stream: ONLY its own take's audio, as it is in the files - the out stream carries on past the edit into its handle, the in stream starts before it
            juce::int64 prevHi = from;
            for (auto& id : ss.ids)
            {
                const auto* rp = [&] () -> const EditRegion* { for (auto& r : e->regions) if (r.id == id) return &r; return nullptr; }();
                if (rp == nullptr) continue;
                const auto& r = *rp; juce::int64 lo = 0, hi = 0; ss.span (r, from, to, lo, hi);
                lo = juce::jmax (lo, prevHi);
                if (hi <= lo) continue;
                prevHi = hi;
                for (auto& f : r.files)
                {
                    const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                    if (ti == trackIds.end()) continue;
                    parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, r.srcIn + (lo - r.startSample), hi - lo, lo - from });
                }
            }
        }
        else
        {
            std::vector<juce::int64> cuts { from, to };
            for (auto& r : e->regions) { if (r.startSample > from && r.startSample < to) cuts.push_back (r.startSample); if (r.endSample() > from && r.endSample() < to) cuts.push_back (r.endSample()); }
            std::sort (cuts.begin(), cuts.end()); cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
            for (size_t k = 0; k + 1 < cuts.size(); ++k)
            {
                const auto t0 = cuts[k], t1 = cuts[k + 1];
                const int ri = e->regionAt (t0 + (t1 - t0) / 2);
                if (ri < 0) continue;                                                       // a gap: silence
                const auto& r = e->regions[(size_t) ri];
                for (auto& f : r.files)
                {
                    const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                    if (ti == trackIds.end()) continue;
                    parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, r.srcIn + (t0 - r.startSample), t1 - t0, t0 - from });
                }
            }
        }
    }
    if (trackIds.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in the marked area."); if (pair != nullptr) pair->closeAll(); return; }
    // the same parts as a TrackParts (for a denoise: the room tone learned from a box of this area on the real files), and the project's room tone for the picture tracks
    auto tpArea = std::make_shared<TrackParts>();
    tpArea->ids = trackIds;
    for (size_t i = 0; i < parts.size(); ++i) { tpArea->names.push_back (juce::String()); tpArea->parts.emplace_back(); for (auto& pt : parts[i]) tpArea->parts.back().push_back ({ pt.file, pt.fileStart, pt.srcA, pt.n, pt.dst }); }
    for (auto& r : e->regions) for (auto& f : r.files) for (size_t i = 0; i < trackIds.size(); ++i) if (f.trackId == trackIds[i] && tpArea->names[i].isEmpty()) tpArea->names[i] = f.trackName;
    auto projectRT = std::make_shared<const Project::RoomTone> (app.project.roomTone);
    RepairContext c; c.rate = rate; c.length = to - from;
    c.roomSource = projectRT->source;
    for (auto& id : trackIds) { const auto* t = projectRT->forTrack (id); c.roomMono.push_back (t != nullptr ? t->mono : denoise::Profile()); }
    const juce::String lead = ss.on ? ss.label + ": the sound of the " + (slot == 0 ? "take that plays up to the edit point, carried on past it (its crossfade uses that part)" : "take that plays from the edit point, started before it (its crossfade uses that part)")
                                      + ", on its own. Correct it here; correct the other stream in the other window; then press Write back in BOTH. The edit point and its fades stay exactly where they are. " : juce::String();
    c.what = lead + (juce::String (only.empty() ? "All " : "The ") + juce::String ((int) trackIds.size()) + " tracks between marks 1 and 2 are shown as one picture, "
                       + (pieces > 1 && ! ss.on ? "across the edit points (" + juce::String (pieces) + " pieces). " : juce::String ("")) + "Draw a box round the noise (time and pitch), then press ReTouch (or De-click if it is clicks, or Denoise for background noise with the room tone): "
                       "the box is rebuilt from the clean sound next to it, on every track. Press Play to listen, Undo if it did not work, and Write back to file when you are happy. Each piece is repaired in its own place: the edit points and their fades stay exactly as they are, and the original files are not touched (Undo fix brings them back).");
    const auto total = to - from;
    c.trackIds = trackIds; c.trackNames = tpArea->names;
    c.where = e->name; c.areaStart = (double) from / rate;
    c.readChannels = [parts] (juce::int64 a, juce::int64 b, AudioJob& j)
    {
        // each track's channels over [a, b) of the area: whoever owns each moment, every channel as its file has it (silence where no piece is heard)
        std::vector<std::vector<std::vector<float>>> out;
        const auto n = juce::jmax ((juce::int64) 0, b - a);
        for (auto& tparts : parts)
        {
            if (j.cancelled()) break;
            std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
            int nc = 1;
            for (auto& pt : tparts) { readers.push_back (audioops::openReader (formats(), pt.file)); if (readers.back() != nullptr) nc = juce::jmax (nc, (int) readers.back()->numChannels); }
            std::vector<std::vector<float>> ch ((size_t) nc, std::vector<float> ((size_t) n, 0.0f));
            for (size_t k = 0; k < tparts.size(); ++k)
            {
                const auto& pt = tparts[k];
                if (readers[k] == nullptr) continue;
                const auto p0 = juce::jmax (a, pt.dst), p1 = juce::jmin (b, pt.dst + pt.n);
                if (p1 <= p0) continue;
                juce::int64 rel = pt.srcA - pt.fileStart + (p0 - pt.dst), cnt = p1 - p0, o = p0 - a;
                if (rel < 0) { cnt += rel; o -= rel; rel = 0; }
                if (cnt <= 0) continue;
                std::vector<std::vector<float>> x; audioops::readRange (*readers[k], rel, cnt, x);
                if (x.empty()) continue;
                for (size_t c2 = 0; c2 < ch.size(); ++c2)
                {
                    const auto& src2 = x[juce::jmin (c2, x.size() - 1)];
                    for (juce::int64 q = 0; q < cnt && q < (juce::int64) src2.size(); ++q) ch[c2][(size_t) (o + q)] = src2[(size_t) q];
                }
            }
            out.push_back (std::move (ch));
        }
        return out;
    };
    wireConsonants (c, app);
    c.load = [parts, total] (AudioJob& j)
    {
        std::vector<std::vector<float>> tracks;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (j.cancelled()) break;
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, parts.size()));
            std::vector<float> mono ((size_t) total, 0.0f);
            for (auto& pt : parts[i])
            {
                auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue;
                juce::int64 rel = pt.srcA - pt.fileStart, n = pt.n, dst = pt.dst;
                if (rel < 0) { n += rel; dst -= rel; rel = 0; }
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*r, rel, n, ch);
                for (auto& x : ch) for (size_t k = 0; k < x.size() && dst + (juce::int64) k < total; ++k) mono[(size_t) dst + k] += x[k] / (float) ch.size();
            }
            tracks.push_back (std::move (mono));
        }
        return tracks;
    };
    struct State { std::vector<RegionJob> jobs; std::vector<EditRegion> old; juce::String suffix; PreviewApplied applied; };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    auto deleteFiles = [st] { for (auto& rj : st->jobs) for (auto& r : rj.results) r.file.deleteFile(); st->jobs.clear(); };
    c.prepare = [fixFolder, regionsCopy, from, to, rate, st, only, deleteFiles, ss, tpArea, projectRT] (const std::vector<RepairOp>& ops, AudioJob& j) -> juce::String
    {
        if (ops.empty()) return "Nothing to do.";
        deleteFiles();
        st->suffix = ops.back().spec.shortName();
        const auto h = (juce::int64) (1.0 * rate);        // each piece is made again from its own sound with 1 s of untouched sound either side (the audio next to the marks, for the fades)
        const auto shortest = (juce::int64) (0.03 * rate);
        // each piece gets the changes that fall inside it, in ITS OWN samples: the same box or click search, applied to its own audio
        juce::int64 prevHi = from;
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= from || r.startSample >= to) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
            // the stretch this piece corrects: its own part of the marked area, and (a stream) the part of its take past the edit point, which its crossfade uses
            juce::int64 lo = r0, hi = r1;
            if (ss.on) { ss.span (r, from, to, lo, hi); lo = juce::jmax (lo, prevHi); if (hi > lo) prevHi = hi; }
            if (hi - lo < 2) continue;
            const auto a = r.srcIn + (r0 - r.startSample), b = r.srcIn + (r1 - r.startSample);
            std::vector<audioops::FixOp> mine;
            for (auto& o : ops)
            {
                const auto i0 = juce::jmax (lo, from + o.t0), i1 = juce::jmin (hi, from + o.t1);
                if (i1 - i0 < 2) continue;
                if (o.spec.kind == FixSpec::Kind::Paste)                                 // a pasted consonant keeps its own start, even where it reaches across this piece's start:
                {                                                                        // each piece adds its own part of it, in the right place
                    mine.push_back (audioops::FixOp { o.spec, r.srcIn + (from + o.t0 - r.startSample), r.srcIn + (from + o.t1 - r.startSample) });
                    continue;
                }
                if (o.t1 - o.t0 > shortest && i1 - i0 <= shortest) continue;           // a sliver of a bigger search is not taken for 'the click itself'
                const auto s0 = r.srcIn + (i0 - r.startSample);
                auto sp = o.spec;
                if (sp.lostKnown)
                {
                    // the lost samples found on all the tracks: those in this piece, counted from its own start; the time squeeze that puts them back stays inside the piece
                    // that is put in (35 ms clear of its ends: the joins there cross-fade with the untouched take, which must have the same timing)
                    std::vector<FixSpec::Lost> keep;
                    for (auto l : sp.lost) { const auto atEdit = from + o.t0 + l.at; if (atEdit >= i0 && atEdit < i1) { l.at = atEdit - i0; keep.push_back (l); } }
                    sp.lost = keep;
                    const auto xf = (juce::int64) (0.035 * rate);
                    sp.lostLo = (a + xf) - s0; sp.lostHi = (b - xf) - s0;
                }
                mine.push_back (audioops::FixOp { sp, s0, r.srcIn + (i1 - r.startSample) });
            }
            if (mine.empty()) continue;
            if (r1 <= r0) continue;
            RegionJob rj;
            const auto fa = juce::jmin (a, r.srcIn + (lo - r.startSample)), fb = juce::jmax (b, r.srcIn + (hi - r.startSample));
            prepRegionJob (fixFolder, r, rj, a, b, fa - h, fb + h, fa, fb, ops.back().spec, only);
            rj.ops = mine;
            st->jobs.push_back (std::move (rj));
        }
        if (st->jobs.empty()) return "Nothing to do in this area.";
        {   // a denoise among the changes: every file's room-tone profiles (the project's, or learned from the box of this area)
            const auto err = attachRoomTone (st->jobs, ops, *regionsCopy, *tpArea, projectRT, rate, j);
            if (err.isNotEmpty()) { deleteFiles(); return err; }
        }
        for (size_t i = 0; i < st->jobs.size(); ++i)
        {
            const auto err = runRegionJob (st->jobs[i], j, (float) i / (float) st->jobs.size(), (float) (i + 1) / (float) st->jobs.size());
            if (err.isNotEmpty()) { deleteFiles(); return err; }
        }
        return {};
    };
    if (pair != nullptr)
    {
        // a stream window: the pair puts the corrected pieces of both streams into the edit (always from the original edit), and keeps one undo step for both
        pair->jobsOf[slot] = [st] { return &st->jobs; };
        pair->suffix[slot] = "repair";
        c.apply = [pair, slot, st] { if (st->jobs.empty()) return false; return pair->put (slot, true); };
        c.unapply = [pair, slot] { pair->put (slot, false); };
        c.accept = [] {};
        c.pairWritten = [pair, slot] (std::function<bool()> commit) { pair->wrote (slot, std::move (commit)); };
    }
    else
    {
        c.apply = [&app, eid, st, deleteFiles]
        {
            auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || st->jobs.empty()) return false;
            st->old = e2->regions;
            const bool ok = applyRegionJobs (*e2, st->old, st->jobs, st->suffix);
            if (! ok) { e2->regions = st->old; deleteFiles(); return false; }
            st->applied = true; app.project.changed();
            return true;
        };
        c.unapply = [&app, eid, st]
        {
            if (! st->applied) return;
            if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); }
            st->applied = false;
        };
        c.accept = [&app, eid, st] { for (auto& rj : st->jobs) rj.results.clear(); st->applied = false; setUndoForEdit (app, eid, st->old, "spectral repair"); app.project.changed(); };
    }
    c.discard = deleteFiles;
    const double areaStart = (double) from / rate;
    c.play = [&app, eid, areaStart, nm] (double x, double y, bool loop)
    {
        const auto err = app.playEdit (eid, areaStart + x, areaStart + y, loop);
        if (err.isNotEmpty()) say (nm, err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, eid, areaStart]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - areaStart);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    gSpecPrefs = app.props.getUserSettings();
    auto dlg = std::make_unique<RepairDialog> (std::move (c));
    juce::Component::SafePointer<RepairDialog> sp (dlg.get());
    if (pair != nullptr) launchStream (pair, slot, std::move (dlg), nm + " - " + ss.label, parent);
    else launchDialog (std::move (dlg), nm, parent, true, [sp] { return sp == nullptr || sp->mayClose(); });
}


// ======================================================================================================== Noise mode (edit window)
/** The whole edit in one spectral window, built once at several resolutions (SpectralPyramid), with Gain, ReTouch, De-click and the background denoiser
    all to hand: each is applied the moment it is made, on the result of the last, as new piece files put into the edit (the edit plays through them at once);
    Undo takes the last one off; Write back to clip keeps what has been done (the window stays open); Finished closes and throws away what was not written. */
class NoiseModeDialog : public JobDialogBase
{
public:
    NoiseModeDialog (AppContext& a, const juce::Uuid& editId, double sampleRate, juce::int64 editLength, std::vector<juce::Uuid> onlyTracks, const juce::String& what)
        : app (a), eid (editId), rate (sampleRate), length (editLength), only (std::move (onlyTracks))
    {
        setSize (1100, 800);
        fixFolder = app.project.audioFolder().getChildFile ("Fixes");
        if (auto* e = app.project.findEdit (eid)) { base = e->regions; cur = std::make_shared<TrackParts> (buildTrackParts (*e, 0, length, only)); }
        projectRT = std::make_shared<const Project::RoomTone> (app.project.roomTone);
        intro.setText (what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::text);
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (sensCap, "Click sensitivity"); cap (strengthCap, "Strength"); cap (surroundCap, "Surrounding"); cap (weightCap, "Before <-> After");
        cap (amountCap, "Amount"); cap (residualCap, "Residual"); cap (roomCap, "");
        auto slider = [this] (juce::Slider& sl, double lo, double hi, double step, double v, const juce::String& suffix, int boxW)
        {
            sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (lo, hi, step); sl.setValue (v, juce::dontSendNotification); sl.setScrollWheelEnabled (false);
            sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, boxW, 22); sl.setTextValueSuffix (suffix);
            sl.onValueChange = [this] { updateSurround(); };
            addAndMakeVisible (sl);
        };
        slider (sens, 1, 10, 1, 6, "", 50); slider (strength, 0, 100, 1, 80, " %", 56); slider (surround, 10, 400, 5, 100, " %", 56); slider (weight, 0, 100, 1, 50, " %", 56);
        slider (amount, -6.0, 12.0, 0.5, 0.0, " dB", 62); slider (residual, -30.0, -3.0, 1.0, -12.0, " dB", 62);
        amount.setDoubleClickReturnValue (true, 0.0); residual.setDoubleClickReturnValue (true, -12.0);
        methodBox.addItem ("Method: Attenuate", 1); methodBox.addItem ("Method: Replace", 2);
        methodBox.addItem ("Method: Stray consonant", 3);
        methodBox.setSelectedId (1, juce::dontSendNotification);
        dirBox.addItem ("Direction: Horizontal", 1); dirBox.addItem ("Direction: Vertical", 2); dirBox.addItem ("Direction: 2D (both)", 3);
        dirBox.setSelectedId (1, juce::dontSendNotification);
        for (int b : { 128, 256, 512, 1024, 2048, 4096 }) bandsBox.addItem ("Bands: " + juce::String (b), b);
        bandsBox.setSelectedId (1024, juce::dontSendNotification);
        multiBtn.setToggleState (true, juce::dontSendNotification); linkBtn.setToggleState (true, juce::dontSendNotification);
        methodBox.setTooltip ("ReTouch. Attenuate: keeps the music under the noise and only turns down what sticks out (a stray consonant is the same, matched to the side away from the main one). Replace: rebuilds the box from the surroundings.");
        dirBox.setTooltip ("Where the material comes from: the sound before and after the box (Horizontal), the sound above and below it (Vertical), or both (2D).");
        strength.setTooltip ("How much of the repair replaces the original sound inside the box."); surround.setTooltip ("How much of the sound around the box is used, as a percentage of the box.");
        weight.setTooltip ("0 uses only the sound before the box, 100 only the sound after it, 50 both."); sens.setTooltip ("De-click: how readily clicks are found (1 few .. 10 many).");
        amount.setTooltip ("Denoise: how much louder than measured the room tone is taken to be. Double-click for 0."); residual.setTooltip ("Denoise: how much of the room tone is left in the box (the gain never goes below this). Double-click for -12.");
        specBtn.setTooltip ("How the picture is drawn: sharpness, colours, and the levels shown.");
        specBtn.onClick = [this] { openSpectroSettings(); };
        for (auto* x : std::initializer_list<juce::Component*> { &methodBox, &dirBox, &bandsBox, &multiBtn, &linkBtn, &specBtn, &scaleBox, &gainBtn, &retouchBtn, &declickBtn, &learnBtn, &denoiseBtn,
                                                                 &revertBtn, &finishBtn, &doneBtn, &startBtn, &rewBtn, &playBtn, &fwdBtn, &endBtn, &loopBtn, &clock }) addAndMakeVisible (x);
        scaleBox.addItem ("Pitch scale: Mel (as iZotope RX)", 6); scaleBox.addItem ("Pitch scale: linear (even Hz)", 1); scaleBox.addItem ("Pitch scale: mostly linear", 2); scaleBox.addItem ("Pitch scale: halfway", 3);
        scaleBox.addItem ("Pitch scale: mostly musical", 4); scaleBox.addItem ("Pitch scale: musical (every octave the same height)", 5);
        scaleBox.setSelectedId (6, juce::dontSendNotification); view.setMel();
        scaleBox.onChange = [this] { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; const int id = scaleBox.getSelectedId(); if (id == 6) view.setMel(); else view.setScale (w[juce::jlimit (0, 4, id - 1)]); };
        view.onNote = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };
        view.onNeedDetail = [this] (const PictureRequest& rq) { startDetail (rq); };
        view.onView = [this] { sliceForView(); };
        view.onSelection = [this] { refreshControls(); };
        dirBox.onChange = [this] { refreshControls(); }; methodBox.onChange = [this] { refreshControls(); };
        // the transport, as in the edit window: start, back 5 s, play / stop, forward 5 s, end - and a clock that can be typed into
        startBtn.setIcon (Icon::Start, "Back to the start of the edit [Home]"); rewBtn.setIcon (Icon::Rewind, "Back 5 seconds");
        playBtn.setIcon (Icon::Play, "Play / Stop [Space]"); fwdBtn.setIcon (Icon::Forward, "Forward 5 seconds"); endBtn.setIcon (Icon::End, "To the end of the edit [End]");
        playBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        playBtn.setTooltip ("Plays from the red playhead (click the time ruler to place it; without one, from the left of the page) through the mixers, all the way to the END of the edit: the page turns by itself as the playhead reaches its edge. "
                            "Every fix is heard the moment it is made. Press again to stop. Key: Space");
        startBtn.setTooltip ("The playhead goes back to the beginning of the edit (and keeps playing from there if it was playing).");
        rewBtn.setTooltip ("Back 5 seconds"); fwdBtn.setTooltip ("Forward 5 seconds");
        endBtn.setTooltip ("The playhead goes to the end of the edit.");
        loopBtn.setTooltip ("Repeat the PAGE you can see until you press Stop. Without Loop, playing carries on to the end of the whole edit and the page turns by itself.");
        playBtn.onClick = [this] { if (! built || running) return; if (isPlayingNow()) app.stopPlayback(); else playPressed(); };
        startBtn.onClick = [this] { goTo (0.0); };
        rewBtn.onClick = [this] { jumpBy (-5.0); };
        fwdBtn.onClick = [this] { jumpBy (5.0); };
        endBtn.onClick = [this] { goTo (seconds()); };
        loopBtn.onClick = [this]
        {
            if (! built || running || ! isPlayingNow()) return;
            const double pos = juce::jmax (0.0, app.playheadSeconds());
            app.stopPlayback(); playRange (pos);                         // (carries on from where it is: now looping the page, or on to the end)
        };
        clock.setSampleRate (rate);
        clock.setCaption ("Edit");
        clock.setTooltip ("Where the playhead is. Double-click to type a time and jump there; click the small label to change how the time is written.");
        clock.onJump = [this] (double t) { goTo (t); };
        clock.follow ([this] { return transportPos(); }, [this] { return isPlayingNow() ? TimeReadout::State::playing : TimeReadout::State::stopped; }, 30);
        for (auto* l : { &info, &roomCap }) { l->setFont (juce::FontOptions (12.5f)); l->setMinimumHorizontalScale (0.7f); }
        status.setFont (juce::FontOptions (12.5f)); status.setMinimumHorizontalScale (0.75f);
        gainBtn.onClick = [this] { openGain(); }; retouchBtn.onClick = [this] { retouch (false); }; declickBtn.onClick = [this] { retouch (true); };
        learnBtn.onClick = [this] { roomFromBox(); }; denoiseBtn.onClick = [this] { denoise(); };
        revertBtn.onClick = [this] { revert(); }; finishBtn.onClick = [this] { writeBack(); }; doneBtn.onClick = [this] { if (running) job.requestCancel(); else if (mayClose()) closeDialogOf (this); };
        for (auto* b : { &gainBtn, &retouchBtn, &declickBtn, &learnBtn, &denoiseBtn }) b->setColour (juce::TextButton::buttonColourId, theme::accent);
        finishBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
        gainBtn.setTooltip ("Gain: makes the sound inside the box louder or quieter (fading in and out at its edges), and nothing else. Key: G");
        retouchBtn.setTooltip ("ReTouch: rebuilds or turns down the box you drew from its surroundings, on every track, and puts it into the edit at once. Key: R");
        declickBtn.setTooltip ("De-click: finds and mends the clicks in the time span of the box (or of the whole edit if there is no box). Key: D");
        learnBtn.setTooltip ("Draw a box over a part of the picture that holds nothing but the room (its pitch range does not matter: the whole spectrum is learned) and press this: that box is the room tone for Denoise here, instead of the project's (the edit window's Room tone button). Key: L");
        denoiseBtn.setTooltip ("Denoise: takes the background noise out of the box you drew - its time span and its pitch range, on every track - with the room tone, and puts it into the edit at once. Above and below the box's pitch range nothing is touched; the denoising fades in and out at the box's edges. Key: N");
        revertBtn.setTooltip ("Takes the last fix off again (as many times as there are fixes since the last Write back).");
        finishBtn.setTooltip ("Keeps everything done so far in the edit (the Undo fix command of the edit window brings the audio back to how it was when this window opened, or when you last wrote back) and carries on: the window stays open.");
        doneBtn.setTooltip ("Closes the window. Fixes made since the last Write back are thrown away.");
        ticker.owner = this; ticker.startTimerHz (30);
        app.undoHold += 1;
        setupTabs();
        loadControls();
        setWantsKeyboardFocus (true);
        for (auto* ch : getChildren()) if (dynamic_cast<juce::Button*> (ch) != nullptr || dynamic_cast<juce::ComboBox*> (ch) != nullptr) ch->setWantsKeyboardFocus (false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NoiseModeDialog> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
        refreshControls();
        buildPicture();
    }
    ~NoiseModeDialog() override
    {
        saveControls();
        ticker.stopTimer(); ticker.owner = nullptr;
        if (detailCancel != nullptr) detailCancel->store (true);
        app.stopPlayback();
        discardUncommitted();
        app.undoHold -= 1;
    }
    // ---- the tools at the bottom: one tab for each tool (only one set of controls shows at a time, so the picture keeps the room), then the transport and the fixes ----
    enum { kTabTouch, kTabGain, kTabClick, kTabDenoise, kTabCount };
    static constexpr int kRowH = 24, kRowGap = 4, kPanelPad = 6, kTabW = 104, kTabGap = 12, kCapGap = 12, kTransportH = 26, kActionW = 116;      // (kActionW: the button that does the work, the same size and in the same place on every tab)
    static constexpr int sliderW = 140;                                           // a slider with its number: short on purpose
    static constexpr int kChoicesW = 184 + 8 + 180 + 8 + 96 + 8 + 132 + 8 + 112;                       // the five choices of ReTouch in a row
    static constexpr int kAmountsW = (58 + sliderW) + kCapGap + (116 + sliderW) + kCapGap + (100 + sliderW) + 16 + kActionW;   // its three sliders and the button in a row
    static constexpr int kBarMinW = 90, kFixesW = 90 + 8 + 150 + 8 + 150;
    int tab = kTabTouch;
    juce::TextButton tabBtn[kTabCount];
    juce::Label gainHint;
    juce::Rectangle<int> panelR, contentR;                                       // the tool panel and the part of it that holds the controls (set by layoutBottom)
    /** ReTouch has two rows (the choices; the sliders and the button) and, where the window is narrow, one more for each that does not fit in a row. */
    static int reTouchRows (int w) { return (w >= kChoicesW ? 1 : 2) + (w >= kAmountsW ? 1 : 2); }
    int listenW() const { return 5 * 34 + 4 * 2 + 6 + 64 + 8 + clock.getIdealWidth (28) + 4; }                // the transport: five buttons, Loop and the clock
    int transportRows (int w) const { return w >= listenW() + 8 + kBarMinW + 16 + kFixesW ? 1 : 2; }
    static juce::Rectangle<int> rowOf (juce::Rectangle<int> inner, int row) { return { inner.getX(), inner.getY() + row * (kRowH + kRowGap), inner.getWidth(), kRowH }; }
    /** Puts a control at the left of the row (nothing if it is hidden: the ones after it move up to fill the space). */
    static void putLeft (juce::Rectangle<int>& row, juce::Component& c, int w, int gap = 8)
    {
        if (! c.isVisible()) return;
        c.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
    }
    /** A caption and the slider after it. */
    static void putSlider (juce::Rectangle<int>& row, juce::Label& cap, juce::Slider& sl, int capW)
    {
        if (! sl.isVisible()) { cap.setBounds ({}); return; }
        cap.setBounds (row.removeFromLeft (capW)); sl.setBounds (row.removeFromLeft (sliderW)); row.removeFromLeft (kCapGap);
    }
    std::vector<juce::Component*> tabControls (int t)
    {
        switch (t)
        {
            case kTabTouch:   return { &methodBox, &dirBox, &bandsBox, &multiBtn, &linkBtn, &strengthCap, &strength, &surroundCap, &surround, &weightCap, &weight, &retouchBtn };
            case kTabGain:    return { &gainBtn, &gainHint };
            case kTabClick:   return { &sensCap, &sens, &declickBtn };
            default:          return { &amountCap, &amount, &residualCap, &residual, &learnBtn, &roomCap, &denoiseBtn };
        }
    }
    /** Lights the tab that is showing and no other. (By hand, not with a radio group: JUCE's radio group runs the click action of the key it turns off, and that put ReTouch back on.) */
    void lightTab() { for (int i = 0; i < kTabCount; ++i) tabBtn[i].setToggleState (i == tab, juce::dontSendNotification); }
    void selectTab (int t)
    {
        tab = juce::jlimit (0, (int) kTabCount - 1, t);
        lightTab();
        refreshControls();                                                       // (which also lays the controls out again)
    }
    void setupTabs()
    {
        const char* names[] = { "ReTouch [R]", "Gain [G]", "De-click [D]", "Denoise [N]" };
        const char* tips[] = { "ReTouch: rebuilds or turns down the box you drew from its surroundings (key R).", "Gain: makes the sound in the box louder or quieter (key G).",
                               "De-click: finds and mends clicks (key D).", "Denoise: takes the room noise out of the box (keys L and N)." };
        for (int i = 0; i < kTabCount; ++i)
        {
            auto& t = tabBtn[i]; t.setButtonText (names[i]); t.setTooltip (tips[i]); t.setClickingTogglesState (false);
            t.setColour (juce::TextButton::buttonOnColourId, theme::accent);
            t.onClick = [this, i] { selectTab (i); };
            addAndMakeVisible (t);
        }
        lightTab();
        gainHint.setText ("Louder or quieter inside the box, from silence up to +18 dB, and nothing else. Draw a box, then press Gain [G].", juce::dontSendNotification);
        gainHint.setColour (juce::Label::textColourId, theme::dimText); gainHint.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (gainHint);
    }
    /** The controls of the tab that is showing, in the panel's content area. */
    void placeTab (juce::Rectangle<int> in, int rows)
    {
        auto lastRow = rowOf (in, rows - 1);                                     // the button that does the work is in the same place whichever tab shows: the bottom right corner of the panel
        const auto action = lastRow.removeFromRight (kActionW);
        switch (tab)
        {
            case kTabTouch:
            {   // first row what to do and how, then how much, and the button that does it
                int row = 0;
                auto r = rowOf (in, row++);
                putLeft (r, methodBox, 184); putLeft (r, dirBox, 180); putLeft (r, bandsBox, 96);
                if (in.getWidth() < kChoicesW) r = rowOf (in, row++);                                        // narrow: the two ticks on a row of their own
                putLeft (r, multiBtn, 132); putLeft (r, linkBtn, 112);
                r = rowOf (in, row++);
                if (in.getWidth() >= kAmountsW)
                {
                    retouchBtn.setBounds (action); r.removeFromRight (kActionW + 16);
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116); putSlider (r, weightCap, weight, 100);
                }
                else                                                                                           // narrow: Before <-> After and the button go on a second row
                {
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116);
                    r = rowOf (in, row++);
                    putSlider (r, weightCap, weight, 100);
                    retouchBtn.setBounds (action);
                }
                break;
            }
            case kTabGain:
            {
                gainBtn.setBounds (action); gainHint.setBounds (rowOf (in, 0));
                break;
            }
            case kTabClick:
            {
                auto r = rowOf (in, 0);
                sensCap.setBounds (r.removeFromLeft (112)); r.removeFromLeft (8); sens.setBounds (r.removeFromLeft (sliderW));
                declickBtn.setBounds (action);
                break;
            }
            default:
            {
                auto r1 = rowOf (in, 0), r2 = rowOf (in, 1);
                amountCap.setBounds (r1.removeFromLeft (62)); amount.setBounds (r1.removeFromLeft (sliderW)); r1.removeFromLeft (kCapGap);
                residualCap.setBounds (r1.removeFromLeft (66)); residual.setBounds (r1.removeFromLeft (sliderW));
                denoiseBtn.setBounds (action); r2.removeFromRight (kActionW + 8);
                learnBtn.setBounds (r2.removeFromLeft (190)); r2.removeFromLeft (8); roomCap.setBounds (r2);
                break;
            }
        }
    }
    /** The tool panel (the tabs in two columns on the left, the controls of the chosen one beside them) and under it the transport and the fixes.
        Returns the height they need at this width (place = false only measures). */
    int layoutBottom (juce::Rectangle<int> area, bool place)
    {
        const int tabsW = 2 * kTabW + kRowGap;
        const int contentW = juce::jmax (100, area.getWidth() - 2 * kPanelPad - tabsW - kTabGap);
        const int rows = juce::jmax (2, reTouchRows (contentW));                  // (the same height whichever tab shows, so the picture does not jump)
        const int panelH = 2 * kPanelPad + rows * kRowH + (rows - 1) * kRowGap;
        const int trRows = transportRows (area.getWidth());
        const int total = panelH + trRows * (kRowGap + kTransportH);
        if (! place) return total;
        panelR = { area.getX(), area.getY(), area.getWidth(), panelH };
        auto inner = panelR.reduced (kPanelPad);
        auto tabsArea = inner.removeFromLeft (tabsW); inner.removeFromLeft (kTabGap);
        for (int i = 0; i < kTabCount; ++i) tabBtn[i].setBounds (tabsArea.getX() + (i % 2) * (kTabW + kRowGap), tabsArea.getY() + (i / 2) * (kRowH + kRowGap), kTabW, kRowH);
        contentR = inner;
        placeTab (contentR, rows);
        auto placeFixes = [this] (juce::Rectangle<int> r)
        {
            doneBtn.setBounds (r.removeFromRight (150)); r.removeFromRight (8);
            finishBtn.setBounds (r.removeFromRight (150)); r.removeFromRight (8);
            revertBtn.setBounds (r.removeFromRight (90));
        };
        auto row = juce::Rectangle<int> (area.getX(), panelR.getBottom() + kRowGap, area.getWidth(), kTransportH);
        if (trRows == 1) placeFixes (row.removeFromRight (kFixesW));
        for (auto* b : { &startBtn, &rewBtn, &playBtn, &fwdBtn, &endBtn }) { b->setBounds (row.removeFromLeft (34).withSizeKeepingCentre (34, 26)); row.removeFromLeft (2); }
        row.removeFromLeft (6); loopBtn.setBounds (row.removeFromLeft (64)); row.removeFromLeft (8);
        const int clockW = clock.getIdealWidth (28) + 4;
        clock.setBounds (row.removeFromLeft (clockW)); row.removeFromLeft (8);
        bar.setBounds (row.withTrimmedRight (trRows == 1 ? 16 : 0).reduced (0, 3));                      // (the progress bar, while something is being made)
        if (trRows == 2) placeFixes ({ area.getX(), row.getBottom() + kRowGap, area.getWidth(), kTransportH });
        return total;
    }
    void paint (juce::Graphics& g) override
    {
        if (panelR.isEmpty()) return;
        const auto b = panelR.toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.16f)); g.fillRoundedRectangle (b, 5.0f);
        const auto line = theme::manuscript() ? theme::giltDim() : theme::border.brighter (0.25f);
        g.setColour (line); g.drawRoundedRectangle (b, 5.0f, 1.0f);
        const float dx = (float) contentR.getX() - (float) kTabGap * 0.5f;
        g.setColour (line.withAlpha (0.7f)); g.drawLine (dx, b.getY() + 5.0f, dx, b.getBottom() - 5.0f, 1.0f);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        const int bottomH = layoutBottom (r, false);                             // how high the tools need to be at this width
        auto bottom = r.removeFromBottom (bottomH);
        layoutBottom (bottom, true);
        r.removeFromBottom (4);
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        {
            auto ir = r.removeFromBottom (22);
            specBtn.setBounds (ir.removeFromRight (150)); ir.removeFromRight (6); scaleBox.setBounds (ir.removeFromRight (270)); ir.removeFromRight (6);
            info.setBounds (ir.reduced (2, 0));
        }
        view.setBounds (r);
        repaint (bottom.expanded (2));
    }
protected:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)
        {
            if (spaceIsForRecording()) return true;
            if (built && ! running) { if (isPlayingNow()) app.stopPlayback(); else playPressed(); }
            return true;
        }
        const auto ch = k.getTextCharacter();
        if (! k.getModifiers().isAnyModifierKeyDown())
        {
            if (ch == 'r' || ch == 'R') { selectTab (kTabTouch); if (retouchBtn.isEnabled()) retouch (false); return true; }
            if (ch == 'g' || ch == 'G') { selectTab (kTabGain); if (gainBtn.isEnabled()) openGain(); return true; }
            if (ch == 'd' || ch == 'D') { selectTab (kTabClick); if (declickBtn.isEnabled()) retouch (true); return true; }
            if (ch == 'l' || ch == 'L') { selectTab (kTabDenoise); if (learnBtn.isEnabled()) roomFromBox(); return true; }
            if (ch == 'n' || ch == 'N') { selectTab (kTabDenoise); if (denoiseBtn.isEnabled()) denoise(); return true; }
        }
        if (k == juce::KeyPress::homeKey) { if (built && ! running) goTo (0.0); return true; }
        if (k == juce::KeyPress::endKey)  { if (built && ! running) goTo (seconds()); return true; }
        if (built && view.zoomKey (k)) return true;
        return false;
    }
private:
    struct Step { std::vector<EditRegion> before; std::vector<RegionJob> jobs; juce::String label; double t0 = 0.0, t1 = 0.0; };
    juce::String prefKey (const char* k) const { return juce::String ("noisemode.") + k; }
    void loadControls()
    {
        auto& p = uiPrefs();
        auto num = [&] (const char* k, juce::Slider& s) { const auto key = prefKey (k); if (p.containsKey (key)) s.setValue (juce::jlimit (s.getMinimum(), s.getMaximum(), p.getDoubleValue (key)), juce::dontSendNotification); };
        auto id = [&] (const char* k, juce::ComboBox& b) { const auto key = prefKey (k); if (p.containsKey (key) && b.indexOfItemId (p.getIntValue (key)) >= 0) b.setSelectedId (p.getIntValue (key), juce::dontSendNotification); };
        num ("sens", sens); num ("strength", strength); num ("surround", surround); num ("weight", weight); num ("amount", amount); num ("residual", residual);
        id ("method", methodBox); id ("bands", bandsBox); id ("dir", dirBox); id ("scale", scaleBox);
        tab = juce::jlimit (0, (int) kTabCount - 1, p.getIntValue (prefKey ("tab"), kTabTouch)); lightTab();
        if (p.containsKey (prefKey ("multi"))) multiBtn.setToggleState (p.getBoolValue (prefKey ("multi")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("link"))) linkBtn.setToggleState (p.getBoolValue (prefKey ("link")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("roll"))) view.setRoll (p.getDoubleValue (prefKey ("roll")));
        const int sc = scaleBox.getSelectedId();
        if (sc == 6) view.setMel(); else { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; view.setScale (w[juce::jlimit (0, 4, sc - 1)]); }
    }
    void saveControls()
    {
        auto& p = uiPrefs();
        p.setValue (prefKey ("sens"), sens.getValue()); p.setValue (prefKey ("strength"), strength.getValue()); p.setValue (prefKey ("surround"), surround.getValue()); p.setValue (prefKey ("weight"), weight.getValue());
        p.setValue (prefKey ("amount"), amount.getValue()); p.setValue (prefKey ("residual"), residual.getValue());
        p.setValue (prefKey ("method"), methodBox.getSelectedId()); p.setValue (prefKey ("bands"), bandsBox.getSelectedId()); p.setValue (prefKey ("dir"), dirBox.getSelectedId());
        p.setValue (prefKey ("scale"), scaleBox.getSelectedId()); p.setValue (prefKey ("tab"), tab);
        p.setValue (prefKey ("multi"), multiBtn.getToggleState()); p.setValue (prefKey ("link"), linkBtn.getToggleState()); p.setValue (prefKey ("roll"), view.rollValue());
    }
    double seconds() const { return (double) length / rate; }
    EditDef* edit() const { return app.project.findEdit (eid); }
    bool isPlayingNow() const { return app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == eid; }
    void tick()
    {
        const bool playing = isPlayingNow();
        const double pos = playing ? juce::jmax (0.0, app.playheadSeconds()) : -1.0;
        view.setPlayhead (pos);
        playBtn.setButtonText (playing ? "Stop" : "Play");                     // (the picture on the button turns into a stop square)
        // the page turns with the playhead: when it reaches the right edge of the page it was on, the next page comes (until the end of the edit)
        if (playing && ! loopBtn.getToggleState() && lastTickPos >= view.visT0() && lastTickPos < view.visT1()
            && pos >= view.visT1() - 0.01 * view.pageSpan() && view.visT1() < seconds() - 1.0e-6)
            view.showFrom (view.visT1());
        lastTickPos = pos;
    }
    struct Ticker : juce::Timer { NoiseModeDialog* owner = nullptr; void timerCallback() override { if (owner != nullptr) owner->tick(); } } ticker;
    bool consonantMethod() const { return methodBox.getSelectedId() == 3; }
    bool lookingAcross() const { return dirBox.getSelectedId() != 2 || consonantMethod(); }
    void updateSurround() { view.setSurround (built && view.hasSelection() && ! consonantMethod(), surround.getValue(), dirBox.getSelectedId() - 1, dirBox.getSelectedId() != 2 ? weight.getValue() / 100.0 : 0.5); }
    void refreshControls()
    {
        for (auto* c : tabControls (tab)) c->setVisible (true);                    // (the rules below hide what the chosen method does not use)
        updateSurround();
        const bool ready = built && ! running;
        const bool cons = consonantMethod();
        const bool att = methodBox.getSelectedId() == 1 || cons;
        for (auto* c : std::initializer_list<juce::Component*> { &dirBox, &surround, &surroundCap }) c->setVisible (! cons);      // a stray consonant: always the sound before / after, its own search length
        weightCap.setText (cons ? "Side (50 = auto)" : "Before <-> After", juce::dontSendNotification);
        bandsBox.setVisible (att); multiBtn.setVisible (att); linkBtn.setVisible (att);
        weight.setVisible (lookingAcross()); weightCap.setVisible (lookingAcross());
        for (int t = 0; t < kTabCount; ++t) if (t != tab) for (auto* c : tabControls (t)) c->setVisible (false);      // only the tab that is showing
        for (auto* b : std::initializer_list<juce::Button*> { &startBtn, &rewBtn, &playBtn, &fwdBtn, &endBtn, &loopBtn }) b->setEnabled (ready);
        clock.setEnabled (ready);
        gainBtn.setEnabled (ready && view.hasSelection()); retouchBtn.setEnabled (ready && view.hasSelection()); declickBtn.setEnabled (ready);
        learnBtn.setEnabled (ready && view.hasSelection()); denoiseBtn.setEnabled (ready && view.hasSelection() && haveRoomTone());
        revertBtn.setEnabled (ready && ! steps.empty()); finishBtn.setEnabled (ready && ! steps.empty());
        roomCap.setText (roomShortText(), juce::dontSendNotification); roomCap.setTooltip (roomText());
        juce::String t;
        if (view.hasSelection()) t << "Box: " << juce::String (view.selT0(), 2) << " - " << juce::String (view.selT1(), 2) << " s, " << (int) view.selF0() << " - " << (int) view.selF1() << " Hz.  ";
        t << juce::String ((int) steps.size()) << " fix(es) not yet written back";
        info.setText (t, juce::dontSendNotification);
        resized();                                                               // (what is shown decides how the controls are packed)
    }
    // ---- the picture: the pyramid (built once), slices of it for what is in view, and the sharp picture of a zoomed-in view read from the files
    void buildPicture()
    {
        pyramid = std::make_shared<SpectralPyramid>();
        auto py = pyramid; auto parts = cur; const double sr = rate; const double secs = seconds();
        status.setText ("Building the picture of the whole edit (" + juce::String (secs / 60.0, 1) + " min, " + juce::String ((int) parts->ids.size()) + " tracks)... you can wait, or close and come back.", juce::dontSendNotification);
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        pyramidBusy = true;
        runJob ([py, parts, sr, secs] (AudioJob& j) -> juce::String
                {
                    if (parts->ids.empty()) return "There is no audio in this edit.";
                    py->setUp (sr, secs);
                    const int frames = py->levels[0].frames;
                    ReaderCache cache;
                    for (size_t i = 0; i < parts->parts.size(); ++i)
                    {
                        const auto& pp = parts->parts[i];
                        const bool ok = py->addTrack (0, frames, [&pp, &cache] (juce::int64 start, int n, float* dst) { readTrackMix (pp, start, n, dst, &cache); }, i == 0,
                                                      [&j, &i, &parts] { j.setProgress ((float) i / (float) parts->parts.size()); return j.cancelled(); });
                        if (! ok) return "Cancelled.";
                    }
                    py->settle();
                    return {};
                },
                [self] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->pyramidBusy = false;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); return; }
                    self->built = true;
                    self->sliceForView();
                    self->status.setText ("Ready. Space plays from the red playhead (or the left of the page) to the end of the edit, and the page turns by itself (arrows zoom: right / left in time, down / up in pitch; click the ruler to place the playhead, drag it to move, double-click it for the whole edit). "
                                          "Draw a box and press G (gain), R (ReTouch), D (de-click) or N (denoise, with the project's room tone - or draw a box over room only and press L first). Each fix goes into the edit at once; Undo takes it off; Write back keeps them.", juce::dontSendNotification);
                    self->refreshControls();
                });
    }
    /** The quick picture for what is in view: a slice of the best level of the pyramid (the whole picture is far too big to hand over at once). */
    void sliceForView()
    {
        if (pyramidBusy || pyramid == nullptr || ! pyramid->valid()) return;         // (busy first: while the job builds the pyramid its levels are being filled)
        const double t0 = juce::jmax (0.0, view.visT0()), t1 = juce::jmin (seconds(), view.visT1());
        if (t1 <= t0) return;
        double a = 0.0, b = 0.0;
        auto s = pyramid->slice (t0, t1, 2400, a, b);
        view.setDataRange (std::move (s), a, b, seconds());
    }
    /** Makes the quick picture again over [t0, t1] seconds (after a fix): the frames of that stretch, every track, read from the pieces as they are now. */
    void refreshPicture (double t0, double t1)
    {
        if (pyramid == nullptr || ! pyramid->valid()) return;
        auto py = pyramid; auto parts = cur;
        const int f0 = juce::jmax (0, py->frameAt (t0 - 1.0)), f1 = juce::jmin (py->levels[0].frames, py->frameAt (t1 + 1.0) + 1);
        pyramidBusy = true;
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        runJob ([py, parts, f0, f1] (AudioJob& j) -> juce::String
                {
                    ReaderCache cache;
                    for (size_t i = 0; i < parts->parts.size(); ++i)
                    {
                        const auto& pp = parts->parts[i];
                        if (! py->addTrack (f0, f1, [&pp, &cache] (juce::int64 start, int n, float* dst) { readTrackMix (pp, start, n, dst, &cache); }, i == 0, [&j] { return j.cancelled(); })) return "Cancelled.";
                    }
                    py->settle (f0, f1);
                    return {};
                },
                [self] (const juce::String&)
                {
                    if (self == nullptr) return;
                    self->pyramidBusy = false;
                    self->sliceForView(); self->view.invalidateDetail();
                    self->refreshControls();
                });
        refreshControls();                                                   // (the fixes and Undo wait until the picture is made again)
    }
    void startDetail (const PictureRequest& rq)
    {
        if (! built || cur == nullptr) return;
        if (rq.t1 - rq.t0 > kDetailMaxSeconds) return;                               // zoomed far out: the quick picture is all there is (the sharp one would read the whole edit)
        if (detailCancel != nullptr) detailCancel->store (true);
        auto cancel = std::make_shared<std::atomic<bool>> (false); detailCancel = cancel;
        auto parts = cur; const double sr = rate; juce::Component::SafePointer<NoiseModeDialog> self (this);
        {   // every track of the view is read into memory: a long view of many tracks is left to the quick picture (36 tracks of 3 minutes at 96 kHz were 2.5 GB)
            const double samples = (rq.t1 - rq.t0 + 1.0) * sr * (double) parts->parts.size();
            if (samples * sizeof (float) > 512.0 * 1024.0 * 1024.0) return;
        }
        std::thread ([parts, rq, sr, cancel, self]
        {
            try                                                                  // (nothing may escape a thread: out of memory leaves the quick picture, nothing worse)
            {
                const double margin = 0.5;
                const juce::int64 s0 = juce::jmax<juce::int64> (0, (juce::int64) std::llround ((rq.t0 - margin) * sr)), s1 = (juce::int64) std::llround ((rq.t1 + margin) * sr);
                std::vector<std::vector<float>> tracks (parts->parts.size());
                ReaderCache cache;
                for (size_t i = 0; i < parts->parts.size(); ++i)
                {
                    if (cancel->load()) return;                                  // (a newer view, or the window closed: let go before reading more)
                    tracks[i].assign ((size_t) juce::jmax<juce::int64> (0, s1 - s0), 0.0f);
                    readTrackMix (parts->parts[i], s0, s1 - s0, tracks[i].data(), &cache);
                }
                const double off = (double) s0 / sr;
                auto pic = picture::render (tracks, sr, rq.t0 - off, rq.t1 - off, rq.w, rq.h, rq.axis, rq.opt, [cancel] { return cancel->load(); });
                pic.id = rq.id; pic.t0 = rq.t0; pic.t1 = rq.t1;
                if (cancel->load()) return;
                juce::MessageManager::callAsync ([self, p = std::make_shared<DetailPicture> (std::move (pic))] { if (self != nullptr) self->view.setDetail (std::move (*p)); });
            }
            catch (...) {}
        }).detach();
    }
    void openSpectroSettings()
    {
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        launchDialog (std::make_unique<SpectroSettingsDialog> (view.settings(), [self] (const SpectroSettings& n)
        {
            if (self == nullptr) return;
            self->view.setSettings (n); n.save (gSpecPrefs);
            if (gSpecPrefs != nullptr) gSpecPrefs->saveIfNeeded();
        }), "Spectrogram settings", this);
    }
    // ---- playing: from the playhead to the end of the whole edit (the page turns with it); Loop repeats the page in view
    /** Where the playhead is: the running one, else where Play would start. */
    double transportPos() const { return isPlayingNow() ? juce::jmax (0.0, app.playheadSeconds()) : view.startPoint(); }
    void playRange (double from)
    {
        const bool loop = loopBtn.getToggleState();
        from = juce::jlimit (0.0, seconds(), from);
        if (loop) { if (from < view.visT0() || from >= view.visT1()) from = view.visT0(); }
        else if (from >= seconds() - 0.01) { from = 0.0; view.placeCue (0.0); view.showTime (0.0); }        // (nothing left to play from the very end: start again)
        lastPlayTo = loop ? juce::jmin (seconds(), view.visT1()) : seconds();
        lastTickPos = from;
        const auto err = app.playEdit (eid, from, lastPlayTo, loop);
        if (err.isNotEmpty()) status.setText (err, juce::dontSendNotification);
    }
    void playPressed() { if (! built || running) return; app.stopPlayback(); playRange (view.startPoint()); }
    /** The playhead goes to t (the page turns to show it); if it was playing it carries on from there. */
    void goTo (double t)
    {
        if (! built || running) return;
        t = juce::jlimit (0.0, seconds(), t);
        const bool wasPlaying = isPlayingNow();
        view.placeCue (t); view.showTime (t);
        if (wasPlaying)
        {
            app.stopPlayback();
            if (t < seconds() - 0.01) playRange (t);
        }
        clock.setPosition (t);
    }
    void jumpBy (double seconds_) { goTo (transportPos() + seconds_); }
    /** The edit changed under a running playback (a fix went in or came out): it starts again from where it was. */
    void restartIfPlaying()
    {
        if (! isPlayingNow()) return;
        const double pos = app.playheadSeconds();
        app.stopPlayback();
        if (loopBtn.getToggleState()) { const auto err = app.playEdit (eid, view.startPoint(), lastPlayTo, true); if (err.isNotEmpty()) status.setText (err, juce::dontSendNotification); }
        else if (pos < lastPlayTo) { const auto err = app.playEdit (eid, pos, lastPlayTo, false); if (err.isNotEmpty()) status.setText (err, juce::dontSendNotification); }
    }
    // ---- the fixes: each is made on the job thread from the pieces as they are now, then put into the edit (the next one works on the result)
    using Prepare = std::function<juce::String (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j)>;
    void runFix (const juce::String& label, const juce::String& suffix, double t0, double t1, Prepare prepare)
    {
        auto* e = edit(); if (e == nullptr || running || ! built) return;
        auto regions = std::make_shared<std::vector<EditRegion>> (e->regions);
        auto jobs = std::make_shared<std::vector<RegionJob>>();
        status.setText (label + "...", juce::dontSendNotification);
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        refreshControls();
        for (auto* b : { &gainBtn, &retouchBtn, &declickBtn, &learnBtn, &denoiseBtn, &revertBtn, &finishBtn }) b->setEnabled (false);
        runJob ([regions, jobs, prepare] (AudioJob& j) { return prepare (*regions, *jobs, j); },
                [self, regions, jobs, label, suffix, t0, t1] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    auto deleteFiles = [jobs] { for (auto& rj : *jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs->clear(); };
                    if (err.isNotEmpty()) { deleteFiles(); self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    auto* ed = self->edit();
                    if (ed == nullptr) { deleteFiles(); return; }
                    Step st; st.before = ed->regions; st.jobs = std::move (*jobs); st.label = label; st.t0 = t0; st.t1 = t1;
                    if (! applyRegionJobs (*ed, st.before, st.jobs, suffix))
                    {
                        ed->regions = st.before; for (auto& rj : st.jobs) for (auto& r : rj.results) r.file.deleteFile();
                        self->status.setText ("The fix could not be put into the edit (" + gApplyWhy + ").", juce::dontSendNotification); self->refreshControls(); return;
                    }
                    self->steps.push_back (std::move (st));
                    self->app.project.changed();
                    self->cur = std::make_shared<TrackParts> (buildTrackParts (*ed, 0, self->length, self->only));
                    self->restartIfPlaying();
                    self->status.setText (label + " done and in the edit (" + juce::String ((int) self->steps.size()) + " fix(es) not yet written back). Play to listen; Undo takes it off; Write back to clip keeps it.", juce::dontSendNotification);
                    self->refreshControls();
                    self->refreshPicture (t0, t1);
                },
                [jobs] (const juce::String&) { for (auto& rj : *jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs->clear(); });   // (the window was closed meanwhile: its new files go)
    }
    FixSpec repairSpec (bool declick) const
    {
        FixSpec spec;
        if (declick) { spec.kind = FixSpec::Kind::Declick; spec.sensitivity = sens.getValue(); return spec; }
        spec.kind = FixSpec::Kind::Patch; spec.f0 = view.selF0(); spec.f1 = view.selF1(); spec.sensitivity = sens.getValue();
        spec.repair.mode = methodBox.getSelectedId() == 2 ? RepairMode::Replace : RepairMode::Attenuate;
        spec.repair.consonant = consonantMethod();
        spec.repair.bands = bandsBox.getSelectedId(); spec.repair.multiRes = multiBtn.getToggleState(); spec.repair.linkChannels = linkBtn.getToggleState();
        spec.repair.strength = strength.getValue() / 100.0;
        spec.repair.direction = spec.repair.consonant ? RepairDirection::LeftRight : (RepairDirection) (dirBox.getSelectedId() - 1);
        spec.repair.contextPct = surround.getValue(); spec.repair.weighting = weight.getValue() / 100.0;
        return spec;
    }
    void retouch (bool declick)
    {
        if (! declick && ! view.hasSelection()) return;
        juce::int64 t0 = 0, t1 = length;
        if (view.hasSelection()) { t0 = (juce::int64) std::llround (view.selT0() * rate); t1 = (juce::int64) std::llround (view.selT1() * rate); }
        const auto spec = repairSpec (declick);
        const RepairOp op { spec, t0, t1 };
        const auto folder = fixFolder; const double sr = rate; const auto len = length; const auto onlyT = only;
        runFix (declick ? "De-click" : "ReTouch", declick ? "declick" : "repair", (double) t0 / rate, (double) t1 / rate,
                [folder, sr, len, onlyT, op] (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j) { return prepareRepairJobs (folder, regions, 0, len, sr, { op }, onlyT, jobs, j); });
    }
    void openGain()
    {
        if (! built || running || ! view.hasSelection()) return;
        struct GainPop : public juce::Component
        {
            juce::Slider sl; juce::TextButton apply { "Apply" }, cancel { "Cancel" }; juce::Label cap;
            std::function<void (double)> onApply;
            GainPop (double start)
            {
                cap.setText ("Gain of the box", juce::dontSendNotification); cap.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (cap);
                sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (-60.0, 18.0, 0.1); sl.setValue (start, juce::dontSendNotification);
                sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22); sl.setDoubleClickReturnValue (true, 0.0);
                sl.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : (v > 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; };
                sl.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.retainCharacters ("-+0123456789.").getDoubleValue(); };
                sl.setSkewFactorFromMidPoint (-6.0);
                addAndMakeVisible (sl); addAndMakeVisible (apply); addAndMakeVisible (cancel);
                apply.setColour (juce::TextButton::buttonColourId, theme::accent);
                apply.onClick = [this] { const double v = sl.getValue(); auto cb = onApply; if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); if (cb) cb (v); };
                cancel.onClick = [this] { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
                setSize (380, 92);
            }
            void resized() override
            {
                auto r = getLocalBounds().reduced (8);
                cap.setBounds (r.removeFromTop (20)); sl.setBounds (r.removeFromTop (28)); r.removeFromTop (6);
                cancel.setBounds (r.removeFromRight (80)); r.removeFromRight (6); apply.setBounds (r.removeFromRight (80));
            }
        };
        auto pop = std::make_unique<GainPop> (uiPrefs().getDoubleValue (prefKey ("gainDb"), -6.0));
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        pop->onApply = [self] (double db)
        {
            if (self == nullptr || ! self->view.hasSelection()) return;
            uiPrefs().setValue (self->prefKey ("gainDb"), db);
            if (std::abs (db) < 0.05) { self->status.setText ("Gain 0 dB: nothing to change.", juce::dontSendNotification); return; }
            auto spec = self->repairSpec (false);
            spec.repair.mode = RepairMode::Gain; spec.repair.gainDb = db <= -59.95 ? -200.0 : db; spec.repair.strength = 1.0; spec.repair.consonant = false;
            const juce::int64 t0 = (juce::int64) std::llround (self->view.selT0() * self->rate), t1 = (juce::int64) std::llround (self->view.selT1() * self->rate);
            const RepairOp op { spec, t0, t1 };
            const auto folder = self->fixFolder; const double sr = self->rate; const auto len = self->length; const auto onlyT = self->only;
            self->runFix ("Gain", "gain", (double) t0 / sr, (double) t1 / sr,
                          [folder, sr, len, onlyT, op] (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j) { return prepareRepairJobs (folder, regions, 0, len, sr, { op }, onlyT, jobs, j); });
        };
        juce::CallOutBox::launchAsynchronously (std::move (pop), gainBtn.getScreenBounds(), nullptr);
    }
    bool haveRoomTone() const { return rtBoxT1 > rtBoxT0 || projectRT->valid(); }
    /** The same in a few words, for the little caption in the Denoise box (the full sentence is its tooltip). */
    juce::String roomShortText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s";
        if (projectRT->valid()) return "Room tone: the project's";
        return "Room tone: none yet";
    }
    juce::String roomText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: the box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s";
        if (projectRT->valid()) return "Room tone: the project's (" + projectRT->source + ")";
        return "Room tone: none yet - draw a box over room only and press L, or use the edit window's Room tone button";
    }
    /** L: the box is the room tone (learned from the files under it when a denoise is made). */
    void roomFromBox()
    {
        if (! built || running || ! view.hasSelection()) return;
        rtBoxT0 = view.selT0(); rtBoxT1 = view.selT1();
        if (rtBoxT1 - rtBoxT0 < 0.5) { status.setText ("The room-tone box is too short: make it at least half a second long (a second or more is better).", juce::dontSendNotification); rtBoxT0 = rtBoxT1 = -1.0; refreshControls(); return; }     // (what learnDenoise needs: a shorter box was taken here, then refused at Write back)
        rtParts = cur;
        status.setText (roomText() + ". Now draw a box round the noise to take out and press Denoise (N).", juce::dontSendNotification);
        refreshControls();
    }
    /** N: the box denoised - its time span and pitch range, every track - with the room tone, straight into the edit. */
    void denoise()
    {
        if (! built || running || ! view.hasSelection() || ! haveRoomTone() || cur == nullptr) return;
        FixSpec spec; spec.kind = FixSpec::Kind::Denoise; spec.f0 = view.selF0(); spec.f1 = view.selF1();
        spec.dn.amountDb = amount.getValue(); spec.dn.residualDb = residual.getValue();
        spec.dnBoxT0 = rtBoxT1 > rtBoxT0 ? rtBoxT0 : -1.0; spec.dnBoxT1 = rtBoxT1 > rtBoxT0 ? rtBoxT1 : -1.0;
        const juce::int64 t0 = (juce::int64) std::llround (view.selT0() * rate), t1 = (juce::int64) std::llround (view.selT1() * rate);
        const RepairOp op { spec, t0, t1 };
        const auto folder = fixFolder; const double sr = rate; const auto len = length; const auto onlyT = only; auto prt = projectRT;
        auto parts = rtBoxT1 > rtBoxT0 && rtParts != nullptr ? rtParts : cur;      // (the room-tone box is learned from the sound as it was when it was marked)
        runFix ("Denoise", "denoise", (double) t0 / rate, (double) t1 / rate,
                [folder, sr, len, onlyT, op, parts, prt] (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j)
                {
                    // the jobs are made (the files not yet): their room-tone profiles first, then the files
                    std::vector<RepairOp> ops { op };
                    auto err = prepareRepairJobs (folder, regions, 0, len, sr, ops, onlyT, jobs, j, false);
                    if (err.isNotEmpty()) return err;
                    err = attachRoomTone (jobs, ops, regions, *parts, prt, sr, j);
                    if (err.isNotEmpty()) { for (auto& rj : jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs.clear(); return err; }
                    return runRepairJobs (jobs, j);
                });
    }
    void revert()
    {
        auto* e = edit(); if (e == nullptr || steps.empty() || running) return;
        auto st = std::move (steps.back()); steps.pop_back();
        e->regions = st.before;
        for (auto& rj : st.jobs) for (auto& r : rj.results) r.file.deleteFile();
        app.project.changed();
        cur = std::make_shared<TrackParts> (buildTrackParts (*e, 0, length, only));
        if (rtParts != nullptr)                                                 // the room-tone box marked on the sound of this fix: its files are gone, so the sound as it is now
        {
            bool gone = false;
            for (auto& tps : rtParts->parts) for (auto& pt : tps) for (auto& rj : st.jobs) for (auto& r : rj.results) gone = gone || pt.file == r.file;
            if (gone) rtParts = cur;
        }
        restartIfPlaying();
        status.setText (st.label + " taken off again (" + juce::String ((int) steps.size()) + " fix(es) left).", juce::dontSendNotification);
        refreshPicture (st.t0, st.t1);
    }
    /** Write back to clip: what has been done stays in the edit (one Undo fix step back to 'base'); the window carries on. */
    void writeBack()
    {
        if (running || ! keepSteps()) return;
        status.setText ("Written back: the fixes are in the edit (Undo fix in the edit window brings back the audio as it was). Carry on, or press Finished.", juce::dontSendNotification);
        refreshControls();
    }
    bool keepSteps()
    {
        auto* e = edit(); if (e == nullptr || steps.empty()) return false;
        for (auto& st : steps) for (auto& rj : st.jobs) rj.results.clear();
        setUndoForEdit (app, eid, base, "noise mode (" + juce::String ((int) steps.size()) + " fixes)");
        base = e->regions; steps.clear();
        app.project.changed();
        return true;
    }
public:
    /** Closing (the X, Escape or Finished) with fixes not yet written back asks first: keep them (as Write back to clip does), throw them away, or stay
        (they used to go without a word - Escape is easily pressed). False when it has asked: the answer closes the window. */
    bool mayClose()
    {
        if (steps.empty()) return true;
        const int n = (int) steps.size();
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Noise mode")
                                          .withMessage (juce::String (n) + (n == 1 ? " fix has" : " fixes have") + " not been written back to the clip yet. Keep "
                                                        + (n == 1 ? "it" : "them") + " in the edit, or throw " + (n == 1 ? "it" : "them") + " away?")
                                          .withButton (n == 1 ? "Keep it" : "Keep them").withButton (n == 1 ? "Throw it away" : "Throw them away").withButton ("Cancel"),
                                      [self] (int r)
                                      {
                                          if (self == nullptr || r == 0) return;
                                          if (r == 1) self->keepSteps();                  // (a fix still being made when the window closes is not kept)
                                          closeDialogOf (self.getComponent());            // (the ones not kept are thrown away as it closes)
                                      });
        return false;
    }
private:
    void discardUncommitted()
    {
        if (steps.empty()) return;
        if (auto* e = edit()) { e->regions = base; app.project.changed(); }
        for (auto& st : steps) for (auto& rj : st.jobs) for (auto& r : rj.results) r.file.deleteFile();
        steps.clear();
    }
    static constexpr double kDetailMaxSeconds = 180.0;
    AppContext& app; juce::Uuid eid; double rate = 48000.0; juce::int64 length = 0; std::vector<juce::Uuid> only; juce::File fixFolder;
    InfoNote intro; juce::Label info, sensCap, strengthCap, surroundCap, weightCap, amountCap, residualCap, roomCap;
    SpectrogramView view; juce::ComboBox methodBox, dirBox, bandsBox, scaleBox; juce::ToggleButton multiBtn { "Multi-resolution" }, linkBtn { "Link stereo" };
    juce::TextButton specBtn { "Spectrogram settings..." };
    juce::Slider sens, strength, surround, weight, amount, residual;
    IconButton startBtn { "Start" }, rewBtn { "Rewind" }, playBtn { "Play" }, fwdBtn { "Forward" }, endBtn { "End" };
    juce::ToggleButton loopBtn { "Loop" };
    TimeReadout clock;
    double lastTickPos = -1.0;                                          // where the playhead was at the last tick (for the page turn)
    juce::TextButton gainBtn { "Gain [G]" }, retouchBtn { "ReTouch [R]" }, declickBtn { "De-click [D]" }, learnBtn { "Room tone [L]" }, denoiseBtn { "Denoise [N]" };
    juce::TextButton revertBtn { "Undo" }, finishBtn { "Write back to clip" }, doneBtn { "Finished de-noising" };
    std::shared_ptr<std::atomic<bool>> detailCancel;
    std::shared_ptr<SpectralPyramid> pyramid; bool pyramidBusy = false, built = false;
    std::shared_ptr<TrackParts> cur;                                    // the tracks and their pieces as the edit is now (replaced after every fix)
    std::shared_ptr<TrackParts> rtParts;                                // ... as they were when the room-tone box was marked (L): it is learned from that sound - once the
                                                                        // room tone itself had been denoised, a profile learned from it again was some 12 dB too low
    std::vector<EditRegion> base;                                       // the edit as it was when the window opened, or at the last Write back
    std::vector<Step> steps;                                            // the fixes since, each with the state before it (for Undo)
    std::shared_ptr<const Project::RoomTone> projectRT;                 // the project's room tone as it was when the window opened
    double rtBoxT0 = -1.0, rtBoxT1 = -1.0;                              // a box of the picture marked as the room tone (seconds), else the project's
    double lastPlayTo = 0.0;
};

void noiseModeEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "Noise mode", parent, [ap = &app, eid] (juce::Component* p) { noiseModeEdit (*ap, eid, p); })) return;
    const juce::String nm = "Noise mode";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    juce::int64 end = 0; for (auto& r : e->regions) end = juce::jmax (end, r.endSample());
    if (end < (juce::int64) (1.0 * rate)) { say (nm, "There is no audio in this edit yet."); return; }
    const auto only = e->fixTracks;
    const auto tp = buildTrackParts (*e, 0, end, only);
    if (tp.ids.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in the edit."); return; }
    const juce::String what = juce::String (only.empty() ? "All " : "The ") + juce::String ((int) tp.ids.size()) + " tracks of the whole edit (" + juce::String ((double) end / rate / 60.0, 1) + " min, " + juce::String (tp.pieces) + " pieces) in one picture, built once at several zoom levels. "
        "Gain, ReTouch, De-click and Denoise work on the box you draw (as in the ReTouch window); Denoise uses the project's room tone (the edit window's Room tone button), or a box of this picture marked with L. "
        "Every fix goes into the edit the moment it is made, as new piece files (the originals are never touched), and the next fix works on the result; Play hears them at once. "
        "Play carries on from the red playhead to the end of the edit and the page turns by itself (Loop repeats the page you can see); the transport buttons, the clock (double-click to type a time) and the Home / End keys work as in the edit window. "
        "Undo takes the last one off. Write back to clip keeps what has been done (one Undo fix step in the edit window) and the window stays open; Finished closes it (if some fixes are not written back yet, it asks whether to keep them).";
    gSpecPrefs = app.props.getUserSettings();
    auto dlg = std::make_unique<NoiseModeDialog> (app, eid, rate, end, only, what);
    juce::Component::SafePointer<NoiseModeDialog> sp (dlg.get());
    launchDialog (std::move (dlg), nm, parent, true, [sp] { return sp == nullptr || sp->mayClose(); });
}

// ---- the pitch curve (draw the pitch against time, audition it, accept or revert)
struct EditCurveState { std::vector<RegionJob> jobs; std::vector<EditRegion> old; PreviewApplied applied; };

static void openPitchCurve (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 tIn, juce::int64 tOut, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot);

void pitchCurveEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "The pitch curve", parent, [ap = &app, eid] (juce::Component* p) { pitchCurveEdit (*ap, eid, p); })) return;
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch curve", "Stop recording first."); return; }
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say ("Pitch curve", "First mark the part with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key. The pitch line is drawn between the two marks."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) ++pieces;
    if (pieces == 0) { say ("Pitch curve", "There is no audio between the marks."); return; }
    StreamSel outS, inS;
    const int sm = detectStreams (*e, tIn, tOut, outS, inS, "Pitch curve");
    if (sm < 0) return;
    if (sm == 0) { openPitchCurve (app, eid, parent, tIn, tOut, StreamSel(), nullptr, -1); return; }
    auto pair = makePair (app, eid, "Pitch curve", "pitch curve");
    openPitchCurve (app, eid, parent, tIn, tOut, outS, pair, 0);
    openPitchCurve (app, eid, parent, tIn, tOut, inS, pair, 1);
    pair->focus (0);
}

static void openPitchCurve (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 tIn, juce::int64 tOut, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot)
{
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut && (! ss.on || ss.has (r.id))) ++pieces;
    const double fixIn = e->fixIn, fixOut = e->fixOut;
    CurveContext c; c.allowPad = true; c.rate = rate; c.length = tOut - tIn;
    const juce::String lead = ss.on ? ss.label + ": the sound of the " + (slot == 0 ? "take that plays up to the edit point, carried on past it" : "take that plays from the edit point, started before it")
                                      + ", on its own. Draw its line here, the other stream's line in the other window, then Accept in BOTH; the edit point and its fades stay exactly where they are. " : juce::String();
    c.what = lead + "Draw how the pitch should change along the audio between marks 1 and 2 (" + juce::String (fixOut - fixIn, 2) + " s, " + juce::String (pieces) + " piece(s)), on " + (e->fixTracks.empty() ? juce::String ("every track") : juce::String ((int) e->fixTracks.size()) + " chosen track(s)") + ". "
             "The middle line is no change; up raises the pitch (+100 cents at the top), down lowers it. Press Preview (key R) to hear it, then Accept or Revert. "
             "The original files are never touched.";
    auto st = std::make_shared<EditCurveState>(); st->applied.app = &app;
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    const auto only = e->fixTracks;
    c.load = [regionsCopy, tIn, tOut, only, ss] (AudioJob& j)
    {
        std::vector<float> mix ((size_t) (tOut - tIn), 0.0f);
        juce::int64 prevHi = tIn;
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut || r.files.empty()) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            if (j.cancelled()) break;
            juce::int64 from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            if (ss.on) { ss.span (r, tIn, tOut, from, to); from = juce::jmax (from, prevHi); if (to > from) prevHi = to; }      // a stream: its own take, carried on past the edit (out) / started before it (in)
            if (to <= from) continue;
            const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                auto rd = audioops::openReader (formats(), f.file); if (rd == nullptr) continue;
                juce::int64 rel = a - f.fileStart, n = b - a, dst = from - tIn;
                if (rel < 0) { n += rel; dst -= rel; rel = 0; }
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*rd, rel, n, ch);
                for (auto& x : ch) for (size_t k = 0; k < x.size() && (size_t) dst + k < mix.size(); ++k) mix[(size_t) dst + k] += x[k];
            }
        }
        std::vector<std::vector<float>> out; out.push_back (std::move (mix)); return out;
    };
    c.prepare = [fixFolder, regionsCopy, tIn, tOut, rate, st, only, ss] (const FixSpec& spec, double padSec, AudioJob& j) -> juce::String
    {
        st->jobs.clear();
        const auto pad = (juce::int64) std::llround (padSec * rate);
        juce::int64 prevHi = tIn;
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            juce::int64 lo = from, hi = to;                                  // a stream also corrects the part of its take past the edit point, which its crossfade uses
            if (ss.on) { ss.span (r, tIn, tOut, lo, hi); lo = juce::jmax (lo, prevHi); if (hi > lo) prevHi = hi; }
            if (to <= from) continue;
            lo = juce::jmin (lo, from); hi = juce::jmax (hi, to);
            FixSpec sj = spec; sj.curveZero = (double) pad - (double) (lo - tIn);          // curve time 0 is mark 1, wherever this piece starts
            RegionJob rj; const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            const auto fa = r.srcIn + (lo - r.startSample), fb = r.srcIn + (hi - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, fa - pad, fb + pad, fa - pad, fb + pad, sj, only);
            rj.curveZero = sj.curveZero;
            st->jobs.push_back (std::move (rj));
        }
        for (size_t i = 0; i < st->jobs.size(); ++i)
        {
            const auto err = runRegionJob (st->jobs[i], j, (float) i / (float) st->jobs.size(), (float) (i + 1) / (float) st->jobs.size());
            if (err.isNotEmpty()) { for (auto& done : st->jobs) for (auto& r : done.results) r.file.deleteFile(); st->jobs.clear(); return err; }
        }
        return {};
    };
    auto deleteFiles = [st] { for (auto& j : st->jobs) for (auto& r : j.results) r.file.deleteFile(); st->jobs.clear(); };
    if (pair != nullptr)
    {
        pair->jobsOf[slot] = [st] { return &st->jobs; };
        pair->suffix[slot] = "pitch curve";
        c.apply = [pair, slot, st] { if (st->jobs.empty()) return false; st->applied = true; return pair->put (slot, true); };
        c.revert = [pair, slot, st, deleteFiles] { if (st->applied) { st->applied = false; pair->put (slot, false); } deleteFiles(); };
        c.unapply = [pair, slot, st] { if (! st->applied) return; st->applied = false; pair->put (slot, false); };
        c.accept = [] {};
        c.pairWritten = [pair, slot] (std::function<bool()> commit) { pair->wrote (slot, std::move (commit)); };
    }
    else
    {
        c.apply = [&app, eid, st, deleteFiles]
        {
            auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || st->jobs.empty()) { deleteFiles(); return false; }
            st->old = e2->regions;
            const bool allOk = applyRegionJobs (*e2, st->old, st->jobs, "pitch curve");
            if (! allOk) { e2->regions = st->old; deleteFiles(); return false; }
            st->applied = true; app.project.changed();
            return true;
        };
        c.revert = [&app, eid, st, deleteFiles]
        {
            if (st->applied) { if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); } st->applied = false; }
            deleteFiles();
        };
        c.unapply = [&app, eid, st]
        {
            if (! st->applied) return;
            if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); }
            st->applied = false;                                  // (the corrected files are kept, ready to be put back)
        };
        c.accept = [&app, eid, st] { st->jobs.clear(); st->applied = false; setUndoForEdit (app, eid, st->old, "pitch curve"); app.project.changed(); };
    }
    c.play = [&app, eid, fixIn, fixOut] (double fromSec, double toSec, bool loop)
    {
        const auto err = app.playEdit (eid, fixIn + fromSec, juce::jmin (fixOut, fixIn + toSec), loop);          // the part in view (inside the marked part), nothing either side
        if (err.isNotEmpty()) say ("Pitch curve", err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, eid, fixIn]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - fixIn);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    auto dlg = withPitchTabs (std::make_unique<CurveDialog> (std::move (c)), pair != nullptr);
    if (pair != nullptr) launchStream (pair, slot, std::move (dlg), "Pitch - " + ss.label, parent);
    else launchDialog (std::move (dlg), "Pitch", parent, true);
}

/** The Pitch button of the edit window: Pitch curve first, Pitch process on the second tab. */
void pitchTabsEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "Pitch correction", parent, [ap = &app, eid] (juce::Component* p) { pitchTabsEdit (*ap, eid, p); })) return;
    gProcessPage.reset(); gMakeProcessPage = true; pitchEdit (app, eid, parent); gMakeProcessPage = false;
    if (gProcessPage == nullptr) return;                                   // (a message was shown: nothing is marked)
    pitchCurveEdit (app, eid, parent);
    gProcessPage.reset();
}

void undoLastFix (AppContext& app)
{
    if (! app.fixUndo) { say ("Undo fix", "There is no pitch correction, repair or denoise to undo."); return; }
    if (! app.fixUndoStillValid())
    {
        // the edit has moved on since the fix: putting its pieces back as they were before the fix would take those changes with it (3.1.4)
        const auto label = app.fixUndoLabel;
        app.fixUndo = nullptr;
        say ("Undo fix", "It has changed since the " + label + ", so Undo fix cannot take just that out any more. Ctrl+Z steps back through the changes, the " + label + " among them.");
        return;
    }
    auto f = std::move (app.fixUndo); app.fixUndo = nullptr;
    f();
}

}} // namespace td::fixtools
