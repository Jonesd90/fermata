#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <memory>

namespace td
{
/** Consonants kept for moving and re-use (3.6.0). In the ReTouch window a box round a consonant can be COPIED: what the stray-consonant ReTouch would take
    out of that box - the consonant itself, above the vowels and the room around it - is kept, track by track, every channel of every track as it is (nothing
    is ever mixed down). "Copy and remove" also takes it out where it was. "Paste consonant here" adds it on top of the sound at the window's playhead, each
    track's audio on the same track, so an early or late consonant can be moved to sound together with the others, or a missing one put in.

    The audio of one consonant: for each track it was copied from, that track's channels (the same count as the track's file). 'lead' is how many samples
    of it come before the start of the box it was copied from (the paste puts the box's start at the playhead; the consonant's own soft start may reach a
    little earlier). */
struct ConsonantAudio
{
    double sampleRate = 48000.0;
    juce::int64 lead = 0;
    std::vector<juce::Uuid> trackIds;
    std::vector<juce::String> trackNames;
    std::vector<std::vector<std::vector<float>>> tracks;            // [track][channel][sample]

    juce::int64 length() const
    {
        juce::int64 n = 0;
        for (auto& t : tracks) for (auto& c : t) n = juce::jmax (n, (juce::int64) c.size());
        return n;
    }
    /** The channels kept for this track, or null when the consonant was not copied from it. */
    const std::vector<std::vector<float>>* forTrack (const juce::Uuid& id) const
    {
        for (size_t i = 0; i < trackIds.size() && i < tracks.size(); ++i) if (trackIds[i] == id) return &tracks[i];
        return nullptr;
    }
};

/** One track of a kept consonant: where its audio is (a float WAV in the project's Consonants folder, its channels as the track's file has them). */
struct ConsonantTrack
{
    juce::Uuid trackId;
    juce::String trackName;
    juce::File file;
    int numChannels = 1;
};

/** A consonant kept with the project (the list in the ReTouch window's Consonants tab). Saved in the project file; not an undo step. */
struct ConsonantDef
{
    juce::Uuid id;
    juce::String name;                       // what the user called it ("t", "s - soft" ...), or made up for a moved one
    juce::String from;                       // where it was copied: "003 - Requiem, 0:12.40"
    double sampleRate = 48000.0;
    juce::int64 length = 0, lead = 0;        // samples (see ConsonantAudio)
    juce::Time made;
    std::vector<ConsonantTrack> tracks;

    /** "t  (003 - Requiem, 0:12.40; 85 ms, 8 tracks)" - for the list. */
    juce::String listText() const
    {
        const int ms = sampleRate > 0.0 ? juce::roundToInt (1000.0 * (double) length / sampleRate) : 0;
        return name + "  (" + from + "; " + juce::String (ms) + " ms, " + juce::String ((int) tracks.size()) + (tracks.size() == 1 ? " track)" : " tracks)");
    }
};
} // namespace td
