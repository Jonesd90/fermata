#include "Project.h"
#include "WavRepair.h"
#include <algorithm>

namespace td
{
namespace
{
using juce::var; using juce::DynamicObject; using juce::String;

var obj() { return var (new DynamicObject()); }
void put (var& o, const char* k, const var& v) { o.getDynamicObject()->setProperty (k, v); }

var slotsToVar (const InsertSlot* slots, int count = kNumSlots)
{
    juce::Array<var> arr;
    for (int i = 0; i < count; ++i)
    {
        var s = obj();
        if (auto* p = slots[i].getProcessorUnsafe())
        {
            put (s, "desc", p->getDescription());
            put (s, "state", p->getStateBase64());
        }
        else
        {
            put (s, "desc", slots[i].savedDescription);
            put (s, "state", slots[i].savedState);
        }
        put (s, "bypass", slots[i].bypass.get());
        arr.add (s);
    }
    return arr;
}

void slotsFromVar (InsertSlot* slots, const var& v, const InsertFactory& factory, double sr = 48000.0, int maxBlock = 512, int count = kNumSlots)
{
    if (! v.isArray()) return;
    for (int i = 0; i < count && i < v.size(); ++i)
    {
        auto desc = v[i].getProperty ("desc", {}).toString();
        auto state = v[i].getProperty ("state", {}).toString();
        slots[i].bypass.set ((bool) v[i].getProperty ("bypass", false));
        if (desc.isEmpty()) continue;
        std::unique_ptr<InsertProcessor> p;
        if (factory) p = factory (desc, state);
        if (p != nullptr) slots[i].set (std::move (p), sr, maxBlock, 2);
        else { slots[i].savedDescription = desc; slots[i].savedState = state; }
    }
}

// ---- mixer pieces as data (used both for saving and for copying a mix)
var sendsToVar (const SendList& l, bool inserts)
{
    juce::Array<var> arr;
    for (auto& s : l.pool)
    {
        bool hasInserts = false;
        for (auto& sl : s->slots) hasInserts = hasInserts || sl.mightBeLoaded() || sl.savedDescription.isNotEmpty();
        if (! s->active() && ! hasInserts) continue;
        var o = obj();
        put (o, "dest", s->dest.toString()); put (o, "gainDb", s->gainDb.get()); put (o, "pre", s->pre.get());
        if (inserts) put (o, "slots", slotsToVar (s->slots, kSendSlots));
        arr.add (o);
    }
    return arr;
}

void sendsFromVar (SendList& l, const var& v, const InsertFactory& factory, double sr, int block, bool inserts)
{
    std::vector<SendState*> seen;
    if (auto* a = v.getArray())
        for (auto& sv : *a)
        {
            auto* s = l.get (juce::Uuid (sv["dest"].toString()));
            s->gainDb.set ((float) (double) sv["gainDb"]); s->pre.set ((bool) sv["pre"]);
            if (inserts) slotsFromVar (s->slots, sv["slots"], factory, sr, block, kSendSlots);
            seen.push_back (s);
        }
    for (auto& s : l.pool)                                         // sends the other side did not have are switched off
        if (std::find (seen.begin(), seen.end(), s.get()) == seen.end()) s->gainDb.set (-100.0f);
}

var stripToVar (const StripState& s, const juce::Uuid& trackId, bool inserts)
{
    var so = obj();
    put (so, "track", trackId.toString()); put (so, "gainDb", s.gainDb.get()); put (so, "pan", s.pan.get());
    put (so, "panL", s.panL.get()); put (so, "panR", s.panR.get());
    put (so, "mute", s.mute.get()); put (so, "solo", s.solo.get());
    put (so, "outOn", s.outOn.get()); put (so, "outDest", s.outDest.toString());
    put (so, "sendList", sendsToVar (s.sends, inserts));
    if (inserts) put (so, "slots", slotsToVar (s.slots));
    return so;
}

void stripFromVar (StripState& s, const var& sv, const InsertFactory& factory, double sr, int block, bool inserts, const std::vector<juce::Uuid>& legacyFx)
{
    s.gainDb.set ((float) (double) sv["gainDb"]); s.pan.set ((float) (double) sv["pan"]);
    if (sv.hasProperty ("panL")) { s.panL.set ((float) (double) sv["panL"]); s.panR.set ((float) (double) sv["panR"]); }
    s.mute.set ((bool) sv["mute"]); s.solo.set ((bool) sv["solo"]);
    s.outOn.set (sv.hasProperty ("outOn") ? (bool) sv["outOn"] : false);          // older projects had no output button: nothing was routed that way
    s.outDest = juce::Uuid (sv["outDest"].toString());
    if (sv.hasProperty ("sendList")) sendsFromVar (s.sends, sv["sendList"], factory, sr, block, inserts);
    else if (auto* old = sv["sends"].getArray())                   // projects from before buses: one send level per effects channel
        for (int k = 0; k < old->size() && k < (int) legacyFx.size(); ++k)
            if ((double) (*old)[k] > kSendOffDb) s.sends.get (legacyFx[(size_t) k])->gainDb.set ((float) (double) (*old)[k]);
    if (inserts) slotsFromVar (s.slots, sv["slots"], factory, sr, block);
}

var busToVar (const BusState& s, const juce::Uuid& busId, bool inserts, bool withOutput)
{
    var o = obj();
    put (o, "bus", busId.toString()); put (o, "gainDb", s.gainDb.get()); put (o, "mute", s.mute.get());
    if (withOutput) put (o, "outFirst", s.outFirst.load());
    put (o, "outOn", s.outOn.get()); put (o, "outDest", s.outDest.toString());
    put (o, "sendList", sendsToVar (s.sends, inserts));
    if (inserts) put (o, "slots", slotsToVar (s.slots));
    return o;
}

void busFromVar (BusState& s, const var& v, const InsertFactory& factory, double sr, int block, bool inserts, bool withOutput)
{
    s.gainDb.set ((float) (double) v["gainDb"]); s.mute.set ((bool) v["mute"]);
    if (withOutput && v.hasProperty ("outFirst")) s.outFirst.store ((int) v["outFirst"]);
    s.outOn.set (v.hasProperty ("outOn") ? (bool) v["outOn"] : false);
    s.outDest = juce::Uuid (v["outDest"].toString());
    if (v.hasProperty ("sendList")) sendsFromVar (s.sends, v["sendList"], factory, sr, block, inserts);
    if (inserts) slotsFromVar (s.slots, v["slots"], factory, sr, block);
}
} // namespace

Project::Project() { createDefaultDesign(); }

void Project::createDefaultDesign()
{
    inputs.clear(); tracks.clear(); buses.clear(); outputs.clear(); crMicInputs.clear(); tbOutputs.clear(); altMixer = juce::Uuid::null(); retireAllMixers(); takeWindows.clear(); edits.clear(); mastering = MasteringDef(); preampDevices.clear();
    setInputCount (8);
    setOutputCount (4);
    addMixer ("Processing mixer");
    addTakeWindow ("Piece 1");
    syncMixers();
}

void Project::syncWithDevice (const juce::StringArray& ins, const juce::StringArray& outs)
{
    if (ins.isEmpty() && outs.isEmpty()) return;
    bool differs = (int) inputs.size() != ins.size() || (int) outputs.size() != outs.size();
    for (int i = 0; i < ins.size() && ! differs; ++i) differs = inputs[(size_t) i].driverName != ins[i];
    for (int i = 0; i < outs.size() && ! differs; ++i) differs = outputs[(size_t) i].driverName != outs[i];
    if (! differs) return;
    inputs.resize ((size_t) juce::jmin (ins.size(), kMaxInputs));
    outputs.resize ((size_t) juce::jmin (outs.size(), 256));
    for (int i = 0; i < (int) inputs.size(); ++i)
    {
        inputs[(size_t) i].driverName = ins[i];
        if (inputs[(size_t) i].name == ins[i]) inputs[(size_t) i].name.clear();      // older projects stored the driver name here
    }
    for (int i = 0; i < (int) outputs.size(); ++i)
    {
        outputs[(size_t) i].driverName = outs[i];
        if (outputs[(size_t) i].name == outs[i]) outputs[(size_t) i].name.clear();     // older projects stored the driver name here
    }
    structureChanged();
}

void Project::setInputName (int i, const String& newName)
{
    if (! juce::isPositiveAndBelow (i, (int) inputs.size())) return;
    auto& in = inputs[(size_t) i];
    const auto old = in.name, trimmed = newName.trim();
    if (old == trimmed) return;
    in.name = trimmed;
    // a mono track fed by this input that still has an automatic name (or the old name) takes the new name,
    // so the name you give the input is the one at the top of the mixer strip
    if (trimmed.isNotEmpty())
        for (auto& t : tracks)
        {
            if (t.format != TrackFormat::Mono || t.inputOf (0) != i) continue;
            const bool automatic = t.name == old || t.name == in.driverName || (t.name.startsWith ("Mono ") && t.name.substring (5).containsOnly ("0123456789"));
            if (automatic) t.name = trimmed;
        }
    structureChanged();
}

String Project::inputLabel (int i) const
{
    if (! juce::isPositiveAndBelow (i, (int) inputs.size())) return "(none)";
    const auto& in = inputs[(size_t) i];
    return String (i + 1) + " - " + (in.driverName.isNotEmpty() ? in.driverName : "Input " + String (i + 1)) + (in.name.isNotEmpty() ? " - " + in.name : String());
}

String Project::outputLabel (int i) const
{
    if (! juce::isPositiveAndBelow (i, (int) outputs.size())) return String (i + 1);
    const auto& o = outputs[(size_t) i];
    return String (i + 1) + " - " + (o.driverName.isNotEmpty() ? o.driverName : "Output " + String (i + 1)) + (o.name.isNotEmpty() ? " - " + o.name : String());
}

void Project::setInputCount (int n)
{
    n = juce::jlimit (0, kMaxInputs, n);
    inputs.resize ((size_t) n);
    structureChanged();
}

void Project::setOutputCount (int n)
{
    n = juce::jlimit (0, 128, n);
    outputs.resize ((size_t) n);
    structureChanged();
}

std::vector<const TrackDef*> Project::editTracks (const EditDef& e) const
{
    std::vector<const TrackDef*> v;
    if (e.trackIds.empty()) { for (auto& t : tracks) v.push_back (&t); return v; }
    for (auto& id : e.trackIds) for (auto& t : tracks) if (t.id == id) { v.push_back (&t); break; }
    return v;
}

std::vector<std::pair<juce::Uuid, std::pair<juce::String, int>>> Project::missingTracksOf (const EditDef& e) const
{
    std::vector<std::pair<juce::Uuid, std::pair<juce::String, int>>> out;
    auto scan = [&] (const std::vector<EditRegion>& rs)
    {
        for (auto& r : rs) for (auto& f : r.files)
        {
            bool have = false; for (auto& t : tracks) if (t.id == f.trackId) { have = true; break; }
            if (have) continue;
            bool listed = false; for (auto& o : out) if (o.first == f.trackId) { listed = true; break; }
            if (! listed) out.push_back ({ f.trackId, { f.trackName.isNotEmpty() ? f.trackName : juce::String ("Track"), juce::jmax (1, f.numChannels) } });
        }
    };
    scan (e.regions); scan (e.overdubs);
    return out;
}

void Project::restoreTrack (const juce::Uuid& id, const juce::String& trackName, int channels)
{
    for (auto& t : tracks) if (t.id == id) return;
    auto& t = addTrack (trackName, channels >= 2 ? TrackFormat::Stereo : TrackFormat::Mono, 0);
    t.id = id; t.inputs.fill (-1);                       // a restored track has no input: it only plays the edit's audio
    syncMixers();
    structureChanged();
}

void Project::materialiseEditTracks (EditDef& e) const
{
    if (! e.trackIds.empty()) return;
    for (auto& t : tracks) e.trackIds.push_back (t.id);
}

TrackDef& Project::addTrack (const String& trackName, TrackFormat f, int firstInput)
{
    TrackDef t;
    t.name = trackName; t.format = f;
    assignInputsFrom (t, juce::jmax (0, firstInput));
    tracks.push_back (t);
    syncMixers();
    structureChanged();
    return tracks.back();
}

void Project::assignInputsFrom (TrackDef& t, int first) const
{
    t.inputs.fill (-1);
    const int n = (int) inputs.size();
    for (int c = 0; c < t.channelCount(); ++c)
        t.inputs[(size_t) c] = (first + c < n || n == 0) ? first + c : -1;
}

void Project::moveTrack (int from, int to)
{
    const int n = (int) tracks.size();
    if (! juce::isPositiveAndBelow (from, n) || ! juce::isPositiveAndBelow (to, n) || from == to) return;
    auto t = tracks[(size_t) from];
    tracks.erase (tracks.begin() + from);
    tracks.insert (tracks.begin() + to, t);
    structureChanged();
}

void Project::removeTrack (int index)
{
    if (juce::isPositiveAndBelow (index, (int) tracks.size()))
        tracks.erase (tracks.begin() + index);
    structureChanged();
}

BusDef& Project::addBus (const String& busName, bool external)
{
    BusDef f; f.name = busName; f.external = external;
    buses.push_back (f);
    syncMixers();
    structureChanged();
    return buses.back();
}

void Project::removeBus (int index)
{
    if (juce::isPositiveAndBelow (index, (int) buses.size()))
        buses.erase (buses.begin() + index);
    structureChanged();
}

std::vector<int> Project::busIndices (bool external) const
{
    std::vector<int> r;
    for (size_t i = 0; i < buses.size(); ++i) if (buses[i].external == external) r.push_back ((int) i);
    return r;
}

void Project::moveBus (int index, int delta)
{
    if (! juce::isPositiveAndBelow (index, (int) buses.size()) || delta == 0) return;
    const auto same = busIndices (buses[(size_t) index].external);
    const auto pos = (int) (std::find (same.begin(), same.end(), index) - same.begin());
    const int to = pos + delta;
    if (! juce::isPositiveAndBelow (to, (int) same.size())) return;
    std::swap (buses[(size_t) same[(size_t) pos]], buses[(size_t) same[(size_t) to]]);
    structureChanged();
}

BusDef* Project::findBus (const juce::Uuid& id)
{
    for (auto& b : buses) if (b.id == id) return &b;
    return nullptr;
}

NodeKind Project::kindOf (const juce::Uuid& id) const
{
    for (auto& t : tracks) if (t.id == id) return NodeKind::Track;
    for (auto& b : buses) if (b.id == id) return b.external ? NodeKind::ExtBus : NodeKind::IntBus;
    return NodeKind::None;
}

void Project::setChannelColour (const juce::Uuid& id, juce::uint32 argb)
{
    for (auto& t : tracks) if (t.id == id) { t.colour = argb; changed(); return; }
    for (auto& b : buses)  if (b.id == id) { b.colour = argb; changed(); return; }
}

juce::uint32 Project::channelColour (const juce::Uuid& id) const
{
    for (auto& t : tracks) if (t.id == id) return t.colour;
    for (auto& b : buses)  if (b.id == id) return b.colour;
    return 0;
}

String Project::outputFedBy (int out) const
{
    juce::StringArray names;
    for (auto& m : mixers)
        for (auto& b : buses)
            if (b.external)
                for (auto& bs : m->busPool)
                    if (bs->busId == b.id)
                    {
                        const int f = bs->outFirst.load();
                        if (f >= 0 && (f == out || f + 1 == out)) names.add (m->name + ": " + b.name);
                    }
    return names.joinIntoString (", ");
}

String Project::mixerOutputsText (const MixerState& m) const
{
    juce::StringArray parts;
    for (auto& b : buses)
        if (b.external)
            for (auto& bs : m.busPool)
                if (bs->busId == b.id)
                {
                    const int f = bs->outFirst.load();
                    if (f >= 0) parts.add (b.name + " -> " + juce::String (f + 1) + "-" + juce::String (f + 2));
                }
    return parts.isEmpty() ? juce::String ("(no outputs)") : parts.joinIntoString (", ");
}

String Project::nodeName (const juce::Uuid& id) const
{
    for (auto& t : tracks) if (t.id == id) return t.name;
    for (auto& b : buses) if (b.id == id) return b.name;
    return {};
}

SendList* Project::sendsOf (MixerState& m, const juce::Uuid& id) const
{
    switch (kindOf (id))
    {
        case NodeKind::Track:  return &m.stripFor (id)->sends;
        case NodeKind::IntBus: return &m.busFor (id)->sends;
        default: break;
    }
    return nullptr;
}

bool Project::sendAllowed (const MixerState& cm, const juce::Uuid& src, const juce::Uuid& dest) const
{
    auto& m = const_cast<MixerState&> (cm);
    const auto sk = kindOf (src), dk = kindOf (dest);
    if (src == dest || (sk != NodeKind::Track && sk != NodeKind::IntBus) || dk == NodeKind::None) return false;
    // would dest (through the sends that are on) already lead back to src?
    std::vector<juce::Uuid> todo { dest }, seen;
    while (! todo.empty())
    {
        const auto cur = todo.back(); todo.pop_back();
        if (cur == src) return false;
        if (std::find (seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back (cur);
        if (auto* l = sendsOf (m, cur))
            for (auto& s : l->pool) if (s->active()) todo.push_back (s->dest);
    }
    return true;
}

SendResult Project::setSendLevel (MixerState& m, const juce::Uuid& src, const juce::Uuid& dest, float db)
{
    auto* l = sendsOf (m, src);
    if (l == nullptr) return SendResult::Refused;
    auto* s = l->find (dest);
    const bool turningOn = db > kSendOffDb && (s == nullptr || ! s->active());
    if (turningOn && ! sendAllowed (m, src, dest)) return SendResult::Refused;
    if (s == nullptr) { if (db <= kSendOffDb) return SendResult::Changed; s = l->get (dest); }
    s->gainDb.set (db <= kSendOffDb ? -100.0f : db);
    markDirty();
    return turningOn ? SendResult::RoutingChanged : SendResult::Changed;
}

MixerState& Project::addMixer (const String& mixerName)
{
    auto m = std::make_unique<MixerState>();
    m->name = mixerName;
    mixers.push_back (std::move (m));
    syncMixers();
    structureChanged();
    return *mixers.back();
}

void Project::removeMixer (int index)
{
    if (juce::isPositiveAndBelow (index, (int) mixers.size()))
        { retiredMixers.push_back (std::move (mixers[(size_t) index])); mixers.erase (mixers.begin() + index); }   // kept alive: the audio thread may still hold a pointer
    structureChanged();
}

TakeWindowDef& Project::addTakeWindow (const String& windowName)
{
    auto w = std::make_unique<TakeWindowDef>();
    w->name = windowName;
    takeWindows.push_back (std::move (w));
    structureChanged();
    return *takeWindows.back();
}

void Project::removeTakeWindow (int index)
{
    if (juce::isPositiveAndBelow (index, (int) takeWindows.size()))
        takeWindows.erase (takeWindows.begin() + index);
    structureChanged();
}

EditDef& Project::addEdit (const String& editName, const juce::Uuid& windowId)
{
    auto e = std::make_unique<EditDef>();
    e->name = editName; e->windowId = windowId;
    edits.push_back (std::move (e));
    changed();
    return *edits.back();
}

EditDef* Project::findEdit (const juce::Uuid& id)
{
    for (auto& e : edits) if (e->id == id) return e.get();
    return nullptr;
}

EditDef* Project::editForWindow (const juce::Uuid& windowId)
{
    for (auto& e : edits) if (e->windowId == windowId) return e.get();
    return nullptr;
}

TakeGroup* Project::findTake (const juce::Uuid& windowId, const juce::Uuid& takeId)
{
    if (auto* w = findTakeWindow (windowId)) return w->findGroup (takeId);
    return nullptr;
}

TrackDef* Project::findTrack (const juce::Uuid& id)
{
    for (auto& t : tracks) if (t.id == id) return &t;
    return nullptr;
}

TakeWindowDef* Project::findTakeWindow (const juce::Uuid& id)
{
    for (auto& w : takeWindows) if (w->id == id) return w.get();
    return nullptr;
}

juce::Array<int> Project::inputsOfTrack (const TrackDef& t) const
{
    juce::Array<int> r;
    for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) >= 0) r.add (t.inputOf (c));
    return r;
}

void Project::retireAllMixers()
{
    for (auto& m : mixers) retiredMixers.push_back (std::move (m));
    mixers.clear();
}

void Project::syncMixers()
{
    for (auto& m : mixers)
    {
        for (auto& t : tracks)
        {
            const auto n = m->stripPool.size();
            auto* st = m->stripFor (t.id);
            if (m->stripPool.size() != n && newStripsToMain) st->outOn.set (true);     // a new track is heard straight away
        }
        for (auto& f : buses)
        {
            const auto n = m->busPool.size();
            auto* bs = m->busFor (f.id);
            if (m->busPool.size() != n && newStripsToMain && ! f.external) bs->outOn.set (true);
        }
    }
}

void Project::adoptPreamp (int inputIndex, const PreampSettings& s)
{
    if (! juce::isPositiveAndBelow (inputIndex, (int) inputs.size())) return;
    inputs[(size_t) inputIndex].preamp = s;
    sendChangeMessage();                                  // refresh the windows; nothing to save, the hardware is the source of truth
}

void Project::setPreamp (int inputIndex, const PreampSettings& s)
{
    if (! juce::isPositiveAndBelow (inputIndex, (int) inputs.size())) return;
    inputs[(size_t) inputIndex].preamp = s;
    if (preampDriver != nullptr) preampDriver->apply (inputIndex, s);
    changed();
}

var Project::mixSnapshot (const MixerState& m, bool withInserts) const
{
    var root = obj();
    juce::Array<var> strips, bs;
    for (auto& t : tracks)
    {
        const StripState* s = nullptr;
        for (auto& p : m.stripPool) if (p->trackId == t.id) s = p.get();
        if (s != nullptr) strips.add (stripToVar (*s, t.id, withInserts));
    }
    for (auto& f : buses)
    {
        const BusState* s = nullptr;
        for (auto& p : m.busPool) if (p->busId == f.id) s = p.get();
        if (s != nullptr) bs.add (busToVar (*s, f.id, withInserts, false));   // an Ext Bus keeps its own driver outputs
    }
    put (root, "strips", strips); put (root, "buses", bs); put (root, "inserts", withInserts);
    return root;
}

void Project::applyMix (MixerState& to, const var& snap, double sampleRate, int maxBlock)
{
    syncMixers();
    const bool withInserts = (bool) snap.getProperty ("inserts", false);
    const std::vector<juce::Uuid> noLegacy;
    if (auto* ss = snap["strips"].getArray())
        for (auto& sv : *ss)
        {
            const juce::Uuid id (sv["track"].toString());
            StripState* s = nullptr;
            for (auto& p : to.stripPool) if (p->trackId == id) s = p.get();
            if (s != nullptr) stripFromVar (*s, sv, insertFactory, sampleRate, maxBlock, withInserts, noLegacy);   // (a track that no longer exists is skipped)
        }
    if (auto* fs = snap["buses"].getArray())
        for (auto& fv : *fs)
        {
            const juce::Uuid id (fv["bus"].toString());
            BusState* f = nullptr;
            for (auto& p : to.busPool) if (p->busId == id) f = p.get();
            if (f != nullptr) busFromVar (*f, fv, insertFactory, sampleRate, maxBlock, withInserts, false);
        }
    markDirty();
}

juce::File Project::audioFolder() const
{
    auto base = projectFile != juce::File() ? projectFile.getParentDirectory()
                                            : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata");
    return base.getChildFile ("Recorded Media");
}

juce::File Project::projectFolder() const
{
    return projectFile != juce::File() ? projectFile.getParentDirectory() : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Fermata");
}
juce::File Project::processingFolder() const { return projectFolder().getChildFile ("Processing Media"); }
juce::File Project::bouncedFolder() const { return projectFolder().getChildFile ("Bounced Media"); }
juce::File Project::masteredFolder() const { return bouncedFolder().getChildFile ("Mastered Audio"); }
void Project::createFolders() const
{
    audioFolder().createDirectory(); processingFolder().createDirectory(); bouncedFolder().createDirectory(); masteredFolder().createDirectory();
}

/** A file path as saved: the full path, and (for a file inside the project folder) the path from the project folder, so the whole folder can be moved or copied. */
static void putFilePath (var& o, const juce::File& file, const juce::File& base)
{
    put (o, "file", file.getFullPathName());
    if (base != juce::File() && file.isAChildOf (base)) put (o, "rel", file.getRelativePathFrom (base).replaceCharacter ('\\', '/'));
}
static var waitingToVar (const WaitingPiece& w, const juce::File& base)
{
    var o = obj();
    put (o, "name", w.name); put (o, "from", (juce::int64) w.from); put (o, "to", (juce::int64) w.to);
    juce::Array<var> fs;
    for (auto& f : w.files) { var fo = obj(); putFilePath (fo, f, base); put (fo, "channels", 1); fs.add (fo); }      // ("channels" marks it as an audio file for the project copy)
    put (o, "files", fs);
    return o;
}
/** The file for a saved path: inside the project folder it is found from the relative path (so a moved or copied project uses its OWN files). */
static juce::File getFilePath (const var& fv, const juce::File& base)
{
    const auto rel = fv["rel"].toString();
    if (rel.isNotEmpty() && base != juce::File())
    {
        auto f = base.getChildFile (rel);
        if (f.existsAsFile()) return f;
    }
    return juce::File (fv["file"].toString());
}

static WaitingPiece waitingFromVar (const var& o, const juce::File& base)
{
    WaitingPiece w;
    w.name = o["name"].toString(); w.from = (juce::int64) o["from"]; w.to = (juce::int64) o["to"];
    if (auto* fs = o["files"].getArray()) for (auto& fv : *fs) w.files.push_back (getFilePath (fv, base));
    return w;
}

juce::File Project::takeFolder (const TakeWindowDef& w) const
{
    return audioFolder().getChildFile (sanitiseForFile (w.name));
}

// ---------------------------------------------------------------- persistence
var Project::toVar (bool editorialOnly) const
{
    var root = obj();
    put (root, "format", "Fermata project");
    put (root, "version", 1);
    put (root, "name", name);

    if (! editorialOnly)                                  // (the undo history only looks at the takes, the edits and the mastering: no inputs, tracks, mixers or plug-in states)
    {
    { var wb = obj(); for (auto& kv : windowBounds) put (wb, kv.first.toRawUTF8(), kv.second); put (root, "windowBounds", wb); }
    juce::Array<var> in;
    for (auto& i : inputs)
    {
        var o = obj();
        put (o, "name", i.name); put (o, "driverName", i.driverName);
        put (o, "gainDb", i.preamp.gainDb); put (o, "phantom", i.preamp.phantom); put (o, "line", i.preamp.line);
        put (o, "lowCut", i.preamp.lowCut); put (o, "polarity", i.preamp.polarity); put (o, "boost", i.preamp.boost); put (o, "pad", i.preamp.pad); put (o, "zHigh", i.preamp.zHigh);
        in.add (o);
    }
    put (root, "inputs", in);
    {
        juce::Array<var> pd;
        for (auto& d : preampDevices) pd.add (d.toVar());
        put (root, "preampDevices", pd);
    }

    juce::Array<var> tr;
    for (auto& t : tracks)
    {
        var o = obj();
        put (o, "id", t.id.toString()); put (o, "name", t.name); put (o, "format", (int) t.format);
        put (o, "surround", t.surroundChannels); put (o, "colour", (juce::int64) t.colour); put (o, "armed", t.isArmed()); put (o, "monitor", (int) t.monitor());
        juce::Array<var> ins;
        for (int c = 0; c < t.channelCount(); ++c) ins.add (t.inputOf (c));
        put (o, "inputs", ins);
        tr.add (o);
    }
    put (root, "tracks", tr);

    juce::Array<var> fx;
    for (auto& f : buses) { var o = obj(); put (o, "id", f.id.toString()); put (o, "name", f.name); put (o, "external", f.external); put (o, "colour", (juce::int64) f.colour); fx.add (o); }
    put (root, "buses", fx);

    juce::Array<var> outs;
    for (auto& o : outputs) { var oo = obj(); put (oo, "name", o.name); put (oo, "driverName", o.driverName); outs.add (oo); }
    put (root, "outputs", outs);
    {
        juce::Array<juce::var> cr; for (int i : crMicInputs) cr.add (i);
        put (root, "crMicInputs", juce::var (cr));
    }
    put (root, "altMixer", altMixer.toString());
    { juce::Array<var> tb; for (int o : tbOutputs) tb.add (o); put (root, "tbOutputs", juce::var (tb)); }

    juce::Array<var> mx;
    for (auto& m : mixers)
    {
        var o = obj();
        put (o, "id", m->id.toString()); put (o, "name", m->name);
        juce::Array<var> strips;
        for (auto& t : tracks)
        {
            const StripState* s = nullptr;
            for (auto& p : m->stripPool) if (p->trackId == t.id) s = p.get();
            if (s != nullptr) strips.add (stripToVar (*s, t.id, true));
        }
        put (o, "strips", strips);
        put (o, "mainBus", m->mainBus.toString());
        put (o, "ditherBits", m->ditherBits.load());
        juce::Array<var> fxs;
        for (auto& f : buses)
        {
            const BusState* s = nullptr;
            for (auto& p : m->busPool) if (p->busId == f.id) s = p.get();
            if (s != nullptr) fxs.add (busToVar (*s, f.id, true, true));
        }
        put (o, "busStates", fxs);
        mx.add (o);
    }
    put (root, "mixers", mx);
    }

    juce::Array<var> tw;
    for (auto& w : takeWindows)
    {
        var o = obj();
        put (o, "id", w->id.toString()); put (o, "name", w->name); put (o, "defaultLabel", w->defaultLabel);
        put (o, "nextNumber", w->nextNumber);
        put (o, "targetEdit", w->targetEdit.toString());
        put (o, "markTake", w->markTake.toString()); put (o, "markIn", w->markIn); put (o, "markOut", w->markOut);
        put (o, "editTake", w->editTake.toString()); put (o, "editIn", w->editIn); put (o, "editOut", w->editOut); put (o, "overdub", w->overdub);
        put (o, "cursorTake", w->cursorTake.toString()); put (o, "cursorSeconds", w->cursorSeconds); put (o, "playhead", w->playheadSeconds);
        { juce::Array<var> hid; for (auto& h : w->hiddenTracks) hid.add (h.toString()); put (o, "hiddenTracks", hid); }
        juce::Array<var> gs;
        for (auto& g : w->groups)
        {
            var go = obj();
            put (go, "id", g.id.toString()); put (go, "number", g.number); put (go, "label", g.label);
            put (go, "start", g.startSeconds); put (go, "length", (juce::int64) g.lengthSamples);
            put (go, "rate", g.sampleRate); put (go, "time", g.recordedAt.toMilliseconds());
            put (go, "fadeIn", g.fadeInSeconds); put (go, "fadeOut", g.fadeOutSeconds); put (go, "dud", g.dud); put (go, "barIn", g.barIn); put (go, "barOut", g.barOut);
            juce::Array<var> fs;
            for (auto& f : g.files)
            {
                var fo = obj();
                put (fo, "track", f.trackId.toString()); put (fo, "trackName", f.trackName);
                putFilePath (fo, f.file, projectFile.getParentDirectory()); put (fo, "channels", f.numChannels);
                fs.add (fo);
            }
            put (go, "files", fs);
            if (! g.waiting.empty())
            {
                juce::Array<var> ws; for (auto& w : g.waiting) ws.add (waitingToVar (w, projectFile.getParentDirectory()));
                put (go, "waiting", ws);
            }
            gs.add (go);
        }
        put (o, "groups", gs);
        tw.add (o);
    }
    put (root, "takeWindows", tw);

    juce::Array<var> eds;
    for (auto& e : edits)
    {
        var o = obj();
        put (o, "id", e->id.toString()); put (o, "name", e->name); put (o, "window", e->windowId.toString());
        put (o, "rate", e->sampleRate); put (o, "insertIndex", e->insertIndex); put (o, "markIn", e->markIn); put (o, "markOut", e->markOut);
        put (o, "fixIn", e->fixIn); put (o, "fixOut", e->fixOut);
        auto regionToVar = [this] (const EditRegion& r)
        {
            var ro = obj();
            put (ro, "id", r.id.toString()); put (ro, "window", r.windowId.toString()); put (ro, "take", r.takeId.toString());
            put (ro, "takeName", r.takeName); put (ro, "in", (juce::int64) r.srcIn); put (ro, "out", (juce::int64) r.srcOut);
            put (ro, "start", (juce::int64) r.startSample); put (ro, "sourceLength", (juce::int64) r.sourceLength); put (ro, "barIn", r.barIn); put (ro, "barOut", r.barOut);
            put (ro, "rate", r.sampleRate); put (ro, "curve", (int) r.curve);
            put (ro, "inStart", r.inStart); put (ro, "inEnd", r.inEnd); put (ro, "outStart", r.outStart); put (ro, "outEnd", r.outEnd);
            juce::Array<var> rf;
            for (auto& f : r.files)
            {
                var fo = obj();
                put (fo, "track", f.trackId.toString()); put (fo, "trackName", f.trackName);
                putFilePath (fo, f.file, projectFile.getParentDirectory()); put (fo, "channels", f.numChannels);
                if (f.fileStart != 0) put (fo, "fileStart", (juce::int64) f.fileStart);
                rf.add (fo);
            }
            put (ro, "files", rf);
            if (r.waiting.active()) put (ro, "waiting", waitingToVar (r.waiting, projectFile.getParentDirectory()));
            juce::Array<var> gs;
            for (auto& g : r.gains)
            {
                var go = obj();
                put (go, "at", (juce::int64) g.at); put (go, "ramp", g.ramp);
                juce::Array<var> dbs; for (float d : g.db) dbs.add ((double) d);
                put (go, "db", dbs);
                gs.add (go);
            }
            put (ro, "gains", gs);
            return ro;
        };
        juce::Array<var> rs, ods;
        for (auto& r : e->regions)  rs.add (regionToVar (r));
        for (auto& r : e->overdubs) ods.add (regionToVar (r));
        put (o, "overdubs", ods); put (o, "playhead", e->playheadSeconds);
        { juce::Array<var> ti; for (auto& id : e->trackIds) ti.add (id.toString()); put (o, "trackIds", ti); }
        put (o, "automationOn", e->automationOn);
        {
            juce::Array<var> ls;
            for (auto& l : e->lanes)
            {
                var lo = obj();
                put (lo, "id", l.id.toString()); put (lo, "track", l.trackId.toString()); put (lo, "param", l.param);
                put (lo, "mixer", l.mixerId.toString()); put (lo, "locked", l.locked);
                juce::Array<var> ps;
                for (auto& p : l.pts)
                {
                    var po = obj();
                    put (po, "id", p.id.toString()); put (po, "region", p.region.toString()); put (po, "take", p.take.toString()); put (po, "src", (juce::int64) p.srcPos);
                    put (po, "time", (juce::int64) p.time); put (po, "value", (double) p.value); put (po, "floating", p.floating);
                    ps.add (po);
                }
                put (lo, "points", ps);
                ls.add (lo);
            }
            put (o, "autoLanes", ls);
        }
        put (o, "regions", rs);
        eds.add (o);
    }
    put (root, "edits", eds);
    put (root, "mastering", mastering.toVar());
    return root;
}

bool Project::fromVar (const var& root)
{
    if (! root.isObject() || (root.getProperty ("format", {}).toString() != "Fermata project" && root.getProperty ("format", {}).toString() != "TakeDAW project"))
        return false;

    inputs.clear(); tracks.clear(); buses.clear(); outputs.clear(); crMicInputs.clear(); tbOutputs.clear(); altMixer = juce::Uuid::null(); retireAllMixers(); takeWindows.clear(); edits.clear(); mastering = MasteringDef(); preampDevices.clear();
    name = root.getProperty ("name", "Untitled").toString();
    windowBounds.clear();
    if (auto* wb = root["windowBounds"].getDynamicObject())
        for (auto& pr : wb->getProperties()) windowBounds[pr.name.toString()] = pr.value.toString();

    preampDevices.clear();
    if (auto* a = root["preampDevices"].getArray())
        for (auto& v : *a) preampDevices.push_back (PreampDeviceCfg::fromVar (v));
    if (auto* a = root["inputs"].getArray())
        for (auto& v : *a)
        {
            InputChannel i; i.name = v["name"].toString(); i.driverName = v["driverName"].toString();
            i.preamp.gainDb = (float) (double) v["gainDb"]; i.preamp.phantom = (bool) v["phantom"];
            i.preamp.line = (bool) v["line"]; i.preamp.lowCut = (bool) v["lowCut"]; i.preamp.polarity = (bool) v["polarity"]; i.preamp.boost = (bool) v["boost"]; i.preamp.pad = (bool) v["pad"]; i.preamp.zHigh = (bool) v["zHigh"];
            if (i.name == i.driverName) i.name.clear();          // older projects stored the driver name as 'your name'
            inputs.push_back (i);
        }
    if (auto* a = root["tracks"].getArray())
        for (auto& v : *a)
        {
            TrackDef t; t.id = juce::Uuid (v["id"].toString()); t.name = v["name"].toString();
            t.format = (TrackFormat) (int) v["format"]; t.surroundChannels = (int) v["surround"]; t.colour = (juce::uint32) (juce::int64) v["colour"];
            t.setArmed ((bool) v["armed"]); t.setMonitor ((Monitor) juce::jlimit (0, 3, (int) v["monitor"]));
            if (auto* ins = v["inputs"].getArray())
                for (int c = 0; c < kMaxTrackChannels && c < ins->size(); ++c) t.inputs[(size_t) c] = (int) (*ins)[c];
            else for (int c = 0; c < t.channelCount(); ++c) t.inputs[(size_t) c] = (int) v["firstInput"] + c;   // older project files
            tracks.push_back (t);
        }
    std::vector<juce::Uuid> legacyFx;                      // projects from before Int / Ext buses stored 'effects channels'
    if (auto* a = root["buses"].getArray())
        for (auto& v : *a) { BusDef f; f.id = juce::Uuid (v["id"].toString()); f.name = v["name"].toString(); f.external = (bool) v["external"]; f.colour = (juce::uint32) (juce::int64) v["colour"]; buses.push_back (f); }
    else if (auto* a = root["fx"].getArray())
        for (auto& v : *a) { BusDef f; f.id = juce::Uuid (v["id"].toString()); f.name = v["name"].toString(); buses.push_back (f); legacyFx.push_back (f.id); }
    if (auto* a = root["outputs"].getArray())
        for (auto& v : *a)
        {
            OutputDef o;
            if (v.isObject()) { o.name = v["name"].toString(); o.driverName = v["driverName"].toString(); if (o.name == o.driverName) o.name.clear(); } else o.name = v.toString();   // older files stored just the name
            outputs.push_back (o);
        }
    crMicInputs.clear();
    if (auto* ca = root["crMicInputs"].getArray()) { for (auto& v : *ca) if ((int) v >= 0 && (int) v < (int) inputs.size() && ! isCrMic ((int) v)) crMicInputs.push_back ((int) v); }
    else if (! root["crMicInput"].isVoid() && (int) root["crMicInput"] >= 0) crMicInputs.push_back ((int) root["crMicInput"]);      // older projects: one CR input
    if (auto* ta = root["tbOutputs"].getArray()) { for (auto& v : *ta) tbOutputs.push_back ((int) v); }
    else if (! root["tbOutput"].isVoid() && (int) root["tbOutput"] >= 0) tbOutputs.push_back ((int) root["tbOutput"]);          // older projects: one pair
    crMicInputs.erase (std::remove_if (crMicInputs.begin(), crMicInputs.end(), [this] (int i) { return i < 0 || i >= (int) inputs.size(); }), crMicInputs.end());
    tbOutputs.erase (std::remove_if (tbOutputs.begin(), tbOutputs.end(), [this] (int o) { return o < 0 || o >= (int) outputs.size(); }), tbOutputs.end());
    std::sort (tbOutputs.begin(), tbOutputs.end()); tbOutputs.erase (std::unique (tbOutputs.begin(), tbOutputs.end()), tbOutputs.end());
    altMixer = root["altMixer"].toString().isEmpty() ? juce::Uuid::null() : juce::Uuid (root["altMixer"].toString());

    if (auto* a = root["mixers"].getArray())
        for (auto& v : *a)
        {
            auto m = std::make_unique<MixerState>();
            m->id = juce::Uuid (v["id"].toString()); m->name = v["name"].toString();
            m->mainBus = juce::Uuid (v["mainBus"].toString());
            if (v.hasProperty ("ditherBits")) { const int db = (int) v["ditherBits"]; m->ditherBits.store (db == 16 || db == 0 ? db : 24); }
            if (auto* ss = v["strips"].getArray())
                for (auto& sv : *ss)
                    stripFromVar (*m->stripFor (juce::Uuid (sv["track"].toString())), sv, insertFactory, restoreSampleRate, restoreBlock, true, legacyFx);
            for (const char* key : { "busStates", "fxReturns" })
                if (auto* fs = v[key].getArray())
                    for (auto& fv : *fs)
                        busFromVar (*m->busFor (juce::Uuid (fv.hasProperty ("bus") ? fv["bus"].toString() : fv["fx"].toString())), fv, insertFactory, restoreSampleRate, restoreBlock, true, true);
            mixers.push_back (std::move (m));
        }

    if (auto* a = root["takeWindows"].getArray())
        for (auto& v : *a)
        {
            auto w = std::make_unique<TakeWindowDef>();
            w->id = juce::Uuid (v["id"].toString()); w->name = v["name"].toString();
            w->defaultLabel = v["defaultLabel"].toString(); w->nextNumber = (int) v["nextNumber"];
            if (v.hasProperty ("targetEdit")) w->targetEdit = juce::Uuid (v["targetEdit"].toString());
            if (v.hasProperty ("markTake")) { w->markTake = juce::Uuid (v["markTake"].toString()); w->markIn = (double) v["markIn"]; w->markOut = (double) v["markOut"];
                                              w->cursorTake = juce::Uuid (v["cursorTake"].toString()); w->cursorSeconds = (double) v["cursorSeconds"]; }
            if (v.hasProperty ("editTake")) { w->editTake = juce::Uuid (v["editTake"].toString()); w->editIn = (double) v["editIn"]; w->editOut = (double) v["editOut"]; }
            w->overdub = (bool) v["overdub"];
            if (v.hasProperty ("playhead")) w->playheadSeconds = juce::jmax (0.0, (double) v["playhead"]);
            if (auto* hid = v["hiddenTracks"].getArray()) for (auto& h : *hid) w->hiddenTracks.push_back (juce::Uuid (h.toString()));
            if (auto* gs = v["groups"].getArray())
                for (auto& gv : *gs)
                {
                    TakeGroup g;
                    g.id = juce::Uuid (gv["id"].toString()); g.number = (int) gv["number"]; g.label = gv["label"].toString();
                    g.startSeconds = (double) gv["start"]; g.lengthSamples = (juce::int64) gv["length"];
                    g.sampleRate = (double) gv["rate"]; g.recordedAt = juce::Time ((juce::int64) gv["time"]);
                    if (gv.hasProperty ("fadeIn"))  g.fadeInSeconds  = juce::jmax (0.0, (double) gv["fadeIn"]);
                    if (g.fadeInSeconds <= 0.0011 && g.fadeInSeconds > 0.0) g.fadeInSeconds = kDefaultEdgeFade;       // the old 1 ms default becomes the new one
                    if (gv.hasProperty ("fadeOut")) g.fadeOutSeconds = juce::jmax (0.0, (double) gv["fadeOut"]);
                    if (g.fadeOutSeconds <= 0.0011 && g.fadeOutSeconds > 0.0) g.fadeOutSeconds = kDefaultEdgeFade;
                    g.dud = (bool) gv["dud"]; g.barIn = juce::jmax (0, (int) gv["barIn"]); g.barOut = juce::jmax (0, (int) gv["barOut"]);
                    if (auto* fs = gv["files"].getArray())
                        for (auto& fv : *fs)
                        {
                            TakeFile f; f.trackId = juce::Uuid (fv["track"].toString()); f.trackName = fv["trackName"].toString();
                            f.file = getFilePath (fv, loadBase()); f.numChannels = (int) fv["channels"];
                            g.files.push_back (f);
                        }
                    if (auto* ws = gv["waiting"].getArray()) for (auto& wv : *ws) g.waiting.push_back (waitingFromVar (wv, loadBase()));
                    w->groups.push_back (std::move (g));
                }
            takeWindows.push_back (std::move (w));
        }

    if (auto* a = root["edits"].getArray())
        for (auto& v : *a)
        {
            auto e = std::make_unique<EditDef>();
            e->id = juce::Uuid (v["id"].toString()); e->name = v["name"].toString(); e->windowId = juce::Uuid (v["window"].toString());
            e->sampleRate = (double) v["rate"]; e->insertIndex = (int) v["insertIndex"];
            if (v.hasProperty ("markIn")) { e->markIn = (double) v["markIn"]; e->markOut = (double) v["markOut"]; }
            if (v.hasProperty ("fixIn")) { e->fixIn = (double) v["fixIn"]; e->fixOut = (double) v["fixOut"]; }
            auto regionFromVar = [this] (const var& rv)
            {
                EditRegion r;
                r.id = juce::Uuid (rv["id"].toString()); r.windowId = juce::Uuid (rv["window"].toString()); r.takeId = juce::Uuid (rv["take"].toString());
                r.takeName = rv["takeName"].toString(); r.srcIn = (juce::int64) rv["in"]; r.srcOut = (juce::int64) rv["out"];
                r.startSample = (juce::int64) rv["start"]; r.sourceLength = (juce::int64) rv["sourceLength"];
                r.barIn = juce::jmax (0, (int) rv["barIn"]); r.barOut = juce::jmax (0, (int) rv["barOut"]);
                r.sampleRate = (double) rv["rate"]; r.curve = (FadeCurve) juce::jlimit (0, (int) FadeCurve::Count - 1, (int) rv["curve"]);
                r.inStart = (double) rv["inStart"]; r.inEnd = (double) rv["inEnd"]; r.outStart = (double) rv["outStart"]; r.outEnd = (double) rv["outEnd"];
                if (auto* fs = rv["files"].getArray())
                    for (auto& fv : *fs)
                    {
                        RegionFile f; f.trackId = juce::Uuid (fv["track"].toString()); f.trackName = fv["trackName"].toString();
                        f.file = getFilePath (fv, loadBase()); f.numChannels = (int) fv["channels"];
                        f.fileStart = (juce::int64) fv["fileStart"];
                        r.files.push_back (f);
                    }
                if (rv.hasProperty ("waiting")) r.waiting = waitingFromVar (rv["waiting"], loadBase());
                if (auto* gs = rv["gains"].getArray())
                    for (auto& gv : *gs)
                    {
                        GainChange g; g.at = (juce::int64) gv["at"]; g.ramp = (double) gv["ramp"];
                        if (auto* dbs = gv["db"].getArray()) for (auto& d : *dbs) g.db.push_back ((float) (double) d);
                        g.db.resize (r.files.size(), 0.0f);
                        r.gains.push_back (g);
                    }
                return r;
            };
            if (auto* rs = v["regions"].getArray())  for (auto& rv : *rs) e->regions.push_back (regionFromVar (rv));
            if (auto* os = v["overdubs"].getArray()) for (auto& rv : *os) e->overdubs.push_back (regionFromVar (rv));
            if (auto* ta = v["trackIds"].getArray()) for (auto& tv : *ta) { juce::Uuid u (tv.toString()); if (! u.isNull()) e->trackIds.push_back (u); }
            if (v.hasProperty ("playhead")) e->playheadSeconds = juce::jmax (0.0, (double) v["playhead"]);
            e->automationOn = (bool) v["automationOn"];
            if (auto* ls = v["autoLanes"].getArray())
                for (auto& lv : *ls)
                {
                    AutoLane l; l.id = juce::Uuid (lv["id"].toString()); l.trackId = juce::Uuid (lv["track"].toString());
                    l.param = lv["param"].toString(); l.mixerId = juce::Uuid (lv["mixer"].toString()); l.locked = (bool) lv["locked"];
                    if (auto* ps = lv["points"].getArray())
                        for (auto& pv : *ps)
                        {
                            AutoPoint p; p.id = juce::Uuid (pv["id"].toString()); p.region = juce::Uuid (pv["region"].toString()); p.take = juce::Uuid (pv["take"].toString());
                            p.srcPos = (juce::int64) pv["src"]; p.time = (juce::int64) pv["time"]; p.value = (float) (double) pv["value"];
                            p.floating = (bool) pv["floating"];
                            l.pts.push_back (p);
                        }
                    l.sortPoints();
                    e->lanes.push_back (std::move (l));
                }
            e->hardenEnds();                                    // older projects: the ends of the edit get their short fades too
            e->clampAll();
            edits.push_back (std::move (e));
        }

    mastering.fromVar (root["mastering"]);
    mastering.sync (edits);
    syncMixers();
    structureChanged();
    dirty = false;
    return true;
}

// ------------------------------------------------------------------------------------------------ undo
juce::String Project::undoState() const
{
    var v = toVar (true);
    auto strip = [] (const var& o, std::initializer_list<const char*> keys)
    {
        if (auto* d = o.getDynamicObject()) for (auto* k : keys) d->removeProperty (juce::Identifier (k));
    };
    if (auto* a = v["takeWindows"].getArray())
        for (auto& w : *a) strip (w, { "markTake", "markIn", "markOut", "editTake", "editIn", "editOut", "cursorTake", "cursorSeconds", "playhead", "hiddenTracks", "overdub" });
    if (auto* a = v["edits"].getArray())
        for (auto& e : *a) strip (e, { "markIn", "markOut", "fixIn", "fixOut", "playhead", "insertIndex" });
    if (auto m = v["mastering"]; m.isObject()) strip (m, { "view" });
    return juce::JSON::toString (v, true);
}

void Project::undoTick (bool recording, bool force)
{
    if (! undoPending) return;
    if (! force && juce::Time::getMillisecondCounter() - undoDirtyAt < 500) return;
    undoPending = false;
    if (recording) { undoCurrent = {}; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); return; }     // the take growing is not an undo step: look again after it stops
    const auto now = undoState();
    if (undoCurrent.isEmpty()) { undoCurrent = now; return; }
    if (now == undoCurrent) return;
    undoStack.push_back (undoCurrent);
    if ((int) undoStack.size() > kUndoLevels) undoStack.erase (undoStack.begin());
    redoStack.clear();
    undoCurrent = now;
}

namespace
{
void restoreEditorial (Project& p, const var& snapshot, const juce::Uuid& liveTake)
{
    Project tmp;
    tmp.restoreSampleRate = p.restoreSampleRate; tmp.restoreBlock = p.restoreBlock;
    if (! tmp.fromVar (snapshot)) return;
    for (auto& te : tmp.edits)
    {
        if (auto* live = p.findEdit (te->id))
        {
            te->markIn = live->markIn; te->markOut = live->markOut; te->fixIn = live->fixIn; te->fixOut = live->fixOut;
            te->playheadSeconds = live->playheadSeconds; te->insertIndex = live->insertIndex;
            *live = std::move (*te);
        }
        else p.edits.push_back (std::move (te));
    }
    for (auto& tw : tmp.takeWindows)
    {
        auto* live = p.findTakeWindow (tw->id);
        if (live == nullptr) continue;
        for (auto& tg : tw->groups)
        {
            if (tg.id == liveTake) continue;
            if (auto* lg = live->findGroup (tg.id))
            {
                lg->label = tg.label; lg->startSeconds = tg.startSeconds; lg->fadeInSeconds = tg.fadeInSeconds; lg->fadeOutSeconds = tg.fadeOutSeconds;
                lg->dud = tg.dud; lg->barIn = tg.barIn; lg->barOut = tg.barOut; lg->number = tg.number;
            }
            else live->groups.push_back (tg);                    // a take that was removed from the window comes back
        }
        std::stable_sort (live->groups.begin(), live->groups.end(), [] (const TakeGroup& a, const TakeGroup& b) { return a.number < b.number; });
        live->targetEdit = tw->targetEdit;
    }
    const int view = p.mastering.view; const auto selI = p.mastering.selectedItem, selC = p.mastering.selectedClip;
    p.mastering = tmp.mastering;
    p.mastering.view = view; p.mastering.selectedItem = selI; p.mastering.selectedClip = selC;
    p.mastering.sync (p.edits);
}
}

bool Project::undo (const juce::Uuid& liveTake)
{
    undoTick (false, true);
    if (undoStack.empty()) return false;
    const auto target = undoStack.back(); undoStack.pop_back();
    redoStack.push_back (undoCurrent);
    restoreEditorial (*this, juce::JSON::parse (target), liveTake);
    structureChanged();
    undoPending = false; undoCurrent = undoState();
    return true;
}

bool Project::redo (const juce::Uuid& liveTake)
{
    undoTick (false, true);
    if (redoStack.empty()) return false;
    const auto target = redoStack.back(); redoStack.pop_back();
    undoStack.push_back (undoCurrent);
    restoreEditorial (*this, juce::JSON::parse (target), liveTake);
    structureChanged();
    undoPending = false; undoCurrent = undoState();
    return true;
}

bool Project::saveAs (const juce::File& f, juce::String& error)
{
    projectFile = f;
    return save (error);
}

bool Project::save (juce::String& error)
{
    if (projectFile == juce::File()) { error = "No project file chosen"; return false; }
    projectFile.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (projectFile);
    if (! tmp.getFile().replaceWithText (juce::JSON::toString (toVar(), false)) || ! tmp.overwriteTargetFileWithTemporary())
    {
        error = "Could not write " + projectFile.getFullPathName();
        return false;
    }
    dirty = false;
    return true;
}

std::unique_ptr<Project> Project::cloneForRender (double sampleRate, int maxBlock) const
{
    auto c = std::make_unique<Project>();
    c->insertFactory = insertFactory;                      // the copy gets its own plug-in instances, built from the saved state of the live ones
    c->restoreSampleRate = sampleRate; c->restoreBlock = maxBlock;
    c->fromVar (toVar());
    c->projectFile = projectFile;
    return c;
}

juce::StringArray Project::repairInterruptedRecordings()
{
    juce::StringArray report;
    for (auto& w : takeWindows)
        for (auto& g : w->groups)
        {
            bool mended = false; juce::int64 longest = 0;
            for (auto& f : g.files)
            {
                if (! f.file.existsAsFile()) continue;
                const auto r = wavrepair::inspect (f.file, true);
                if (! r.ok) continue;
                if (r.wasBroken) mended = true;
                longest = juce::jmax (longest, r.frames);
            }
            // a take that was recorded when the program stopped was saved with length 0; a mended one is given the length of what was kept
            if (longest > 0 && (mended || g.lengthSamples == 0) && g.lengthSamples != longest)
            {
                g.lengthSamples = longest; mended = true;
            }
            if (mended)
            {
                const double secs = g.sampleRate > 0 ? (double) longest / g.sampleRate : 0.0;
                const int h = (int) (secs / 3600.0), m = (int) std::fmod (secs / 60.0, 60.0), sec = (int) std::fmod (secs, 60.0);
                report.add (w->name + " - take " + w->displayName (g) + ":  " + juce::String::formatted ("%d:%02d:%02d", h, m, sec) + " of audio kept");
            }
        }
    if (! report.isEmpty()) markDirty();
    return report;
}

bool Project::load (const juce::File& f, juce::String& error)
{
    juce::var v;
    auto r = juce::JSON::parse (f.loadFileAsString(), v);
    readingFrom = f.getParentDirectory();
    const bool ok = ! r.failed() && fromVar (v);
    readingFrom = juce::File();
    if (! ok) { error = "Not a valid Fermata project file"; return false; }
    projectFile = f;
    return true;
}
} // namespace td
