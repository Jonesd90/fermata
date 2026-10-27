#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace td
{
/** Reading an AAF (Advanced Authoring Format) file written by another editing program (Pyramix, Pro Tools, Reaper ...): the tracks, where each piece of audio sits on the
    timeline, which audio file it comes from, and the cross-fades. The audio itself is not read: only the file names it refers to.

    The pieces of the AAF's one long timeline are separated wherever nothing plays on any track for more than 'gapSeconds'; each becomes one edit. */

/** One audio file of an imported region: one track's audio. */
struct AafRegionFile
{
    juce::String trackName;       // e.g. "Omni-L"
    int          trackOrder = 0;  // the track's place (top to bottom) in the AAF
    juce::String fileName;        // the file's name without its folder, from the AAF ("LV01 - Gravity 003_Omni-L.wav")
    juce::String url;             // the path the AAF gives for it (informational: the file may well have moved)
};

/** A piece of a take (all its tracks together) on the timeline of one imported edit. Same meaning as an EditRegion, in samples at 'AafResult::sampleRate'. */
struct AafRegion
{
    juce::String takeName;                 // e.g. "LV01 - Gravity 003"
    juce::int64  startSample = 0;          // position of srcIn on the piece's timeline (the piece starts at 0)
    juce::int64  srcIn = 0, srcOut = 0;    // the range used inside the files (srcOut is exclusive)
    double       inStart = 0.0, inEnd = 0.0, outStart = 0.0, outEnd = 0.0;      // fades in seconds, relative to the region's start / end (as in an EditRegion)
    std::vector<AafRegionFile> files;
};

struct AafPiece
{
    juce::String name;                     // from the take names, e.g. "LV01 - Gravity"
    double       startSeconds = 0.0, lengthSeconds = 0.0;      // where it sits in the AAF's timeline (the timeline's first sound = 0)
    std::vector<AafRegion> regions;        // in timeline order
    std::vector<juce::String> trackNames;  // the tracks used, top to bottom
};

struct AafResult
{
    juce::String error;                    // "" = fine
    double       sampleRate = 48000.0;     // the rate the numbers are in (the AAF's edit rate for audio)
    std::vector<AafPiece> pieces;
    std::vector<juce::String> trackNames;  // every track, top to bottom
    juce::StringArray notes;               // things the person should know (unsupported parts, ...)
    int          clips = 0;                // clips read
};

/** Reads the AAF. gapSeconds: nothing on any track for longer than this starts a new piece. */
AafResult readAaf (const juce::File&, double gapSeconds = 10.0);

/** For tests: the file names only. */
juce::String aafFileNameOf (const juce::String& url);
} // namespace td
