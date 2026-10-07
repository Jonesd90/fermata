#pragma once
#include "Common.h"
#include "Edit.h"
#include <algorithm>

namespace td
{
/** One recorded file: one track's audio for one take. */
struct TakeFile
{
    juce::Uuid   trackId;
    juce::String trackName;
    juce::File   file;
    int          numChannels = 1;
};

/** All the files created by one press of Record. They start/stop together and move together. */
struct TakeGroup
{
    juce::Uuid   id;
    int          number = 0;
    juce::String label;               // empty -> the take window's default label
    double       startSeconds = 0.0;  // position on the take window's timeline (moves the whole group)
    juce::int64  lengthSamples = 0;
    double       sampleRate = 48000.0;
    juce::Time   recordedAt;
    double       fadeInSeconds = kDefaultEdgeFade, fadeOutSeconds = kDefaultEdgeFade;   // short fades when the take is played / bounced (the files are untouched)
    std::vector<TakeFile> files;
    bool         dud = false;               // marked with D: nothing good in this take
    std::vector<WaitingPiece> waiting;      // parts sent out to be processed in other software, waiting for the corrected files
    int          barIn = 0, barOut = 0;     // the bars of the music this take starts and ends in (0 = not entered); it contains those bars and all the ones between

    double lengthSeconds() const { return sampleRate > 0 ? (double) lengthSamples / sampleRate : 0.0; }
};

/** Does a take (or piece) that runs from bar 'in' to bar 'out' contain bar 'bar'? (Everything from 'in' to 'out' counts; 'in' = 0 means no bars were entered.) */
inline bool barsContain (int in, int out, int bar) { return in > 0 && bar >= in && bar <= (out > in ? out : in); }
inline juce::String barsLabel (int in, int out)
{
    if (in <= 0) return {};
    return out > in ? "Bar " + juce::String (in) + " - " + juce::String (out) : "Bar " + juce::String (in);
}

/** A 'Take window': all the takes for one piece. */
struct TakeWindowDef
{
    juce::Uuid   id;
    juce::String name { "Take window" };
    juce::String defaultLabel { "Take" };   // (old projects only; the piece title is used now: "005 - Symphony 2 - violin.wav")
    int          nextNumber = 1;
    std::vector<TakeGroup> groups;
    std::vector<juce::Uuid> hiddenTracks;         // tracks removed from THIS window's view (other windows and the mixers are not touched)

    bool isHidden (const juce::Uuid& trackId) const
    {
        for (auto& h : hiddenTracks) if (h == trackId) return true;
        return false;
    }
    void unhide (const juce::Uuid& trackId)
    {
        for (size_t i = 0; i < hiddenTracks.size(); ++i) if (hiddenTracks[i] == trackId) { hiddenTracks.erase (hiddenTracks.begin() + (long) i); return; }
    }
    /** Takes this track's clips out of this take window (the audio files stay on the disk, and edits that use them keep working). */
    void removeTrackClips (const juce::Uuid& trackId)
    {
        for (auto& g : groups)
            g.files.erase (std::remove_if (g.files.begin(), g.files.end(), [&] (const TakeFile& f) { return f.trackId == trackId; }), g.files.end());
        for (size_t i = groups.size(); i-- > 0;)
            if (groups[i].files.empty())
            {
                if (markTake == groups[i].id) { markTake = juce::Uuid::null(); markIn = markOut = -1.0; }
                if (editTake == groups[i].id) { editTake = juce::Uuid::null(); editIn = editOut = -1.0; }
                if (cursorTake == groups[i].id) { cursorTake = juce::Uuid::null(); cursorSeconds = 0.0; }
                groups.erase (groups.begin() + (long) i);
            }
        if (! isHidden (trackId)) hiddenTracks.push_back (trackId);
    }

    // --- editing marks (the take window's IN / OUT for the 'copy to edit' button) ---
    juce::Uuid   markTake = juce::Uuid::null();   // which take the marks are in (null = none)
    juce::Uuid   editTake = juce::Uuid::null();   // the EDIT marks (keys 1 and 2): the part of a take that 'To edit' sends to the edit window
    double       editIn = -1.0, editOut = -1.0;   // seconds from the start of that take (-1 = not set)
    bool         overdub = false;                 // the 'Overdub' box: pieces sent to the edit are laid over it instead of being added to the main track
    juce::Uuid   targetEdit = juce::Uuid::null(); // the edit that this window's 'To edit' button sends to (null = make one)
    double       markIn = -1.0, markOut = -1.0;   // seconds from the start of that take (-1 = not set)
    juce::Uuid   cursorTake = juce::Uuid::null(); // the take window's cursor (where playback starts)
    double       cursorSeconds = 0.0;
    double       playheadSeconds = 0.0;           // the playhead on this window's timeline (set by clicking the ruler; Space plays from here)

    /** Which take the playhead is in: the LAST take that covers 'seconds' (so one laid over another wins). Null if it is between takes. */
    const TakeGroup* takeAt (double seconds) const
    {
        const TakeGroup* hit = nullptr;
        for (auto& g : groups)
            if (seconds >= g.startSeconds && seconds < g.startSeconds + juce::jmax (0.001, g.lengthSeconds())) hit = &g;
        return hit;
    }

    /** Moves the playhead. The cursor (where IN / OUT are marked) follows it: it is the playhead's place inside the take it is over. */
    void setPlayhead (double seconds)
    {
        playheadSeconds = juce::jmax (0.0, seconds);
        if (auto* g = takeAt (playheadSeconds)) { cursorTake = g->id; cursorSeconds = juce::jlimit (0.0, g->lengthSeconds(), playheadSeconds - g->startSeconds); }
        else { cursorTake = juce::Uuid::null(); cursorSeconds = 0.0; }
    }

    /** What Space should play from the playhead: the take it is over, from that point; between takes, the next take from its start. False if nothing is ahead. */
    bool playSpot (juce::Uuid& take, double& fromSeconds) const
    {
        if (auto* g = takeAt (playheadSeconds)) { take = g->id; fromSeconds = playheadSeconds - g->startSeconds; return true; }
        const TakeGroup* next = nullptr;
        for (auto& g : groups) if (g.startSeconds >= playheadSeconds && (next == nullptr || g.startSeconds < next->startSeconds)) next = &g;
        if (next == nullptr) return false;
        take = next->id; fromSeconds = 0.0; return true;
    }

    juce::String labelFor (const TakeGroup& g) const
    {
        return g.label.trim().isNotEmpty() ? g.label.trim() : name.trim().isNotEmpty() ? name.trim() : juce::String ("Take");   // a take is named after the piece
    }

    juce::String displayName (const TakeGroup& g) const { return pad3 (g.number) + " - " + labelFor (g); }

    static juce::String fileNameFor (int number, const juce::String& label, const juce::String& track)
    {
        return pad3 (number) + " - " + sanitiseForFile (label) + " - " + sanitiseForFile (track) + ".wav";
    }

    TakeGroup* findGroup (const juce::Uuid& gid)
    {
        for (auto& g : groups) if (g.id == gid) return &g;
        return nullptr;
    }

    double endSeconds() const
    {
        double e = 0.0;
        for (auto& g : groups) e = juce::jmax (e, g.startSeconds + g.lengthSeconds());
        return e;
    }

    /** Rename the take and every file in it. Returns false (and changes nothing it can't) on failure. */
    bool setLabel (const juce::Uuid& gid, const juce::String& newLabel)
    {
        auto* g = findGroup (gid);
        if (g == nullptr) return false;
        auto oldLabel = g->label;
        g->label = newLabel.trim();
        const auto effective = labelFor (*g);
        bool ok = true;
        for (auto& f : g->files)
        {
            auto target = f.file.getParentDirectory().getChildFile (fileNameFor (g->number, effective, f.trackName));
            if (target == f.file) continue;
            if (target.existsAsFile()) { ok = false; continue; }
            if (f.file.moveFileTo (target)) f.file = target; else ok = false;
        }
        if (! ok) { /* labels stay as typed; files that could not be renamed keep their old names */ }
        juce::ignoreUnused (oldLabel);
        return ok;
    }
};
} // namespace td
