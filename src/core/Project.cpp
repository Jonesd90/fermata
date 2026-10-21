#include "Project.h"
#include "FileSync.h"
#include "WavRepair.h"
#include "AudioRelink.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>

namespace td
{
namespace
{
using juce::var; using juce::DynamicObject; using juce::String;

var obj() { return var (new DynamicObject()); }
var markersToVar (const std::vector<Marker>& list)
{
    juce::Array<var> a;
    for (auto& m : list) { var o = new DynamicObject(); o.getDynamicObject()->setProperty ("id", m.id.toString()); o.getDynamicObject()->setProperty ("name", m.name); o.getDynamicObject()->setProperty ("t", m.seconds); a.add (o); }
    return a;
}
void markersFromVar (const var& v, std::vector<Marker>& list)
{
    list.clear();
    if (auto* a = v.getArray())
        for (auto& o : *a)
        {
            Marker m; m.id = juce::Uuid (o["id"].toString()); m.name = o["name"].toString(); m.seconds = juce::jmax (0.0, (double) o["t"]);
            list.push_back (m);
        }
}
var feedbackToVar (const std::vector<FeedbackNote>& list)
{
    juce::Array<var> a;
    for (auto& n : list)
    {
        var o = new DynamicObject(); auto* d = o.getDynamicObject();
        d->setProperty ("key", n.key); d->setProperty ("who", n.who); d->setProperty ("bar", n.bar); d->setProperty ("text", n.text);
        d->setProperty ("colour", n.colour); d->setProperty ("source", n.source); d->setProperty ("t", n.seconds); if (n.done) d->setProperty ("done", true);
        a.add (o);
    }
    return a;
}
void feedbackFromVar (const var& v, std::vector<FeedbackNote>& list)
{
    list.clear();
    if (auto* a = v.getArray())
        for (auto& o : *a)
        {
            FeedbackNote n; n.key = o["key"].toString(); n.who = o["who"].toString(); n.bar = o["bar"].toString(); n.text = o["text"].toString();
            n.colour = o["colour"].toString(); n.source = o["source"].toString(); n.seconds = juce::jmax (0.0, (double) o["t"]); n.done = (bool) o["done"];
            list.push_back (n);
        }
}
void put (var& o, const char* k, const var& v) { o.getDynamicObject()->setProperty (k, v); }
void put (var& o, const juce::Identifier& k, const var& v) { o.getDynamicObject()->setProperty (k, v); }

// The keys of the takes and pieces, made once: looking a key up by text goes through JUCE's locked string pool every time (a project with 100,000
// recordings read a million of them: a tenth of a second of opening time).
namespace key
{
static const juce::Identifier k_id ("id");
static const juce::Identifier k_number ("number");
static const juce::Identifier k_label ("label");
static const juce::Identifier k_start ("start");
static const juce::Identifier k_length ("length");
static const juce::Identifier k_rate ("rate");
static const juce::Identifier k_time ("time");
static const juce::Identifier k_fadeIn ("fadeIn");
static const juce::Identifier k_fadeOut ("fadeOut");
static const juce::Identifier k_dud ("dud");
static const juce::Identifier k_verified ("verified");
static const juce::Identifier k_barIn ("barIn");
static const juce::Identifier k_barOut ("barOut");
static const juce::Identifier k_roomTone ("roomTone"), k_roomToneIn ("roomToneIn"), k_roomToneOut ("roomToneOut");
static const juce::Identifier k_overruns ("overruns");
static const juce::Identifier k_lost ("lost");
static const juce::Identifier k_writeFailed ("writeFailed");
static const juce::Identifier k_files ("files");
static const juce::Identifier k_track ("track");
static const juce::Identifier k_trackName ("trackName");
static const juce::Identifier k_file ("file");
static const juce::Identifier k_rel ("rel");
static const juce::Identifier k_channels ("channels");
static const juce::Identifier k_orig ("orig");
static const juce::Identifier k_orig2 ("orig2");
static const juce::Identifier k_waiting ("waiting");
static const juce::Identifier k_window ("window");
static const juce::Identifier k_take ("take");
static const juce::Identifier k_takeName ("takeName");
static const juce::Identifier k_in ("in");
static const juce::Identifier k_out ("out");
static const juce::Identifier k_sourceLength ("sourceLength");
static const juce::Identifier k_curve ("curve");
static const juce::Identifier k_outCurve ("outCurve");
static const juce::Identifier k_fixIn ("fixIn");
static const juce::Identifier k_fixOut ("fixOut");
static const juce::Identifier k_normaliseIn ("normaliseIn");
static const juce::Identifier k_inStart ("inStart");
static const juce::Identifier k_inEnd ("inEnd");
static const juce::Identifier k_outStart ("outStart");
static const juce::Identifier k_outEnd ("outEnd");
static const juce::Identifier k_fileStart ("fileStart");
static const juce::Identifier k_gains ("gains");
static const juce::Identifier k_at ("at");
static const juce::Identifier k_ramp ("ramp");
static const juce::Identifier k_autoNorm ("autoNorm");
static const juce::Identifier k_db ("db");
}

/** Runs fn (0 .. n-1). (It was tried on several threads: JUCE's JSON keys all go through one locked string pool, so parsing or reading vars on
    two threads at once took three times as long as on one. Kept as one place to try again should that change.) */
template <typename F> void parallelFor (int n, F&& fn) { for (int i = 0; i < n; ++i) fn (i); }

/** True while a quick (automatic) save writes the project: the plug-ins may hand over a cached state. */
thread_local bool quickPluginState = false;

var slotsToVar (const InsertSlot* slots, int count = kNumSlots)
{
    juce::Array<var> arr;
    for (int i = 0; i < count; ++i)
    {
        var s = obj();
        if (auto* p = slots[i].getProcessorUnsafe())
        {
            put (s, "desc", p->getDescription());
            put (s, "state", quickPluginState ? p->getStateBase64Quick() : p->getStateBase64());
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
        // a muted send is written with its level off and the level it keeps beside it, so an older Fermata (which knows no send mute) keeps it silent
        put (o, "dest", s->dest.toString()); put (o, "gainDb", s->mute.get() ? -100.0f : s->gainDb.get()); put (o, "pre", s->pre.get());
        if (! s->followMute.get()) put (o, "followMute", false);          // (only written when off: on is how every send always was)
        if (s->mute.get()) { put (o, "mute", true); put (o, "mutedGainDb", s->gainDb.get()); }
        if (inserts) put (o, "slots", slotsToVar (s->slots, kSendSlots));
        arr.add (o);
    }
    return arr;
}

void sendsFromVar (SendList& l, const var& v, const InsertFactory& factory, double sr, int block, bool inserts, bool levels = true)
{
    if (! levels && ! inserts) return;
    std::vector<SendState*> seen;
    if (auto* a = v.getArray())
        for (auto& sv : *a)
        {
            auto* s = l.get (juce::Uuid (sv["dest"].toString()));
            if (levels)
            {
                const bool muted = sv.hasProperty ("mute") && (bool) sv["mute"];
                s->gainDb.set ((float) (double) (muted && sv.hasProperty ("mutedGainDb") ? sv["mutedGainDb"] : sv["gainDb"]));
                s->pre.set ((bool) sv["pre"]); s->followMute.set (sv.hasProperty ("followMute") ? (bool) sv["followMute"] : true);
                s->mute.set (muted);
            }
            if (inserts) slotsFromVar (s->slots, sv["slots"], factory, sr, block, kSendSlots);
            seen.push_back (s);
        }
    if (levels)
    for (auto& s : l.pool)                                         // sends the other side did not have are switched off
        if (std::find (seen.begin(), seen.end(), s.get()) == seen.end()) { s->gainDb.set (-100.0f); s->mute.set (false); }
}

var stripToVar (const StripState& s, const juce::Uuid& trackId, bool inserts)
{
    var so = obj();
    put (so, "track", trackId.toString()); put (so, "gainDb", s.gainDb.get()); if (s.inGainDb.get() != 0.0f) put (so, "inGainDb", s.inGainDb.get()); put (so, "pan", s.pan.get());
    put (so, "panL", s.panL.get()); put (so, "panR", s.panR.get());
    put (so, "mute", s.mute.get()); put (so, "solo", s.solo.get());
    put (so, "outOn", s.outOn.get()); put (so, "outDest", s.outDest.toString());
    if (const auto vm = s.vcaMask.load(); vm != 0) put (so, "vcaMask", (juce::int64) vm);
    put (so, "sendList", sendsToVar (s.sends, inserts));
    if (inserts) put (so, "slots", slotsToVar (s.slots));
    return so;
}

void stripFromVar (StripState& s, const var& sv, const InsertFactory& factory, double sr, int block, bool inserts, const std::vector<juce::Uuid>& legacyFx, bool faders = true, bool pan = true, bool sendLevels = true)
{
    if (pan) { s.pan.set ((float) (double) sv["pan"]); if (sv.hasProperty ("panL")) { s.panL.set ((float) (double) sv["panL"]); s.panR.set ((float) (double) sv["panR"]); } }
    if (faders)
    {
        s.gainDb.set ((float) (double) sv["gainDb"]);
        s.inGainDb.set (sv.hasProperty ("inGainDb") ? (float) (double) sv["inGainDb"] : 0.0f);
        s.mute.set ((bool) sv["mute"]); s.solo.set ((bool) sv["solo"]);
        s.outOn.set (sv.hasProperty ("outOn") ? (bool) sv["outOn"] : false);          // older projects had no output button: nothing was routed that way
        s.outDest = juce::Uuid (sv["outDest"].toString());
        s.vcaMask.store ((juce::uint32) (juce::int64) sv.getProperty ("vcaMask", (juce::int64) 0));
    }
    if (sv.hasProperty ("sendList")) sendsFromVar (s.sends, sv["sendList"], factory, sr, block, inserts, sendLevels);
    else if (sendLevels)
        if (auto* old = sv["sends"].getArray())                   // projects from before buses: one send level per effects channel
        for (int k = 0; k < old->size() && k < (int) legacyFx.size(); ++k)
            if ((double) (*old)[k] > kSendOffDb) s.sends.get (legacyFx[(size_t) k])->gainDb.set ((float) (double) (*old)[k]);
    if (inserts) slotsFromVar (s.slots, sv["slots"], factory, sr, block);
}

var busToVar (const BusState& s, const juce::Uuid& busId, bool inserts, bool withOutput)
{
    var o = obj();
    put (o, "bus", busId.toString()); put (o, "gainDb", s.gainDb.get()); put (o, "mute", s.mute.get()); put (o, "solo", s.solo.get());
    if (s.inGainDb.get() != 0.0f) put (o, "inGainDb", s.inGainDb.get());
    if (s.pan.get() != 0.0f) put (o, "pan", s.pan.get());
    if (withOutput) { put (o, "outFirst", s.outFirst.load()); if (s.isMain.get()) put (o, "main", true); }
    put (o, "outOn", s.outOn.get()); put (o, "outDest", s.outDest.toString());
    if (const auto vm = s.vcaMask.load(); vm != 0) put (o, "vcaMask", (juce::int64) vm);
    put (o, "sendList", sendsToVar (s.sends, inserts));
    if (inserts) put (o, "slots", slotsToVar (s.slots));
    return o;
}

void busFromVar (BusState& s, const var& v, const InsertFactory& factory, double sr, int block, bool inserts, bool withOutput, bool faders = true, bool sendLevels = true, bool pan = true)
{
    if (pan) s.pan.set (v.hasProperty ("pan") ? (float) (double) v["pan"] : 0.0f);
    if (faders)
    {
        s.inGainDb.set (v.hasProperty ("inGainDb") ? (float) (double) v["inGainDb"] : 0.0f);
        s.gainDb.set ((float) (double) v["gainDb"]); s.mute.set ((bool) v["mute"]); s.solo.set ((bool) v["solo"]);
        if (withOutput && v.hasProperty ("outFirst")) s.outFirst.store ((int) v["outFirst"]);
        if (withOutput) s.isMain.set ((bool) v.getProperty ("main", false));
        s.outOn.set (v.hasProperty ("outOn") ? (bool) v["outOn"] : false);
        s.outDest = juce::Uuid (v["outDest"].toString());
        s.vcaMask.store ((juce::uint32) (juce::int64) v.getProperty ("vcaMask", (juce::int64) 0));
    }
    if (v.hasProperty ("sendList")) sendsFromVar (s.sends, v["sendList"], factory, sr, block, inserts, sendLevels);
    if (inserts) slotsFromVar (s.slots, v["slots"], factory, sr, block);
}

var vcaStatesToVar (const MixerState& m, const std::vector<VcaDef>& defs)
{
    juce::Array<var> a;
    for (auto& d : defs)
    {
        const auto& s = m.vcas[juce::jlimit (0, kMaxVcas - 1, d.number - 1)];
        var o = obj();
        put (o, "number", d.number); put (o, "gainDb", s.gainDb.get()); put (o, "mute", s.mute.get()); put (o, "solo", s.solo.get());
        put (o, "shown", (m.vcaShown.load() & (1u << juce::jlimit (0, kMaxVcas - 1, d.number - 1))) != 0);
        a.add (o);
    }
    return a;
}
void vcaStatesFromVar (MixerState& m, const var& v)
{
    if (auto* a = v.getArray())
        for (auto& o : *a)
        {
            const int n = (int) o["number"];
            if (n < 1 || n > kMaxVcas) continue;
            auto& s = m.vcas[n - 1];
            s.gainDb.set ((float) (double) o["gainDb"]); s.mute.set ((bool) o["mute"]); s.solo.set ((bool) o["solo"]);
            if ((bool) o["shown"]) m.vcaShown.fetch_or (1u << (n - 1)); else m.vcaShown.fetch_and (~(1u << (n - 1)));
        }
}
} // namespace

Project::Project() { createDefaultDesign(); }

void Project::createDefaultDesign()
{
    inputs.clear(); tracks.clear(); buses.clear(); vcas.clear(); outputs.clear(); crMicInputs.clear(); tbOutputs.clear(); stageGainDb.clear(); stagePan.clear(); stagePlaybackDb = 0.0f; stageOutputDb = 0.0f; altMixer = juce::Uuid::null(); ltcInput = -1; ltcOutput = -1; ltcOutOn = false; retireAllMixers(); takeWindows.clear(); edits.clear(); mastering = MasteringDef(); roomTone = RoomTone(); preampDevices.clear();
    windowFiles.clear(); missingWindowRefs.clear(); loadNotes.clear(); oldWindowFiles.clear();     // (nothing of the project that was open before: its missing files are not this one's)
    setInputCount (8);
    setOutputCount (4);
    addMixer ("Processing mixer");
    addTakeWindow ("Piece 1");
    syncMixers();
}

BusDef& Project::addDefaultMainBus()
{
    auto& b = addBus ("Main", true);
    const auto id = b.id;
    for (auto& m : mixers)
        if (auto* bs = m->busFor (id)) { bs->isMain.set (true); bs->outFirst.store (0); }      // outputs 1-2 (the first pair of the audio driver)
    return *findBus (id);
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
    if (e.trackIds.empty()) { for (auto& t : tracks) if (e.showLtc || ! t.isLtc) v.push_back (&t); return v; }
    for (auto& id : e.trackIds) for (auto& t : tracks) if (t.id == id) { if (e.showLtc || ! t.isLtc) v.push_back (&t); break; }
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

VcaDef* Project::addVca (const juce::String& vcaName, bool inProcessingAndEditMixers)
{
    for (int n = 1; n <= kMaxVcas; ++n)
        if (vcaByNumber (n) == nullptr)
        {
            VcaDef d; d.number = n; d.name = vcaName;
            vcas.push_back (d);
            std::sort (vcas.begin(), vcas.end(), [] (const VcaDef& x, const VcaDef& y) { return x.number < y.number; });
            if (inProcessingAndEditMixers)
                for (size_t i = 0; i < mixers.size(); ++i) if (i == 0 || ! mixers[i]->editId.isNull()) mixers[i]->vcaShown.fetch_or (1u << (n - 1));
            structureChanged();
            return findVca (d.id);
        }
    return nullptr;
}

void Project::removeVca (const juce::Uuid& id)
{
    for (size_t i = 0; i < vcas.size(); ++i)
        if (vcas[i].id == id)
        {
            const int bit = vcas[i].number - 1;
            for (auto& m : mixers)                                         // nothing is controlled by it any more, and a later VCA with this number starts fresh
            {
                for (auto& s : m->stripPool) s->vcaMask.fetch_and (~(1u << bit));
                for (auto& b : m->busPool)   b->vcaMask.fetch_and (~(1u << bit));
                m->vcaShown.fetch_and (~(1u << bit));
                m->vcas[bit].gainDb.set (0.0f); m->vcas[bit].mute.set (false); m->vcas[bit].solo.set (false);
            }
            vcas.erase (vcas.begin() + (std::ptrdiff_t) i);
            break;
        }
    structureChanged();
}

void Project::showVca (MixerState& m, const VcaDef& vca, bool shown)
{
    const juce::uint32 bit = 1u << juce::jlimit (0, kMaxVcas - 1, vca.number - 1);
    if (shown) m.vcaShown.fetch_or (bit); else m.vcaShown.fetch_and (~bit);
    markDirty(); structureChanged();
}

VcaDef* Project::findVca (const juce::Uuid& id)             { for (auto& v : vcas) if (v.id == id) return &v; return nullptr; }
const VcaDef* Project::findVca (const juce::Uuid& id) const { for (auto& v : vcas) if (v.id == id) return &v; return nullptr; }
VcaDef* Project::vcaByNumber (int number)                    { for (auto& v : vcas) if (v.number == number) return &v; return nullptr; }

void Project::setVcaMember (MixerState& m, const VcaDef& vca, const juce::Uuid& node, bool on)
{
    const juce::uint32 bit = 1u << juce::jlimit (0, kMaxVcas - 1, vca.number - 1);
    std::atomic<juce::uint32>* mask = nullptr;
    switch (kindOf (node))
    {
        case NodeKind::Track:  mask = &m.stripFor (node)->vcaMask; break;
        case NodeKind::IntBus: mask = &m.busFor (node)->vcaMask; break;       // (an Ext Bus, being where sound leaves, is never controlled by a VCA)
        default: return;
    }
    if (on) mask->fetch_or (bit); else mask->fetch_and (~bit);
    markDirty();
}

bool Project::isVcaMember (const MixerState& m, const VcaDef& vca, const juce::Uuid& node) const
{
    const juce::uint32 bit = 1u << juce::jlimit (0, kMaxVcas - 1, vca.number - 1);
    for (auto& s : m.stripPool) if (s->trackId == node) return (s->vcaMask.load() & bit) != 0;
    for (auto& b : m.busPool)   if (b->busId == node)   return (b->vcaMask.load() & bit) != 0;
    return false;
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
    for (auto& v : vcas)  if (v.id == id) return NodeKind::Vca;
    return NodeKind::None;
}

void Project::setChannelColour (const juce::Uuid& id, juce::uint32 argb)
{
    for (auto& t : tracks) if (t.id == id) { t.colour = argb; changed(); return; }
    for (auto& b : buses)  if (b.id == id) { b.colour = argb; changed(); return; }
    for (auto& v : vcas)   if (v.id == id) { v.colour = argb; changed(); return; }
}

juce::uint32 Project::channelColour (const juce::Uuid& id) const
{
    for (auto& t : tracks) if (t.id == id) return t.colour;
    for (auto& b : buses)  if (b.id == id) return b.colour;
    for (auto& v : vcas)   if (v.id == id) return v.colour;
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
    for (auto& v : vcas)  if (v.id == id) return v.label();
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

juce::String Project::backupWithCurrentWindows (const juce::String& backupText, const juce::String& currentText)
{
    var b, c;
    if (juce::JSON::parse (backupText, b).failed() || ! b.isObject() || juce::JSON::parse (currentText, c).failed() || ! c.isObject()) return backupText;
    for (auto* key : { "mixerFiles", "takeWindowFiles", "editFiles", "mixers", "takeWindows", "edits" })
    {
        if (c.hasProperty (key)) b.getDynamicObject()->setProperty (key, c[key]);
        else b.getDynamicObject()->removeProperty (key);
    }
    return juce::JSON::toString (b);
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
    const auto at = (size_t) cueEnd();                  // cue mixers go before the Edit mixers
    mixers.insert (mixers.begin() + (std::ptrdiff_t) at, std::move (m));
    syncMixers();
    structureChanged();
    if (onNewWindow) onNewWindow();
    return *mixers[at];
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
    if (onNewWindow) onNewWindow();
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
    if (syncEditMixers()) structureChanged();           // the new Edit gets its own mixer
    else changed();
    if (onNewWindow) onNewWindow();
    return *edits.back();
}

MixerState& Project::makeEditMixer (const EditDef& e)
{
    auto m = std::make_unique<MixerState>();
    m->name = e.name; m->editId = e.id;
    mixers.push_back (std::move (m));
    auto& mx = *mixers.back();
    syncMixers();
    // it starts as a copy of the processing mixer: levels, pans, mutes, sends, plug-ins, and the same Ext buses going to the same outputs
    for (auto& src : mixers)
    {
        if (! src->editId.isNull()) continue;
        applyMix (mx, mixSnapshot (*src, true), restoreSampleRate, restoreBlock);
        mx.ditherBits.store (src->ditherBits.load());
        for (auto& b : buses)
            if (b.external)
            {
                const BusState* sb = nullptr;
                for (auto& p : src->busPool) if (p->busId == b.id) sb = p.get();
                if (sb != nullptr) { mx.busFor (b.id)->outFirst.store (sb->outFirst.load()); mx.busFor (b.id)->isMain.set (sb->isMain.get()); }
            }
        break;                                           // (the first mixer that is not an Edit mixer is the processing mixer)
    }
    return mx;
}

bool Project::syncEditMixers()
{
    bool any = false;
    for (size_t i = mixers.size(); i-- > 0;)             // mixers whose Edit is gone
        if (! mixers[i]->editId.isNull() && findEdit (mixers[i]->editId) == nullptr)
        {
            retiredMixers.push_back (std::move (mixers[i])); mixers.erase (mixers.begin() + (std::ptrdiff_t) i); any = true;
        }
    if (cueEnd() == 0) return any;                       // no processing mixer to copy from (cannot normally happen)
    for (auto& e : edits)
    {
        if (auto* m = mixerOfEdit (e->id)) { if (m->name != e->name) { m->name = e->name; any = true; } }
        else { makeEditMixer (*e); any = true; }
    }
    return any;
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
        bool hasExt = false;                                // does this mixer have an Ext Bus already? (its first one becomes its main output)
        for (auto& f : buses) if (f.external) for (auto& p : m->busPool) if (p->busId == f.id) hasExt = true;
        for (auto& f : buses)
        {
            const auto n = m->busPool.size();
            auto* bs = m->busFor (f.id);
            if (m->busPool.size() != n && newStripsToMain && ! f.external) bs->outOn.set (true);
            if (m->busPool.size() != n && f.external && ! hasExt) { bs->isMain.set (true); hasExt = true; }       // the mixer's first Ext Bus: ticked as its main output
        }
    }
}

void Project::adoptPreamp (int inputIndex, const PreampSettings& s)
{
    if (! juce::isPositiveAndBelow (inputIndex, (int) inputs.size())) return;
    if (inputs[(size_t) inputIndex].preamp.line != s.line)       // for the log: who changed Mic / Line
        juce::Logger::writeToLog ("Preamp: input " + juce::String (inputIndex + 1) + " Mic/Line " + (s.line ? "MIC -> LINE" : "LINE -> MIC") + " reported by the HARDWARE (gain " + juce::String (s.gainDb, 1) + ")");
    inputs[(size_t) inputIndex].preamp = s;
    sendChangeMessage();                                  // refresh the windows; nothing to save, the hardware is the source of truth
}

void Project::setPreamp (int inputIndex, const PreampSettings& s)
{
    if (! juce::isPositiveAndBelow (inputIndex, (int) inputs.size())) return;
    if (inputs[(size_t) inputIndex].preamp.line != s.line)
        juce::Logger::writeToLog ("Preamp: input " + juce::String (inputIndex + 1) + " Mic/Line " + (s.line ? "MIC -> LINE" : "LINE -> MIC") + " set from the program (gain " + juce::String (s.gainDb, 1) + ")");
    auto& cur = inputs[(size_t) inputIndex].preamp;
    PreampSettings kept = s;                              // what the input CAN do comes from the hardware, not from what is being set (a restored memory has none of it)
    kept.lineOnly = cur.lineOnly; kept.hasInstrument = cur.hasInstrument; kept.lowCutChoice = cur.lowCutChoice; kept.steppedPad = cur.steppedPad;
    cur = kept;
    if (preampDriver != nullptr) preampDriver->apply (inputIndex, kept);
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
    put (root, "vcaStates", vcaStatesToVar (m, vcas));
    return root;
}

void Project::applyMix (MixerState& to, const var& snap, double sampleRate, int maxBlock, const MixParts& parts)
{
    syncMixers();
    const bool withInserts = (bool) snap.getProperty ("inserts", false) && parts.inserts;
    const std::vector<juce::Uuid> noLegacy;
    if (auto* ss = snap["strips"].getArray())
        for (auto& sv : *ss)
        {
            const juce::Uuid id (sv["track"].toString());
            StripState* s = nullptr;
            for (auto& p : to.stripPool) if (p->trackId == id) s = p.get();
            if (s != nullptr) stripFromVar (*s, sv, insertFactory, sampleRate, maxBlock, withInserts, noLegacy, parts.faders, parts.pan, parts.sends);   // (a track that no longer exists is skipped)
        }
    if (auto* fs = snap["buses"].getArray())
        for (auto& fv : *fs)
        {
            const juce::Uuid id (fv["bus"].toString());
            BusState* f = nullptr;
            for (auto& p : to.busPool) if (p->busId == id) f = p.get();
            if (f != nullptr) busFromVar (*f, fv, insertFactory, sampleRate, maxBlock, withInserts, false, parts.faders, parts.sends, parts.pan);
        }
    if (parts.faders) vcaStatesFromVar (to, snap["vcaStates"]);
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
juce::File Project::commentsFolderFor (const EditDef& e) const
{
    const auto id = e.id.toString().substring (0, 8);
    auto name = juce::File::createLegalFileName (e.name.trim()).trim(); if (name.isEmpty()) name = "Edit";
    const auto base = projectFolder().getChildFile ("comments");
    for (auto& d : base.findChildFiles (juce::File::findDirectories, false, "* [" + id + "]")) return d;          // the edit may have been renamed since
    return base.getChildFile (name + " [" + id + "]");
}
void Project::saveCommentsCopy (const EditDef& e, const std::vector<juce::File>& importedFiles) const
{
    const auto dir = commentsFolderFor (e);
    dir.deleteRecursively();
    if (! dir.createDirectory().wasOk()) return;
    for (auto& f : importedFiles) f.copyFileTo (dir.getChildFile (f.getFileName()));
    auto json = dir.getChildFile ("comments.json");
    json.replaceWithText (juce::JSON::toString (feedbackToVar (e.feedback)));
}
void Project::restoreSavedComments()
{
    for (auto& e : edits)
    {
        if (e == nullptr || ! e->feedback.empty()) continue;
        auto json = commentsFolderFor (*e).getChildFile ("comments.json");
        juce::var v;
        if (json.existsAsFile() && ! juce::JSON::parse (json.loadFileAsString(), v).failed()) { feedbackFromVar (v, e->feedback); e->showFeedback = true; }
    }
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
    for (auto& f : w.files) { var fo = obj(); putFilePath (fo, f, base); put (fo, key::k_channels, 1); fs.add (fo); }      // ("channels" marks it as an audio file for the project copy)
    put (o, "files", fs);
    if (! w.tracks.empty()) { juce::Array<var> ts; for (auto& t : w.tracks) ts.add (t.toString()); put (o, "tracks", ts); }
    return o;
}
/** The file for a saved path: inside the project folder it is found from the relative path (so a moved or copied project uses its OWN files). */
static juce::File getFilePath (const var& fv, const juce::File& base, bool* moved = nullptr)
{
    const auto rel = fv[key::k_rel].toString();
    if (rel.isNotEmpty() && base != juce::File())
    {
        auto f = base.getChildFile (rel);
        if (f.getFullPathName() == fv[key::k_file].toString()) return f;      // the project is still where it was saved: no need to ask the disk (thousands of files)
        if (f.existsAsFile()) { if (moved != nullptr) *moved = true; return f; }
    }
    return juce::File (fv[key::k_file].toString());
}

static WaitingPiece waitingFromVar (const var& o, const juce::File& base)
{
    WaitingPiece w;
    w.name = o["name"].toString(); w.from = (juce::int64) o["from"]; w.to = (juce::int64) o["to"];
    if (auto* fs = o["files"].getArray()) for (auto& fv : *fs) w.files.push_back (getFilePath (fv, base));
    if (auto* ts = o["tracks"].getArray()) for (auto& t : *ts) w.tracks.push_back (juce::Uuid (t.toString()));
    return w;
}

juce::File Project::takeFolder (const TakeWindowDef& w) const
{
    return audioFolder().getChildFile (sanitiseForFile (w.name));
}

// ---------------------------------------------------------------- persistence
var Project::mixerToVar (const MixerState& mref) const
{
    const MixerState* m = &mref;
    var o = obj();
    put (o, "id", m->id.toString()); put (o, "name", m->name);
    if (! m->editId.isNull()) put (o, "editId", m->editId.toString());
    juce::Array<var> strips;
    for (auto& t : tracks)
    {
        const StripState* s = nullptr;
        for (auto& p : m->stripPool) if (p->trackId == t.id) s = p.get();
        if (s != nullptr) strips.add (stripToVar (*s, t.id, true));
    }
    put (o, "strips", strips);
    put (o, "mainFlags", true);                            // (the main outputs are the Ext buses' own "main" ticks; older projects had one "mainBus")
    if (m->followProcessing.get()) put (o, "followProcessing", true);
    put (o, "ditherBits", m->ditherBits.load());
    if (! m->hiddenStrips.empty())                         // tracks taken out of this mixer (only those the project still has)
    {
        juce::Array<var> hid;
        for (auto& h : m->hiddenStrips) for (auto& t : tracks) if (t.id == h) { hid.add (h.toString()); break; }
        if (! hid.isEmpty()) put (o, "hiddenStrips", hid);
    }
    juce::Array<var> fxs;
    for (auto& f : buses)
    {
        const BusState* s = nullptr;
        for (auto& p : m->busPool) if (p->busId == f.id) s = p.get();
        if (s != nullptr) fxs.add (busToVar (*s, f.id, true, true));
    }
    put (o, "busStates", fxs);
    put (o, "vcaStates", vcaStatesToVar (*m, vcas));
    return o;
}

var Project::editToVar (const EditDef& eref) const
{
    const EditDef* e = &eref;
    var o = obj();
    put (o, "id", e->id.toString()); put (o, "name", e->name); put (o, "window", e->windowId.toString());
    put (o, "rate", e->sampleRate); put (o, "insertIndex", e->insertIndex);
    put (o, "fixIn", e->fixIn); put (o, "fixOut", e->fixOut);
    if (! e->fixTracks.empty()) { juce::Array<var> ft; for (auto& t : e->fixTracks) ft.add (t.toString()); put (o, "fixTracks", ft); }
    auto regionToVar = [this] (const EditRegion& r)
    {
        var ro = obj();
        put (ro, key::k_id, r.id.toString()); put (ro, key::k_window, r.windowId.toString()); put (ro, key::k_take, r.takeId.toString());
        put (ro, key::k_takeName, r.takeName); put (ro, key::k_in, (juce::int64) r.srcIn); put (ro, key::k_out, (juce::int64) r.srcOut);
        put (ro, key::k_start, (juce::int64) r.startSample); put (ro, key::k_sourceLength, (juce::int64) r.sourceLength); put (ro, key::k_barIn, r.barIn); put (ro, key::k_barOut, r.barOut);
        put (ro, key::k_rate, r.sampleRate); put (ro, key::k_curve, (int) r.curve); if (r.outCurveOwn) put (ro, key::k_outCurve, (int) r.outCurve); if (r.fixIn) put (ro, key::k_fixIn, true); if (r.fixOut) put (ro, key::k_fixOut, true); if (r.normaliseIn) put (ro, key::k_normaliseIn, true);
        put (ro, key::k_inStart, r.inStart); put (ro, key::k_inEnd, r.inEnd); put (ro, key::k_outStart, r.outStart); put (ro, key::k_outEnd, r.outEnd);
        juce::Array<var> rf;
        for (auto& f : r.files)
        {
            var fo = obj();
            put (fo, key::k_track, f.trackId.toString()); put (fo, key::k_trackName, f.trackName);
            putFilePath (fo, f.file, projectFile.getParentDirectory()); put (fo, key::k_channels, f.numChannels);
            if (f.fileStart != 0) put (fo, key::k_fileStart, (juce::int64) f.fileStart);
            rf.add (fo);
        }
        put (ro, key::k_files, rf);
        if (r.waiting.active()) put (ro, key::k_waiting, waitingToVar (r.waiting, projectFile.getParentDirectory()));
        juce::Array<var> gs;
        for (auto& g : r.gains)
        {
            var go = obj();
            put (go, key::k_at, (juce::int64) g.at); put (go, key::k_ramp, g.ramp); if (g.autoNorm) put (go, key::k_autoNorm, true);
            juce::Array<var> dbs; for (float d : g.db) dbs.add ((double) d);
            put (go, key::k_db, dbs);
            gs.add (go);
        }
        put (ro, key::k_gains, gs);
        return ro;
    };
    juce::Array<var> rs, ods;
    for (auto& r : e->regions)  rs.add (regionToVar (r));
    for (auto& r : e->overdubs) ods.add (regionToVar (r));
    put (o, "overdubs", ods); put (o, "playhead", e->playheadSeconds);
    { juce::Array<var> ti; for (auto& id : e->trackIds) ti.add (id.toString()); put (o, "trackIds", ti); }
    put (o, "automationOn", e->automationOn); if (e->showLtc) put (o, "showLtc", true);
    if (! e->markers.empty()) put (o, "markers", markersToVar (e->markers));
    if (! e->feedback.empty()) put (o, "feedback", feedbackToVar (e->feedback));
    if (! e->showFeedback) put (o, "showFeedback", false);
    if (! e->showMarkers) put (o, "showMarkers", false);
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
    return o;
}

var Project::takeWindowToVar (const TakeWindowDef& w) const
{
    var o = obj();
    put (o, "id", w.id.toString()); put (o, "name", w.name); put (o, "defaultLabel", w.defaultLabel);
    put (o, "nextNumber", w.nextNumber);
    put (o, "targetEdit", w.targetEdit.toString());
    put (o, "editTake", w.editTake.toString()); put (o, "editIn", w.editIn); put (o, "editOut", w.editOut); put (o, "overdub", w.overdub);
    if (! w.editTracks.empty()) { juce::Array<var> et; for (auto& t : w.editTracks) et.add (t.toString()); put (o, "editTracks", et); }       // Alt + drag: the marked tracks
    put (o, "cursorTake", w.cursorTake.toString()); put (o, "cursorSeconds", w.cursorSeconds); put (o, "playhead", w.playheadSeconds);
    { juce::Array<var> hid; for (auto& h : w.hiddenTracks) hid.add (h.toString()); put (o, "hiddenTracks", hid); }
    if (w.showLtc) put (o, "showLtc", true);
    if (! w.markers.empty()) put (o, "markers", markersToVar (w.markers));
    if (! w.showMarkers) put (o, "showMarkers", false);
    juce::Array<var> gs;
    for (auto& g : w.groups)
    {
        var go = obj();
        put (go, key::k_id, g.id.toString()); put (go, key::k_number, g.number); put (go, key::k_label, g.label);
        put (go, key::k_start, g.startSeconds); put (go, key::k_length, (juce::int64) g.lengthSamples);
        put (go, key::k_rate, g.sampleRate); put (go, key::k_time, g.recordedAt.toMilliseconds());
        put (go, key::k_fadeIn, g.fadeInSeconds); put (go, key::k_fadeOut, g.fadeOutSeconds); put (go, key::k_dud, g.dud); put (go, key::k_barIn, g.barIn); put (go, key::k_barOut, g.barOut);
        if (g.verified) put (go, key::k_verified, true);
        if (g.roomTone) { put (go, key::k_roomTone, true); put (go, key::k_roomToneIn, g.roomToneIn); put (go, key::k_roomToneOut, g.roomToneOut); }
        if (g.overruns > 0) { put (go, key::k_overruns, g.overruns); put (go, key::k_lost, (juce::int64) g.lostSamples); }
        if (g.writeFailed) put (go, key::k_writeFailed, true);
        juce::Array<var> fs;
        for (auto& f : g.files)
        {
            var fo = obj();
            put (fo, key::k_track, f.trackId.toString()); put (fo, key::k_trackName, f.trackName);
            putFilePath (fo, f.file, projectFile.getParentDirectory()); put (fo, key::k_channels, f.numChannels);
            if (f.sourceName.isNotEmpty()) put (fo, key::k_orig, f.sourceName);
            if (f.sourceName2.isNotEmpty()) put (fo, key::k_orig2, f.sourceName2);
            fs.add (fo);
        }
        put (go, key::k_files, fs);
        if (! g.waiting.empty())
        {
            juce::Array<var> ws; for (auto& wp : g.waiting) ws.add (waitingToVar (wp, projectFile.getParentDirectory()));
            put (go, key::k_waiting, ws);
        }
        gs.add (go);
    }
    put (o, "groups", gs);
    return o;
}

var Project::toVar (bool editorialOnly, bool withWindows) const
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
        if (i.preamp.lowCutHz != 80) put (o, "lowCutHz", i.preamp.lowCutHz);
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
        put (o, "surround", t.surroundChannels); put (o, "colour", (juce::int64) t.colour); put (o, "armed", t.isArmed()); put (o, "monitor", (int) t.monitor()); if (t.isLtc) put (o, "ltc", true);
        juce::Array<var> ins;
        for (int c = 0; c < t.channelCount(); ++c) ins.add (t.inputOf (c));
        put (o, "inputs", ins);
        tr.add (o);
    }
    put (root, "tracks", tr);

    juce::Array<var> fx;
    for (auto& f : buses) { var o = obj(); put (o, "id", f.id.toString()); put (o, "name", f.name); put (o, "external", f.external); put (o, "colour", (juce::int64) f.colour); fx.add (o); }
    put (root, "buses", fx);
    {
        juce::Array<var> va;
        for (auto& d : vcas) { var o = obj(); put (o, "id", d.id.toString()); put (o, "number", d.number); put (o, "name", d.name); put (o, "colour", (juce::int64) d.colour); va.add (o); }
        put (root, "vcas", va);
    }

    juce::Array<var> outs;
    for (auto& o : outputs) { var oo = obj(); put (oo, "name", o.name); put (oo, "driverName", o.driverName); outs.add (oo); }
    put (root, "outputs", outs);
    {
        juce::Array<juce::var> cr; for (int i : crMicInputs) cr.add (i);
        put (root, "crMicInputs", juce::var (cr));
    }
    {
        juce::Array<juce::var> sg, sp; for (float v : stageGainDb) sg.add ((double) v); for (float v : stagePan) sp.add ((double) v);
        put (root, "stageGainDb", juce::var (sg)); put (root, "stagePan", juce::var (sp));
        put (root, "stagePlaybackDb", (double) stagePlaybackDb); put (root, "stageOutputDb", (double) stageOutputDb);
    }
    put (root, "altMixer", altMixer.toString());
    put (root, "ltcInput", ltcInput); put (root, "ltcOutput", ltcOutput); put (root, "ltcOutOn", ltcOutOn); put (root, "ltcOutFormat", ltcOutFormat);
    put (root, "ltcOutStartMode", ltcOutStartMode); put (root, "ltcOutStartSeconds", ltcOutStartSeconds);
    { juce::Array<var> tb; for (int o : tbOutputs) tb.add (o); put (root, "tbOutputs", juce::var (tb)); }

    juce::Array<var> mx;
    for (auto& m : mixers) mx.add (mixerToVar (*m));
    put (root, "mixers", mx);
    }

    if (withWindows) addWindowsToVar (root);
    put (root, "mastering", mastering.toVar());
    if (roomTone.valid()) put (root, "roomTone", roomToneToVar (roomTone));
    return root;
}

// the room tone: each profile's noise powers as 32-bit floats, base64 (about 16 KB per channel)
static juce::String profileToString (const denoise::Profile& p)
{
    juce::MemoryBlock mb; std::vector<float> v (p.psd.size()); for (size_t i = 0; i < v.size(); ++i) v[i] = (float) p.psd[i];
    mb.append (v.data(), v.size() * sizeof (float));
    return mb.toBase64Encoding();
}
static denoise::Profile profileFromString (const juce::String& s, int fftSize, double sr)
{
    denoise::Profile p; juce::MemoryBlock mb;
    if (! mb.fromBase64Encoding (s) || mb.getSize() < sizeof (float)) return p;
    const auto n = mb.getSize() / sizeof (float);
    if ((int) n != fftSize / 2 + 1) return p;
    p.fftSize = fftSize; p.sampleRate = sr; p.psd.resize (n);
    const float* f = static_cast<const float*> (mb.getData());
    for (size_t i = 0; i < n; ++i) p.psd[i] = juce::jmax (1.0e-20, (double) f[i]);
    return p;
}
var Project::roomToneToVar (const RoomTone& rt)
{
    var o = obj(); put (o, "source", rt.source);
    juce::Array<var> ts;
    for (auto& t : rt.tracks)
    {
        if (! t.mono.valid()) continue;
        var to = obj(); put (to, "track", t.trackId.toString()); put (to, "name", t.trackName); put (to, "fft", t.mono.fftSize); put (to, "rate", t.mono.sampleRate);
        put (to, "mono", profileToString (t.mono));
        juce::Array<var> ch; for (auto& c : t.perChannel) ch.add (profileToString (c)); put (to, "channels", ch);
        ts.add (to);
    }
    put (o, "tracks", ts);
    return o;
}
Project::RoomTone Project::roomToneFromVar (const var& v)
{
    RoomTone rt;
    if (! v.isObject()) return rt;
    rt.source = v["source"].toString();
    if (auto* a = v["tracks"].getArray())
        for (auto& to : *a)
        {
            RoomToneTrack t; t.trackId = juce::Uuid (to["track"].toString()); t.trackName = to["name"].toString();
            const int fft = (int) to["fft"]; const double sr = (double) to["rate"];
            if (fft < 64 || sr <= 0.0) continue;
            t.mono = profileFromString (to["mono"].toString(), fft, sr);
            if (auto* ch = to["channels"].getArray()) for (auto& c : *ch) { auto p = profileFromString (c.toString(), fft, sr); if (p.valid()) t.perChannel.push_back (p); }
            if (t.mono.valid() && ! t.perChannel.empty()) rt.tracks.push_back (std::move (t));
        }
    return rt;
}

void Project::addWindowsToVar (var& root) const
{
    juce::Array<var> tw;
    for (auto& w : takeWindows) tw.add (takeWindowToVar (*w));
    put (root, "takeWindows", tw);
    juce::Array<var> eds;
    for (auto& e : edits) eds.add (editToVar (*e));
    put (root, "edits", eds);
}

bool Project::fromVar (const var& root)
{
    if (! root.isObject() || (root.getProperty ("format", {}).toString() != "Fermata project" && root.getProperty ("format", {}).toString() != "TakeDAW project"))
        return false;

    inputs.clear(); tracks.clear(); buses.clear(); vcas.clear(); outputs.clear(); crMicInputs.clear(); tbOutputs.clear(); stageGainDb.clear(); stagePan.clear(); stagePlaybackDb = 0.0f; stageOutputDb = 0.0f; altMixer = juce::Uuid::null(); ltcInput = -1; ltcOutput = -1; ltcOutOn = false; retireAllMixers(); takeWindows.clear(); edits.clear(); mastering = MasteringDef(); roomTone = RoomTone(); preampDevices.clear();
    filesMoved = false;
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
            i.preamp.lowCutHz = v.hasProperty ("lowCutHz") ? (int) v["lowCutHz"] : 80;
            if (i.name == i.driverName) i.name.clear();          // older projects stored the driver name as 'your name'
            inputs.push_back (i);
        }
    if (auto* a = root["tracks"].getArray())
        for (auto& v : *a)
        {
            TrackDef t; t.id = juce::Uuid (v["id"].toString()); t.name = v["name"].toString();
            t.format = (TrackFormat) (int) v["format"]; t.surroundChannels = (int) v["surround"]; t.colour = (juce::uint32) (juce::int64) v["colour"]; t.isLtc = v.hasProperty ("ltc") && (bool) v["ltc"];
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
    if (auto* a = root["vcas"].getArray())
        for (auto& v : *a)
        {
            VcaDef d; d.id = juce::Uuid (v["id"].toString()); d.number = (int) v["number"]; d.name = v["name"].toString(); d.colour = (juce::uint32) (juce::int64) v["colour"];
            if (d.number >= 1 && d.number <= kMaxVcas) vcas.push_back (d);
        }
    std::sort (vcas.begin(), vcas.end(), [] (const VcaDef& x, const VcaDef& y) { return x.number < y.number; });
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
    stageGainDb.clear(); stagePan.clear();
    if (auto* sg = root["stageGainDb"].getArray()) for (auto& v : *sg) stageGainDb.push_back ((float) (double) v);
    if (auto* sp = root["stagePan"].getArray())    for (auto& v : *sp) stagePan.push_back ((float) (double) v);
    stagePlaybackDb = (float) (double) root.getProperty ("stagePlaybackDb", 0.0); stageOutputDb = (float) (double) root.getProperty ("stageOutputDb", 0.0);
    crMicInputs.erase (std::remove_if (crMicInputs.begin(), crMicInputs.end(), [this] (int i) { return i < 0 || i >= (int) inputs.size(); }), crMicInputs.end());
    tbOutputs.erase (std::remove_if (tbOutputs.begin(), tbOutputs.end(), [this] (int o) { return o < 0 || o >= (int) outputs.size(); }), tbOutputs.end());
    std::sort (tbOutputs.begin(), tbOutputs.end()); tbOutputs.erase (std::unique (tbOutputs.begin(), tbOutputs.end()), tbOutputs.end());
    ltcInput = (int) root.getProperty ("ltcInput", -1); ltcOutput = (int) root.getProperty ("ltcOutput", -1); ltcOutOn = (bool) root.getProperty ("ltcOutOn", false);
    ltcOutFormat = (int) root.getProperty ("ltcOutFormat", 1); ltcOutStartMode = (int) root.getProperty ("ltcOutStartMode", 0); ltcOutStartSeconds = (double) root.getProperty ("ltcOutStartSeconds", 0.0);
    altMixer = root["altMixer"].toString().isEmpty() ? juce::Uuid::null() : juce::Uuid (root["altMixer"].toString());

    if (auto* a = root["mixers"].getArray())
        for (auto& v : *a)
        {
            auto m = std::make_unique<MixerState>();
            m->id = juce::Uuid (v["id"].toString()); m->name = v["name"].toString();
            m->mainBus = juce::Uuid (v["mainBus"].toString());
            if (v["editId"].toString().isNotEmpty()) m->editId = juce::Uuid (v["editId"].toString());
            if (v.hasProperty ("ditherBits")) { const int db = (int) v["ditherBits"]; m->ditherBits.store (db == 16 || db == 0 ? db : 24); }
            if (auto* ss = v["strips"].getArray())
                for (auto& sv : *ss)
                    stripFromVar (*m->stripFor (juce::Uuid (sv["track"].toString())), sv, insertFactory, restoreSampleRate, restoreBlock, true, legacyFx);
            for (const char* key : { "busStates", "fxReturns" })
                if (auto* fs = v[key].getArray())
                    for (auto& fv : *fs)
                        busFromVar (*m->busFor (juce::Uuid (fv.hasProperty ("bus") ? fv["bus"].toString() : fv["fx"].toString())), fv, insertFactory, restoreSampleRate, restoreBlock, true, true);
            vcaStatesFromVar (*m, v["vcaStates"]);
            if (! v.hasProperty ("mainFlags"))                // a project from before 2.65: its one main bus (or the first Ext Bus) becomes the ticked one
            {
                auto mainId = juce::Uuid::null();               // (a default juce::Uuid is a NEW random id, not "none")
                for (auto& b : buses) if (b.external && b.id == m->mainBus) mainId = b.id;
                if (mainId.isNull()) for (auto& b : buses) if (b.external) { mainId = b.id; break; }
                if (! mainId.isNull()) m->busFor (mainId)->isMain.set (true);
            }
            m->followProcessing.set ((bool) v.getProperty ("followProcessing", false) && m->editId.isNull());
            if (auto* hs = v["hiddenStrips"].getArray())
                for (auto& h : *hs) if (h.toString().isNotEmpty()) m->hiddenStrips.push_back (juce::Uuid (h.toString()));
            mixers.push_back (std::move (m));
        }

    // The take windows and the edits are built from their vars one by one (makeTakeWindow / makeEdit read only 'base', nothing of the project).
    const auto base = loadBase();
    auto makeTakeWindow = [&base] (const var& v, bool& moved)
    {
            auto w = std::make_unique<TakeWindowDef>();
            w->id = juce::Uuid (v["id"].toString()); w->name = v["name"].toString();
            w->defaultLabel = v["defaultLabel"].toString(); w->nextNumber = (int) v["nextNumber"];
            if (v.hasProperty ("targetEdit")) w->targetEdit = juce::Uuid (v["targetEdit"].toString());
            if (v.hasProperty ("cursorTake")) { w->cursorTake = juce::Uuid (v["cursorTake"].toString()); w->cursorSeconds = (double) v["cursorSeconds"]; }      // (older projects also had separate Bounce I / O flags here: gone, the 1 / 2 flags do that job)
            if (v.hasProperty ("editTake")) { w->editTake = juce::Uuid (v["editTake"].toString()); w->editIn = (double) v["editIn"]; w->editOut = (double) v["editOut"]; }
            if (auto* et = v["editTracks"].getArray()) for (auto& t : *et) w->editTracks.push_back (juce::Uuid (t.toString()));
            w->overdub = (bool) v["overdub"];
            if (v.hasProperty ("playhead")) w->playheadSeconds = juce::jmax (0.0, (double) v["playhead"]);
            w->showLtc = v.hasProperty ("showLtc") && (bool) v["showLtc"];
            markersFromVar (v["markers"], w->markers); w->showMarkers = v["showMarkers"].isVoid() ? true : (bool) v["showMarkers"];
            if (auto* hid = v["hiddenTracks"].getArray()) for (auto& h : *hid) w->hiddenTracks.push_back (juce::Uuid (h.toString()));
            if (auto* gs = v["groups"].getArray())
                for (auto& gv : *gs)
                {
                    TakeGroup g;
                    g.id = juce::Uuid (gv[key::k_id].toString()); g.number = (int) gv[key::k_number]; g.label = gv[key::k_label].toString();
                    g.startSeconds = (double) gv[key::k_start]; g.lengthSamples = (juce::int64) gv[key::k_length];
                    g.sampleRate = (double) gv[key::k_rate]; g.recordedAt = juce::Time ((juce::int64) gv[key::k_time]);
                    if (gv.hasProperty (key::k_fadeIn))  g.fadeInSeconds  = juce::jmax (0.0, (double) gv[key::k_fadeIn]);
                    if (g.fadeInSeconds <= 0.0011 && g.fadeInSeconds > 0.0) g.fadeInSeconds = kDefaultEdgeFade;       // the old 1 ms default becomes the new one
                    if (gv.hasProperty (key::k_fadeOut)) g.fadeOutSeconds = juce::jmax (0.0, (double) gv[key::k_fadeOut]);
                    if (g.fadeOutSeconds <= 0.0011 && g.fadeOutSeconds > 0.0) g.fadeOutSeconds = kDefaultEdgeFade;
                    g.dud = (bool) gv[key::k_dud]; g.verified = (bool) gv[key::k_verified]; g.barIn = juce::jmax (0, (int) gv[key::k_barIn]); g.barOut = juce::jmax (0, (int) gv[key::k_barOut]);
                    g.roomTone = (bool) gv[key::k_roomTone]; g.roomToneIn = gv.hasProperty (key::k_roomToneIn) ? (double) gv[key::k_roomToneIn] : -1.0; g.roomToneOut = gv.hasProperty (key::k_roomToneOut) ? (double) gv[key::k_roomToneOut] : -1.0;
                    g.overruns = juce::jmax (0, (int) gv[key::k_overruns]); g.lostSamples = juce::jmax<juce::int64> (0, (juce::int64) gv[key::k_lost]); g.writeFailed = (bool) gv[key::k_writeFailed];
                    if (auto* fs = gv[key::k_files].getArray())
                        for (auto& fv : *fs)
                        {
                            TakeFile f; f.trackId = juce::Uuid (fv[key::k_track].toString()); f.trackName = fv[key::k_trackName].toString();
                            f.file = getFilePath (fv, base, &moved); f.numChannels = (int) fv[key::k_channels]; f.sourceName = fv[key::k_orig].toString(); f.sourceName2 = fv[key::k_orig2].toString();
                            g.files.push_back (f);
                        }
                    if (auto* ws = gv[key::k_waiting].getArray()) for (auto& wv : *ws) g.waiting.push_back (waitingFromVar (wv, base));
                    w->groups.push_back (std::move (g));
                }
            return w;
    };
    if (auto* a = root["takeWindows"].getArray())
    {
        std::vector<std::unique_ptr<TakeWindowDef>> made ((size_t) a->size()); std::vector<char> moved ((size_t) a->size(), 0);
        parallelFor (a->size(), [&] (int i) { bool m = false; made[(size_t) i] = makeTakeWindow (a->getReference (i), m); moved[(size_t) i] = m ? 1 : 0; });
        for (size_t i = 0; i < made.size(); ++i) { takeWindows.push_back (std::move (made[i])); if (moved[i]) filesMoved = true; }
    }

    auto makeEdit = [&base] (const var& v)
    {
            auto e = std::make_unique<EditDef>();
            e->id = juce::Uuid (v["id"].toString()); e->name = v["name"].toString(); e->windowId = juce::Uuid (v["window"].toString());
            e->sampleRate = (double) v["rate"]; e->insertIndex = (int) v["insertIndex"];
            if (v.hasProperty ("fixIn")) { e->fixIn = (double) v["fixIn"]; e->fixOut = (double) v["fixOut"]; }
            if (auto* ft = v["fixTracks"].getArray()) for (auto& t : *ft) e->fixTracks.push_back (juce::Uuid (t.toString()));
            auto regionFromVar = [&base] (const var& rv)
            {
                EditRegion r;
                r.id = juce::Uuid (rv[key::k_id].toString()); r.windowId = juce::Uuid (rv[key::k_window].toString()); r.takeId = juce::Uuid (rv[key::k_take].toString());
                r.takeName = rv[key::k_takeName].toString(); r.srcIn = (juce::int64) rv[key::k_in]; r.srcOut = (juce::int64) rv[key::k_out];
                r.startSample = (juce::int64) rv[key::k_start]; r.sourceLength = (juce::int64) rv[key::k_sourceLength];
                r.barIn = juce::jmax (0, (int) rv[key::k_barIn]); r.barOut = juce::jmax (0, (int) rv[key::k_barOut]);
                r.sampleRate = (double) rv[key::k_rate]; r.curve = (FadeCurve) juce::jlimit (0, (int) FadeCurve::Count - 1, (int) rv[key::k_curve]);
                r.fixIn = (bool) rv[key::k_fixIn]; r.fixOut = (bool) rv[key::k_fixOut]; r.normaliseIn = (bool) rv[key::k_normaliseIn];
                if (rv.hasProperty (key::k_outCurve)) r.setOutCurve ((FadeCurve) juce::jlimit (0, (int) FadeCurve::Count - 1, (int) rv[key::k_outCurve]));
                r.inStart = (double) rv[key::k_inStart]; r.inEnd = (double) rv[key::k_inEnd]; r.outStart = (double) rv[key::k_outStart]; r.outEnd = (double) rv[key::k_outEnd];
                if (auto* fs = rv[key::k_files].getArray())
                    for (auto& fv : *fs)
                    {
                        RegionFile f; f.trackId = juce::Uuid (fv[key::k_track].toString()); f.trackName = fv[key::k_trackName].toString();
                        f.file = getFilePath (fv, base); f.numChannels = (int) fv[key::k_channels];
                        f.fileStart = (juce::int64) fv[key::k_fileStart];
                        r.files.push_back (f);
                    }
                if (rv.hasProperty (key::k_waiting)) r.waiting = waitingFromVar (rv[key::k_waiting], base);
                if (auto* gs = rv[key::k_gains].getArray())
                    for (auto& gv : *gs)
                    {
                        GainChange g; g.at = (juce::int64) gv[key::k_at]; g.ramp = (double) gv[key::k_ramp]; g.autoNorm = (bool) gv[key::k_autoNorm];
                        if (auto* dbs = gv[key::k_db].getArray()) for (auto& d : *dbs) g.db.push_back ((float) (double) d);
                        g.db.resize (r.files.size(), 0.0f);
                        r.gains.push_back (g);
                    }
                return r;
            };
            if (auto* rs = v["regions"].getArray())  for (auto& rv : *rs) e->regions.push_back (regionFromVar (rv));
            if (auto* os = v["overdubs"].getArray()) for (auto& rv : *os) e->overdubs.push_back (regionFromVar (rv));
            if (auto* ta = v["trackIds"].getArray()) for (auto& tv : *ta) { juce::Uuid u (tv.toString()); if (! u.isNull()) e->trackIds.push_back (u); }
            if (v.hasProperty ("playhead")) e->playheadSeconds = juce::jmax (0.0, (double) v["playhead"]);
            e->automationOn = (bool) v["automationOn"]; e->showLtc = v.hasProperty ("showLtc") && (bool) v["showLtc"]; markersFromVar (v["markers"], e->markers); feedbackFromVar (v["feedback"], e->feedback); e->showFeedback = v["showFeedback"].isVoid() ? true : (bool) v["showFeedback"]; e->showMarkers = v["showMarkers"].isVoid() ? true : (bool) v["showMarkers"];
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
            e->commitAllJoins();                                // older projects: the edit points that were moved in the trim window get their pieces' edges moved with them (the sound does not change)
            return e;
    };
    if (auto* a = root["edits"].getArray())
    {
        std::vector<std::unique_ptr<EditDef>> made ((size_t) a->size());
        parallelFor (a->size(), [&] (int i) { made[(size_t) i] = makeEdit (a->getReference (i)); });
        for (auto& e : made) edits.push_back (std::move (e));
    }

    mastering.fromVar (root["mastering"]);
    roomTone = roomToneFromVar (root["roomTone"]);
    mastering.sync (edits);
    std::stable_partition (mixers.begin(), mixers.end(), [] (const std::unique_ptr<MixerState>& m) { return m->editId.isNull(); });     // the Edit mixers come last
    syncMixers();
    syncEditMixers();                                    // projects from before Edit mixers: every Edit gets one (a copy of the processing mixer)
    structureChanged();
    dirty = false;
    return true;
}

// ------------------------------------------------------------------------------------------------ undo
namespace
{
/** A 64-bit FNV-1a style mix over the fields the undo history looks at. Strings and paths go in by their own 64-bit hash. */
struct UndoHasher
{
    juce::uint64 h = 1469598103934665603ull;
    void mix (juce::uint64 x) noexcept { h ^= x; h *= 1099511628211ull; }
    void add (bool b) noexcept { mix (b ? 0x9e3779b97f4a7c15ull : 0x2545f4914f6cdd1dull); }
    void add (int i) noexcept { mix ((juce::uint64) (juce::int64) i ^ 0xa0761d6478bd642full); }
    void add (juce::int64 i) noexcept { mix ((juce::uint64) i ^ 0xe7037ed1a0b428dbull); }
    void add (double d) noexcept { juce::uint64 b; std::memcpy (&b, &d, sizeof b); mix (b ^ 0x8ebc6af09c88c6e3ull); }
    void add (float f) noexcept { add ((double) f); }
    void add (const juce::String& s) noexcept { mix ((juce::uint64) s.hashCode64()); mix ((juce::uint64) s.length() + 0x589965cc75374cc3ull); }
    void add (const juce::Uuid& u) noexcept { juce::uint64 a, b; std::memcpy (&a, u.getRawData(), 8); std::memcpy (&b, u.getRawData() + 8, 8); mix (a); mix (b ^ 0x1d8e4e27c47d124full); }
    void add (const juce::File& f) noexcept { add (f.getFullPathName()); }
    void add (const juce::Time& t) noexcept { add (t.toMilliseconds()); }
    void count (size_t n) noexcept { mix ((juce::uint64) n ^ 0xc6a4a7935bd1e995ull); }
    void add (const WaitingPiece& w) noexcept
    {
        add (w.name); add (w.from); add (w.to);
        count (w.files.size()); for (auto& f : w.files) add (f);
        count (w.tracks.size()); for (auto& t : w.tracks) add (t);
    }
};
}

juce::uint64 Project::undoHash (const TakeWindowDef& w)
{
    UndoHasher h;
    h.add (w.id); h.add (w.name); h.add (w.defaultLabel); h.add (w.nextNumber); h.add (w.targetEdit); h.add (w.showLtc);
    h.count (w.groups.size());
    for (auto& g : w.groups)
    {
        h.add (g.id); h.add (g.number); h.add (g.label); h.add (g.startSeconds); h.add (g.lengthSamples); h.add (g.sampleRate); h.add (g.recordedAt);
        h.add (g.fadeInSeconds); h.add (g.fadeOutSeconds); h.add (g.dud); h.add (g.barIn); h.add (g.barOut);
        h.add (g.overruns); h.add (g.lostSamples); h.add (g.writeFailed); h.add (g.verified);
        h.count (g.files.size());
        for (auto& f : g.files) { h.add (f.trackId); h.add (f.trackName); h.add (f.file); h.add (f.numChannels); h.add (f.sourceName); h.add (f.sourceName2); }
        h.count (g.waiting.size());
        for (auto& wp : g.waiting) h.add (wp);
    }
    return h.h;
}

juce::uint64 Project::undoHash (const EditDef& e)
{
    UndoHasher h;
    h.add (e.id); h.add (e.name); h.add (e.windowId); h.add (e.sampleRate); h.add (e.automationOn); h.add (e.showLtc);
    h.count (e.trackIds.size()); for (auto& t : e.trackIds) h.add (t);
    auto region = [&h] (const EditRegion& r)
    {
        h.add (r.id); h.add (r.windowId); h.add (r.takeId); h.add (r.takeName); h.add (r.srcIn); h.add (r.srcOut); h.add (r.startSample); h.add (r.sourceLength);
        h.add (r.barIn); h.add (r.barOut); h.add (r.sampleRate); h.add ((int) r.curve); h.add (r.outCurveOwn); if (r.outCurveOwn) h.add ((int) r.outCurve);
        h.add (r.fixIn); h.add (r.fixOut); h.add (r.normaliseIn); h.add (r.inStart); h.add (r.inEnd); h.add (r.outStart); h.add (r.outEnd);
        h.count (r.files.size());
        for (auto& f : r.files) { h.add (f.trackId); h.add (f.trackName); h.add (f.file); h.add (f.numChannels); h.add (f.fileStart); }
        h.add (r.waiting.active()); if (r.waiting.active()) h.add (r.waiting);
        h.count (r.gains.size());
        for (auto& g : r.gains) { h.add (g.at); h.add (g.ramp); h.add (g.autoNorm); h.count (g.db.size()); for (float d : g.db) h.add (d); }
    };
    h.count (e.regions.size());  for (auto& r : e.regions)  region (r);
    h.count (e.overdubs.size()); for (auto& r : e.overdubs) region (r);
    h.count (e.lanes.size());
    for (auto& l : e.lanes)
    {
        h.add (l.id); h.add (l.trackId); h.add (l.param); h.add (l.mixerId); h.add (l.locked);
        h.count (l.pts.size());
        for (auto& p : l.pts) { h.add (p.id); h.add (p.region); h.add (p.take); h.add (p.srcPos); h.add (p.time); h.add (p.value); h.add (p.floating); }
    }
    return h.h;
}

juce::uint64 Project::undoHash (const MasteringDef& m)
{
    var v = m.toVar();                                     // (small: a list of pieces and the disc's details; the text is the cheapest exact hash)
    if (auto* o = v.getDynamicObject()) o->removeProperty ("view");
    return (juce::uint64) juce::JSON::toString (v, true).hashCode64();
}

juce::uint64 Project::saveHash (const TakeWindowDef& w)
{
    UndoHasher h; h.mix (undoHash (w));                    // everything the history looks at, plus the view state that is saved with the window
    h.add (w.editTake); h.add (w.editIn); h.add (w.editOut); h.count (w.editTracks.size()); for (auto& t : w.editTracks) h.add (t);
    h.add (w.cursorTake); h.add (w.cursorSeconds); h.add (w.playheadSeconds); h.add (w.overdub); h.add (w.showMarkers);
    h.count (w.hiddenTracks.size()); for (auto& t : w.hiddenTracks) h.add (t);
    h.count (w.markers.size()); for (auto& m : w.markers) { h.add (m.id); h.add (m.name); h.add (m.seconds); }
    return h.h;
}

juce::uint64 Project::saveHash (const EditDef& e)
{
    UndoHasher h; h.mix (undoHash (e));
    h.add (e.fixIn); h.add (e.fixOut); h.count (e.fixTracks.size()); for (auto& t : e.fixTracks) h.add (t);
    h.add (e.playheadSeconds); h.add (e.insertIndex); h.add (e.showFeedback); h.add (e.showMarkers);
    h.count (e.markers.size()); for (auto& m : e.markers) { h.add (m.id); h.add (m.name); h.add (m.seconds); }
    h.count (e.feedback.size());
    for (auto& n : e.feedback) { h.add (n.key); h.add (n.who); h.add (n.bar); h.add (n.text); h.add (n.colour); h.add (n.source); h.add (n.seconds); h.add (n.done); }
    return h.h;
}

namespace
{
void strip (const var& o, std::initializer_list<const char*> keys)
{
    if (auto* d = o.getDynamicObject()) for (auto* k : keys) d->removeProperty (juce::Identifier (k));
}
const std::initializer_list<const char*> kTakeViewKeys = { "editTake", "editIn", "editOut", "editTracks", "cursorTake", "cursorSeconds", "playhead", "hiddenTracks", "overdub", "markers", "showMarkers" };
const std::initializer_list<const char*> kEditViewKeys = { "markIn", "markOut", "fixIn", "fixOut", "fixTracks", "playhead", "insertIndex", "markers", "feedback", "showFeedback", "showMarkers" };
}

juce::String Project::undoText (const TakeWindowDef& w) const
{
    var v = takeWindowToVar (w);                           // (one window's var, by the same code that saves it)
    strip (v, kTakeViewKeys);
    return juce::JSON::toString (v, true);
}

juce::String Project::undoText (const EditDef& e) const
{
    var v = editToVar (e);
    strip (v, kEditViewKeys);
    return juce::JSON::toString (v, true);
}

juce::String Project::undoText (const MasteringDef& m) const
{
    var v = m.toVar();
    strip (v, { "view" });
    return juce::JSON::toString (v, true);
}

juce::String Project::undoTextOf (UndoSnap& snap) const
{
    if (snap.text.isEmpty())
    {
        if (snap.takeCopy != nullptr) snap.text = undoText (*snap.takeCopy);
        else if (snap.editCopy != nullptr) snap.text = undoText (*snap.editCopy);
        snap.takeCopy.reset(); snap.editCopy.reset();
    }
    return snap.text;
}

void Project::undoReset()
{
    undoStack.clear(); redoStack.clear(); undoPending = false;
    undoTakes.clear(); undoEdits.clear();
    // every window is copied, not serialised: a copy costs about a millisecond, the text of a big window several, and most are never needed
    for (auto& w : takeWindows) { UndoSnap s; s.hash = undoHash (*w); s.takeCopy = std::make_shared<TakeWindowDef> (*w); undoTakes[w->id] = std::move (s); }
    for (auto& e : edits)       { UndoSnap s; s.hash = undoHash (*e); s.editCopy = std::make_shared<EditDef> (*e); undoEdits[e->id] = std::move (s); }
    undoMastering = { undoHash (mastering), undoText (mastering) };
}

void Project::undoTick (bool recording, bool force)
{
    if (! undoPending) return;
    if (! force && juce::Time::getMillisecondCounter() - undoDirtyAt < 500) return;
    undoPending = false;
    if (recording) { undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); return; }     // the take growing is not an undo step: look again after it stops
    UndoStep step;
    // the take windows: a changed hash means the window is serialised again and its previous text becomes part of the step
    std::set<juce::Uuid> seen;
    for (auto& w : takeWindows)
    {
        seen.insert (w->id);
        const auto hash = undoHash (*w);
        auto it = undoTakes.find (w->id);
        if (it == undoTakes.end()) { undoTakes[w->id] = { hash, undoText (*w) }; continue; }         // a new window: nothing to go back to
        if (it->second.hash == hash) continue;
        step.pieces.push_back ({ UndoKind::takeWindow, w->id, undoTextOf (it->second) });
        it->second = { hash, undoText (*w) };
    }
    for (auto it = undoTakes.begin(); it != undoTakes.end();) it = seen.count (it->first) ? std::next (it) : undoTakes.erase (it);     // (removed windows are not undone: their files went with them)
    seen.clear();
    for (auto& e : edits)
    {
        seen.insert (e->id);
        const auto hash = undoHash (*e);
        auto it = undoEdits.find (e->id);
        if (it == undoEdits.end()) { undoEdits[e->id] = { hash, undoText (*e) }; continue; }
        if (it->second.hash == hash) continue;
        step.pieces.push_back ({ UndoKind::edit, e->id, undoTextOf (it->second) });
        it->second = { hash, undoText (*e) };
    }
    for (auto it = undoEdits.begin(); it != undoEdits.end();) it = seen.count (it->first) ? std::next (it) : undoEdits.erase (it);
    {
        const auto hash = undoHash (mastering);
        if (hash != undoMastering.hash)
        {
            step.pieces.push_back ({ UndoKind::mastering, juce::Uuid::null(), undoMastering.text });
            undoMastering = { hash, undoText (mastering) };
        }
    }
    if (step.pieces.empty()) return;
    undoStack.push_back (std::move (step));
    if ((int) undoStack.size() > kUndoLevels) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

namespace
{
var rootWith (const char* key, const var& v, bool asArray)
{
    var root = obj(); put (root, "format", "Fermata project");
    if (asArray) { juce::Array<var> a; a.add (v); put (root, key, a); } else put (root, key, v);
    return root;
}

/** Puts one window back as its snapshot says, keeping what the snapshot does not cover (view state, the take being recorded).
    True when the project's structure changed (an edit came back, which needs a mixer): the caller then raises structureChanged rather than changed. */
bool restorePiece (Project& p, const Project::UndoPiece& piece, const juce::Uuid& liveTake)
{
    const var snapshot = juce::JSON::parse (piece.text);
    if (! snapshot.isObject()) return false;
    Project tmp;
    tmp.restoreSampleRate = p.restoreSampleRate; tmp.restoreBlock = p.restoreBlock; tmp.projectFile = p.projectFile;
    if (piece.kind == Project::UndoKind::mastering)
    {
        const int view = p.mastering.view; const auto selI = p.mastering.selectedItem, selC = p.mastering.selectedClip;
        p.mastering.fromVar (snapshot);
        p.mastering.view = view; p.mastering.selectedItem = selI; p.mastering.selectedClip = selC;
        p.mastering.sync (p.edits);
        return false;
    }
    if (piece.kind == Project::UndoKind::edit)
    {
        if (! tmp.fromVar (rootWith ("edits", snapshot, true)) || tmp.edits.empty()) return false;
        auto& te = tmp.edits.front();
        bool structural = false;
        if (auto* live = p.findEdit (te->id))
        {
            te->fixIn = live->fixIn; te->fixOut = live->fixOut; te->fixTracks = live->fixTracks;
            te->playheadSeconds = live->playheadSeconds; te->insertIndex = live->insertIndex; te->markers = live->markers; te->feedback = live->feedback; te->showFeedback = live->showFeedback; te->showMarkers = live->showMarkers;
            *live = std::move (*te);
        }
        else { p.edits.push_back (std::move (te)); p.syncEditMixers(); structural = true; }
        p.mastering.sync (p.edits);
        return structural;
    }
    if (! tmp.fromVar (rootWith ("takeWindows", snapshot, true)) || tmp.takeWindows.empty()) return false;
    auto& tw = tmp.takeWindows.front();
    auto* live = p.findTakeWindow (tw->id);
    if (live == nullptr) return false;
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
    live->name = tw->name; live->defaultLabel = tw->defaultLabel; live->nextNumber = tw->nextNumber;
    return false;
}
}

bool Project::undo (const juce::Uuid& liveTake)
{
    undoTick (false, true);
    if (undoStack.empty()) return false;
    const auto step = std::move (undoStack.back()); undoStack.pop_back();
    UndoStep back; bool structural = false;                // what redo will put back: the windows as they are now
    for (auto& piece : step.pieces)
    {
        if (piece.kind == UndoKind::takeWindow) { auto it = undoTakes.find (piece.id); if (it != undoTakes.end()) back.pieces.push_back ({ piece.kind, piece.id, undoTextOf (it->second) }); }
        else if (piece.kind == UndoKind::edit) { auto it = undoEdits.find (piece.id); if (it != undoEdits.end()) back.pieces.push_back ({ piece.kind, piece.id, undoTextOf (it->second) }); }
        else back.pieces.push_back ({ piece.kind, piece.id, undoMastering.text });
        structural = restorePiece (*this, piece, liveTake) || structural;
    }
    redoStack.push_back (std::move (back));
    if (structural) structureChanged(); else changed();           // (a structure change rebuilds the main window's strips, the mixer windows and the engine's plan: only when an edit came back)
    undoPending = false;
    for (auto& piece : step.pieces)                        // the snapshots follow (what was just put back is the state as of now)
    {
        if (piece.kind == UndoKind::takeWindow) { if (auto* w = findTakeWindow (piece.id)) undoTakes[piece.id] = { undoHash (*w), undoText (*w) }; }
        else if (piece.kind == UndoKind::edit) { if (auto* e = findEdit (piece.id)) undoEdits[piece.id] = { undoHash (*e), undoText (*e) }; }
        else undoMastering = { undoHash (mastering), undoText (mastering) };
    }
    return true;
}

bool Project::redo (const juce::Uuid& liveTake)
{
    undoTick (false, true);
    if (redoStack.empty()) return false;
    const auto step = std::move (redoStack.back()); redoStack.pop_back();
    UndoStep back; bool structural = false;
    for (auto& piece : step.pieces)
    {
        if (piece.kind == UndoKind::takeWindow) { auto it = undoTakes.find (piece.id); if (it != undoTakes.end()) back.pieces.push_back ({ piece.kind, piece.id, undoTextOf (it->second) }); }
        else if (piece.kind == UndoKind::edit) { auto it = undoEdits.find (piece.id); if (it != undoEdits.end()) back.pieces.push_back ({ piece.kind, piece.id, undoTextOf (it->second) }); }
        else back.pieces.push_back ({ piece.kind, piece.id, undoMastering.text });
        structural = restorePiece (*this, piece, liveTake) || structural;
    }
    undoStack.push_back (std::move (back));
    if (structural) structureChanged(); else changed();           // (a structure change rebuilds the main window's strips, the mixer windows and the engine's plan: only when an edit came back)
    undoPending = false;
    for (auto& piece : step.pieces)
    {
        if (piece.kind == UndoKind::takeWindow) { if (auto* w = findTakeWindow (piece.id)) undoTakes[piece.id] = { undoHash (*w), undoText (*w) }; }
        else if (piece.kind == UndoKind::edit) { if (auto* e = findEdit (piece.id)) undoEdits[piece.id] = { undoHash (*e), undoText (*e) }; }
        else undoMastering = { undoHash (mastering), undoText (mastering) };
    }
    return true;
}

bool Project::saveAs (const juce::File& f, juce::String& error)
{
    projectFile = f;
    return save (error);
}

bool Project::save (juce::String& error, bool quick)
{
    if (projectFile == juce::File()) { error = "No project file chosen"; return false; }
    projectFile.getParentDirectory().createDirectory();
    dirty = false;                                        // (cleared before the snapshot: a change made while the text is being made is kept for the next save)
    quickPluginState = quick;
    var root = toVar (false, false);                      // (the windows go to their own files: splitWindowFiles adds them to root only if that fails)
    quickPluginState = false;
    // The order matters for a save that is interrupted (the program or the computer stops half way): first the window files, then the project file,
    // and only then are the old window files tidied away and the list of files updated. Stopped after any step, the folder still opens: a window
    // file is always complete (replaceTextSafely), the project file either the old one, its .bak, or the new one, and no file it lists is gone.
    std::map<String, WindowFile> next; std::vector<OldWindowFile> old;
    if (splitWindowFiles (root, next, old))               // the edits and take windows go to their own files (if that fails the project is written whole, as before)
    {
        windowFiles = next;                                // (the files are written: a later save must know they are this project's own)
        oldWindowFiles.insert (oldWindowFiles.end(), old.begin(), old.end());
    }
    if (! filesync::replaceTextSafely (projectFile, juce::JSON::toString (root, false), true, &error))
    {
        dirty = true;
        return false;
    }
    const auto folder = projectFolder();
    for (auto& o : oldWindowFiles)                         // (also what an earlier save that failed left behind)
    {
        filesync::backupOf (o.file).deleteFile();
        if (! o.file.existsAsFile()) continue;
        if (o.renamed) { o.file.deleteFile(); continue; }
        auto dir = folder.getChildFile ("_removed"); dir.createDirectory();
        o.file.moveFileTo (dir.getNonexistentChildFile (o.file.getFileNameWithoutExtension(), o.file.getFileExtension()));
    }
    oldWindowFiles.clear();
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
    size_t total = 0, done = 0;
    for (auto& w : takeWindows) total += w->groups.size();
    const auto inside = projectFolder();
    for (auto& w : takeWindows)
        for (auto& g : w->groups)
        {
            if ((done++ % 32) == 0) reportLoad ("Checking the recordings (" + String ((int) done) + " of " + String ((int) total) + " takes)", total > 0 ? (double) done / (double) total : 1.0);
            // A take that was still recording when the program stopped was saved with length 0. A finished take only needs a look if one of its files is
            // shorter than its length says (the computer lost power before everything reached the disk). Asking for a file's size is quick; opening
            // thousands of files one by one is what made big projects slow to open. A take found whole once is marked verified and saved so: the next
            // open does not ask the disk about it at all (3000 takes of 20 files were 60,000 questions, half a second even on a quick drive) - unless the
            // project has moved or been copied since (filesMoved), when every take is looked at again.
            bool look = g.lengthSamples <= 0;
            if (! look && g.verified && ! filesMoved) continue;
            bool allThere = true;
            for (size_t i = 0; ! look && i < g.files.size(); ++i)
            {
                const auto& f = g.files[i];
                if (! f.file.hasFileExtension ("wav;wave;bwf")) continue;                     // (only WAV files can be mended)
                const auto size = f.file.getSize();
                if (size <= 0) { allThere = false; continue; }                                 // missing: nothing to mend (and not verified: it may come back)
                const int bytes = (inside != juce::File() && f.file.isAChildOf (inside)) ? 3 : 2;  // Fermata writes 24-bit; a linked original may be 16-bit
                if (size < 44 + g.lengthSamples * (juce::int64) juce::jmax (1, f.numChannels) * bytes) look = true;
            }
            if (! look) { if (allThere && ! g.files.empty() && ! g.verified) { g.verified = true; markDirty(); } continue; }
            bool mended = false; juce::int64 longest = 0;
            const bool wasRecording = g.lengthSamples <= 0;                                // (saved so when Record was pressed; Stop writes the length)
            for (auto& f : g.files)
            {
                if (! f.file.existsAsFile()) continue;
                const bool ours = inside != juce::File() && f.file.isAChildOf (inside);
                const auto r = wavrepair::inspect (f.file, true, wasRecording && ours);
                if (! r.ok) continue;
                if (r.wasBroken) mended = true;
                longest = juce::jmax (longest, r.frames);
            }
            // a take that was recorded when the program stopped was saved with length 0; a mended one is given the length of what was kept
            if (longest > 0 && (mended || g.lengthSamples == 0) && g.lengthSamples != longest)
            {
                g.lengthSamples = longest; mended = true;
            }
            if (longest > 0 && g.verified == mended) { g.verified = ! mended; markDirty(); }          // looked at and whole: verified; mended: looked at again next time
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
    reportLoad ("Reading the project file", 0.02);
    windowFiles.clear(); missingWindowRefs.clear(); loadNotes.clear();
    const auto t0 = juce::Time::getMillisecondCounterHiRes(); double tMain = 0.0, tWindows = 0.0, tSetup = 0.0, tHashes = 0.0;
    // A damaged (or missing) project file: the save before it is in <name>.fermata.bak and is used instead
    const int read = filesync::readJsonWithBackup (f, v, [] (const juce::var& x) { return x.isObject(); });
    tMain = juce::Time::getMillisecondCounterHiRes() - t0;
    readingFrom = f.getParentDirectory();
    bool ok = read > 0;
    if (read == 2)
        loadNotes.add ("The project file " + f.getFileName() + " could not be read, so the save before it (" + filesync::backupOf (f).getFileName() + ") was used. "
                       "Anything changed after that save is not in the project. The unreadable file was kept as " + filesync::damagedCopyOf (f).getFileName() + ".");
    if (ok)
    {
        const bool oldStyle = ! v.hasProperty ("takeWindowFiles") && ! v.hasProperty ("editFiles")
                              && ((v["takeWindows"].isArray() && v["takeWindows"].size() > 0) || (v["edits"].isArray() && v["edits"].size() > 0));
        if (oldStyle)                                      // a project from before window files: keep the old file as it was, once
        {
            auto bak = f.getSiblingFile (f.getFileName() + ".before-window-files");
            if (! bak.exists()) f.copyFileTo (bak);
        }
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        mergeWindowFiles (v);
        tWindows = juce::Time::getMillisecondCounterHiRes() - t1;
        reportLoad ("Setting up the tracks, mixers and plug-ins", -1.0);
        const auto t2 = juce::Time::getMillisecondCounterHiRes();
        ok = fromVar (v);
        tSetup = juce::Time::getMillisecondCounterHiRes() - t2;
        if (ok && cueEnd() == 0)                           // the processing mixer's file is missing: start a fresh one (the missing file stays listed only if it is not replaced)
        {
            auto m = std::make_unique<MixerState>(); m->name = "Processing mixer";
            mixers.insert (mixers.begin(), std::move (m));
            syncMixers(); syncEditMixers();
            missingWindowRefs.erase (std::remove_if (missingWindowRefs.begin(), missingWindowRefs.end(), [] (const var& r) { return r["kind"].toString() == "mixer"; }), missingWindowRefs.end());
            loadNotes.add ("A new, empty processing mixer was made. Put the missing mixer file back and open the project again to get the old one.");
        }
    }
    readingFrom = juce::File();
    if (! ok) { error = "Not a valid Fermata project file"; return false; }
    projectFile = f;
    restoreSavedComments();
    const auto t3 = juce::Time::getMillisecondCounterHiRes();
    quickPluginState = true; refreshWindowFileHashes(); quickPluginState = false;      // (the plug-ins' states as just loaded, not asked for again)
    tHashes = juce::Time::getMillisecondCounterHiRes() - t3;
    juce::Logger::writeToLog ("Project read in " + String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms: project file " + String (juce::roundToInt (tMain))
                              + " ms, " + String ((int) windowFiles.size()) + " window files " + String (juce::roundToInt (tWindows)) + " ms, set up " + String (juce::roundToInt (tSetup))
                              + " ms, file hashes " + String (juce::roundToInt (tHashes)) + " ms");
    return true;
}

// ------------------------------------------------------------------------------------------------ window files
namespace
{
const char* const kEditFormat = "Fermata edit";
const char* const kTakeFormat = "Fermata take window";
const char* const kMixerFormat = "Fermata mixer";

/** Calls f for every audio file the takes and edits use. */
template <typename F> void forEachAudioFile (std::vector<std::unique_ptr<TakeWindowDef>>& tws, std::vector<std::unique_ptr<EditDef>>& eds, F&& f)
{
    for (auto& w : tws)
        for (auto& g : w->groups)
        {
            for (auto& tf : g.files) f (tf.file);
            for (auto& wp : g.waiting) for (auto& wf : wp.files) f (wf);
        }
    for (auto& e : eds)
    {
        auto doRegion = [&] (EditRegion& r)
        {
            for (auto& rf : r.files) f (rf.file);
            for (auto& wf : r.waiting.files) f (wf);
        };
        for (auto& r : e->regions) doRegion (r);
        for (auto& r : e->overdubs) doRegion (r);
    }
}

/** A fingerprint of what a window file would hold, worked out from the data itself (quicker than writing all of it out as text first). Only used to
    tell whether a window changed since it was last written, so an unchanged take window with thousands of takes costs almost nothing to save. */
void fingerprint (const var& v, juce::uint64& h)
{
    auto mix = [&h] (juce::uint64 x) { h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    if (v.isVoid() || v.isUndefined()) { mix (1); return; }
    if (v.isBool())   { mix ((bool) v ? 3 : 2); return; }
    if (v.isInt() || v.isInt64()) { mix (4); mix ((juce::uint64) (juce::int64) v); return; }
    if (v.isDouble()) { const double d = (double) v; juce::uint64 b = 0; std::memcpy (&b, &d, sizeof (b)); mix (5); mix (b); return; }
    if (v.isString()) { mix (6); mix ((juce::uint64) v.toString().hashCode64()); return; }
    if (auto* a = v.getArray())
    {
        mix (7); mix ((juce::uint64) a->size());
        for (auto& x : *a) fingerprint (x, h);
        return;
    }
    if (auto* o = v.getDynamicObject())
    {
        mix (8); mix ((juce::uint64) o->getProperties().size());
        for (auto& p : o->getProperties()) { mix ((juce::uint64) p.name.toString().hashCode64()); fingerprint (p.value, h); }
        return;
    }
    mix (9); mix ((juce::uint64) v.toString().hashCode64());
}
String fingerprintOf (const var& v) { juce::uint64 h = 1469598103934665603ull; fingerprint (v, h); return String::toHexString ((juce::int64) h); }

/** Which of the 'wanted' ids (track ids) are used anywhere in v: as a value, as a name, or after a "kind:" prefix. */
void findIds (const var& v, const std::set<String>& wanted, std::set<String>& found)
{
    if (v.isString())
    {
        const auto t = v.toString();
        if (wanted.count (t) > 0) found.insert (t);
        else if (t.containsChar (':')) { const auto tail = t.fromLastOccurrenceOf (":", false, false); if (wanted.count (tail) > 0) found.insert (tail); }
        return;
    }
    if (auto* a = v.getArray()) { for (auto& x : *a) findIds (x, wanted, found); return; }
    if (auto* o = v.getDynamicObject())
        for (auto& p : o->getProperties())
        {
            const auto nm = p.name.toString();
            if (wanted.count (nm) > 0) found.insert (nm);
            findIds (p.value, wanted, found);
        }
}

void collectIds (const var& o, std::vector<String>& out, const char* arrayKey)
{
    if (auto* a = o[arrayKey].getArray()) for (auto& x : *a) { auto i = x["id"].toString(); if (i.isNotEmpty()) out.push_back (i); }
}
}

String Project::takeFileHash (const TakeWindowDef& w, const String& tracksHash) const
{
    return tracksHash + "-" + String::toHexString ((juce::int64) saveHash (w));
}

String Project::editFileHash (const EditDef& e, const String& tracksHash, const var& rootMixers, var* mixerOut) const
{
    String mixerHash;                                      // (the mixers' vars come from root: built once per save, the plug-ins asked for their state once)
    if (auto* ma = rootMixers.getArray())
        for (auto& mv : *ma)
            if (mv["editId"].toString() == e.id.toString()) { mixerHash = fingerprintOf (mv); if (mixerOut != nullptr) *mixerOut = mv; break; }
    return tracksHash + "-" + String::toHexString ((juce::int64) saveHash (e)) + "-" + mixerHash;
}

void Project::refreshWindowFileHashes()
{
    // after a project is read: what is in memory is what the files hold, so the hashes are set to it and the first save writes only what changes
    var root = toVar (false, false);
    const auto tracksHash = fingerprintOf (root["tracks"]);
    for (auto& w : takeWindows) { auto it = windowFiles.find (w->id.toString()); if (it != windowFiles.end()) it->second.hash = takeFileHash (*w, tracksHash); }
    for (auto& e : edits)       { auto it = windowFiles.find (e->id.toString()); if (it != windowFiles.end()) it->second.hash = editFileHash (*e, tracksHash, root["mixers"], nullptr); }
}

bool Project::splitWindowFiles (var& root, std::map<String, WindowFile>& next, std::vector<OldWindowFile>& old)
{
    next.clear(); old.clear();
    if (projectFile == juce::File()) { addWindowsToVar (root); return false; }
    const auto folder = projectFolder();
    std::set<String> used;
    struct Pending { juce::File file; String text; };
    std::vector<Pending> pending;
    juce::Array<var> takeRefs, editRefs;
    std::set<String> editMixerIds;

    auto place = [&] (const String& id, const String& nm, const String& sub, const String& ext)
    {
        const auto base = sanitiseForFile (nm);
        auto it = windowFiles.find (id);
        for (int n = 1; n < 10000; ++n)
        {
            const auto rel = sub + "/" + base + (n > 1 ? " (" + String (n) + ")" : String()) + ext;
            if (used.count (rel.toLowerCase()) > 0) continue;
            const bool ours = it != windowFiles.end() && it->second.rel.equalsIgnoreCase (rel);
            if (! ours && folder.getChildFile (rel).exists()) continue;       // never overwrite a file that is not this window's
            used.insert (rel.toLowerCase());
            return rel;
        }
        return sub + "/" + id + ext;
    };

    std::set<String> trackIds;                             // (the tracks a window uses are written into its file, so another project can match them by name)
    if (auto* ta = root["tracks"].getArray()) for (auto& t : *ta) { auto tid = t["id"].toString(); if (tid.isNotEmpty()) trackIds.insert (tid); }
    const auto tracksHash = fingerprintOf (root["tracks"]);          // (a renamed track is in every window's file: they are all written again)

    // A window's file is written only when the window changed: a hash of the window itself (saveHash: cheap, no text) with the tracks' and, for an
    // edit, its mixer's. Nothing of an unchanged window is serialised, which is what makes saving a project with thousands of takes quick.
    auto fileVar = [&] (const char* format, const char* key, const var& wv, const var& mixer)
    {
        var fr = obj();
        put (fr, "format", format); put (fr, "version", 1); put (fr, key, wv);
        if (mixer.isObject()) put (fr, "mixer", mixer);
        std::set<String> usedIds; findIds (wv, trackIds, usedIds);
        juce::Array<var> tl;
        if (auto* ta = root["tracks"].getArray())
            for (auto& t : *ta) { auto tid = t["id"].toString(); if (tid.isNotEmpty() && usedIds.count (tid) > 0) tl.add (t); }
        put (fr, "tracks", tl);
        return fr;
    };
    auto consider = [&] (const String& id, const String& nm, const String& hash, const char* sub, const char* ext, juce::Array<var>& refs, const std::function<String()>& text)
    {
        const auto rel = place (id, nm, sub, ext);
        auto prev = windowFiles.find (id);
        const bool same = prev != windowFiles.end() && prev->second.rel == rel && prev->second.hash == hash && folder.getChildFile (rel).existsAsFile();
        if (! same) pending.push_back ({ folder.getChildFile (rel), text() });
        next[id] = { rel, hash };
        var ref = obj(); put (ref, "id", id); put (ref, "name", nm); put (ref, "file", rel);
        refs.add (ref);
    };
    for (auto& w : takeWindows)
        consider (w->id.toString(), w->name, takeFileHash (*w, tracksHash), "Take Windows", ".fmtake", takeRefs,
                  [&] { return juce::JSON::toString (fileVar (kTakeFormat, "window", takeWindowToVar (*w), var()), false); });
    for (auto& e : edits)
    {
        var mixer;
        const auto hash = editFileHash (*e, tracksHash, root["mixers"], &mixer);
        if (mixer.isObject()) editMixerIds.insert (mixer["id"].toString());
        consider (e->id.toString(), e->name, hash, "Edit Windows", ".fmedit", editRefs,
                  [&] { return juce::JSON::toString (fileVar (kEditFormat, "edit", editToVar (*e), mixer), false); });
    }
    juce::Array<var> mixerRefs;
    if (auto* ma = root["mixers"].getArray())              // the mixers that are not an Edit's own (the processing mixer, the cue mixers)
        for (auto& m : *ma)
        {
            if (m["editId"].toString().isNotEmpty()) continue;
            var fr = obj();
            put (fr, "format", kMixerFormat); put (fr, "version", 1); put (fr, "mixer", m);
            put (fr, "tracks", root["tracks"]); put (fr, "buses", root["buses"]);       // (what the strips refer to: lets another project match them by name)
            consider (m["id"].toString(), m["name"].toString(), fingerprintOf (fr), "Mixer Windows", ".fmmix", mixerRefs, [&] { return juce::JSON::toString (fr, false); });
        }
    for (auto& m : missingWindowRefs)                      // files that could not be read: still listed
    {
        const auto k = m["kind"].toString();
        (k == "edit" ? editRefs : k == "mixer" ? mixerRefs : takeRefs).add (m);
    }

    for (auto& p : pending)                                // write everything first: if anything fails the project is saved whole
        if (! filesync::replaceTextSafely (p.file, p.text, true)) { next.clear(); addWindowsToVar (root); return false; }

    // files of windows that are gone (or that moved to a new name): tidied away by the caller once the project file is written
    for (auto& kv : windowFiles)
    {
        auto it = next.find (kv.first);
        if (it != next.end() && it->second.rel.equalsIgnoreCase (kv.second.rel)) continue;
        old.push_back ({ folder.getChildFile (kv.second.rel), it != next.end() });
    }

    juce::Array<var> keepMixers;
    if (auto* ma = root["mixers"].getArray()) for (auto& m : *ma) if (m["editId"].toString().isNotEmpty() && editMixerIds.count (m["id"].toString()) == 0) keepMixers.add (m);   // (an Edit mixer whose edit is not there)
    put (root, "mixers", keepMixers); put (root, "mixerFiles", mixerRefs);
    put (root, "takeWindows", juce::Array<var>()); put (root, "edits", juce::Array<var>());
    put (root, "takeWindowFiles", takeRefs); put (root, "editFiles", editRefs);
    return true;
}

void Project::mergeWindowFiles (var& root)
{
    const auto base = loadBase();
    // Every window file is read and parsed first, then they are merged into the project in their saved order. A file that is missing or damaged is replaced by its .bak (the save before)
    // when there is one; the note says so.
    struct Read { const char* format; const char* fileKey; var ref; String rel; var fv; int result = 0; };
    std::vector<Read> reads;
    auto queue = [&] (const char* refsKey, const char* format, const char* fileKey)
    {
        if (auto* refs = root[refsKey].getArray()) for (auto& r : *refs) reads.push_back ({ format, fileKey, r, r["file"].toString(), var(), 0 });
    };
    queue ("mixerFiles", kMixerFormat, "mixer");
    queue ("takeWindowFiles", kTakeFormat, "window");
    queue ("editFiles", kEditFormat, "edit");
    reportLoad ("Reading " + String ((int) reads.size()) + " window files", 0.05);
    parallelFor ((int) reads.size(), [&] (int i)
    {
        auto& rd = reads[(size_t) i];
        rd.result = filesync::readJsonWithBackup (base.getChildFile (rd.rel), rd.fv, [&] (const var& x) { return x.isObject() && x["format"].toString() == rd.format && x[rd.fileKey].isObject(); });
    });
    reportLoad ("Reading " + String ((int) reads.size()) + " window files", 0.5);

    size_t next = 0;
    auto take = [&] (const String& what, const char* kind) -> var*            // the next read, in order; a missing one is listed and skipped
    {
        auto& rd = reads[next++];
        const String name = what + " \"" + rd.ref["name"].toString() + "\"";
        if (rd.result == 2)
            loadNotes.add (name + " had a file that could not be read, so the save before it was used (" + rd.rel + ".bak). "
                           "Anything changed in it after that save is not there. The unreadable file was kept as " + rd.rel + ".damaged.");
        if (rd.result > 0) { windowFiles[rd.fv[rd.fileKey]["id"].toString()] = { rd.rel, {} }; return &rd.fv; }
        loadNotes.add (name + " is missing its file:  " + rd.rel);
        var m = obj(); put (m, "id", rd.ref["id"]); put (m, "name", rd.ref["name"]); put (m, "file", rd.rel); put (m, "kind", kind);
        missingWindowRefs.push_back (m);
        return nullptr;
    };
    if (auto* refs = root["mixerFiles"].getArray())         // the mixers first, in their saved order (the first is the processing mixer)
    {
        juce::Array<var> all;
        for (int i = 0; i < refs->size(); ++i) if (auto* fv = take ("The mixer", "mixer")) all.add ((*fv)["mixer"]);
        if (auto* old = root["mixers"].getArray()) for (auto& m : *old) all.add (m);
        put (root, "mixers", all);
    }
    auto go = [&] (const char* arrayKey, const char* refsKey, const char* fileKey, const char* what, const char* kind)
    {
        auto* refs = root[refsKey].getArray();
        if (refs == nullptr) return;
        var arr = root[arrayKey];
        if (! arr.isArray()) { arr = var (juce::Array<var>()); put (root, arrayKey, arr); }
        for (int i = 0; i < refs->size(); ++i)
            if (auto* fv = take (what, kind))
            {
                arr.getArray()->add ((*fv)[fileKey]);
                if ((*fv)["mixer"].isObject() && root["mixers"].isArray()) root["mixers"].getArray()->add ((*fv)["mixer"]);
            }
    };
    go ("takeWindows", "takeWindowFiles", "window", "The take window", "take");
    go ("edits", "editFiles", "edit", "The edit", "edit");
}

void Project::forgetWindowFile (const String& id)
{
    auto it = windowFiles.find (id);
    if (it == windowFiles.end()) return;
    auto f = projectFolder().getChildFile (it->second.rel);
    windowFiles.erase (it);
    if (f.existsAsFile() && ! f.moveToTrash()) f.deleteFile();
}

bool Project::deleteEditAndFile (const juce::Uuid& id)
{
    for (size_t i = 0; i < edits.size(); ++i)
        if (edits[i]->id == id)
        {
            edits.erase (edits.begin() + (std::ptrdiff_t) i);
            syncEditMixers();
            mastering.sync (edits);
            forgetWindowFile (id.toString());
            structureChanged();
            return true;
        }
    return false;
}

bool Project::deleteTakeWindowAndFile (const juce::Uuid& id)
{
    for (size_t i = 0; i < takeWindows.size(); ++i)
        if (takeWindows[i]->id == id)
        {
            removeTakeWindow ((int) i);
            forgetWindowFile (id.toString());
            return true;
        }
    return false;
}

int Project::missingAudioCount (bool quick) const
{
    auto& self = const_cast<Project&> (*this);
    // (quick) the files of takes found whole at the last open are not asked about one by one - only their FOLDER is (one question per folder, however
    // many takes it holds): a Recorded Media folder that was moved or renamed is found out at once, a single file gone from it is found out when it is played
    std::map<String, bool> folderThere;
    auto folderExists = [&] (const juce::File& f)
    {
        const auto dir = f.getParentDirectory().getFullPathName();
        auto it = folderThere.find (dir);
        if (it == folderThere.end()) it = folderThere.emplace (dir, f.getParentDirectory().isDirectory()).first;
        return it->second;
    };
    std::set<String> trusted;
    if (quick && ! filesMoved)
        for (auto& w : takeWindows) for (auto& g : w->groups) if (g.verified && g.lengthSamples > 0) for (auto& tf : g.files) if (folderExists (tf.file)) trusted.insert (tf.file.getFullPathName());
    std::set<String> seen; int n = 0;
    forEachAudioFile (self.takeWindows, self.edits, [&] (juce::File& f)
    {
        const auto path = f.getFullPathName();
        if (path.isEmpty() || ! seen.insert (path).second || trusted.count (path) > 0) return;
        if (quick && ! folderExists (f)) { ++n; return; }                            // (its folder is gone: no need to ask about the file)
        if (! f.existsAsFile()) ++n;
    });
    return n;
}

juce::StringArray Project::missingAudioList (int atMost) const
{
    juce::StringArray out; std::set<String> seen; int more = 0;
    auto add = [&] (const juce::File& f, const String& where)
    {
        if (f == juce::File() || ! seen.insert (f.getFullPathName()).second || f.existsAsFile()) return;
        if (out.size() < atMost) out.add (f.getFileName() + "   (" + where + ")"); else ++more;
    };
    for (auto& w : takeWindows)
        for (auto& g : w->groups)
        {
            for (auto& tf : g.files) add (tf.file, "Takes - " + w->name + ", " + w->displayName (g) + ", " + tf.trackName);
            for (auto& wp : g.waiting) for (auto& wf : wp.files) add (wf, "Takes - " + w->name + ", " + w->displayName (g) + ", waiting for corrected audio");
        }
    for (auto& e : edits)
    {
        auto doRegion = [&] (const EditRegion& r)
        {
            for (auto& rf : r.files) add (rf.file, "Edit - " + e->name + ", " + r.takeName + ", " + rf.trackName);
            for (auto& wf : r.waiting.files) add (wf, "Edit - " + e->name + ", " + r.takeName + ", waiting for corrected audio");
        };
        for (auto& r : e->regions) doRegion (r);
        for (auto& r : e->overdubs) doRegion (r);
    }
    if (more > 0) out.add ("... and " + String (more) + " more");
    return out;
}

juce::String Project::RelinkReport::summary() const
{
    juce::StringArray s;
    if (byName > 0) s.add (String (byName) + (byName == 1 ? " file found by its name" : " files found by their names"));
    if (renamed > 0) s.add (String (renamed) + (renamed == 1 ? " renamed file found" : " renamed files found") + " (by length, rate, depth and channels)");
    if (unsure > 0) s.add (String (unsure) + (unsure == 1 ? " file fits" : " files fit") + " several files in the folder: not linked, so nothing is guessed");
    if (stillMissing > 0) s.add (String (stillMissing) + (stillMissing == 1 ? " file is" : " files are") + " still missing");
    if (s.isEmpty()) return "Nothing was missing.";
    return s.joinIntoString (".\n") + ".";
}

Project::RelinkReport Project::relinkAudio (const juce::File& searchFolder, const std::function<void (const juce::String&, double)>& progress)
{
    RelinkReport rep;
    // what the project knows about each missing file, once per file (an edit's piece usually uses a take's own file: the take says the most about it);
    // the files that ARE there are remembered too, so none of them is taken for a missing one
    std::vector<relink::Wanted> wanted;
    std::map<String, size_t> index;                                                // full path -> wanted
    std::set<juce::File> inUse;
    std::set<String> names, fromTakes;                                             // every track name the project knows (as recorded, and as the tracks are called now); the paths a take described
    for (auto& t : tracks) names.insert (t.name);
    auto want = [&] (const juce::File& f) -> relink::Wanted*
    {
        if (f == juce::File()) return nullptr;
        if (f.existsAsFile()) { inUse.insert (f); return nullptr; }
        const auto path = f.getFullPathName();
        auto it = index.find (path);
        if (it != index.end()) return &wanted[it->second];
        index[path] = wanted.size();
        relink::Wanted w; w.file = f;
        wanted.push_back (w);
        return &wanted.back();
    };
    for (auto& w : takeWindows)
        for (auto& g : w->groups)
        {
            for (auto& tf : g.files)
            {
                names.insert (tf.trackName);
                if (auto* x = want (tf.file))
                {
                    fromTakes.insert (tf.file.getFullPathName());
                    x->sampleRate = g.sampleRate; x->numChannels = juce::jmax (1, tf.numChannels); x->lengthSamples = g.lengthSamples;
                    x->ownRecording = tf.sourceName.isEmpty();                                   // (not imported from another file)
                    x->bitsPerSample = x->ownRecording ? 24 : 0;                                  // (recorded by Fermata: always 24 bit)
                    x->trackName = tf.trackName; x->takeTag = pad3 (g.number); x->windowName = w->name; x->recordedAt = g.recordedAt;
                }
            }
            for (auto& wp : g.waiting) for (auto& wf : wp.files) want (wf);                       // (name only: nothing else is known about them)
        }
    for (auto& e : edits)
    {
        auto doRegion = [&] (EditRegion& r)
        {
            for (auto& rf : r.files)
            {
                names.insert (rf.trackName);
                if (auto* x = want (rf.file))
                    if (fromTakes.count (rf.file.getFullPathName()) == 0)                         // (not a take's file: what the piece knows)
                    {
                        // The take's length is the file's only for the take's own, whole file. A piece that was processed (de-clicked, pitch-corrected, sent out and
                        // back) has "(what was done)" after its take name and its files may be short ones starting at the take's start: their length is not known.
                        const juce::int64 len = rf.fileStart == 0 && ! r.takeName.contains (" (") ? r.sourceLength : 0;
                        if (x->sampleRate > 0.0) { if (len <= 0) x->lengthSamples = 0; continue; }       // (another piece described it already: the doubt wins)
                        x->sampleRate = r.sampleRate; x->numChannels = juce::jmax (1, rf.numChannels); x->lengthSamples = len;
                        x->trackName = rf.trackName;
                        if (r.takeName.length() >= 4 && r.takeName.substring (0, 3).containsOnly ("0123456789") && r.takeName[3] == ' ') x->takeTag = r.takeName.substring (0, 3);
                        for (auto& w : takeWindows) if (w->id == r.windowId) x->windowName = w->name;
                    }
            }
            for (auto& wf : r.waiting.files) want (wf);
        };
        for (auto& r : e->regions) doRegion (r);
        for (auto& r : e->overdubs) doRegion (r);
    }
    if (wanted.empty()) return rep;

    juce::StringArray trackNames;
    for (auto& n : names) if (n.trim().isNotEmpty()) trackNames.add (n);
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    const auto results = relink::find (wanted, searchFolder, formats, progress, inUse, trackNames);
    std::map<String, juce::File> replace;
    for (size_t k = 0; k < wanted.size(); ++k)
    {
        const auto& w = wanted[k]; const auto& r = results[k];
        if (r.found != juce::File())
        {
            replace[w.file.getFullPathName()] = r.found;
            if (r.how.startsWith ("the same name")) ++rep.byName; else ++rep.renamed;
            rep.found.add (w.file.getFileName() + "  ->  " + r.found.getFileName() + "  (" + r.how + ")");
            juce::Logger::writeToLog ("Relink: " + w.file.getFullPathName() + " -> " + r.found.getFullPathName() + " (" + r.how + ")");
        }
        else if (! r.alternatives.isEmpty())
        {
            ++rep.unsure;
            rep.unsureLines.add (w.file.getFileName() + ": fits " + String (r.alternatives.size()) + " files (" + r.alternatives.joinIntoString (", ") + ")");
            juce::Logger::writeToLog ("Relink: " + w.file.getFullPathName() + " fits several files, not linked: " + r.alternatives.joinIntoString (", "));
        }
        else
        {
            ++rep.stillMissing; rep.missing.add (w.file.getFileName());
            if (rep.stillMissing <= 100) juce::Logger::writeToLog ("Relink: " + w.file.getFullPathName() + " not found in " + searchFolder.getFullPathName());
        }
    }
    if (rep.stillMissing > 100) juce::Logger::writeToLog ("Relink: ... and " + String (rep.stillMissing - 100) + " more files not found");
    if (replace.empty()) return rep;
    for (auto& w : takeWindows)
        for (auto& g : w->groups)
            for (auto& tf : g.files) if (replace.count (tf.file.getFullPathName()) > 0) g.verified = false;      // (looked at again at the next open)
    forEachAudioFile (takeWindows, edits, [&] (juce::File& f)
    {
        auto it = replace.find (f.getFullPathName());
        if (it != replace.end()) f = it->second;
    });
    structureChanged();
    undoReset();           // not an undo step: Ctrl+Z must never quietly put the missing paths back (the save before is in the project's .bak)
    return rep;
}

bool Project::importMixerFile (const juce::File& file, String& message)
{
    var fv;
    if (juce::JSON::parse (file.loadFileAsString(), fv).failed() || ! fv.isObject() || fv["format"].toString() != kMixerFormat || ! fv["mixer"].isObject())
    { message = "That is not a Fermata mixer file."; return false; }
    // tracks and buses of the file are matched with this project's by name; the strips of the ones that are not here are left out
    std::vector<std::pair<String, String>> subs;
    if (auto* ta = fv["tracks"].getArray())
        for (auto& t : *ta)
        {
            const auto nm = t["name"].toString(); const int ch = t["inputs"].isArray() ? t["inputs"].size() : 1;
            for (auto& mine : tracks) if (mine.name.equalsIgnoreCase (nm) && mine.channelCount() == ch) { subs.push_back ({ t["id"].toString(), mine.id.toString() }); break; }
        }
    if (auto* ba = fv["buses"].getArray())
        for (auto& b : *ba)
        {
            const auto nm = b["name"].toString(); const bool ext = (bool) b["external"];
            for (auto& mine : buses) if (mine.name.equalsIgnoreCase (nm) && mine.external == ext) { subs.push_back ({ b["id"].toString(), mine.id.toString() }); break; }
        }
    auto text = juce::JSON::toString (fv, true);
    for (auto& sb : subs) text = text.replace (sb.first, sb.second);
    if (juce::JSON::parse (text, fv).failed()) { message = "Could not read that file."; return false; }
    // keep only what exists here
    juce::Array<var> tk, bs;
    if (auto* ta = fv["tracks"].getArray()) for (auto& t : *ta) if (findTrack (juce::Uuid (t["id"].toString())) != nullptr) tk.add (t);
    for (auto& b : buses) { var o = obj(); put (o, "id", b.id.toString()); put (o, "name", b.name); put (o, "external", b.external); put (o, "colour", (juce::int64) b.colour); bs.add (o); }
    Project tmp;
    tmp.restoreSampleRate = restoreSampleRate; tmp.restoreBlock = restoreBlock; tmp.insertFactory = insertFactory;
    var root = obj();
    put (root, "format", "Fermata project"); put (root, "version", 1); put (root, "name", "import");
    put (root, "tracks", tk); put (root, "buses", bs);
    var mv = fv["mixer"];
    put (mv, "editId", ""); put (mv, "id", juce::Uuid().toString());
    juce::Array<var> mxs; mxs.add (mv); put (root, "mixers", mxs);
    if (! tmp.fromVar (root) || tmp.mixers.empty()) { message = "Could not read that file."; return false; }
    String nm = fv["mixer"]["name"].toString(); if (nm.isEmpty()) nm = "Mixer";
    auto taken = [&] (const String& n) { for (auto& m : mixers) if (m->name == n) return true; return false; };
    String use = nm; for (int i = 2; taken (use); ++i) use = nm + " (" + String (i) + ")";
    auto& m = addMixer (use);
    applyMix (m, tmp.mixSnapshot (*tmp.mixers.front(), true), restoreSampleRate, restoreBlock);
    m.ditherBits.store (tmp.mixers.front()->ditherBits.load());
    message = "Mixer \"" + use + "\" added as a new mixer. Its output buttons and driver outputs are as in the file for the strips that matched; check the outputs of its Ext Buses.";
    structureChanged();
    return true;
}

bool Project::importWindowFile (const juce::File& file, String& message, juce::Uuid* newId, int* missingFiles)
{
    var fv;
    if (juce::JSON::parse (file.loadFileAsString(), fv).failed() || ! fv.isObject()) { message = "That is not a Fermata edit or take window file."; return false; }
    const auto format = fv["format"].toString();
    const bool isEdit = format == kEditFormat, isTake = format == kTakeFormat;
    if (! isEdit && ! isTake) { message = "That is not a Fermata edit or take window file."; return false; }
    var win = fv[isEdit ? "edit" : "window"];
    if (! win.isObject()) { message = "That file is damaged (it has no " + String (isEdit ? "edit" : "take window") + " in it)."; return false; }

    // ids to change: tracks are matched with this project's tracks by name; clashing window / region / take ids get new ones
    std::vector<std::pair<String, String>> subs;
    std::vector<std::tuple<juce::Uuid, String, int>> toRestore;
    if (auto* ta = fv["tracks"].getArray())
        for (auto& t : *ta)
        {
            juce::Uuid tid (t["id"].toString());
            if (tid.isNull() || findTrack (tid) != nullptr) continue;
            const int ch = t["inputs"].isArray() ? t["inputs"].size() : 1;
            const auto nm = t["name"].toString();
            TrackDef* same = nullptr;
            for (auto& mine : tracks) if (mine.name.equalsIgnoreCase (nm) && mine.channelCount() == ch) { same = &mine; break; }
            if (same != nullptr) subs.push_back ({ tid.toString(), same->id.toString() });
            else toRestore.push_back ({ tid, nm, ch });
        }
    std::vector<String> renew;
    if (isEdit)
    {
        if (findEdit (juce::Uuid (win["id"].toString())) != nullptr)
        {
            renew.push_back (win["id"].toString());
            collectIds (win, renew, "regions"); collectIds (win, renew, "overdubs"); collectIds (win, renew, "autoLanes");
            if (auto* ls = win["autoLanes"].getArray()) for (auto& l : *ls) collectIds (l, renew, "points");
            if (fv["mixer"].isObject()) renew.push_back (fv["mixer"]["id"].toString());
        }
    }
    else if (findTakeWindow (juce::Uuid (win["id"].toString())) != nullptr)
    {
        renew.push_back (win["id"].toString());
        collectIds (win, renew, "groups");
    }
    for (auto& id : renew) if (id.isNotEmpty()) subs.push_back ({ id, juce::Uuid().toString() });
    if (! subs.empty())
    {
        auto text = juce::JSON::toString (fv, true);
        for (auto& s : subs) text = text.replace (s.first, s.second);
        if (juce::JSON::parse (text, fv).failed()) { message = "Could not read that file."; return false; }
        win = fv[isEdit ? "edit" : "window"];
    }

    // a name that is not used yet
    {
        const auto wanted = win["name"].toString();
        auto taken = [&] (const String& n)
        {
            for (auto& e : edits) if (isEdit && e->name == n) return true;
            for (auto& w : takeWindows) if (! isEdit && w->name == n) return true;
            return false;
        };
        String n = wanted.isEmpty() ? String (isEdit ? "Edit" : "Take window") : wanted;
        for (int i = 2; taken (n); ++i) n = wanted + " (" + String (i) + ")";
        put (win, "name", n);
        if (isEdit && fv["mixer"].isObject()) { var mx = fv["mixer"]; put (mx, "name", n); }
    }

    for (auto& r : toRestore) restoreTrack (std::get<0> (r), std::get<1> (r), std::get<2> (r));

    // read it with a private project, then take the window over
    Project tmp;
    tmp.restoreSampleRate = restoreSampleRate; tmp.restoreBlock = restoreBlock;
    tmp.readingFrom = loadBase();
    var root = obj();
    put (root, "format", "Fermata project"); put (root, "version", 1); put (root, "name", "import");
    put (root, "tracks", fv["tracks"].isArray() ? fv["tracks"] : var (juce::Array<var>()));
    juce::Array<var> tws, eds, mxs;
    if (isEdit) { eds.add (win); if (fv["mixer"].isObject()) mxs.add (fv["mixer"]); } else tws.add (win);
    put (root, "takeWindows", tws); put (root, "edits", eds); put (root, "mixers", mxs);
    if (! tmp.fromVar (root)) { message = "Could not read that file."; return false; }

    // the audio files the imported window uses that are not there (each once)
    int missing = 0; std::set<String> seen;
    auto count = [&] (const juce::File& f) { if (f != juce::File() && seen.insert (f.getFullPathName()).second && ! f.existsAsFile()) ++missing; };
    if (isTake)
    {
        if (tmp.takeWindows.empty()) { message = "That file has no take window in it."; return false; }
        takeWindows.push_back (std::move (tmp.takeWindows.front()));
        if (newId != nullptr) *newId = takeWindows.back()->id;
        for (auto& g : takeWindows.back()->groups)
        {
            g.verified = false;                            // (found whole in the OTHER project: this one looks for itself at its next open)
            for (auto& tf : g.files) count (tf.file);
            for (auto& wp : g.waiting) for (auto& wf : wp.files) count (wf);
        }
    }
    else
    {
        if (tmp.edits.empty()) { message = "That file has no edit in it."; return false; }
        edits.push_back (std::move (tmp.edits.front()));
        const auto eid = edits.back()->id;
        if (newId != nullptr) *newId = eid;
        syncEditMixers();                                  // the edit gets its mixer ...
        if (auto* mine = mixerOfEdit (eid))                // ... and the levels it was saved with
            if (auto* theirs = tmp.mixerOfEdit (eid))
                applyMix (*mine, tmp.mixSnapshot (*theirs, false), restoreSampleRate, restoreBlock);
        mastering.sync (edits);
        auto doRegion = [&] (const EditRegion& r) { for (auto& rf : r.files) count (rf.file); for (auto& wf : r.waiting.files) count (wf); };
        for (auto& r : edits.back()->regions) doRegion (r);
        for (auto& r : edits.back()->overdubs) doRegion (r);
    }
    structureChanged();
    message = String (isEdit ? "Edit" : "Take window") + " \"" + win["name"].toString() + "\" added.";
    if (missing > 0) message << "  " << missing << " of its audio files could not be found.";
    if (missingFiles != nullptr) *missingFiles = missing;
    return true;
}
} // namespace td
