#include "MasterRender.h"
#include "MasterExport.h"
#include "Playback.h"
#include <map>

namespace td
{
namespace
{
juce::CriticalSection& storeLock() { static juce::CriticalSection l; return l; }

void removeKeys (juce::var& v, std::initializer_list<const char*> keys)
{
    if (auto* o = v.getDynamicObject()) for (auto k : keys) o->removeProperty (juce::Identifier (k));
}
} // namespace

juce::String makeRenderParams (const Project& p, const MasteringDef& def, RenderParams& rp)
{
    rp = RenderParams();
    auto err = resolveRenderSource (p, def, rp.mixerId, rp.sourceId);
    rp.ownMixers = def.mixerId.isNull();
    rp.tailSeconds = def.tailSeconds;
    return err;
}

juce::String masterRenderFingerprint (const Project& cp, const EditDef& e, const RenderParams& rp)
{
    auto& p = const_cast<Project&> (cp);
    juce::var ev = p.editToVar (e);
    removeKeys (ev, { "name", "window", "insertIndex", "markIn", "markOut", "fixIn", "fixOut", "fixTracks", "playhead", "trackIds" });   // (where the cursor is does not change the sound)
    juce::String text = juce::JSON::toString (ev, true);

    // the audio files the pieces use: a file mended in place changes the sound without changing the edit
    auto addFiles = [&text] (const std::vector<EditRegion>& rs)
    {
        for (auto& r : rs)
            for (auto& f : r.files)
                text << "|" << f.file.getFullPathName() << ":" << juce::String (f.file.getSize()) << ":" << juce::String (f.file.getLastModificationTime().toMilliseconds());
    };
    addFiles (e.regions); addFiles (e.overdubs);

    // the mixers the sound goes through: the Edit's own (or the one chosen for everything), and any other mixer an automation lane drives
    std::vector<const MixerState*> ms;
    auto add = [&ms] (const MixerState* m) { if (m != nullptr && std::find (ms.begin(), ms.end(), m) == ms.end()) ms.push_back (m); };
    const MixerState* chosen = nullptr;
    if (rp.ownMixers) chosen = p.mixerOfEdit (e.id);
    if (chosen == nullptr) for (auto& m : p.mixers) if (m->id == rp.mixerId) chosen = m.get();
    if (chosen == nullptr && ! p.mixers.empty()) chosen = p.mixers.front().get();
    add (chosen);
    if (e.automationOn) for (auto& l : e.lanes) if (! l.pts.empty() && ! l.mixerId.isNull()) for (auto& m : p.mixers) if (m->id == l.mixerId) add (m.get());
    for (auto* m : ms)
    {
        juce::var mv = p.mixerToVar (*m);
        removeKeys (mv, { "name", "ditherBits" });                                 // (the render is 32-bit float: no dither)
        // what the automation moves is not part of the mixer's own settings: playing the Edit leaves the controls wherever the automation got to
        if (e.automationOn)
            for (auto& l : e.lanes)
            {
                if (l.pts.empty()) continue;
                const bool theirs = l.mixerId.isNull() ? (m == p.mixerOfEdit (e.id) || (p.mixerOfEdit (e.id) == nullptr && m == p.mixers.front().get())) : l.mixerId == m->id;
                if (! theirs) continue;
                if (auto* strips = mv["strips"].getArray())
                    for (auto& sv : *strips)
                    {
                        if (sv["track"].toString() != l.trackId.toString()) continue;
                        if (l.param == autoparam::fader) removeKeys (sv, { "gainDb" });
                        else if (l.param == autoparam::pan) removeKeys (sv, { "pan" });
                        else if (l.param == autoparam::gain) removeKeys (sv, { "inGainDb" });
                        else if (autoparam::isSend (l.param))
                        {
                            if (auto* sl = sv["sendList"].getArray())
                                for (auto& so : *sl) if (so["dest"].toString() == autoparam::sendDest (l.param).toString()) removeKeys (so, { "gainDb" });
                        }
                        else if (int slot = 0, idx = 0; autoparam::parsePlugin (l.param, slot, idx))
                        {
                            if (auto* sl = sv["slots"].getArray()) if (slot < sl->size()) removeKeys (sl->getReference (slot), { "state" });
                        }
                    }
            }
        text << "|M:" << juce::JSON::toString (mv, true);
    }

    // the design (formats, buses) and the render settings
    for (auto& t : p.tracks) text << "|T:" << t.id.toString() << ":" << (int) t.format << ":" << t.surroundChannels;
    for (auto& b : p.buses)  text << "|B:" << b.id.toString() << ":" << (b.external ? 1 : 0);
    text << "|S:" << rp.sourceId.toString() << ":" << juce::String (rp.tailSeconds, 4) << ":" << (rp.ownMixers ? "own" : rp.mixerId.toString());
    // two 64-bit FNV-1a hashes with different starting values (128 bits in all)
    juce::uint64 h1 = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
    for (auto c = text.toUTF8(); *c != 0; ++c) { h1 = (h1 ^ (juce::uint8) *c) * 1099511628211ull; h2 = (h2 ^ (juce::uint8) *c) * 0x100000001B3ull + 0x7F4A7C15ull; }
    return juce::String::toHexString ((juce::int64) h1) + juce::String::toHexString ((juce::int64) h2) + "-" + juce::String (text.length());
}

// ----------------------------------------------------------------------------- the two slots
juce::File MasterRenders::folderFor (const Project& p) { return p.projectFolder().getChildFile ("Mastering renders"); }

std::map<juce::String, MasterRenders::Entry> MasterRenders::load() const
{
    std::map<juce::String, Entry> m;
    const auto v = juce::JSON::parse (dir.getChildFile ("renders.json"));
    if (auto* o = v["edits"].getDynamicObject())
        for (auto& kv : o->getProperties())
        {
            Entry e;
            e.active = (int) kv.value["active"]; e.pinned = (bool) kv.value["pinned"]; e.stale = (bool) kv.value["stale"];
            if (e.active < 0 || e.active > 1) e.active = -1;
            for (int i = 0; i < 2; ++i)
            {
                const auto sv = kv.value["slots"][i];
                e.s[i].fp = sv["fp"].toString(); e.s[i].rate = (double) sv["rate"]; e.s[i].frames = (juce::int64) sv["frames"];
            }
            m[kv.name.toString()] = e;
        }
    return m;
}

void MasterRenders::save (const std::map<juce::String, Entry>& m) const
{
    auto* edits = new juce::DynamicObject();
    for (auto& kv : m)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("active", kv.second.active); o->setProperty ("pinned", kv.second.pinned); o->setProperty ("stale", kv.second.stale);
        juce::Array<juce::var> ss;
        for (int i = 0; i < 2; ++i)
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("fp", kv.second.s[i].fp); so->setProperty ("rate", kv.second.s[i].rate); so->setProperty ("frames", (juce::int64) kv.second.s[i].frames);
            ss.add (juce::var (so));
        }
        o->setProperty ("slots", juce::var (ss));
        edits->setProperty (juce::Identifier (kv.first), juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("edits", juce::var (edits));
    dir.createDirectory();
    dir.getChildFile ("renders.json").replaceWithText (juce::JSON::toString (juce::var (root), false));
}

MasterRenders::Info MasterRenders::check (const juce::Uuid& id, const juce::String& fp)
{
    const juce::ScopedLock sl (storeLock());
    auto m = load();
    auto& e = m[id.toString()];
    Info r;
    if (e.active >= 0 && ! valid (id, e, e.active)) e.active = -1;
    if (e.active < 0) for (int i = 0; i < 2; ++i) if (valid (id, e, i)) { e.active = i; break; }      // (the list lost track of which is current)
    if (e.active >= 0) r.hasBackup = valid (id, e, 1 - e.active);
    if (e.pinned && ! r.hasBackup) e.pinned = false;
    auto fill = [&] (int i) { r.file = slotFile (id, i); r.rate = e.s[i].rate; r.frames = e.s[i].frames; };
    if (e.active < 0) r.state = State::None;
    else if (e.pinned) { r.state = State::Previous; fill (e.active); }
    else if (! e.stale && e.s[e.active].fp == fp) { r.state = State::Ready; fill (e.active); }
    else if (! e.stale && valid (id, e, 1 - e.active) && e.s[1 - e.active].fp == fp) { e.active = 1 - e.active; r.state = State::Ready; fill (e.active); r.hasBackup = valid (id, e, 1 - e.active); }
    else { r.state = State::Stale; fill (e.active); }
    save (m);
    return r;
}

void MasterRenders::commit (const juce::Uuid& id, const juce::String& fp, double rate, juce::int64 frames, const juce::File& produced)
{
    const juce::ScopedLock sl (storeLock());
    auto m = load();
    auto& e = m[id.toString()];
    const int target = e.active < 0 ? 0 : 1 - e.active;           // always the OLDER of the two
    const auto dest = slotFile (id, target);
    dest.getParentDirectory().createDirectory();
    dest.deleteFile();
    if (! produced.moveFileTo (dest)) { produced.deleteFile(); return; }
    e.s[target].fp = fp; e.s[target].rate = rate; e.s[target].frames = frames;
    e.active = target; e.pinned = false; e.stale = false;
    save (m);
}

bool MasterRenders::setUsePrevious (const juce::Uuid& id, bool on)
{
    const juce::ScopedLock sl (storeLock());
    auto m = load();
    auto& e = m[id.toString()];
    if (e.active < 0 || ! valid (id, e, 1 - e.active)) return false;
    if (on != e.pinned) { e.active = 1 - e.active; e.pinned = on; e.stale = false; }
    save (m);
    return true;
}

void MasterRenders::markStale (const juce::Uuid& id)
{
    const juce::ScopedLock sl (storeLock());
    auto m = load();
    auto& e = m[id.toString()];
    e.stale = true; e.pinned = false;
    save (m);
}

void MasterRenders::removeTemps() const
{
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "mrtmp-*")) f.deleteFile();
}

void MasterRenders::clearAll()
{
    const juce::ScopedLock sl (storeLock());
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav")) f.deleteFile();
    dir.getChildFile ("renders.json").deleteFile();
}

// ----------------------------------------------------------------------------- the batch
MasterRenderBatch::MasterRenderBatch (const Project& live, const std::vector<juce::Uuid>& editIds, const RenderParams& rp, bool force)
    : folder (MasterRenders::folderFor (live))
{
    MasterRenders store (folder);
    auto& p = const_cast<Project&> (live);
    for (auto& id : editIds)
    {
        auto* e = p.findEdit (id);
        if (e == nullptr || e->isEmpty()) continue;
        Item it; it.editId = id; it.rate = e->sampleRate;
        it.fp = masterRenderFingerprint (live, *e, rp);
        if (force) store.markStale (id);
        const auto info = store.check (id, it.fp);
        it.backup = info.hasBackup;
        if (info.state == MasterRenders::State::Ready || info.state == MasterRenders::State::Previous) { it.file = info.file; it.frames = info.frames; it.previous = info.state == MasterRenders::State::Previous; if (info.rate > 0.0) it.rate = info.rate; }
        else it.stale = true;
        items.push_back (std::move (it));
    }
    // the pieces that need rendering, one Bouncer per sample rate (a Bouncer renders at one rate)
    for (int i = 0; i < (int) items.size(); ++i)
    {
        if (! items[(size_t) i].stale) continue;
        Group* g = nullptr;
        for (auto& gr : groups) if (std::abs (gr.rate - items[(size_t) i].rate) < 0.5) g = &gr;
        if (g == nullptr) { groups.emplace_back(); g = &groups.back(); g->rate = items[(size_t) i].rate; }
        g->idx.push_back (i);
    }
    for (auto& g : groups)
    {
        BounceSettings bs;
        bs.folder = folder; bs.mixerId = rp.mixerId; bs.sources = { rp.sourceId };
        bs.addOutputName = false; bs.floatFiles = true; bs.normalise = false; bs.tailSeconds = rp.tailSeconds; bs.sampleRate = g.rate;
        for (int i : g.idx)
        {
            auto* e = p.findEdit (items[(size_t) i].editId);
            BounceItem bi;
            bi.name = "mrtmp-" + e->id.toString();
            bi.start = e->firstSample(); bi.end = e->lengthSamples();
            bi.segments = segmentsForEdit (live, *e);
            bi.automationEdit = e->automationOn ? e->id : juce::Uuid::null();
            if (rp.ownMixers) if (auto* em = live.mixerOfEdit (e->id)) bi.mixerId = em->id;
            bs.items.push_back (std::move (bi));
        }
        g.bouncer = std::make_unique<Bouncer> (live, bs);
        if (g.bouncer->getPrepareError().isNotEmpty()) { prepareError = g.bouncer->getPrepareError(); break; }
    }
}

MasterRenderBatch::~MasterRenderBatch() = default;

int MasterRenderBatch::toRender() const
{
    int n = 0;
    for (auto& it : items) if (it.stale) ++n;
    return n;
}

juce::String MasterRenderBatch::run (std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    if (prepareError.isNotEmpty()) return prepareError;
    MasterRenders store (folder);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    juce::String problem;
    for (auto& g : groups)
    {
        if (g.bouncer == nullptr) continue;
        auto br = g.bouncer->render (progress, cancel);
        g.bouncer.reset();
        if (br.cancelled) { store.removeTemps(); return "Cancelled."; }
        if (br.error.isNotEmpty()) { store.removeTemps(); return br.error; }
        if (br.files.size() != (int) g.idx.size()) { store.removeTemps(); return "The renderer made a different number of files than expected."; }
        for (int k = 0; k < (int) g.idx.size(); ++k)
        {
            auto& it = items[(size_t) g.idx[(size_t) k]];
            const juce::File produced (br.files[k]);
            juce::int64 frames = 0;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (produced) }) frames = r->lengthInSamples;
            store.commit (it.editId, it.fp, it.rate, frames, produced);
            const auto info = store.check (it.editId, it.fp);
            it.file = info.file; it.frames = info.frames; it.stale = false; it.previous = false; it.backup = info.hasBackup;
        }
    }
    store.removeTemps();
    for (auto& it : items) if (it.stale || ! it.file.existsAsFile()) problem = "A render is missing.";
    return problem;
}

// ----------------------------------------------------------------------------- the background job
MasterRenderJob::MasterRenderJob (std::unique_ptr<MasterRenderBatch> b, std::function<void (juce::String)> done)
    : juce::Thread ("Mastering renders"), batch (std::move (b)), onDone (std::move (done))
{
    startThread (juce::Thread::Priority::normal);
}

MasterRenderJob::~MasterRenderJob()
{
    cancelFlag = true;
    stopThread (15000);
}

void MasterRenderJob::run()
{
    const auto err = batch->run (&progress, &cancelFlag);
    auto fn = onDone;
    juce::MessageManager::callAsync ([fn, err] { if (fn) fn (err); });
}
} // namespace td
