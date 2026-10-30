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
    juce::String sourceName2;          // for a stereo track joined from two mono files: the right one's original name
    juce::String sourceName;           // the file name it had when it was imported ("LV01 - Gravity 003_Omni-L.wav"); lets an imported AAF find it again. Empty = recorded here
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
    bool         verified = false;          // its files were found whole when the project was opened (saved: the next open does not ask the disk about them again, see Project::repairInterruptedRecordings)
    int          overruns = 0;              // blocks that did not reach the disk buffer in time while it was recorded (their time is silence in the files)
    juce::int64  lostSamples = 0;           // how much audio those overruns cost (the most on any one file)
    bool         writeFailed = false;       // a file stopped taking data while recording (the drive was full, or gone): the take is cut short
    std::vector<WaitingPiece> waiting;      // parts sent out to be processed in other software, waiting for the corrected files
    int          barIn = 0, barOut = 0;     // the bars of the music this take starts and ends in (0 = not entered); it contains those bars and all the ones between
    bool         roomTone = false;          // marked (right-click) as the take that holds the room tone: nothing but the room, on every track (one take per take window)
    double       roomToneIn = -1.0, roomToneOut = -1.0;   // seconds of the take that are the room tone (-1 = the whole take)
    juce::String notes;                     // free text about this take (right-click the take); empty = none. Shown on hover and in the take sheet

    double lengthSeconds() const { return sampleRate > 0 ? (double) lengthSamples / sampleRate : 0.0; }
};

/** "0.3 s", "12 s", "2 min 05 s": how much audio a number of samples is, for the overrun warnings. */
inline juce::String lostText (juce::int64 samples, double rate)
{
    const double secs = rate > 0 ? (double) samples / rate : 0.0;
    if (secs < 10.0) return juce::String (secs, 1) + " s";
    if (secs < 60.0) return juce::String ((int) secs) + " s";
    return juce::String ((int) (secs / 60.0)) + " min " + juce::String ((int) secs % 60).paddedLeft ('0', 2) + " s";
}

/** Does a take (or piece) that runs from bar 'in' to bar 'out' contain bar 'bar'? (Everything from 'in' to 'out' counts; 'in' = 0 means no bars were entered.) */
inline bool barsContain (int in, int out, int bar) { return in > 0 && bar >= in && bar <= (out > in ? out : in); }
inline juce::String barsLabel (int in, int out)
{
    if (in <= 0) return {};
    return out > in ? "Bar " + juce::String (in) + " - " + juce::String (out) : "Bar " + juce::String (in);
}
/** 3.18.4: the bar range in shorter and shorter forms, for a take too narrow to show all of it: "Bar 17 - 39", "17 - 39", "17-39", "17-", "17"
    (one bar: "Bar 17", "17"). The word "Bar" goes before any number does, and a number is never cut in half, so a narrow take never shows a
    wrong bar. Empty when no bars were entered. */
inline std::vector<juce::String> barsLabelForms (int in, int out)
{
    if (in <= 0) return {};
    const juce::String a (in);
    if (out <= in) return { barsLabel (in, out), a };
    const juce::String b (out);
    return { barsLabel (in, out), a + " - " + b, a + "-" + b, a + "-", a };
}
/** 3.18.4: the first of 'forms' (longest first) that is no wider than 'room', as measured by 'widthOf'. When none is: if 'cutLast', the last form
    with letters left off its end until it fits (no "...", so a narrow take still shows what it can); otherwise nothing. (For the words along
    the bottom of a take in a take window.) */
template <typename WidthOf>
juce::String fitLabel (const std::vector<juce::String>& forms, bool cutLast, int room, WidthOf&& widthOf)
{
    if (room <= 0 || forms.empty()) return {};
    for (auto& f : forms) if (widthOf (f) <= room) return f;
    if (! cutLast) return {};
    const auto& last = forms.back();
    int lo = 0, hi = last.length() - 1;                   // the longest start of it that fits (halving the range: a few measurements, not one per letter)
    while (lo < hi)
    {
        const int mid = (lo + hi + 1) / 2;
        if (widthOf (last.substring (0, mid).trimEnd()) <= room) lo = mid; else hi = mid - 1;
    }
    return last.substring (0, lo).trimEnd();
}

/** A 'Take window': all the takes for one piece. */
struct TakeWindowDef
{
    juce::Uuid   id;
    juce::String name { "Take window" };
    juce::String defaultLabel { "Take" };   // (old projects only; the piece title is used now: "005 - Symphony 2 - violin.wav")
    int          nextNumber = 1;
    std::vector<TakeGroup> groups;
    bool showLtc = false;                         // the LTC track's row is shown in this window (the triangle button)
    bool showMarkers = true;                      // the Markers tick box
    std::vector<Marker> markers;                  // quick markers along the top of this window (not part of undo)
    std::vector<juce::Uuid> hiddenTracks;         // tracks removed from THIS window's view (other windows and the mixers are not touched)
    int pieceBars = 0;                            // how many bars the piece has (the take chart, 3.8.0); 0 = not entered (the chart then runs to the last bar any take reaches)
    bool takeMatching = false;                    // 3.18.0: this window's takes are analysed for J and Find other versions (off until switched on; a setting, not part of undo)

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
                if (editTake == groups[i].id) { editTake = juce::Uuid::null(); editIn = editOut = -1.0; }
                if (cursorTake == groups[i].id) { cursorTake = juce::Uuid::null(); cursorSeconds = 0.0; }
                groups.erase (groups.begin() + (long) i);
            }
        if (! isHidden (trackId)) hiddenTracks.push_back (trackId);
    }

    // --- editing marks (the take window's IN / OUT for the 'copy to edit' button) ---
    juce::Uuid   editTake = juce::Uuid::null();   // the EDIT marks (keys 1 and 2): the part of a take that 'To edit' sends to the edit window
    double       editIn = -1.0, editOut = -1.0;   // seconds from the start of that take (-1 = not set)
    std::vector<juce::Uuid> editTracks;            // Alt + drag: only these tracks are marked (empty = every track of the take)
    bool editUses (const juce::Uuid& track) const { return editTracks.empty() || std::find (editTracks.begin(), editTracks.end(), track) != editTracks.end(); }
    bool         overdub = false;                 // the 'Overdub' box: pieces sent to the edit are laid over it instead of being added to the main track
    juce::Uuid   targetEdit = juce::Uuid::null(); // the edit that this window's 'To edit' button sends to (null = make one)
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
/** Bar coverage (3.8.0, the take chart): which bars of a piece how many takes contain, from the bars entered for each take (its IN and OUT Bar). */
namespace coverage
{
/** The last bar the chart runs to: the piece's bars when entered, or else the last bar any take reaches (and never less than that). */
inline int lastBar (const TakeWindowDef& w)
{
    int last = juce::jmax (0, w.pieceBars);
    for (auto& g : w.groups) if (g.barIn > 0) last = juce::jmax (last, juce::jmax (g.barIn, g.barOut));
    return last;
}
/** For bars 1 .. lastBar (index 0 = bar 1): how many takes contain each (duds never count). */
inline std::vector<int> counts (const TakeWindowDef& w)
{
    std::vector<int> c ((size_t) lastBar (w), 0);
    for (auto& g : w.groups)
        if (g.barIn > 0 && ! g.dud)
            for (int b = g.barIn; b <= juce::jmax (g.barIn, g.barOut) && b <= (int) c.size(); ++b) ++c[(size_t) (b - 1)];
    return c;
}
/** A run of bars that the same takes cover. */
struct Run
{
    int from = 0, to = 0;                 // bars, both included
    std::vector<int> takes;               // their numbers, in recording order (duds left out)
    int count() const { return (int) takes.size(); }
};
/** The piece cut into runs: a new run starts wherever the set of takes changes (bar 1 to lastBar, every bar in exactly one run). */
inline std::vector<Run> runs (const TakeWindowDef& w)
{
    std::vector<Run> out;
    const int last = lastBar (w);
    std::vector<const TakeGroup*> order;
    for (auto& g : w.groups) if (g.barIn > 0 && ! g.dud) order.push_back (&g);
    std::stable_sort (order.begin(), order.end(), [] (const TakeGroup* a, const TakeGroup* b) { return a->number < b->number; });
    for (int b = 1; b <= last; ++b)
    {
        std::vector<int> has;
        for (auto* g : order) if (barsContain (g->barIn, g->barOut, b)) has.push_back (g->number);
        if (! out.empty() && out.back().takes == has && out.back().to == b - 1) { out.back().to = b; continue; }
        Run r; r.from = r.to = b; r.takes = std::move (has);
        out.push_back (std::move (r));
    }
    return out;
}
/** The colour of a bar that n takes cover (3.11.0), on the take chart, the bar strip and the printed sheet alike: no take crimson, one take bright red,
    two red-orange, three a darkish yellow, then green from four takes (very light) getting darker with every take up to ten (dark green), and the same
    dark green beyond. Red-green-blue, 0 - 255. */
struct Rgb { int r = 0, g = 0, b = 0; };
inline Rgb colourFor (int takes)
{
    if (takes <= 0) return { 165, 14, 42 };                  // crimson
    if (takes == 1) return { 255, 45, 35 };                  // bright red
    if (takes == 2) return { 255, 112, 28 };                 // red-orange
    if (takes == 3) return { 204, 158, 18 };                 // darkish yellow
    const float t = juce::jlimit (0.0f, 1.0f, (float) (takes - 4) / 6.0f);       // 4 takes: 0 (very light) ... 10 or more: 1 (dark)
    const Rgb light { 200, 240, 194 }, dark { 18, 98, 40 };
    auto mix = [t] (int a, int b) { return (int) std::lround ((float) a + (float) (b - a) * t); };
    return { mix (light.r, dark.r), mix (light.g, dark.g), mix (light.b, dark.b) };
}
/** True when text on that colour should be dark (the light greens). */
inline bool isLight (const Rgb& c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b > 150.0; }

/** "Bars 51 - 58: only take 3", "Bar 12: no take", "Bars 1 - 20: 2 takes (1, 4)". */
inline juce::String describe (const Run& r)
{
    juce::String s = r.to > r.from ? "Bars " + juce::String (r.from) + " - " + juce::String (r.to) : "Bar " + juce::String (r.from);
    s << ": ";
    if (r.takes.empty()) return s + "no take";
    if (r.takes.size() == 1) return s + "only take " + juce::String (r.takes.front());
    juce::StringArray n; for (int t : r.takes) n.add (juce::String (t));
    return s + juce::String ((int) r.takes.size()) + " takes (" + n.joinIntoString (", ") + ")";
}
} // namespace coverage
} // namespace td
