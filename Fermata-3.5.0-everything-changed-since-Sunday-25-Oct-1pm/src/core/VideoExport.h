#pragma once
#include "Project.h"
#include "Bounce.h"
#include "Ltc.h"

namespace td::videoexport
{
/** Export for video edit (3.2.0). Three stereo files of exactly the same length, from the first audible sample of the edit to the last:
      1. <name>.wav             the stereo mix of the edit (the edit's own mixer, its main output), as Bounce makes it;
      2. <name> - LTC.wav       the timecode of the takes the edit was cut from, as audio, with no fades at all: see gateLtcRuns;
      3. <name> - check.wav     the mix summed to one channel on the left and the timecode on the right, so the video editor can hear that the
                                two agree.
    The timecode is the one recorded on the LTC track (from a Tentacle box, say) of the take each piece came from: decoded at the piece's own
    source position and made again as clean code, so the cameras that carry the same time of day line up with the exact takes used. */

/** One piece of the edit as the LTC file sees it: where its fade-in begins and where its audio ends (its fade-out's end), in timeline samples. */
struct PieceEdge { juce::int64 fadeInStart = 0, end = 0; };
/** A run of timecode in the LTC file: from / to in timeline samples (to exclusive), for the piece 'piece' of the list given. */
struct GatedRun { size_t piece = 0; juce::int64 from = 0, to = 0; };

/** The rule the person set: a piece's code starts the moment its fade-in begins and stops 'gapSamples' (a quarter of a second) BEFORE the next
    piece's fade-in begins, so no code ever runs through a crossfade and every join is preceded by a short silence; the last piece's code runs to
    the end of its audio. A piece whose fade-in is followed by the next fade-in within the gap - a very short edit - is left out altogether, so
    the longer runs on either side of it survive (the run before it goes on to a gap before the piece after it). 'pieces' in timeline order. */
std::vector<GatedRun> gateLtcRuns (const std::vector<PieceEdge>& pieces, juce::int64 gapSamples);

/** The timecode at sample 'fileSample' of channel 'channel' of a file holding LTC as audio: decoded from the seconds around that point (not
    extrapolated from the file's start, so a box whose clock differs a little from the recorder's is still read right at that moment). */
struct CodeAt { bool found = false; double seconds = 0.0; double fps = 25.0; bool drop = false; juce::String why; };
CodeAt timecodeAt (juce::AudioFormatManager&, const juce::File&, int channel, juce::int64 fileSample);

/** LtcGenerator's format code for a frame rate (see LtcGenerator::formatInfo). */
int formatCodeFor (double fps, bool drop);

/** What the export found and did, for the message at the end (and the log). */
struct Result
{
    juce::String error;                       // empty: all three files were written
    juce::File mixFile, ltcFile, checkFile;
    int pieces = 0, runs = 0, withoutCode = 0, dropped = 0;    // pieces in the edit; runs of code written; pieces whose take had no readable timecode; very short pieces left out
    double fps = 0.0; bool drop = false;
    juce::StringArray notes;                  // one line per piece that had no code, and anything else worth saying
    juce::int64 lengthSamples = 0;
};

struct Settings
{
    juce::File folder;
    juce::String name;                        // the base name of the three files
    double gapSeconds = 0.25;                 // the silence before every join (the person's quarter of a second)
    bool includeOverdubs = true;              // the overdub layer's pieces count as pieces too (they are heard)
};

/** Everything the export needs from the project, read on the message thread (the worker thread then touches only files). */
struct Plan
{
    struct Piece
    {
        PieceEdge edge;                       // on the timeline
        juce::String takeName; double atSeconds = 0.0;      // for the notes
        juce::File ltcFile; juce::int64 ltcSample = 0;      // where its fade-in's first sample is in the take's LTC file (ltcFile empty: no LTC file for this take)
    };
    std::vector<Piece> pieces;                // in timeline order
    juce::int64 start = 0, end = 0;           // the export: the edit's first to last audible sample
    double rate = 48000.0;
    juce::String baseName;
    bool noLtcTrack = false;                  // the project has no LTC track at all
    juce::String error;                       // (an empty edit)
};
Plan planFor (const Project&, const EditDef&, const Settings&);
/** The mix's settings: the edit through its own mixer (the processing mixer when it has none) to that mixer's main output, as the Bounce window
    would make them; one item, the whole export. Message thread. The Bouncer is then made on the message thread too (its plug-ins are re-created
    there) and handed to exportForVideo. */
BounceSettings mixSettingsFor (const Project&, const EditDef&, const Plan&, const Settings&);

/** Runs the export on a worker thread: decodes each piece's timecode, renders the mix with 'bouncer', writes the three files. progress 0..1. */
Result exportForVideo (const Plan&, Bouncer& bouncer, const Settings&, juce::AudioFormatManager&, std::atomic<float>* progress, const std::atomic<bool>* cancel);
} // namespace td::videoexport
