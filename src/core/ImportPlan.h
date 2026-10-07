#pragma once
#include "AudioOps.h"

namespace td
{
/** One audio file chosen for import, as found on disk. */
struct ImportSource
{
    juce::File   file;
    int          channels = 0;
    juce::int64  length = 0;
    double       sampleRate = 0.0;
    bool         floatData = false;
    juce::Time   modified;
    juce::StringArray channelNames;          // from the iXML chunk of a polyphonic WAV (may be empty)
};

/** One track's audio inside an imported take: a whole mono / stereo file, or one channel of a polyphonic file. */
struct ImportPart
{
    int          source = 0;                 // index into ImportPlan::sources
    int          channel = -1;               // -1 = the whole file, otherwise this channel of a polyphonic file
    juce::String trackKey;                   // what ties the same track together in different takes
    juce::String trackName;
    int          trackChannels = 1;          // 1 = mono track, 2 = stereo track
};

struct ImportTake
{
    int          number = 0;                 // the take number found in the file names (0 = none)
    juce::int64  length = 0;
    double       sampleRate = 0.0;
    juce::Time   modified;
    std::vector<ImportPart> parts;
};

struct ImportTrack { juce::String key, name; int channels = 1; };

/** The result of looking at a pile of files: which belong together as a take, and which track each is. */
struct ImportPlan
{
    std::vector<ImportSource> sources;
    std::vector<ImportTake>   takes;         // in the order they will be placed (take 1 first)
    std::vector<ImportTrack>  tracks;        // in the order they will appear
    juce::StringArray         notes;         // things the person should know (skipped files, mixed sample rates ...)
};

/** Looks at the files (header information only) and groups them. Pure logic: no files are read beyond what 'sources' already holds. */
ImportPlan buildImportPlan (std::vector<ImportSource> sources);

/** Reads the header information of the files. Files that cannot be read are listed in 'problems'. */
std::vector<ImportSource> inspectFiles (juce::AudioFormatManager&, const juce::Array<juce::File>&, juce::StringArray& problems);

/** The track names of a polyphonic WAV from its iXML chunk (channel order); empty if there are none. */
juce::StringArray readIXmlTrackNames (const juce::File&, int channels);

/** Copies the audio of every take into 'folder' as 24-bit WAV files (mono / stereo files, or one file per channel of a polyphonic file).
    files[takeIndex][partIndex] receives the new files. Runs on a job thread. Returns an error text ("" = fine). */
juce::String copyImportFiles (juce::AudioFormatManager&, const ImportPlan&, const juce::File& folder, const juce::String& label,
                              const std::vector<int>& takeNumbers, std::vector<std::vector<juce::File>>& files, AudioJob*);
} // namespace td
