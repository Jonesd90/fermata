#pragma once
#include "Project.h"
#include "Dither.h"
#include "Playback.h"

namespace td
{
/** Real-time engine: routes device inputs into every mixer, records armed tracks, writes outputs.
    Nothing on the audio thread allocates, locks (beyond try-locks) or touches the UI. */
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    explicit AudioEngine (Project&);
    ~AudioEngine() override;

    // --- device ---
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;

    // --- also usable without a device (tests) ---
    void prepare (double sampleRate, int maxBlock);
    void process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples);

    /** Call after any change to tracks / mixers / effects channels (message thread). */
    void rebuildPlan();

    // --- offline rendering (bounce out). Use on a private engine that belongs to a private copy of the project; no audio device needed. ---
    /** Choose the mixer to render and what to capture from it: the post-fader signal of any audio tracks / Int Buses / Ext Buses
        (their ids). Each one is captured separately, in the order given. Call before prepare(). */
    void setOfflineTarget (const juce::Uuid& mixerId, std::vector<juce::Uuid> tapIds);
    /** Runs n samples (<= maxBlock) of the chosen mixer with the given audio on its tracks (one buffer per project track) and
        puts each chosen output into its own stereo buffer in 'captures' (one per output, see setOfflineTarget). */
    void renderOffline (const std::vector<juce::AudioBuffer<float>>& trackAudio, int n, std::vector<juce::AudioBuffer<float>>& captures);

    // --- recording ---
    /** Starts recording every armed track into a new take group of the given window.
        Returns an error message, or an empty string on success. */
    juce::String startRecording (TakeWindowDef&);
    /** Stops, flushes files to disk and finalises the take group. Returns the group id. */
    juce::Uuid stopRecording();
    bool   isRecording() const noexcept   { return rec.load() != nullptr; }
    double recordedSeconds() const;
    int    overruns() const;
    juce::Uuid currentTakeId() const      { return currentGroupId; }

    // --- session mode: the inputs are kept for the last few seconds, so a take starts that long BEFORE Record was pressed ---
    static constexpr double kPreRollSeconds = 6.0;
    /** Switch session mode on / off (message thread). The listening stream is never touched by this, nor by starting or stopping a recording. */
    void setSessionMode (bool on);
    bool isSessionMode() const noexcept   { return sessionMode.load(); }

    // --- playback (the mixers are fed from disk instead of the live inputs while this plays) ---
    /** Takes over the session and starts it. Returns an error message, or an empty string. */
    juce::String startPlayback (std::unique_ptr<PlaybackSession>);
    void stopPlayback();
    /** Turns looping of the running playback on or off (off: it plays on to its end). */
    void setPlaybackLooping (bool on) noexcept { if (auto* p = playback.load()) p->setLooping (on); }
    bool isPlaying() const noexcept        { return playback.load() != nullptr; }
    /** True once a session has reached its end (call stopPlayback() from the message thread to tidy up). */
    bool playbackFinished() const noexcept { auto* p = playback.load(); return p != nullptr && p->isFinished(); }
    double playbackSeconds() const noexcept { auto* p = playback.load(); return p != nullptr ? p->getPositionSeconds() : 0.0; }
    int playbackUnderruns() const noexcept { auto* p = playback.load(); return p != nullptr ? p->getUnderruns() : 0; }

    /** The waveform recorded so far for one track (message thread, only while recording).
        data holds, for every bin and every channel, a min/max pair (+-32767): data[(bin * numChannels + channel) * 2].
        There are 'bins' bins, each 'binSamples' samples long. */
    bool getLivePeaks (const juce::Uuid& trackId, const juce::int16*& data, int& bins, int& binSamples, int* numChannels = nullptr) const;

    // --- engineer audition ---
    /** While set (to a mixer other than the first one), that mixer's mix is also played on the first mixer's outputs
        and the first mixer's own mix is not: the engineer listens to someone else's mix. nullptr = normal. */
    void setAuditionMixer (const MixerState* m) noexcept { auditionMixer.store (m); }
    const MixerState* getAuditionMixer() const noexcept  { return auditionMixer.load(); }

    // --- phase scope: the post-fader stereo signal of one audio track / Int Bus / Ext Bus (a StripState* or BusState* of a mixer) ---
    static constexpr int kScopeFrames = 16384;
    void setScopeSource (const void* stripOrBus) noexcept { scopeSource.store (stripOrBus); }
    const void* getScopeSource() const noexcept          { return scopeSource.load(); }
    /** Copies the most recent frames (up to maxFrames, oldest first) into l / r; returns how many. Message thread. */
    int readScope (float* l, float* r, int maxFrames) const noexcept;

    // --- talkback: the Control Room mic and playback to the studio, on one reserved stereo pair of driver outputs (TB) ---
    /** crMicInputs: the driver inputs that make up the CR mic (any number; they are summed). tbOutputFirst: the first driver output of the TB pair (-1 = none).
        Nothing the mixers send ever reaches the two TB outputs: they carry only the CR mic (while it is open) and, while the
        talkback playback is on and a take or edit is playing, the processing mixer's main output computed with EVERY live input off. */
    void setTalkbackRouting (const std::vector<int>& crMicInputs, int tbOutputFirst) noexcept
    {
        for (auto& f : crSel) f.store (false, std::memory_order_relaxed);
        for (int i : crMicInputs) if (i >= 0 && i < kMaxInputs) crSel[(size_t) i].store (true, std::memory_order_relaxed);     // any number of inputs together make the CR mic
        setTalkbackOutputs (tbOutputFirst >= 0 ? std::vector<int> { tbOutputFirst } : std::vector<int>());
    }
    /** Any number of talkback pairs: each entry is the first output of a pair (that output and the next one: left and right). A pair that starts on the
        last output is simply mono (the right channel is ignored). Pairs may overlap; an output in two of them carries both. */
    void setTalkbackRouting (const std::vector<int>& crMicInputs, const std::vector<int>& tbFirsts) noexcept
    {
        for (auto& f : crSel) f.store (false, std::memory_order_relaxed);
        for (int i : crMicInputs) if (i >= 0 && i < kMaxInputs) crSel[(size_t) i].store (true, std::memory_order_relaxed);
        setTalkbackOutputs (tbFirsts);
    }
    void setTalkbackOutputs (const std::vector<int>& firsts) noexcept
    {
        int m[kMaxInputs] = {};
        for (int f : firsts)
        {
            if (f < 0 || f >= kMaxInputs) continue;
            m[f] |= 1; if (f + 1 < kMaxInputs) m[f + 1] |= 2;
        }
        bool any = false;
        for (int o = 0; o < kMaxInputs; ++o) { tbMask[(size_t) o].store (m[o], std::memory_order_relaxed); any = any || m[o] != 0; }
        tbAny.store (any);
    }
    /** The stage speaker mixer (what the TB pair carries): a gain (dB) and a pan (-1 left .. +1 right) for each CR input, one gain for the playback
        and one for the whole output. Centre pan is full level on both sides, 0 dB everywhere is the old behaviour. -60 dB means off. Message thread. */
    void setStageMix (const std::vector<float>& inputDb, const std::vector<float>& inputPan, float playbackDb, float outputDb) noexcept
    {
        auto lin = [] (float db) { return db <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain (db); };
        for (int i = 0; i < kMaxInputs; ++i)
        {
            const float g = i < (int) inputDb.size() ? lin (inputDb[(size_t) i]) : 1.0f;
            const float p = juce::jlimit (-1.0f, 1.0f, i < (int) inputPan.size() ? inputPan[(size_t) i] : 0.0f);
            stageL[(size_t) i].store (g * juce::jmin (1.0f, 1.0f - p), std::memory_order_relaxed);
            stageR[(size_t) i].store (g * juce::jmin (1.0f, 1.0f + p), std::memory_order_relaxed);
        }
        stagePb.store (lin (playbackDb), std::memory_order_relaxed);
        stageOut.store (lin (outputDb), std::memory_order_relaxed);
    }
    /** The loudest sample (absolute) sent to the stage speakers since the last call; then it starts again. Message thread (the red PEAK light). */
    float takeStagePeak() noexcept { return stagePeakSeen.exchange (0.0f); }
    void setCrMic (bool open) noexcept { crOn.store (open); }
    void setTalkbackPlayback (bool on) noexcept { tbPlay.store (on); }

    // --- metering (message thread) ---
    float takeInputPeak (int inputIndex) noexcept;   // returns peak since last call, then resets
    float takeOutputPeak (int outputIndex) noexcept; // same, for what is sent to the driver outputs

    double getSampleRate() const noexcept { return sampleRate; }
    int    getMaxBlock()   const noexcept { return maxBlock; }
    int    getNumInputs()  const noexcept { return deviceInputs; }
    int    getNumOutputs() const noexcept { return deviceOutputs; }
    bool   isRunning() const noexcept     { return running.load(); }

private:
    struct Node;
    struct SendPlan { SendState* st; Node* dest; };
    /** One channel of a mixer: an Audio Track, an Int Bus or an Ext Bus. The nodes are kept in an order in which every
        channel comes after all the channels that send to it. */
    struct Node
    {
        NodeKind kind = NodeKind::Track;
        StripState* strip = nullptr;
        BusState* bus = nullptr;
        int trackIndex = -1;
        std::vector<SendPlan> sends;
        bool receives = false;                  // other channels send here
        bool isMainOut = false;                 // the mixer's main Ext Bus (what 'talkback playback' takes from the processing mixer)
        int tapIdx = -1;                        // offline render: which capture output its post-fader signal goes to (-1 = none)
        juce::AudioBuffer<float> incoming;      // stereo: the sum of what they send
    };
    struct MixerPlan
    {
        MixerState* mixer = nullptr;
        std::vector<std::unique_ptr<Node>> order;
        juce::AudioBuffer<float> chanBuf, pre, post, preSend, sendTmp;
    };
    struct TrackPlan { int numCh = 1; int inputs[kMaxTrackChannels] {}; std::shared_ptr<TrackLive> live; };
    struct Plan
    {
        std::vector<TrackPlan> tracks;
        std::vector<std::unique_ptr<MixerPlan>> mixers;
        std::vector<const juce::AudioBuffer<float>*> disk;      // per track: the audio to play instead of the live input (audio thread fills it)
    };
    /** Waveform overview of what is being recorded, written by the audio thread, read by the take window. */
    struct LivePeaks
    {
        static constexpr int kBinsPerSecond = 32, kMaxSeconds = 3 * 3600;
        std::vector<juce::int16> data;       // per bin and channel a min,max pair, scaled to +-32767: data[(bin * numCh + channel) * 2]
        std::atomic<int> bins { 0 };
        int binSamples = 1500, count = 0, numCh = 1;
        float curMin[kMaxTrackChannels] {}, curMax[kMaxTrackChannels] {};
        void prepare (double sampleRate, int channels)
        {
            numCh = juce::jlimit (1, (int) kMaxTrackChannels, channels);
            binSamples = juce::jmax (64, (int) std::lround (sampleRate / kBinsPerSecond));
            data.assign ((size_t) kBinsPerSecond * kMaxSeconds * 2 * (size_t) numCh, 0);
        }
        void add (const float* const* chans, int numCh, int n) noexcept;
    };
    struct RecTrack
    {
        TakeFile info;
        TrackPlan route;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        std::unique_ptr<LivePeaks> peaks;
    };
    /** The last seconds of every driver input that a track is patched to (session mode). Written by the audio thread; never reallocated
        while a recording uses it. Positions are counted in samples since the ring was made. */
    struct PreRoll
    {
        int cap = 0, preLen = 0;                  // samples kept per input / samples added in front of a take
        std::vector<int> slotOf;                  // driver input -> slot (-1 = not kept)
        std::vector<int> inputs;                  // slot -> driver input
        std::vector<float> data;                  // slot * cap + position % cap
        std::atomic<juce::int64> writePos { 0 };  // the position of the next sample to arrive
    };
    struct RecordingSession
    {
        std::vector<RecTrack> tracks;
        std::atomic<juce::int64> samples { 0 };   // samples delivered to the files so far (pre-roll included)
        std::atomic<int> overruns { 0 };
        juce::int64 startSample = 0;
        juce::Uuid windowId, groupId;
        // session mode: the files are fed from the PreRoll ring, starting preLen samples before the first block of the session
        bool useRing = false;
        PreRoll* ring = nullptr;
        juce::int64 startPos = -1, first = 0, delivered = 0;
        std::atomic<juce::int64> livePos { 0 };
    };

    void processMixer (MixerPlan&, const float* const* in, int numIn, int offset, int n, float* const* out, int numOut, bool playing,
                       bool silenceOwnOutput, int alsoToFirst, bool playbackOnly = false, float* tapL = nullptr, float* tapR = nullptr);
    void waitForAudioThread();
    void ensurePreRoll();                           // message thread: (re)makes the ring when session mode is on and nothing is recording
    void deliverFromRing (RecordingSession&, juce::int64 from, juce::int64 to);
    std::unique_ptr<Plan> buildPlan() const;

    Project& project;
    double sampleRate = 48000.0;
    int maxBlock = 512;
    int deviceInputs = 0, deviceOutputs = 0;

    std::atomic<Plan*> plan { nullptr };
    std::atomic<RecordingSession*> rec { nullptr };
    std::atomic<PlaybackSession*> playback { nullptr };
    std::atomic<PreRoll*> preRollPtr { nullptr };
    std::atomic<bool> sessionMode { false };
    std::atomic<const MixerState*> auditionMixer { nullptr };
    juce::Uuid offlineMixerId;                       // offline rendering (see setOfflineTarget)
    std::vector<juce::Uuid> offlineTaps;
    std::vector<juce::AudioBuffer<float>>* captureBufs = nullptr;  // only while renderOffline() runs
    std::unique_ptr<PlaybackSession> playSession;   // owned by the message thread
    std::atomic<juce::uint64> blockCounter { 0 };
    std::atomic<bool> running { false };
    std::atomic<juce::int64> samplePosition { 0 };
    std::atomic<float> inputPeak[kMaxInputs];
    std::atomic<float> outputPeak[kMaxInputs];

    std::atomic<const void*> scopeSource { nullptr };
    std::vector<float> scopeRing = std::vector<float> ((size_t) (2 * kScopeFrames), 0.0f);
    std::atomic<juce::int64> scopeWrite { 0 };

    std::vector<float> zeros;
    dither::Bank outDither;                         // audio thread: one noise generator per output channel
    int outDitherBits[kMaxInputs] {};               // audio thread: this block, the word length each output channel is dithered to (0 = none)
    std::array<std::atomic<bool>, kMaxInputs> crSel {};
    std::array<std::atomic<int>, kMaxInputs> tbMask {};      // per output: bit 1 = it carries the left of a talkback pair, bit 2 = the right
    std::atomic<bool> tbAny { false };
    std::array<std::atomic<float>, kMaxInputs> stageL {}, stageR {};     // the stage speaker mixer: each CR input's level into the left / right of a TB pair
    std::atomic<float> stagePb { 1.0f }, stageOut { 1.0f };
    std::atomic<float> stagePeakSeen { 0.0f };                                                           // audio thread: loudest sample sent to a TB output since the last look
    float stagePrevL[kMaxInputs] {}, stagePrevR[kMaxInputs] {}, stagePrevPb = 1.0f, stagePrevOut = 1.0f;     // audio thread: last block's values (so a fader move glides)
    std::atomic<bool> crOn { false }, tbPlay { false };
    float crGain = 0.0f;                            // audio thread: the CR mic's own fade (no click when it opens or closes)
    std::vector<float> tbTap;                       // audio thread: the playback-only main output of the processing mixer, left then right
    std::unique_ptr<RecordingSession> session;      // owned by the message thread
    juce::TimeSliceThread writerThread { "Disk writer" };
    juce::WavAudioFormat wav;
    juce::Uuid currentGroupId;

    juce::CriticalSection planLock;                 // serialises rebuildPlan() calls
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace td
