#pragma once
#include <set>
#include "Project.h"
#include <functional>

namespace td
{
/** A change of level of one file: from timeline sample 'at' the gain glides (over 'ramp' samples, 0 = instantly) from 'from' to 'to'. */
struct GainStep { juce::int64 at = 0, ramp = 0; float from = 1.0f, to = 1.0f; };

class PlaybackSession;

/** The automation of an edit, ready for the audio thread: each lane points straight at the control it moves.

    Delay compensation: a plug-in that holds the sound back (look-ahead, linear phase) makes the sound reaching every control after it late by
    that much (the engine lines all paths up again at the outputs). So each lane is read that many samples EARLIER: 'lat' points at the figure
    the engine keeps for the place where the control acts (InsertSlot / SendState / StripState / BusState / VcaState autoLat...), and a move
    drawn under a note lands on that note. Without it, a fader behind a 50 ms look-ahead moved 50 ms early. */
struct AutomationPlan
{
    /** A channel a VCA lane controls: it gets the VCA's value read at its OWN lateness (channels under one VCA can be late by different amounts). */
    struct VcaMember { Param* db = nullptr; std::atomic<juce::uint32>* mask = nullptr; const std::atomic<int>* lat = nullptr; };
    /** Either 'target' (fader, pan, send level) or 'slot' + 'paramIndex' (a plug-in parameter, sent when it changes, and again in each of the first 'refresh' blocks of
        a play, so a plug-in that missed or ignored the first one (it was busy, or still settling as the transport started) is still told where the lane stands). */
    struct Item { Param* target = nullptr; InsertSlot* slot = nullptr; int paramIndex = 0; int steps = 0; mutable float lastSent = -1.0f; mutable int refresh = 24; AutoLane lane;    // steps: the plug-in parameter is a switch / selector with this many positions (0 = smooth)
                  const std::atomic<int>* lat = nullptr;
                  SendState* send = nullptr;                    // a send-level lane: its send (held in the plan while this plays: begin / end)
                  juce::uint32 vcaBit = 0; std::vector<VcaMember> members; };     // a VCA lane: its bit, and the channels it controls
    std::vector<Item> items;
    /** Sets every automated control to its value for the sound now passing it. t = the timeline sample now going in; with 'ps' (playback) the
        place 'lat' samples back is asked of the session, so a loop going round is followed. Allocation-free. */
    void apply (juce::int64 t, const PlaybackSession* ps = nullptr) const noexcept;
    /** Message thread, before it plays: marks the sends it moves (SendState::autoHeld). True if any: the engine's plan must then be rebuilt. */
    bool begin() const noexcept;
    /** Message thread, once the audio thread has stopped applying it: the channels go back to their VCAs' own faders, the sends are released.
        True if any send was held (rebuild the plan). */
    bool end() const noexcept;
};
/** Builds the automation plan of an Edit, aimed at the mixers' controls: a lane that names no mixer drives the Edit's own mixer (onProcessingMixer: the processing
    mixer instead). Null when the Edit has no automation, or nothing in it can play (lanes whose track or mixer is gone are left out; a Pan lane only drives
    mono tracks). Whether the lanes are shown makes no difference: what is written plays. */
std::shared_ptr<AutomationPlan> automationPlanFor (Project&, const EditDef&, bool onProcessingMixer = false, const juce::Uuid& unassignedMixer = juce::Uuid::null());

/** One stretch of one file placed on the playback timeline. */
struct PlaySegment
{
    int          trackIndex = 0;             // index into Project::tracks
    juce::File   file;
    double       rate = 0.0;                 // the file's sample rate as the project knows it (0 = not known): checked before playing, without opening the file
    juce::int64  srcOffset = 0;              // file sample = timeline sample + srcOffset
    juce::int64  begin = 0, end = 0;         // audible span on the timeline [begin, end)
    juce::int64  fadeInStart = 0,  fadeInLen = 0;     // len 0 = no fade
    juce::int64  fadeOutStart = 0, fadeOutLen = 0;
    FadeCurve    inCurve = FadeCurve::EqualPower, outCurve = FadeCurve::EqualPower;
    std::vector<GainStep> gainSteps;                  // volume changes, sorted by 'at' (none = unity)

    /** The gain these steps give at timeline sample t. */
    float gainAt (juce::int64 t) const noexcept
    {
        float g = 1.0f;
        for (auto& st : gainSteps)
        {
            if (t < st.at) break;
            g = (st.ramp > 0 && t < st.at + st.ramp) ? st.from + (st.to - st.from) * (float) (t - st.at) / (float) st.ramp : st.to;
        }
        return g;
    }
};

/** Streams audio from disk (on a background thread, crossfades and all) so the audio thread only copies.
    The engine feeds the result to the mixers in place of the live inputs while it plays. */
class PlaybackSession : private juce::Thread
{
public:
    PlaybackSession (std::vector<int> trackChannelCounts, std::vector<PlaySegment> segments,
                     juce::int64 startPosition, juce::int64 endPosition, double sampleRate, int maxBlock);
    ~PlaybackSession() override;

    /** Gets the files ready (they are opened by the disk thread as the play reaches them). Returns an error message or an empty string. */
    juce::String open();
    /** Starts the disk thread. waitForPreload: waits (at most 3 s) until the first second or so has been read, then returns (the tests, and
        whatever must have sound at once). Without it (Play in the program's windows, 2.102) it returns at once: until the reader has the first
        moments ready (primeSamples, about a third of a second) the session gives silence and its position stays at the start - so the playhead
        waits for the sound - and then it plays. Pressing Play never holds up the screen, whatever the disk is doing. */
    void start (bool waitForPreload = true);
    /** True once the session has enough read to play (at once after a start that waited). Any thread. */
    bool isPrimed() const noexcept { return primed.load (std::memory_order_acquire); }
    /** Milliseconds from start() to the first sound (0 until it has started). Any thread. */
    double msToFirstSound() const noexcept { return firstSoundMs.load(); }

    /** Automation applied while this plays (an edit's lanes); null = none. Set before the session is started. */
    std::shared_ptr<const AutomationPlan> automation;
    /** The Edit being played (null = a take, a clip or something else): it plays through that Edit's own mixer, and only that one. */
    juce::Uuid editId = juce::Uuid::null();
    /** >= 0: this session is a finished stereo mix (track 0, two channels). It goes straight to the driver outputs directOut and directOut + 1, at unity,
        without passing any mixer (the Mastering window's play of the renders). */
    int directOut = -1;

    // ---- audio thread ----
    /** Moves the next n samples into the track buffers. */
    void pull (int n) noexcept;
    const juce::AudioBuffer<float>& trackBuffer (int trackIndex) const noexcept { return trackBuf[(size_t) trackIndex]; }
    int numTracks() const noexcept { return (int) trackBuf.size(); }

    /** While true the session starts again at its start position each time it reaches its end (seamlessly: the disk thread just wraps). Any thread.
        Switched off, the pass that is playing finishes and the session ends (the process windows' auditions). */
    void setLooping (bool on) noexcept { looping = on; }
    /** Where the play carries on to if the loop is let go of (releaseLoop): set before start. Default: nowhere (it ends at the loop's end). */
    void setContinueTo (juce::int64 end) noexcept { continueTo = juce::jmax (endPos, end); }
    /** The take and edit windows' Loop switched off while it plays: the sound goes straight on past the end of the loop (mark 2) to continueTo, without
        a gap - also when the disk reader had already gone round to the start again (that audio is skipped). Any thread. */
    void releaseLoop() noexcept { releaseRequest.store (true); notify(); }       // (wakes the reader at once: the sooner it acts, the less is skipped)

    // ---- any thread ----
    bool isFinished() const noexcept        { return finished.load(); }
    double getSampleRate() const noexcept   { return sampleRate; }
    juce::int64 getPosition() const noexcept { return position.load(); }
    /** Where on the TIMELINE the sound being played is (for automation): the start of what was asked for, plus what has been played, going round with a loop.
        (getPosition() only counts what was played since the start.) */
    juce::int64 getTimelinePosition() const noexcept { return timelineAt (position.load()); }
    /** The timeline place of the sound that went in 'ago' samples before what getTimelinePosition() gives (for automation behind plug-ins that
        delay the sound: see AutomationPlan). Before the start of the play it simply counts back from the start (nothing came from there); round
        a loop it goes back to the end of the loop. */
    juce::int64 getTimelinePositionAgo (juce::int64 ago) const noexcept { return timelineAt (position.load() - juce::jmax ((juce::int64) 0, ago)); }
    /** How far along the timeline (seconds after the start position) the sound is that was played 'playedSeconds' into the session: round and
        round in a loop, straight on once the loop was let go of. For the playhead. */
    double timelineSecondsFromStart (double playedSeconds) const noexcept
    {
        if (sampleRate <= 0.0) return 0.0;
        return (double) (timelineAt ((juce::int64) std::llround (playedSeconds * sampleRate)) - startPos) / sampleRate;
    }
    double getPositionSeconds() const noexcept { return sampleRate > 0 ? (double) position.load() / sampleRate : 0.0; }
    /** Like getPositionSeconds, but between two audio blocks it moves on with the clock (never past the next block), so a playhead drawn from it
        glides instead of stepping a block at a time. For drawing only. */
    double getSmoothPositionSeconds() const noexcept;
    int getUnderruns() const noexcept       { return underruns.load(); }

    /** Opens the reader of one segment's file (null if it cannot be read). */
    using Opener = std::function<std::unique_ptr<juce::AudioFormatReader> (const PlaySegment&)>;
    /** Renders [from, from+n) of the segments into 'out' (one buffer per track). 'readers' has one entry per segment; an entry that is null is
        opened with 'opener' the first time its segment is rendered (none: the segment is skipped). The files are opened as they are needed and
        closed once the render has passed them (see releaseReadersBefore): an edit of thousands of pieces used to open every file the moment Play
        was pressed, with a read buffer for each. Public so tests can check the maths. */
    static void renderSegments (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers, const std::vector<PlaySegment>& segs,
                                juce::int64 from, int n, std::vector<juce::AudioBuffer<float>>& out, juce::AudioBuffer<float>& scratch,
                                const Opener* opener = nullptr);
    /** Closes the readers of the segments that end at or before 'pos' (they are not needed again unless the play loops). */
    static void releaseReadersBefore (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers, const std::vector<PlaySegment>& segs, juce::int64 pos);
    /** Renders with this session's own readers (the tests). */
    void renderNow (juce::int64 from, int n, std::vector<juce::AudioBuffer<float>>& out, juce::AudioBuffer<float>& work) { renderSegments (readers, segments, from, n, out, work, &opener); }
    std::vector<std::unique_ptr<juce::AudioFormatReader>>& getReaders() { return readers; }
    int openReaderCount() const { int n = 0; for (auto& r : readers) if (r != nullptr) ++n; return n; }
    /** The read buffer of each open file: 1 MB, less when many are open at once (96 MB between them all), never under 32 KB. */
    static int bufferBytesFor (size_t numOpen) noexcept
    {
        const auto share = (juce::int64) (96 << 20) / (juce::int64) juce::jmax ((size_t) 1, numOpen);
        return (int) juce::jlimit ((juce::int64) (32 << 10), (juce::int64) (1 << 20), share);
    }
    /** An opener for files read through a buffer of bufBytes: used by the session and by the offline render (Bounce). A file whose sample rate is
        not 'rate' (when rate > 0) is refused (null), and 'problem' (if given) says why. */
    static Opener makeOpener (juce::AudioFormatManager& formats, int bufBytes, double rate, juce::String* problem);
    const std::vector<PlaySegment>& getSegments() const { return segments; }

private:
    void run() override;
    /** The timeline place of the sound 'played' samples into the session (see getTimelinePosition). */
    juce::int64 timelineAt (juce::int64 played) const noexcept
    {
        const auto lf = linearFrom.load (std::memory_order_acquire);
        if (lf >= 0 && played >= lf) return linearTimeline.load (std::memory_order_relaxed) + (played - lf);
        if (played < 0 || loopLen <= 0) return startPos + played;
        return startPos + ((looping.load() || played > loopLen) ? played % loopLen : played);
    }
    /** Audio thread: copies up to n ready samples into the track buffers at 'offset'; returns how many. */
    int readRing (int offset, int n) noexcept;

    std::vector<int> channelCounts;
    std::vector<PlaySegment> segments;
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;      // one per segment, opened as they are needed (disk thread)
    Opener opener;
    juce::AudioFormatManager formats;
    juce::int64 startPos, endPos, renderPos;
    const juce::int64 loopEnd, loopLen;               // the end the session was made with (the loop's end), and start to it
    juce::int64 continueTo;                           // see setContinueTo
    std::atomic<bool> releaseRequest { false };       // see releaseLoop
    // disk thread: how much it has put in the ring, where the pass it is reading began in that count, and where the faded last moments of the
    // passes still in the ring begin in it (a short loop can be several passes ahead of what is heard)
    juce::int64 written = 0, wrapStream = 0;
    std::vector<juce::int64> tails;
    // once the loop is let go of: from consumed sample linearFrom on, the timeline runs straight on from linearTimeline (-1: not let go of)
    std::atomic<juce::int64> linearFrom { -1 }, linearTimeline { 0 };
    // a loop let go of after the reader had already faded its end / gone round: the audio thread skips dropLen samples once it has consumed dropAt
    std::atomic<juce::int64> dropAt { -1 }, dropLen { 0 }, dropTimeline { 0 };
    double sampleRate;
    int maxBlock;

    static constexpr int kRing = 1 << 17;
    juce::AbstractFifo fifo { kRing };
    // [track][channel][kRing]: plain arrays, so no juce::AudioBuffer flag is shared between the two threads. Not cleared when made: only what the
    // reader has written is ever read (40 tracks' worth is 20 MB, and filling it with zeros used to be part of every press of Play)
    std::vector<std::vector<juce::HeapBlock<float>>> ring;
    std::atomic<bool> primed { true };                // see start()
    int primeTarget = 16384;                          // samples the reader must have ready before a start that did not wait plays
    juce::int64 primingWaited = 0;                    // audio thread: samples of silence given while waiting for that (it gives up after 3 s)
    double startedMs = 0.0;
    std::atomic<double> firstSoundMs { 0.0 };
    std::set<juce::String> unreadable;                // disk thread: files that could not be opened (each is logged once)
    std::vector<juce::AudioBuffer<float>> chunk, trackBuf;
    juce::AudioBuffer<float> scratch;
    std::atomic<bool> renderDone { false }, finished { false }, looping { false };
    std::atomic<juce::int64> position { 0 };
    std::atomic<int> underruns { 0 };
    // the clock for getSmoothPositionSeconds: where playback was at the last audio block, when (ms), and how long that block was. clockSeq is odd
    // while the audio thread writes them, so a reader can tell it caught them half written.
    std::atomic<juce::uint32> clockSeq { 0 };
    std::atomic<juce::int64> clockPos { 0 };
    std::atomic<double> clockMs { 0.0 };
    std::atomic<int> clockBlock { 0 };
};

// ---- building segments from the project ----

/** Plays one take (all of its tracks). Timeline sample = take sample + timelineShift. */
std::vector<PlaySegment> segmentsForTake (const Project&, const TakeGroup&, juce::int64 timelineShift = 0);
/** Plays the whole edit with its crossfades. */
std::vector<PlaySegment> segmentsForEdit (const Project&, const EditDef&);
/** Where a piece's LTC stops (exclusive): see addRegionSegments. 'next' is the following piece of the edit (or nullptr). */
juce::int64 ltcEndOf (const EditRegion&, const EditRegion* next) noexcept;
/** Plays the edit's visual guide on its own, where it stands on the edit's timeline (2.102: its S button). Nothing when there is no guide. */
std::vector<PlaySegment> segmentsForGuide (const Project&, const EditDef&);
/** The track the guide plays on (its own; if that has gone, one with the same name and channels, else the first with as many channels); -1 = none. */
int guideTrackIndex (const Project&, const EditGuide&);
/** Plays one region's raw files on the timeline of the file's own samples (the trim window's 'play original'). */
std::vector<PlaySegment> segmentsForRegionOriginal (const Project&, const EditRegion&);
} // namespace td
