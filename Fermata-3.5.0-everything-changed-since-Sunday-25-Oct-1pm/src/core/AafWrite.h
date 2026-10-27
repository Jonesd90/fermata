#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace td::aafwrite
{
/** Writing an AAF (3.3.0): the counterpart of Aaf.h, which reads one. The file is laid out the way Pyramix lays out the AAFs it writes for
    Pro Tools and Resolve (the sample AAF the person gave was the model):
      - one master clip per audio file, named "<take> (<n>) <track>", linked to the WAV by its path (nothing is embedded);
      - every file points at one "generic source" - a 24-hour tape with a timecode track - and its position on that tape is its time of day,
        which is where Pro Tools' original time stamp and Resolve's source timecode come from; a stereo file is one clip with two channels;
      - the timeline: one slot per track and channel, a sequence of fillers, clips and audio dissolves (the crossfades), and a timecode track
        for the timeline itself.
    An AAF also carries a dictionary of every class and type it uses: that part comes from the generated AafModel.h. */

/** An audio file the edit uses. */
struct SourceFile
{
    juce::File   file;
    juce::String name;                    // the master clip's name ("LV01 - Gravity 003 (1) Omni-L")
    juce::int64  lengthSamples = 0;
    int          channels = 1, bits = 24;
    double       sampleRate = 48000.0;
    double       startSeconds = -1.0;     // the time of day of the file's first sample (from its take's LTC); negative = not known (00:00:00:00 is written)
};

/** A piece of a file on one track of the timeline (one channel of the file). Positions and lengths in samples at the document's rate. */
struct Clip
{
    size_t       file = 0;                // index into Document::files
    int          channel = 0;             // which channel of that file
    juce::int64  at = 0;                  // where the clip's audio begins on the timeline (the beginning of its fade-in)
    juce::int64  length = 0;              // how much of it plays (to the end of its fade-out)
    juce::int64  sourceStart = 0;         // the file sample that plays at 'at'
    juce::int64  fadeIn = 0, fadeOut = 0; // fade lengths (a fade from or to silence; a crossfade is the overlap with the neighbouring clip)
    juce::int64  cutPoint = -1;           // when this clip overlaps the one before it (a crossfade): where the join is inside the overlap; -1 = half way
};

struct Track { juce::String name; std::vector<Clip> clips; };   // clips in timeline order; consecutive clips may overlap (the crossfade)

struct Document
{
    juce::String name;                    // the timeline's name (the edit's)
    double       sampleRate = 48000.0;    // the edit rate of every audio slot
    double       fps = 25.0; bool drop = false;      // the timecode tracks' frame rate
    double       timelineStartSeconds = 3600.0;      // the timeline's own timecode at its start (01:00:00:00 unless the caller says otherwise)
    juce::int64  length = 0;              // the timeline's length in samples (every track is padded to it)
    std::vector<SourceFile> files;
    std::vector<Track> tracks;
    juce::String productVersion;          // for the file's identification ("3.3.0")
};

/** Writes the document as an AAF file. Returns an error text, empty when all is well. */
juce::String writeAaf (const Document&, const juce::File& out);

} // namespace td::aafwrite
