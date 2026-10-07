#pragma once
#include "Project.h"

namespace td
{
/** A change of level of one file: from timeline sample 'at' the gain glides (over 'ramp' samples, 0 = instantly) from 'from' to 'to'. */
struct GainStep { juce::int64 at = 0, ramp = 0; float from = 1.0f, to = 1.0f; };

/** The automation of an edit, ready for the audio thread: each lane points straight at the control it moves. */
struct AutomationPlan
{
    /** Either 'target' (fader, pan, send level) or 'slot' + 'paramIndex' (a plug-in parameter, sent only when it changes). */
    struct Item { Param* target = nullptr; InsertSlot* slot = nullptr; int paramIndex = 0; mutable float lastSent = -1.0f; AutoLane lane; };
    std::vector<Item> items;
    /** Sets every automated control to its value at timeline sample t. Allocation-free. */
    void apply (juce::int64 t) const noexcept
    {
        for (auto& it : items)
        {
            if (it.lane.pts.empty()) continue;
            if (it.target != nullptr) it.target->set (it.lane.valueAt (t, it.target->get()));
            else if (it.slot != nullptr)
            {
                const float v = it.lane.valueAt (t, 0.5f);
                if (std::abs (v - it.lastSent) > 1.0e-5f) { it.slot->setParamNorm (it.paramIndex, v); it.lastSent = v; }
            }
        }
    }
};
/** Builds the plan for an edit (null when automation is off or there is nothing to do). Lanes whose track or mixer is gone are left out;
    a Pan lane only drives mono tracks. */
std::shared_ptr<AutomationPlan> automationPlanFor (Project&, const EditDef&);

/** One stretch of one file placed on the playback timeline. */
struct PlaySegment
{
    int          trackIndex = 0;             // index into Project::tracks
    juce::File   file;
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

    /** Opens the files. Returns an error message or an empty string. */
    juce::String open();
    /** Starts the disk thread and waits until the first second or so is ready. */
    void start();

    /** Automation applied while this plays (an edit's lanes); null = none. Set before the session is started. */
    std::shared_ptr<const AutomationPlan> automation;

    // ---- audio thread ----
    /** Moves the next n samples into the track buffers. */
    void pull (int n) noexcept;
    const juce::AudioBuffer<float>& trackBuffer (int trackIndex) const noexcept { return trackBuf[(size_t) trackIndex]; }
    int numTracks() const noexcept { return (int) trackBuf.size(); }

    /** While true the session starts again at its start position each time it reaches its end (seamlessly: the disk thread just wraps). Any thread. */
    void setLooping (bool on) noexcept { looping = on; }
    bool isLooping() const noexcept    { return looping.load(); }

    // ---- any thread ----
    bool isFinished() const noexcept        { return finished.load(); }
    double getSampleRate() const noexcept   { return sampleRate; }
    juce::int64 getPosition() const noexcept { return position.load(); }
    double getPositionSeconds() const noexcept { return sampleRate > 0 ? (double) position.load() / sampleRate : 0.0; }
    int getUnderruns() const noexcept       { return underruns.load(); }

    /** Renders [from, from+n) of the segments into 'out' (one buffer per track). Public so tests can check the maths. */
    static void renderSegments (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers, const std::vector<PlaySegment>& segs,
                                juce::int64 from, int n, std::vector<juce::AudioBuffer<float>>& out, juce::AudioBuffer<float>& scratch);
    std::vector<std::unique_ptr<juce::AudioFormatReader>>& getReaders() { return readers; }
    const std::vector<PlaySegment>& getSegments() const { return segments; }

private:
    void run() override;

    std::vector<int> channelCounts;
    std::vector<PlaySegment> segments;
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::AudioFormatManager formats;
    juce::int64 startPos, endPos, renderPos;
    double sampleRate;
    int maxBlock;

    static constexpr int kRing = 1 << 17;
    juce::AbstractFifo fifo { kRing };
    std::vector<juce::AudioBuffer<float>> ring, chunk, trackBuf;
    juce::AudioBuffer<float> scratch;
    std::atomic<bool> renderDone { false }, finished { false }, looping { false };
    std::atomic<juce::int64> position { 0 };
    std::atomic<int> underruns { 0 };
};

// ---- building segments from the project ----

/** Plays one take (all of its tracks). Timeline sample = take sample + timelineShift. */
std::vector<PlaySegment> segmentsForTake (const Project&, const TakeGroup&, juce::int64 timelineShift = 0);
/** Plays the whole edit with its crossfades. */
std::vector<PlaySegment> segmentsForEdit (const Project&, const EditDef&);
/** Plays one region's raw files on the timeline of the file's own samples (the trim window's 'play original'). */
std::vector<PlaySegment> segmentsForRegionOriginal (const Project&, const EditRegion&);
} // namespace td
