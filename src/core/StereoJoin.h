#pragma once
// 3.23.0: a stereo track made from two mono tracks (the take window: select the two track names, right-click > Make a stereo track ...). Fermata's files
// are one per track per take, so the pair becomes one new stereo WAV per take (an exact copy of the samples; the mono files stay on the disk), and every
// take and every piece of an edit that used the two tracks uses the stereo track instead. The two mono tracks then leave the project.
// plan() works out what to write (message thread), writeFiles() writes it (a job thread), apply() makes the change (message thread). Ctrl+Z cannot step
// back past it (the tracks are gone), so apply() starts the undo history again.
#include "Project.h"
#include "AudioOps.h"
#include "Automation.h"
#include <limits>
#include <map>

namespace td { namespace stereojoin
{
/** One stereo file to write: the take range [from, to) from a left and a right mono file (each with the take sample its own sample 0 stands for; an empty
    File = silence on that side). A whole take: wantTo < 0, and it runs to the end of the longer file. A piece of an edit: [wantFrom, wantTo) is what the
    piece may need (itself, its fades and 2 s of handle each side), cut by the job to where either file has audio. */
struct FileJob
{
    juce::File left, right;
    juce::int64 leftStart = 0, rightStart = 0;
    juce::int64 wantFrom = 0, wantTo = -1;
    juce::File dest;
    juce::int64 from = 0, to = 0;                 // what was written (set by writeFiles)
};

/** A take whose two mono files become one stereo file. */
struct TakeUse { juce::Uuid window, take; juce::File left, right; size_t job = 0; };
/** A piece of an edit (or an overdub) that played the two tracks. */
struct PieceUse
{
    juce::Uuid edit, piece;
    bool hasLeft = false, hasRight = false;
    juce::File left, right; juce::int64 leftStart = 0, rightStart = 0;
    size_t job = 0;
};

struct Plan
{
    juce::Uuid left, right, stereo;               // the two mono tracks and the new stereo track (its id is chosen here)
    juce::String name;
    std::vector<FileJob> jobs;
    std::vector<TakeUse> takes;
    std::vector<PieceUse> pieces;
    std::vector<juce::Uuid> takeWindows, edits;   // the windows it changes
};

inline const TrackDef* trackOf (const Project& p, const juce::Uuid& id)
{
    for (auto& t : p.tracks) if (t.id == id) return &t;
    return nullptr;
}

/** A name for the stereo track: "Omni" from "Omni L" + "Omni R" (or "Omni Left" / "Omni-R" ...), otherwise both names ("Tr01 + Tr02"). */
inline juce::String suggestName (const juce::String& a, const juce::String& b)
{
    auto stripSide = [] (const juce::String& n, bool& had) -> juce::String
    {
        had = false;
        const auto t = n.trim();
        for (auto* suffix : { "Left", "Right", "L", "R" })
        {
            const juce::String s (suffix);
            if (t.length() > s.length() && t.endsWithIgnoreCase (s))
            {
                const auto rest = t.dropLastCharacters (s.length());
                const auto last = rest.getLastCharacter(), first = t[t.length() - s.length()];
                const bool apart = last == ' ' || last == '-' || last == '_' || last == '.';
                const bool camel = juce::CharacterFunctions::isLowerCase (last) && juce::CharacterFunctions::isUpperCase (first);       // "OmniL", "OmniLeft"
                if (apart || camel)
                {
                    had = true;
                    return rest.trimCharactersAtEnd (" -_.").trim();
                }
            }
        }
        return t;
    };
    bool ha = false, hb = false;
    const auto ra = stripSide (a, ha), rb = stripSide (b, hb);
    if (ha && hb && ra.isNotEmpty() && ra.equalsIgnoreCase (rb)) return ra;
    return a.trim() + " + " + b.trim();
}

/** Does this waiting part (Export for Processing / Edit in RX) involve one of the two tracks? ('files': the tracks of the take / piece it is in.) */
template <typename Files>
inline bool waitingTouches (const WaitingPiece& w, const Files& files, const juce::Uuid& a, const juce::Uuid& b)
{
    if (! w.active()) return false;
    if (w.tracks.empty()) { for (auto& f : files) if (f.trackId == a || f.trackId == b) return true; return false; }
    for (auto& t : w.tracks) if (t == a || t == b) return true;
    return false;
}

/** Works out the change. "" when it can be done (with 'out' filled in), else the reason it cannot, in plain words. */
inline juce::String plan (const Project& p, const juce::Uuid& leftId, const juce::Uuid& rightId, const juce::String& newName, Plan& out)
{
    out = Plan();
    const auto* L = trackOf (p, leftId); const auto* R = trackOf (p, rightId);
    if (L == nullptr || R == nullptr || leftId == rightId) return "Choose two different tracks.";
    if (L->isLtc || R->isLtc) return "The LTC (timecode) track cannot be part of a stereo track.";
    if (L->format != TrackFormat::Mono || R->format != TrackFormat::Mono) return "Both tracks must be mono: " + (L->format != TrackFormat::Mono ? L->name : R->name) + " is not.";
    const auto name = newName.trim();
    if (name.isEmpty()) return "Give the stereo track a name.";
    out.left = leftId; out.right = rightId; out.name = name; out.stereo = juce::Uuid();

    // nothing on the two tracks may be out being processed (its corrected files would come back to a track that is gone)
    for (auto& w : p.takeWindows)
        for (auto& g : w->groups)
            for (auto& wp : g.waiting)
                if (waitingTouches (wp, g.files, leftId, rightId))
                    return "Take " + w->displayName (g) + " in the take window '" + w->name + "' has a part waiting for corrected audio on these tracks (Export for Processing). "
                           "Re-link it, or stop waiting (right-click the block), first.";
    for (auto& e : p.edits)
        for (auto* list : { &e->regions, &e->overdubs })
            for (auto& r : *list)
                if (waitingTouches (r.waiting, r.files, leftId, rightId))
                    return "The edit '" + e->name + "' has a part on these tracks that is " + juce::String (r.waiting.rx ? "out in iZotope RX" : "waiting for corrected audio")
                           + " (" + r.waiting.name + "). Bring it back, or cancel it (right-click the block), first.";

    std::map<juce::String, size_t> wholeTake;                         // "left|right" -> the job of that pair of whole take files
    auto key = [] (const juce::File& a, const juce::File& b) { return a.getFullPathName() + "|" + b.getFullPathName(); };
    for (auto& w : p.takeWindows)
    {
        bool touched = false;
        for (auto& g : w->groups)
        {
            const TakeFile* fl = nullptr; const TakeFile* fr = nullptr;
            for (auto& f : g.files) { if (f.trackId == leftId) fl = &f; else if (f.trackId == rightId) fr = &f; }
            if (fl == nullptr && fr == nullptr) continue;
            if ((fl != nullptr && fl->numChannels != 1) || (fr != nullptr && fr->numChannels != 1))
                return "Take " + w->displayName (g) + " in '" + w->name + "' has a file on " + (fl != nullptr && fl->numChannels != 1 ? L->name : R->name) + " that is not mono.";
            FileJob j;
            if (fl != nullptr) j.left = fl->file;
            if (fr != nullptr) j.right = fr->file;
            j.dest = audioops::uniqueFile (p.takeFolder (*w).getChildFile (TakeWindowDef::fileNameFor (g.number, w->labelFor (g), name)));
            const auto k = key (j.left, j.right);
            size_t idx;
            if (auto it = wholeTake.find (k); it != wholeTake.end()) idx = it->second;      // (the same two files in two takes: written once)
            else { idx = out.jobs.size(); out.jobs.push_back (j); wholeTake[k] = idx; }
            out.takes.push_back ({ w->id, g.id, j.left, j.right, idx });
            touched = true;
        }
        if (touched) out.takeWindows.push_back (w->id);
    }
    const auto fixes = p.audioFolder().getChildFile ("Fixes");
    for (auto& e : p.edits)
    {
        bool touched = false;
        for (auto* list : { &e->regions, &e->overdubs })
            for (auto& r : *list)
            {
                const RegionFile* fl = nullptr; const RegionFile* fr = nullptr;
                for (auto& f : r.files) { if (f.trackId == leftId) fl = &f; else if (f.trackId == rightId) fr = &f; }
                if (fl == nullptr && fr == nullptr) continue;
                PieceUse u;
                u.edit = e->id; u.piece = r.id;
                u.hasLeft = fl != nullptr; u.hasRight = fr != nullptr;
                if (fl != nullptr) { u.left = fl->file; u.leftStart = fl->fileStart; }
                if (fr != nullptr) { u.right = fr->file; u.rightStart = fr->fileStart; }
                // the whole take files of a take being joined: the piece plays the take's new stereo file, from the same place
                const auto it = (u.leftStart == 0 && u.rightStart == 0) ? wholeTake.find (key (u.left, u.right)) : wholeTake.end();
                if (it != wholeTake.end()) u.job = it->second;
                else
                {
                    // anything else (a corrected or repaired piece, a piece with only one of the two): a stereo file of its own, as long as the piece may need
                    const double rate = r.sampleRate > 0 ? r.sampleRate : 48000.0;
                    const auto h = (juce::int64) std::llround (2.0 * rate);
                    FileJob j;
                    j.left = u.left; j.right = u.right; j.leftStart = u.leftStart; j.rightStart = u.rightStart;
                    j.wantFrom = r.srcIn + juce::jmin ((juce::int64) 0, (juce::int64) std::llround (r.inStart * rate)) - h;
                    j.wantTo = r.srcOut + juce::jmax ((juce::int64) 0, (juce::int64) std::llround (r.outEnd * rate)) + h;
                    j.dest = audioops::uniqueFile (fixes.getChildFile (sanitiseForFile (r.takeName.upToFirstOccurrenceOf (" (", false, false) + " - " + name) + " (stereo).wav"));
                    u.job = out.jobs.size(); out.jobs.push_back (j);
                }
                out.pieces.push_back (u);
                touched = true;
            }
        if (touched) out.edits.push_back (e->id);
    }
    if (out.takes.empty() && out.pieces.empty()) return "There is no audio on " + L->name + " or " + R->name + " in any take window or edit.";
    return {};
}

/** Writes every file of the plan (any thread). "" when all went well; otherwise what went wrong, and every file it had made is deleted again. */
inline juce::String writeFiles (juce::AudioFormatManager& fm, Plan& plan, AudioJob* job = nullptr)
{
    std::vector<juce::File> made;
    auto fail = [&] (const juce::String& e) { for (auto& f : made) f.deleteFile(); return e; };
    for (size_t i = 0; i < plan.jobs.size(); ++i)
    {
        if (job != nullptr) { if (job->cancelled()) return fail ("Cancelled."); job->setProgress ((float) i / (float) plan.jobs.size()); }
        auto& j = plan.jobs[i];
        juce::int64 lenL = 0, lenR = 0;
        if (j.left != juce::File())  { auto r = audioops::openReader (fm, j.left);  if (r == nullptr) return fail ("Could not read " + j.left.getFullPathName() + ".");  lenL = r->lengthInSamples; }
        if (j.right != juce::File()) { auto r = audioops::openReader (fm, j.right); if (r == nullptr) return fail ("Could not read " + j.right.getFullPathName() + "."); lenR = r->lengthInSamples; }
        if (j.wantTo < 0) { j.from = 0; j.to = juce::jmax (j.leftStart + lenL, j.rightStart + lenR); }
        else
        {
            juce::int64 lo = std::numeric_limits<juce::int64>::max(), hi = std::numeric_limits<juce::int64>::min();
            auto take = [&] (bool has, juce::int64 start, juce::int64 len)
            {
                if (! has) return;
                const auto a = juce::jmax (start, j.wantFrom), b = juce::jmin (start + len, j.wantTo);
                if (b > a) { lo = juce::jmin (lo, a); hi = juce::jmax (hi, b); }
            };
            take (j.left != juce::File(), j.leftStart, lenL);
            take (j.right != juce::File(), j.rightStart, lenR);
            if (hi <= lo) { lo = juce::jmax ((juce::int64) 0, j.wantFrom); hi = juce::jmax (lo + 1, j.wantTo); }     // (no audio there at all: silence, as it played)
            j.from = lo; j.to = hi;
        }
        if (j.to <= j.from) j.to = j.from + 1;
        juce::String err;
        if (! audioops::joinToStereo (fm, j.left, j.leftStart, j.right, j.rightStart, j.from, j.to, j.dest, err,
                                      [job] { return job == nullptr || ! job->cancelled(); }))
            return fail (err);
        made.push_back (j.dest);
    }
    return {};
}

/** Makes the change, once the files are written (message thread). False with 'why' - and nothing changed - when the project changed since the plan
    (the caller then deletes the files). */
inline bool apply (Project& p, const Plan& plan, juce::String& why)
{
    // still as planned?
    const auto* L = trackOf (p, plan.left); const auto* R = trackOf (p, plan.right);
    if (L == nullptr || R == nullptr) { why = "One of the two tracks has gone."; return false; }
    for (auto& j : plan.jobs) if (! j.dest.existsAsFile()) { why = "A new file is missing: " + j.dest.getFullPathName(); return false; }
    auto sameTake = [&] (const TakeUse& u, TakeGroup*& g) -> bool
    {
        auto* w = p.findTakeWindow (u.window); g = w != nullptr ? w->findGroup (u.take) : nullptr;
        if (g == nullptr) return false;
        juce::File l, r;
        for (auto& f : g->files) { if (f.trackId == plan.left) l = f.file; else if (f.trackId == plan.right) r = f.file; }
        return l == u.left && r == u.right;
    };
    auto samePiece = [&] (const PieceUse& u, EditRegion*& rg) -> bool
    {
        auto* e = p.findEdit (u.edit); rg = e != nullptr ? e->findAny (u.piece) : nullptr;
        if (rg == nullptr) return false;
        const RegionFile* fl = nullptr; const RegionFile* fr = nullptr;
        for (auto& f : rg->files) { if (f.trackId == plan.left) fl = &f; else if (f.trackId == plan.right) fr = &f; }
        if ((fl != nullptr) != u.hasLeft || (fr != nullptr) != u.hasRight) return false;
        if (fl != nullptr && (fl->file != u.left || fl->fileStart != u.leftStart)) return false;
        if (fr != nullptr && (fr->file != u.right || fr->fileStart != u.rightStart)) return false;
        return ! rg->waiting.active() || ! waitingTouches (rg->waiting, rg->files, plan.left, plan.right);
    };
    for (auto& u : plan.takes)  { TakeGroup* g = nullptr; if (! sameTake (u, g))    { why = "The takes changed while the files were being written."; return false; } }
    for (auto& u : plan.pieces) { EditRegion* r = nullptr; if (! samePiece (u, r)) { why = "An edit changed while the files were being written."; return false; } }

    // the new track, where the left one was: its colour, its spot-mic role, the inputs of both
    TrackDef S;
    S.id = plan.stereo; S.name = plan.name; S.format = TrackFormat::Stereo;
    S.colour = L->colour; S.alignRole = L->alignRole;
    S.inputs[0] = L->inputs[0]; S.inputs[1] = R->inputs[0];
    {
        size_t at = 0;
        for (size_t i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].id == plan.left) at = i;
        p.tracks.insert (p.tracks.begin() + (long) at, S);
    }

    // the takes: one stereo file where the two mono ones were (the left one's place)
    for (auto& u : plan.takes)
    {
        TakeGroup* g = nullptr; sameTake (u, g);
        int il = -1, ir = -1;
        for (int i = 0; i < (int) g->files.size(); ++i) { if (g->files[(size_t) i].trackId == plan.left) il = i; else if (g->files[(size_t) i].trackId == plan.right) ir = i; }
        TakeFile tf;
        tf.trackId = plan.stereo; tf.trackName = plan.name; tf.file = plan.jobs[u.job].dest; tf.numChannels = 2;
        if (il >= 0) tf.sourceName = g->files[(size_t) il].sourceName;
        if (ir >= 0) tf.sourceName2 = g->files[(size_t) ir].sourceName.isNotEmpty() ? g->files[(size_t) ir].sourceName : g->files[(size_t) ir].file.getFileName();
        if (il >= 0 && tf.sourceName.isEmpty() && ir >= 0) tf.sourceName = g->files[(size_t) il].file.getFileName();      // (both names kept, for an AAF to find them again)
        const int keep = il >= 0 ? il : ir, drop = il >= 0 ? ir : -1;
        g->files[(size_t) keep] = tf;
        if (drop >= 0) g->files.erase (g->files.begin() + drop);
    }
    // the pieces of the edits: one stereo file in the left one's place; a volume change keeps the level of the two (their average, if they differed)
    for (auto& u : plan.pieces)
    {
        EditRegion* r = nullptr; samePiece (u, r);
        int il = -1, ir = -1;
        for (int i = 0; i < (int) r->files.size(); ++i) { if (r->files[(size_t) i].trackId == plan.left) il = i; else if (r->files[(size_t) i].trackId == plan.right) ir = i; }
        const auto& j = plan.jobs[u.job];
        RegionFile rf;
        rf.trackId = plan.stereo; rf.trackName = plan.name; rf.file = j.dest; rf.numChannels = 2; rf.fileStart = j.from;
        const int keep = il >= 0 ? il : ir, drop = il >= 0 ? ir : -1;
        for (auto& gc : r->gains)
        {
            auto dbOf = [&] (int i) { return juce::isPositiveAndBelow (i, (int) gc.db.size()) ? gc.db[(size_t) i] : 0.0f; };
            const float lvl = drop >= 0 ? 0.5f * (dbOf (keep) + dbOf (drop)) : dbOf (keep);
            if (juce::isPositiveAndBelow (keep, (int) gc.db.size())) gc.db[(size_t) keep] = lvl;
            if (drop >= 0 && juce::isPositiveAndBelow (drop, (int) gc.db.size())) gc.db.erase (gc.db.begin() + drop);
        }
        r->files[(size_t) keep] = rf;
        if (drop >= 0) r->files.erase (r->files.begin() + drop);
    }

    // every list that names the two tracks names the stereo one instead (where the left one was)
    auto replaceIn = [&] (std::vector<juce::Uuid>& v)
    {
        const auto il = std::find (v.begin(), v.end(), plan.left), ir = std::find (v.begin(), v.end(), plan.right);
        if (il == v.end() && ir == v.end()) return;
        const auto at = (il != v.end() ? il : ir) - v.begin();
        v.erase (std::remove_if (v.begin(), v.end(), [&] (const juce::Uuid& x) { return x == plan.left || x == plan.right; }), v.end());
        v.insert (v.begin() + juce::jmin ((long) at, (long) v.size()), plan.stereo);
    };
    for (auto& w : p.takeWindows)
    {
        const bool bothHidden = w->isHidden (plan.left) && w->isHidden (plan.right);
        w->hiddenTracks.erase (std::remove_if (w->hiddenTracks.begin(), w->hiddenTracks.end(), [&] (const juce::Uuid& x) { return x == plan.left || x == plan.right; }), w->hiddenTracks.end());
        if (bothHidden) w->hiddenTracks.push_back (plan.stereo);
        replaceIn (w->editTracks);
    }
    for (auto& e : p.edits)
    {
        replaceIn (e->trackIds);
        replaceIn (e->fixTracks);
        if (e->guide.trackId == plan.left || e->guide.trackId == plan.right) { e->guide.trackId = plan.stereo; e->guide.trackName = plan.name; }
        // automation: the left track's fader, input gain and sends carry on on the stereo track; its pan and plug-in lanes, and the right track's lanes, go
        e->lanes.erase (std::remove_if (e->lanes.begin(), e->lanes.end(), [&] (const AutoLane& l)
                        {
                            if (l.trackId == plan.right) return true;
                            return l.trackId == plan.left && ! (l.param == autoparam::fader || l.param == autoparam::gain || autoparam::isSend (l.param));
                        }), e->lanes.end());
        for (auto& l : e->lanes) if (l.trackId == plan.left) l.trackId = plan.stereo;
    }

    // the mixers: the stereo track's strip starts with the left track's fader, input gain, output, sends and VCAs (no plug-ins); hidden only if both were
    p.syncMixers();
    for (auto& m : p.mixers)
    {
        auto* a = m->stripFor (plan.left); auto* s = m->stripFor (plan.stereo);
        s->gainDb.set (a->gainDb.get()); s->inGainDb.set (a->inGainDb.get());
        s->outOn.set (a->outOn.get()); s->outDest = a->outDest;
        s->vcaMask.store (a->vcaMask.load());
        for (auto& snd : a->sends.pool)
            if (snd->active() && snd->dest != plan.left && snd->dest != plan.right)
            {
                auto* t = s->sends.get (snd->dest);
                t->gainDb.set (snd->gainDb.get()); t->pre.set (snd->pre.get()); t->followMute.set (snd->followMute.get()); t->mute.set (snd->mute.get());
            }
        const bool bothHidden = m->isStripHidden (plan.left) && m->isStripHidden (plan.right);
        m->hiddenStrips.erase (std::remove_if (m->hiddenStrips.begin(), m->hiddenStrips.end(), [&] (const juce::Uuid& x) { return x == plan.left || x == plan.right; }), m->hiddenStrips.end());
        if (bothHidden) m->hiddenStrips.push_back (plan.stereo);
    }

    // the two mono tracks leave the project (their files stay on the disk)
    p.tracks.erase (std::remove_if (p.tracks.begin(), p.tracks.end(), [&] (const TrackDef& t) { return t.id == plan.left || t.id == plan.right; }), p.tracks.end());
    p.structureChanged();
    p.undoReset();                                       // (Ctrl+Z cannot go back to the two tracks: they are gone)
    for (auto& id : plan.takeWindows) p.keepWindow (id);  // (nor can "keep / revert this window" at quit)
    for (auto& id : plan.edits) p.keepWindow (id);
    return true;
}
}} // namespace td::stereojoin
