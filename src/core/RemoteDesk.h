#pragma once
#include "Project.h"
#include "RemoteProtocol.h"

namespace td::remote::desk
{
/** What the Stream Deck + XL (36 keys, 6 dials) can do, as plain functions on the project (the program's windows are reached from RemoteControl.cpp).
    Everything here runs on the message thread and works like the controls in Fermata's own windows. */

constexpr int kMaxInputs = 64;                    // the desk message carries at most this many inputs
constexpr int kMaxCue = kMixerSlots - 1;          // the dials: the processing mixer, then cue mixers 1 - 5

/** A number for the JSON with two decimals (a float such as 0.12f would otherwise be written 0.11999999731779099). */
inline double r2 (float v) { return std::round ((double) v * 100.0) / 100.0; }

// ---- the mixers' main outputs (the dials) -----------------------------------------------------------------------------------------------------------------
/** The mixer a dial means: slot 0 = the processing mixer (always), slots 1 - 5 = the cue mixers in their order. -1 = there is no such mixer. */
inline int mixerIndexForSlot (const Project& p, int slot)
{
    if (slot < 0 || slot > kMaxCue) return -1;
    return slot < p.cueEnd() ? slot : -1;
}
inline BusState* findBus (MixerState& m, const juce::Uuid& id)
{
    for (auto& f : m.busPool) if (f->busId == id) return f.get();
    return nullptr;
}
/** A mixer's main output: the first Ext Bus ticked as MAIN (the one the engine calls "the" main output), else its first Ext Bus. Null = it has none. */
inline BusState* mainBusOf (const Project& p, MixerState& m)
{
    BusState* first = nullptr;
    for (auto& b : p.buses)
        if (b.external)
        {
            auto* bs = findBus (m, b.id);
            if (bs == nullptr) continue;
            if (bs->isMain.get()) return bs;
            if (first == nullptr) first = bs;
        }
    return first;
}
/** A fader step from 'cur' dB: -80 .. +12, and all the way down is off (-100), like the control surface. Turning up from off lands on -79.5. */
inline float faderStep (float cur, float deltaDb)
{
    const float from = cur <= -99.0f ? -80.0f : cur;
    const float nv = juce::jlimit (-80.0f, 12.0f, std::round ((from + deltaDb) * 10.0f) / 10.0f);
    return nv < -79.5f ? -100.0f : nv;
}
inline bool mixOutStep (Project& p, int slot, float deltaDb)
{
    const int mi = mixerIndexForSlot (p, slot); if (mi < 0) return false;
    auto* b = mainBusOf (p, *p.mixers[(size_t) mi]); if (b == nullptr) return false;
    b->gainDb.set (faderStep (b->gainDb.get(), deltaDb));
    p.markDirty();
    return true;
}
inline bool mixOutMute (Project& p, int slot)
{
    const int mi = mixerIndexForSlot (p, slot); if (mi < 0) return false;
    auto* b = mainBusOf (p, *p.mixers[(size_t) mi]); if (b == nullptr) return false;
    b->mute.set (! b->mute.get());
    p.markDirty();
    return true;
}

// ---- the stage speaker panel: playback, the talkback mics, the output --------------------------------------------------------------------------------------
/** Moves one of the stage panel's levels (or puts it back to 0 dB): "playback", "mic1" / "mic2" (the first and second talkback mic, in the order the panel shows them), "out".
    The caller hands the result to the engine (AudioEngine::setStageMix). False = there is no such level. */
inline bool stageStep (Project& p, const juce::String& which, float deltaDb, bool reset)
{
    float* v = nullptr;
    if (which == "playback") v = &p.stagePlaybackDb;
    else if (which == "out") v = &p.stageOutputDb;
    else if (which == "mic1" || which == "mic2")
    {
        const size_t k = which == "mic1" ? 0 : 1;
        if (k >= p.crMicInputs.size()) return false;
        const int in = p.crMicInputs[k];
        if (! juce::isPositiveAndBelow (in, (int) p.inputs.size())) return false;
        if ((int) p.stageGainDb.size() <= in) p.stageGainDb.resize ((size_t) in + 1, 0.0f);
        if ((int) p.stagePan.size() <= in)    p.stagePan.resize ((size_t) in + 1, 0.0f);
        v = &p.stageGainDb[(size_t) in];
    }
    if (v == nullptr) return false;
    *v = reset ? 0.0f : juce::jlimit (-60.0f, 10.0f, std::round ((*v + deltaDb) * 10.0f) / 10.0f);
    p.markDirty();
    return true;
}

// ---- one input: its preamp, its monitor mode ------------------------------------------------------------------------------------------------------------
inline bool hasPreamp (const Project& p, int input)
{
    return juce::isPositiveAndBelow (input, (int) p.inputs.size()) && p.preampDriver != nullptr && p.preampDriver->controls (input);
}
/** The tracks that record from this input (any channel of the track). */
inline std::vector<TrackDef*> tracksOnInput (Project& p, int input)
{
    std::vector<TrackDef*> v;
    for (auto& t : p.tracks)
        for (int c = 0; c < t.channelCount(); ++c)
            if (t.inputOf (c) == input) { v.push_back (&t); break; }
    return v;
}
/** A preamp key of one input: "mic" (Mic / Line, or Line / Instrument on a fixed line input), "v48", "pol", "lc" (on / off, or OFF -> 40 -> 60 -> 80 where the device has a choice),
    "pad" (on / off, or OFF -> -12 -> -24 on the MT48). The same steps as the keys in the main window. False = nothing was changed. */
inline bool preampKey (Project& p, int input, const juce::String& what)
{
    if (! hasPreamp (p, input)) return false;
    PreampSettings s = p.inputs[(size_t) input].preamp;
    const bool lineOnly = s.lineOnly;
    if (what == "mic")
    {
        if (lineOnly) { if (! s.hasInstrument) return false; s.zHigh = ! s.zHigh; }
        else s.line = ! s.line;
    }
    else if (what == "v48") { if (lineOnly) return false; s.phantom = ! s.phantom; }
    else if (what == "pol") s.polarity = ! s.polarity;
    else if (what == "lc")
    {
        if (s.lowCutChoice)
        {
            const int now = s.lowCut ? s.lowCutHz : 0;
            const int next = now == 0 ? 40 : now == 40 ? 60 : now == 60 ? 80 : 0;
            s.lowCut = next != 0; if (next != 0) s.lowCutHz = next;
        }
        else s.lowCut = ! s.lowCut;
    }
    else if (what == "pad")
    {
        if (lineOnly || (s.steppedPad && s.line)) return false;
        if (s.steppedPad) { s.setPadStep ((s.padStep() + 1) % 3); s.zHigh = false; }
        else s.pad = ! s.pad;
    }
    else return false;
    p.setPreamp (input, s);
    return true;
}
/** The preamp gain of one input up or down by deltaDb (0 .. 66 in half dB steps, as the knob in the main window). */
inline bool gainStep (Project& p, int input, float deltaDb)
{
    if (! hasPreamp (p, input)) return false;
    PreampSettings s = p.inputs[(size_t) input].preamp;
    const float nv = juce::jlimit (0.0f, 66.0f, std::round ((s.gainDb + deltaDb) * 2.0f) / 2.0f);
    if (nv == s.gainDb) return true;
    s.gainDb = nv;
    p.setPreamp (input, s);
    return true;
}
/** The monitor mode of the tracks fed by this input goes on one step: E -> S -> S-L -> T -> E. Returns the new mode, or -1 when no track is fed by the input. */
inline int monitorCycle (Project& p, int input)
{
    auto fed = tracksOnInput (p, input);
    if (fed.empty()) return -1;
    const int next = ((int) fed.front()->monitor() + 1) % 4;
    for (auto* t : fed) t->setMonitor ((Monitor) next);
    p.markDirty();
    return next;
}

// ---- what the Stream Deck draws ---------------------------------------------------------------------------------------------------------------------------
/** The "inputs" array of the desk message (one object per driver input, at most kMaxInputs). Keys:
    i index, n the name (yours, else the driver's), d the input's tag ("36-BHAP-12": Project::inputTag), tk in the talkback mic, mon monitor mode of its tracks (-1 = no track; 0 E, 1 S, 2 S-L, 3 T),
    pre the input has preamp controls, and then (only when pre): g gain as shown, line (1 = LINE or INSTR is the lit one), lo a fixed line input, ins it has Line / Instrument, v48 phantom,
    h48 the 48V key exists, pol phase, lc low cut (0 off, 1 on, or the Hz where there is a choice), lcc the choice exists, pad (0 off, 1 on / -12, 2 = -24), stp the pad has three steps,
    hpad the Pad key exists. */
inline juce::var inputsToVar (Project& p)
{
    juce::Array<juce::var> out;
    const int n = juce::jmin ((int) p.inputs.size(), kMaxInputs);
    for (int i = 0; i < n; ++i)
    {
        const auto& in = p.inputs[(size_t) i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("i", i);
        o->setProperty ("n", in.name.trim().isNotEmpty() ? in.name.trim() : p.inputShortName (i));
        o->setProperty ("d", p.inputTag (i));
        o->setProperty ("tk", p.isCrMic (i));
        auto fed = tracksOnInput (p, i);
        o->setProperty ("mon", fed.empty() ? -1 : (int) fed.front()->monitor());
        const bool pre = hasPreamp (p, i);
        o->setProperty ("pre", pre);
        if (pre)
        {
            const auto& s = in.preamp;
            o->setProperty ("g", r2 (s.gainDb + s.shownGainOffset()));
            const bool lo = s.lineOnly;
            o->setProperty ("lo", lo); o->setProperty ("ins", s.hasInstrument);
            o->setProperty ("line", lo ? (s.hasInstrument && s.zHigh) : s.line);                           // for a fixed line input the lit one is INSTR
            o->setProperty ("h48", ! lo); o->setProperty ("v48", s.phantom);
            o->setProperty ("pol", s.polarity);
            o->setProperty ("lcc", s.lowCutChoice);
            o->setProperty ("lc", s.lowCut ? (s.lowCutChoice ? s.lowCutHz : 1) : 0);
            o->setProperty ("stp", s.steppedPad);
            o->setProperty ("hpad", ! lo && ! (s.steppedPad && s.line));
            o->setProperty ("pad", s.steppedPad ? s.padStep() : (s.pad ? 1 : 0));
        }
        out.add (juce::var (o));
    }
    return juce::var (out);
}
/** The "mixers" array: slot 0 = the processing mixer, then cue mixers 1 - 5. Keys: n name, db main output fader (-100 = off), mute, fp a cue mixer that follows the processing mixer, has (it has a main output). Missing mixers give {"has":false}. */
inline juce::var mixersToVar (Project& p)
{
    juce::Array<juce::var> out;
    for (int slot = 0; slot <= kMaxCue; ++slot)
    {
        auto* o = new juce::DynamicObject();
        const int mi = mixerIndexForSlot (p, slot);
        auto* b = mi >= 0 ? mainBusOf (p, *p.mixers[(size_t) mi]) : nullptr;
        o->setProperty ("has", b != nullptr);
        if (mi >= 0)
        {
            o->setProperty ("n", p.mixers[(size_t) mi]->name);
            o->setProperty ("fp", slot > 0 && p.mixers[(size_t) mi]->followProcessing.get());
        }
        if (b != nullptr) { o->setProperty ("db", r2 (b->gainDb.get())); o->setProperty ("mute", b->mute.get()); }
        out.add (juce::var (o));
    }
    return juce::var (out);
}
/** The "stage" object: playback, out (dB), ready (a TB output exists), mics: [{i, n, db}] in the order the panel shows them. */
inline juce::var stageToVar (const Project& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("playback", r2 (p.stagePlaybackDb));
    o->setProperty ("out", r2 (p.stageOutputDb));
    o->setProperty ("ready", ! p.tbOutputs.empty());
    juce::Array<juce::var> mics;
    for (int in : p.crMicInputs)
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("i", in);
        m->setProperty ("n", in >= 0 && in < (int) p.inputs.size() && p.inputs[(size_t) in].name.trim().isNotEmpty() ? p.inputs[(size_t) in].name.trim() : p.inputShortName (in));
        m->setProperty ("db", r2 (p.stageGainOf (in)));
        mics.add (juce::var (m));
    }
    o->setProperty ("mics", juce::var (mics));
    return juce::var (o);
}
/** A linear peak as whole dB for the meters message: -100 for silence, never above +6. */
inline int peakDb (float linear)
{
    if (linear <= 1.0e-5f) return -100;
    return juce::jlimit (-100, 6, juce::roundToInt (20.0f * std::log10 (linear)));
}
} // namespace td::remote::desk
