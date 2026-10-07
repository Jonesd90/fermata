#pragma once
#include "Preamp.h"

namespace td
{
/** The last known full state of the preamps, kept next to the project and rewritten every second while the devices are connected.
    Used when the project is opened again: if the hardware has been changed since, Fermata offers to put it back. */
struct PreampMemory
{
    std::map<int, PreampSettings> inputs;          // driver input index -> its settings

    static bool same (const PreampSettings& a, const PreampSettings& b) noexcept
    {
        return std::abs (a.gainDb - b.gainDb) < 0.05f && a.phantom == b.phantom && a.line == b.line && a.lowCut == b.lowCut
            && a.polarity == b.polarity && a.pad == b.pad && a.zHigh == b.zHigh && a.boost == b.boost;
    }

    juce::var toVar() const
    {
        juce::Array<juce::var> arr;
        for (auto& [i, s] : inputs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("input", i); o->setProperty ("gainDb", (double) s.gainDb); o->setProperty ("phantom", s.phantom);
            o->setProperty ("line", s.line); o->setProperty ("lowCut", s.lowCut); o->setProperty ("polarity", s.polarity);
            o->setProperty ("pad", s.pad); o->setProperty ("zHigh", s.zHigh); o->setProperty ("boost", s.boost);
            arr.add (juce::var (o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("format", "Fermata preamp memory"); root->setProperty ("inputs", arr);
        return juce::var (root);
    }
    static bool fromVar (const juce::var& v, PreampMemory& out)
    {
        out.inputs.clear();
        if (v["format"].toString() != "Fermata preamp memory") return false;
        if (auto* a = v["inputs"].getArray())
            for (auto& e : *a)
            {
                PreampSettings s; s.gainDb = (float) (double) e["gainDb"]; s.phantom = (bool) e["phantom"]; s.line = (bool) e["line"];
                s.lowCut = (bool) e["lowCut"]; s.polarity = (bool) e["polarity"]; s.pad = (bool) e["pad"]; s.zHigh = (bool) e["zHigh"]; s.boost = (bool) e["boost"];
                out.inputs[(int) e["input"]] = s;
            }
        return true;
    }
    bool save (const juce::File& f) const { return f.replaceWithText (juce::JSON::toString (toVar(), false)); }     // written via a temporary file: never half-written
    static bool load (const juce::File& f, PreampMemory& out)
    {
        if (! f.existsAsFile()) return false;
        juce::var v; if (juce::JSON::parse (f.loadFileAsString(), v).failed()) return false;
        return fromVar (v, out);
    }
};
} // namespace td
