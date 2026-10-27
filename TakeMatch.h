#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <atomic>
#include <functional>

namespace td::takematch
{
/** Finding the same place in another take (3.4.0), and every version of a passage (3.5.0). Every take gets a FINGERPRINT: twenty times a second,
    which notes are sounding, each in its own octave (60 notes, C2 to B6), with the loudness beside it. Two takes of the same music have fingerprints
    that follow the same course, however much the tempo, the dynamics or the room differ, and whether the takes start and stop in the same places
    or not. An ALIGNMENT lines the two courses up, letting one stretch against the other (dynamic time warping, open at both ends so a take that
    covers only part of the piece still lines up with the part it has), coarsely first and then finely along the coarse path. A place in one take
    is then read off the path in the other, with a confidence from how alike the two are around that place.
    3.7.0, tuned on real takes (a choir, 20 tracks, two pieces): the notes used to be folded into 12 note classes, which heard the melody, the bass,
    the speed and the loudness and hardly the inner voices - bars 1-2 of a verse scored as the same music as the start of the next verse, whose inner
    parts differ. Each note now keeps its octave. And a choir sinks: one take went from 26 to 55 cents flat, another sat a quarter-tone flat all the
    way. The notes are therefore read against the choir's OWN tuning, measured as it goes (the energy-weighted average distance of 100 Hz - 1.2 kHz
    from the equal-tempered notes, over 5 s), and read leaning flat (-75 to +25 cents): a choir 50 cents flat is not taken for one 50 cents sharp,
    which put two takes of the same music a semitone apart.
    All of it is plain arithmetic on files: no project, no engine. TakeMatchService (ui) runs it in the background and keeps the results. */

struct Fingerprint
{
    static constexpr int kNotes = 60, kLowNote = 36;     // MIDI 36 (C2, 65 Hz) to 95 (B6, 1976 Hz): each note in its own octave
    int bins = kNotes;
    double fps = 20.0;                       // frames per second
    std::vector<float> chroma;               // frames x bins: the strength of each note, every frame scaled to length 1 (silence: the flat vector)
    std::vector<float> loudness;             // dB per frame (full scale = 0)
    juce::String sourceKey;                  // the files it was made from (names, sizes, times): a changed file means a stale fingerprint

    int frames() const noexcept { return (int) loudness.size(); }
    double lengthSeconds() const noexcept { return fps > 0.0 ? (double) frames() / fps : 0.0; }
    bool empty() const noexcept { return loudness.empty(); }
    const float* frame (int i) const noexcept { return chroma.data() + (size_t) i * (size_t) bins; }
    /** Frames [from, to) of it (clipped), as a fingerprint of its own: a passage, or a passage with the music either side. */
    Fingerprint slice (int from, int to) const
    {
        Fingerprint out; out.bins = bins; out.fps = fps;
        from = juce::jlimit (0, frames(), from); to = juce::jlimit (from, frames(), to);
        out.chroma.assign (chroma.begin() + (long) from * bins, chroma.begin() + (long) to * bins);
        out.loudness.assign (loudness.begin() + from, loudness.begin() + to);
        return out;
    }
};

/** Quieter than this (the 8 kHz mix, dBFS) is silence: no evidence either way. */
constexpr double kSilenceDb = -72.0;
/** True when more than 'share' of frames [from, to) are silence (a passage, or the place a version was found: silence matches any silence). */
bool mostlySilent (const Fingerprint&, int from, int to, double share = 0.5);

/** The fingerprint of one channel of audio (any sample rate). For tests, and the inside of analyseFiles. */
Fingerprint analyseSamples (const float* mono, juce::int64 numSamples, double sampleRate, const std::atomic<bool>* cancel = nullptr);

/** The fingerprint of a take: its files mixed together (up to 'maxFiles' of them, all their channels), read block by block. progress 0..1. */
Fingerprint analyseFiles (juce::AudioFormatManager&, const std::vector<juce::File>& files, const std::atomic<bool>* cancel = nullptr,
                          const std::function<void (float)>& progress = {}, int maxFiles = 4);

/** What analyseFiles would put in sourceKey for these files (to tell whether a saved fingerprint is still the right one). */
juce::String sourceKeyFor (const std::vector<juce::File>& files);

/** A fingerprint saved as a small binary file and read back (false: not there, damaged, or an older version). */
bool save (const Fingerprint&, const juce::File&);
bool load (Fingerprint&, const juce::File&);

/** The alignment of take A with take B: the path, as pairs of times (seconds in A, seconds in B), both never decreasing, with the local
    cost at each point (0 = the two sound the same there, 1 = nothing in common). Empty when the takes have no stretch of music in common. */
struct Alignment
{
    std::vector<float> a, b, cost;
    int rotation = 0;                        // semitones B was turned by to match A (a transposed take; normally 0)
    bool empty() const noexcept { return a.empty(); }
    /** The time in B for a time in A, read off the path (interpolated). 'inside' says whether the time lies within the aligned stretch. */
    double map (double secondsInA, bool* inside = nullptr) const;
};
Alignment align (const Fingerprint& a, const Fingerprint& b, const std::atomic<bool>* cancel = nullptr);

/** The same place in the other take, with how sure the match is (0..1). Not found below kMinConfidence, or outside the aligned stretch. */
struct Match { bool found = false; double seconds = 0.0; double confidence = 0.0; juce::String why; };
constexpr double kMinConfidence = 0.55;
Match locate (const Alignment&, double secondsInA);

// ---- finding every version of a passage (3.5.0: "Find other versions" in the edit window) ----

/** A file and where it starts in its take's samples (a repaired or corrected piece is a short file that starts later than 0). */
struct Source { juce::File file; juce::int64 fileStart = 0; };
/** The fingerprint of part of a take: its files from take sample 'from' to 'to' (up to 'maxFiles' of them, mixed). */
Fingerprint analyseRange (juce::AudioFormatManager&, const std::vector<Source>& files, juce::int64 from, juce::int64 to, int maxFiles = 4);

/** One place in a take where the passage occurs. */
struct Version
{
    double start = 0.0, end = 0.0;           // seconds in the take (end exclusive): where the passage (its inner part, see findVersions) begins and finishes there
    double cost = 0.0;                       // how unlike it is (0 = the same): the mean local distance along the whole match
    double stretch = 1.0;                    // its length over the passage's (1.05 = it is 5 % slower there)
};
/** The cut-offs, from real takes (see DEVELOPMENT-NOTES 3.7.0): a passage with the music either side (kContextSeconds in all) is the same music up to
    kVersionCost; the bare passage, used for what a context cannot find (a false start is shorter than it), up to kPossibleCost - and that only
    "possibly": a few seconds can sound the same elsewhere. */
constexpr double kVersionCost = 0.15, kPossibleCost = 0.12, kContextSeconds = 8.0;
/** Every place in 'take' that sounds like 'passage' - a repeat, a false start and a restart, several passes - with the best first. The passage is matched
    whole; its tempo there may be 0.8 to 1.25 times its own. A place is kept when its cost is at most 'maxCost', it is not mostly silence, and it does not
    overlap a better one (two passes of the same music in one take never overlap). 'innerFrom' / 'innerTo' (frames of the passage; -1 = its end): the part
    the user marked, when the passage is that part with the music either side - each version's start and end are then that part's, read off the path. */
std::vector<Version> findVersions (const Fingerprint& passage, const Fingerprint& take, double maxCost = kVersionCost, int innerFrom = 0, int innerTo = -1);

/** Where exactly the marked part is, in a version found with the music either side: the part on its own, searched within 'within' seconds of where the
    context put it. The context decides WHICH place (it tells music apart that a few seconds cannot); the part's own match says where it starts and ends
    there (the context's path wanders by up to half a second inside it). 'found' is returned unchanged when the part on its own finds nothing near. */
Version placePart (const Fingerprint& part, const Fingerprint& take, const Version& found, double within = 1.0);

/** A join to the millisecond: the shape of the sound of 'a' around sample 'sampleA' (a window of 'windowSeconds') is looked for in 'b' within
    'searchSeconds' of 'sampleB'. Returns the sample of b that corresponds to sampleA (sampleB when nothing better is found); 'similarity' (if
    given) gets how well the shapes agree, 0..1. The first file of each (all its channels mixed) is used. */
juce::int64 refineJoin (juce::AudioFormatManager&, const Source& a, juce::int64 sampleA, const Source& b, juce::int64 sampleB,
                        double windowSeconds = 0.1, double searchSeconds = 0.06, double* similarity = nullptr);
} // namespace td::takematch
