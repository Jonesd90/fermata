#pragma once
#include "AudioOps.h"
#include "ConsonantData.h"

namespace td
{
/** Copying, keeping and reading back consonants (3.6.0; the data is in ConsonantData.h, the paste is FixSpec::Kind::Paste in AudioOps). */
namespace consonants
{
/** The stray-consonant ReTouch as a copy uses it: whatever the window's method box says, the box is turned down to the sound beside it (Attenuate, consonant
    mode, the full strength), so what comes out is the consonant itself. The other settings (bands, multi-resolution, linked stereo, the side, the side found on
    all the tracks together) are the window's. Lost samples are never looked for here (a copy must not move anything in time). */
inline FixSpec copySpec (FixSpec s)
{
    s.kind = FixSpec::Kind::Patch;
    s.repair.mode = RepairMode::Attenuate;
    s.repair.consonant = true;
    s.repair.direction = RepairDirection::LeftRight;
    s.repair.strength = 1.0;
    s.lostKnown = true; s.lost.clear();
    return s;
}

/** What the stray-consonant ReTouch takes out of the box [s0, s1), for every track: 'tracks' holds each track's channels over the same stretch (the box with
    the sound round it, which the ReTouch measures), all the same length. The result is the difference between each channel and the channel with the consonant
    turned down - the consonant itself, on each track as that track heard it - trimmed to where any track has more than about -100 dBFS of it (with 2 ms
    more each side), and 'lead' = how far before the box's start it begins. Empty (no tracks) when nothing at all was taken out. */
inline ConsonantAudio extract (const std::vector<juce::Uuid>& ids, const std::vector<juce::String>& names, const std::vector<std::vector<std::vector<float>>>& tracks,
                               double rate, long s0, long s1, const FixSpec& windowSpec)
{
    ConsonantAudio out; out.sampleRate = rate;
    const auto spec = copySpec (windowSpec);
    std::vector<std::vector<std::vector<float>>> diff (tracks.size());
    long first = -1, last = -1, L = 0;
    const float floorLevel = 1.0e-5f;
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        if (tracks[t].empty()) continue;
        auto fixed = tracks[t];
        audioops::applyFix (fixed, rate, s0, s1, spec, (long) (0.02 * rate));
        diff[t].resize (tracks[t].size());
        for (size_t c = 0; c < tracks[t].size(); ++c)
        {
            const auto& a = tracks[t][c]; const auto& b = fixed[c];
            auto& d = diff[t][c]; d.resize (a.size());
            L = juce::jmax (L, (long) a.size());
            for (size_t n = 0; n < a.size(); ++n)
            {
                d[n] = a[n] - b[n];
                if (std::abs (d[n]) > floorLevel) { if (first < 0 || (long) n < first) first = (long) n; if ((long) n > last) last = (long) n; }
            }
        }
    }
    if (first < 0 || last < first) return out;
    const long pad = (long) std::lround (0.002 * rate);
    first = juce::jmax (0L, first - pad); last = juce::jmin (L - 1, last + pad);
    out.lead = (juce::int64) (s0 - first);
    for (size_t t = 0; t < tracks.size(); ++t)
    {
        if (diff[t].empty()) continue;
        std::vector<std::vector<float>> ch;
        for (auto& d : diff[t])
        {
            std::vector<float> clip ((size_t) (last - first + 1), 0.0f);
            for (long n = first; n <= last && n < (long) d.size(); ++n) clip[(size_t) (n - first)] = d[(size_t) n];
            ch.push_back (std::move (clip));
        }
        out.trackIds.push_back (t < ids.size() ? ids[t] : juce::Uuid::null());
        out.trackNames.push_back (t < names.size() ? names[t] : juce::String());
        out.tracks.push_back (std::move (ch));
    }
    return out;
}

/** Writes the consonant's audio to 'folder' (made if needed): one 32-bit float WAV per track, named after the consonant and the track, every channel kept.
    Fills def.tracks, length, lead and rate. Returns "" or what went wrong (then nothing is left behind). */
inline juce::String save (const ConsonantAudio& a, ConsonantDef& def, const juce::File& folder)
{
    if (a.tracks.empty()) return "There is nothing to keep.";
    if (! folder.createDirectory()) return "Cannot make the folder " + folder.getFullPathName();
    def.tracks.clear();
    def.sampleRate = a.sampleRate; def.length = a.length(); def.lead = a.lead;
    const auto stem = sanitiseForFile (def.name.isNotEmpty() ? def.name : juce::String ("consonant"));
    for (size_t t = 0; t < a.tracks.size(); ++t)
    {
        const juce::String tn = t < a.trackNames.size() && a.trackNames[t].isNotEmpty() ? a.trackNames[t] : "track " + juce::String ((int) t + 1);
        const auto f = audioops::uniqueFile (folder.getChildFile (stem + " - " + sanitiseForFile (tn) + ".wav"));
        if (! audioops::writeWav (f, a.tracks[t], a.sampleRate, true))
        {
            for (auto& done : def.tracks) done.file.deleteFile();
            def.tracks.clear();
            return "Cannot write " + f.getFullPathName() + " (is the drive full?)";
        }
        ConsonantTrack ct; ct.trackId = t < a.trackIds.size() ? a.trackIds[t] : juce::Uuid::null(); ct.trackName = tn; ct.file = f; ct.numChannels = (int) a.tracks[t].size();
        def.tracks.push_back (ct);
    }
    return {};
}

/** Reads a kept consonant back from its files. Null + 'error' when a file is missing or does not match what was kept. */
inline std::shared_ptr<ConsonantAudio> load (const ConsonantDef& def, juce::AudioFormatManager& fm, juce::String& error)
{
    auto a = std::make_shared<ConsonantAudio>();
    a->sampleRate = def.sampleRate; a->lead = def.lead;
    for (auto& t : def.tracks)
    {
        auto r = audioops::openReader (fm, t.file);
        if (r == nullptr) { error = "The audio of \"" + def.name + "\" for " + t.trackName + " is missing (" + t.file.getFullPathName() + ")."; return nullptr; }
        if (std::abs (r->sampleRate - def.sampleRate) > 0.5) { error = "The audio of \"" + def.name + "\" for " + t.trackName + " is at another sample rate."; return nullptr; }
        if (r->lengthInSamples > (juce::int64) (60.0 * r->sampleRate)) { error = "The audio of \"" + def.name + "\" for " + t.trackName + " is over a minute long: not a consonant (the file may have been replaced)."; return nullptr; }     // (3.18.3: read whole into memory below - never a whole take by mistake)
        std::vector<std::vector<float>> ch;
        if (! audioops::readRange (*r, 0, r->lengthInSamples, ch) || ch.empty()) { error = "Cannot read " + t.file.getFullPathName(); return nullptr; }
        a->trackIds.push_back (t.trackId); a->trackNames.push_back (t.trackName); a->tracks.push_back (std::move (ch));
    }
    if (a->tracks.empty()) { error = "\"" + def.name + "\" has no audio."; return nullptr; }
    return a;
}

/** The names of the tracks of 'a' that are not among 'ids' (for "this consonant has nothing for ..." / "... gets nothing"). */
inline juce::StringArray tracksMissing (const ConsonantAudio& a, const std::vector<juce::Uuid>& ids)
{
    juce::StringArray out;
    for (size_t t = 0; t < a.trackIds.size(); ++t)
        if (std::find (ids.begin(), ids.end(), a.trackIds[t]) == ids.end()) out.add (t < a.trackNames.size() ? a.trackNames[t] : juce::String());
    return out;
}
} // namespace consonants
} // namespace td
