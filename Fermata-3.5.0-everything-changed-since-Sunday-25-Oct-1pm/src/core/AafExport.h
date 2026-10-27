#pragma once
#include "Project.h"
#include "AafWrite.h"

namespace td::aafexport
{
/** Export AAF (3.3.0): an edit as an AAF for Pro Tools, Resolve, Pyramix or any other program that reads one - the pieces of every track, the
    crossfades, and the takes' audio files linked by their paths (nothing is copied or embedded: the files stay where Fermata recorded them, so
    the receiving program must see the same drive, or relink by file name). Each file carries its source timecode - the time of day its first
    sample was recorded, read from the take's LTC track - so a camera synced to the same box lines up with every clip.
    What the AAF contains is described in AafWrite.h; this file turns an edit into that document. */

struct Settings
{
    juce::File file;                          // the .aaf to write
    bool includeOverdubs = true;              // the overdub layer's pieces go on tracks of their own ("<track> OD")
    double timelineStartSeconds = 3600.0;     // the timeline's own timecode at its start (01:00:00:00)
    bool includeLtcTracks = false;            // the LTC track's audio as a track too (normally left out: the code is carried as timecode, not as sound)
};

/** Everything the export needs from the project, read on the message thread; the worker thread then only touches files. */
struct Plan
{
    aafwrite::Document doc;                   // the files' lengths, channels and start times are filled in by exportAaf
    struct FileInfo { juce::File ltcFile; juce::int64 ltcSample = 0; juce::String takeName; };     // per doc.files entry: where its first sample is in its take's LTC file (ltcFile empty: none)
    std::vector<FileInfo> files;
    bool noLtcTrack = false;
    int clips = 0;
    juce::String error;
};
Plan planFor (const Project&, const EditDef&, const Settings&);

struct Result
{
    juce::String error;
    juce::File file;
    int files = 0, clips = 0, tracks = 0, withoutCode = 0, unreadable = 0;
    double fps = 25.0; bool drop = false;
    juce::StringArray notes;
};
/** Worker thread: reads each file's header and its timecode, then writes the AAF. */
Result exportAaf (Plan&, const Settings&, juce::AudioFormatManager&, std::atomic<float>* progress, const std::atomic<bool>* cancel);
} // namespace td::aafexport
