#pragma once
#include "Common.h"
#include <map>

namespace td
{
/** Everything a remote-controllable mic preamp channel can do. */
struct PreampSettings
{
    float gainDb   = 0.0f;
    bool  phantom  = false;   // +48V
    bool  line     = false;   // false = MIC input, true = LINE input
    bool  lowCut   = false;
    bool  polarity = false;   // polarity invert
    bool  pad      = false;   // PAD
    bool  zHigh    = false;   // high input impedance (the device's z_in)
    bool  boost    = false;   // the mic preamp's Boost (the device calls it "lift")
    int   lowCutHz = 80;      // the low cut's frequency, where the device offers a choice (MT48: 40 / 60 / 80 Hz); the others are fixed at 80
    // what the input CAN do (not saved; filled from the hardware): a fixed line input (the Anubis jack inputs) has no Mic / 48V / Pad / Boost,
    // only gain, polarity, low cut and (zHigh = "Instrument") the Line / Instrument switch
    bool  lineOnly      = false;
    bool  hasInstrument = false;
    bool  lowCutChoice  = false;  // the low cut has several frequencies (MT48): the LC key steps OFF -> 40 -> 60 -> 80 -> OFF
    /** The Neumann MT48's mic inputs: its Pad has three positions, which the device sends as two switches (pad, lift), and which only move the
        gain's SCALE - the gain itself (gainDb, 0..66) stays where it is:
            OFF    = pad off, lift on   ->  shown 12 .. 78 dB
            -12 dB = pad off, lift off  ->  shown  0 .. 66 dB
            -24 dB = pad on,  lift off  ->  shown -12 .. 54 dB
        No Boost key on these (the lift switch is part of the Pad). In Line mode the Pad is not offered and the gain shows 0 .. 66. */
    bool  steppedPad    = false;

    /** steppedPad: the Pad position, 0 = OFF, 1 = -12 dB, 2 = -24 dB. */
    int padStep() const noexcept { return pad ? 2 : (boost ? 0 : 1); }
    void setPadStep (int step) noexcept { pad = step == 2; boost = step == 0; }
    /** What is added to gainDb for the figure shown (and typed): the MT48's Pad scale; 0 for everything else. */
    float shownGainOffset() const noexcept { return (steppedPad && ! line) ? 12.0f - 12.0f * (float) padStep() : 0.0f; }
};

struct PreampDeviceCfg
{
    juce::String name, host;
    int firstInput = 0;        // the driver input (0-based) that this device's first preamp channel feeds; the rest follow in order
    std::map<int, int> map;    // when not empty it replaces firstInput: device preamp channel (0-based) -> driver input (0-based), as patched in ANEMAN
    bool operator== (const PreampDeviceCfg& o) const { return name == o.name && host == o.host && firstInput == o.firstInput && map == o.map; }

    /** The driver input that device channel 'ch' feeds, or -1 if that channel is not patched to any. */
    int inputFor (int ch) const
    {
        if (map.empty()) return firstInput + ch;
        auto it = map.find (ch); return it == map.end() ? -1 : it->second;
    }
    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject(); o->setProperty ("name", name); o->setProperty ("host", host); o->setProperty ("firstInput", firstInput);
        if (! map.empty())
        {
            juce::Array<juce::var> m; for (auto& p : map) { juce::Array<juce::var> pr; pr.add (p.first); pr.add (p.second); m.add (juce::var (pr)); }
            o->setProperty ("map", juce::var (m));
        }
        return juce::var (o);
    }
    static PreampDeviceCfg fromVar (const juce::var& v)
    {
        PreampDeviceCfg c; c.name = v["name"].toString(); c.host = v["host"].toString(); c.firstInput = (int) v["firstInput"];
        if (auto* m = v["map"].getArray()) for (auto& pr : *m) if (auto* a = pr.getArray()) if (a->size() == 2) c.map[(int) (*a)[0]] = (int) (*a)[1];
        return c;
    }
};

/** Talks to the hardware (Hapi / Anubis / MT48). The real implementation will send MIDI over the
    Ravenna link the same way Pyramix does. Until that is captured, NullPreampDriver is used. */
class PreampDriver
{
public:
    virtual ~PreampDriver() = default;
    virtual juce::String getName() const = 0;
    virtual bool isConnected() const = 0;
    /** Push settings for a physical input (0-based). Returns true if the hardware accepted them. */
    virtual bool apply (int inputIndex, const PreampSettings&) = 0;
    /** True if the hardware behind this driver input (0-based) has preamps that this driver can control (a Merging Hapi, MT48 or Anubis); any other input has none of the preamp controls. */
    virtual bool controls (int inputIndex) const { (void) inputIndex; return false; }
};

class NullPreampDriver : public PreampDriver
{
public:
    juce::String getName() const override  { return "Not connected (settings are stored but not sent)"; }
    bool isConnected() const override      { return false; }
    bool apply (int, const PreampSettings&) override { return false; }
};
} // namespace td
