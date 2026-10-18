#pragma once
#include "Project.h"
#include "Dither.h"
#include "Playback.h"
#include "Ltc.h"
#include "MixerWorkers.h"
#include "BackupCopier.h"
#include "HostTransport.h"
#include "TakeWriter.h"

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
    /** Delay compensation. Message thread, about a few times a second: asks every loaded plug-in how late it is and, when anything changed (a plug-in
        loaded, bypassed, or one that changed its own delay), rebuilds the plan so that all paths are lined up again. */
    void checkLatency();
    /** The most this mixer delays its outputs by, in samples (the longest plug-in path through it). 0 for an unknown mixer. */
    int mixerLatency (const MixerState*) const;
    /** Offline render: how many samples the captured signals are late by (the first that many samples of every capture are to be thrown away; render that many extra at the end). */
    int offlineLatency() const;

    // --- offline rendering (bounce out). Use on a private engine that belongs to a private copy of the project; no audio device needed. ---
    /** Choose the mixer to render and what to capture from it: the post-fader signal of any audio tracks / Int Buses / Ext Buses
        (their ids). Each one is captured separately, in the order given. Call before prepare(). */
    void setOfflineTarget (const juce::Uuid& mixerId, std::vector<juce::Uuid> tapIds, std::vector<juce::Uuid> preFaderIds = {});
    // (preFaderIds: the same channels taken BEFORE their fader, after inserts and pan, not touched by mute / solo. Their captures come after the post-fader ones, in the order given.)
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

    // --- the second recording drive (see BackupCopier) ---
    /** Message thread: every new take is also copied, while it records, into this folder (normally on another drive), in a folder named after the
        project. An empty File switches it off. The main recording never waits for it. */
    void setBackupFolder (const juce::File& folder)        { backupRoot = folder; }
    juce::File getBackupFolder() const                     { return backupRoot; }
    /** Message thread: the copies that were thrown away since the last call (the second drive could not keep up, or could not be written). */
    juce::StringArray takeBackupProblems()                 { return backupCopier.takeBinned(); }
    int backupCopiesGoing() const                          { return backupCopier.busy(); }

    // --- the mixers of a block run side by side on helper threads (see MixerWorkers) ---
    /** Message thread: how many helper threads (0 = all the mixers run one after another on the audio thread). Only while the audio device is
        stopped (the threads cannot be replaced under a running block): false, and nothing changes, while it runs. */
    bool setMixerThreads (int count)                       { if (running.load()) return false; mixerWorkers.start (juce::jmax (0, count)); return true; }
    /** How many helpers a block may use right now (see MixerWorkers::setLimit). */
    int  mixerHelperLimit() const noexcept                 { return mixerWorkers.limit_(); }
    int  mixerThreads() const noexcept                     { return mixerWorkers.size(); }
    /** How many blocks have run their mixers side by side so far (a diagnostic). */
    juce::uint64 parallelBlockCount() const noexcept       { return parallelBlocks.load(); }
    /** Tests only: behave as if a device were running, so a rebuilt plan is picked up by the next process() call the way the audio thread would. */
    void setRunningForTests (bool r) noexcept              { running = r; }
    /** Tests only: how far (seconds) the second drive may fall behind before its copy is thrown away. */
    void setBackupMaxLag (double seconds)                  { backupCopier.maxLagSeconds.store (seconds); }
    double recordedSeconds() const;
    /** Blocks that did not reach the disk buffer in time (the disk stalled for longer than the buffer holds). Their time is kept as silence in the files. */
    int    overruns() const;
    /** How much audio those overruns cost, in samples (message thread, while recording). */
    juce::int64 samplesLost() const;
    /** Empty while the recording is fine. Otherwise what is wrong with it: a file stopped taking data (the drive is full, or gone). Any thread. */
    juce::String recordingProblem() const;
    /** How fast the current recording grows on the disk (bytes per second, all its files together); 0 when not recording. */
    double recordingBytesPerSecond() const noexcept   { return recBytesPerSecond.load (std::memory_order_relaxed); }
    /** The folder the current recording's files are in (message thread; a null File when not recording). */
    juce::File recordingFolder() const                { return session != nullptr ? recFolder : juce::File(); }
    /** How many seconds of audio the recording buffers hold: the disk may stall for this long before anything is lost. */
    static constexpr double kRecordBufferSeconds = 8.0;
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
    /** The same, moving on smoothly between audio blocks: for drawing the playhead. */
    double playbackSecondsSmooth() const noexcept { auto* p = playback.load(); return p != nullptr ? p->getSmoothPositionSeconds() : 0.0; }
    int playbackUnderruns() const noexcept { auto* p = playback.load(); return p != nullptr ? p->getUnderruns() : 0; }

    /** The waveform recorded so far for one track (message thread, only while recording).
        data holds, for every bin and every channel, a min/max pair (+-32767): data[(bin * numChannels + channel) * 2].
        There are 'bins' bins, each 'binSamples' samples long. */
    bool getLivePeaks (const juce::Uuid& trackId, const juce::int16*& data, int& bins, int& binSamples, int* numChannels = nullptr) const;

    // --- engineer audition ---
    /** While set (to a mixer other than the first one), that mixer's mix is also played on the first mixer's outputs
        and the first mixer's own mix is not: the engineer listens to someone else's mix. nullptr = normal. */
    void setAuditionMixer (const MixerState* m) noexcept { auditionMixer.store (m); }

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

    // --- LTC (timecode as audio): watch one input channel, and/or make LTC on one output channel ---
    /** Which driver input carries LTC (-1 = none). The lamp in the top bar reads ltcInStatus(). Message thread. */
    void setLtcInput (int inputChannel) noexcept { ltcInCh.store (inputChannel); }
    LtcLive::Status ltcInStatus() const noexcept { return ltcIn.status(); }
    /** Makes LTC on a driver output (on = false: off). formatCode: see LtcGenerator. The timecode (re)starts at startSeconds (seconds since midnight) when
        something other than the start time changed, or when restart is true; calling it again with the same settings changes nothing. Message thread. */
    void setLtcOutput (int outputChannel, bool on, int formatCode, double startSeconds, bool restart = false) noexcept
    {
        const int ch = on ? outputChannel : -1;
        const bool changed = restart || ch != ltcOutCh.load() || formatCode != ltcOutFormat.load();
        ltcOutStart.store (startSeconds);
        ltcOutCh.store (ch); ltcOutFormat.store (formatCode);
        if (changed) ltcOutRestart.fetch_add (1);
    }
    bool ltcOutActive() const noexcept { return ltcOutCh.load() >= 0 && ltcOutRunning.load(); }

    // --- how hard the audio thread works (the load meter and the timing log) ---
    /** What the audio callback did since the last call (message thread; reading starts a new count). Load = time spent in the callback as a share
        of the time the block lasts (its length at the sample rate): 100% means the PC only just keeps up. */
    struct LoadStats
    {
        float average = 0.0f, peak = 0.0f;   // percent
        int blocks = 0, late = 0;             // callbacks, and how many of them took longer than their block lasts (a dropout is likely)
        float recordBufferPeak = 0.0f;        // percent: the fullest any recording buffer got (100% = an overrun)
    };
    LoadStats takeLoadStats() noexcept;

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
    struct SendPlan { SendState* st; Node* dest; PathDelay delay; };     // delay: holds this path back so it arrives together with the others (delay compensation)
    /** One channel of a mixer: an Audio Track, an Int Bus or an Ext Bus. The nodes are kept in an order in which every
        channel comes after all the channels that send to it. */
    struct Node
    {
        NodeKind kind = NodeKind::Track;
        StripState* strip = nullptr;
        BusState* bus = nullptr;
        int trackIndex = -1;
        std::vector<SendPlan> sends;
        bool soloHeard = false;                 // audio thread: feeds something that is soloed (so it stays open while others are muted by solo)
        bool receives = false;                  // other channels send here
        bool isMainOut = false;                 // the mixer's main Ext Bus (what 'talkback playback' takes from the processing mixer)
        int tapIdx = -1;                        // offline render: which capture output its post-fader signal goes to (-1 = none)
        int tapPreIdx = -1;                     // offline render: which capture output its pre-fader signal goes to (-1 = none)
        PathDelay tapDelayPre;
        juce::AudioBuffer<float> incoming;      // stereo: the sum of what they send
        // delay compensation (samples): when the signals arrive at this channel, and when they leave it, counted from the sources
        int inLat = 0, outLat = 0;
        PathDelay srcDelay;                     // Audio Track: holds back its own source (disk / live input) to meet what is sent to it
        PathDelay outDelay;                     // Ext Bus: holds back the driver outputs so every Ext Bus of the mixer comes out together
        PathDelay tapDelay;                     // offline render: the same, for the captured signals
    };
    struct MixerPlan
    {
        MixerState* mixer = nullptr;
        std::vector<std::unique_ptr<Node>> order;
        juce::AudioBuffer<float> chanBuf, pre, post, preSend, sendTmp;
        int latency = 0;                        // the longest path through the mixer (samples): all Ext Bus outputs are this late
        int tapLatency = 0;                     // offline render: the captured signals are this late
        bool wasPlaying = false;                // audio thread
        bool resumeClear = false;               // audio thread: it rested (a cue mixer following the processing mixer): empty its delay lines when it runs again
        // the tail: after playback stops, the mixer that was playing goes on running (with silence going in) until its reverbs have died away,
        // like outboard gear after the tape stops (audio thread only)
        bool ranLive = false;                   // it ran as a live mixer in the last block
        bool tailNow = false;                   // this block it runs only for its tail: no disk audio, no live inputs
        int tailLeft = 0;                       // samples of tail still allowed (at most 30 s)
        int quietFor = 0;                       // how long its outputs have been below -90 dB
        float outPeak = 0.0f;                   // the loudest Ext Bus output in this block
        // running side by side with the other mixers: this mixer writes its driver outputs here, and the audio thread adds them up afterwards
        juce::AudioBuffer<float> outScratch;
        std::vector<float*> outPtrs;            // per driver output: where this mixer writes it (into outScratch), or nullptr
        std::vector<int> touched;               // the driver outputs it wrote in this block (room for every Ext Bus pair and the audition pair)
        int numTouched = 0;
    };
    struct TrackPlan { int numCh = 1; int inputs[kMaxTrackChannels] {}; std::shared_ptr<TrackLive> live; };
    /** One mixer to run in this block, and how. */
    struct MixerJob { MixerPlan* mp = nullptr; bool silenceOwnOutput = false; int alsoToFirst = -1; bool strict = false; bool capMain = false; };   // capMain: copy its main output aside (for the cue mixers that follow it)
    struct Plan
    {
        std::vector<TrackPlan> tracks;
        std::vector<std::unique_ptr<MixerPlan>> mixers;
        std::vector<MixerJob> jobs;                             // audio thread: the mixers that run in this block (room for all of them)
        std::vector<MixerPlan*> followers;                      // audio thread: the cue mixers that follow the processing mixer in this block (room for all)
        int outChannels = 0;                                    // driver outputs the mixers' outScratch buffers have room for
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
        std::unique_ptr<TakeWriter> writer;
        std::unique_ptr<LivePeaks> peaks;
        std::shared_ptr<BackupCopier::Job> backup;              // its copy on the second drive (null when there is none)
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

    void processMixer (const Plan& pl, MixerPlan&, const float* const* in, int numIn, int offset, int n, float* const* out, int numOut, bool playing,
                       bool silenceOwnOutput, int alsoToFirst, bool playbackOnly = false, float* tapL = nullptr, float* tapR = nullptr,
                       float* mainL = nullptr, float* mainR = nullptr);
    /** The audio thread has just picked up a new plan: the sound that is on its way through the old one's delay lines (delay compensation) is moved
        into the new one, so a rebuild (starting or stopping a recording, a routing change) never puts a gap or a click into what you hear. */
    static void carryOver (Plan& from, Plan& to) noexcept;
    /** Does any Ext bus of this mixer go to a driver output? */
    static bool mixerHasOutput (const MixerPlan&) noexcept;
    /** Runs one mixer of the block on whichever thread takes it (see MixerWorkers). */
    static void runMixerJob (void* engine, int index) noexcept;
    struct BlockJobs { Plan* plan = nullptr; const float* const* in = nullptr; int numIn = 0; float* const* out = nullptr; int numOut = 0, n = 0; bool playing = false; float* tapL = nullptr; float* tapR = nullptr;
                       float* mainL = nullptr; float* mainR = nullptr; };
    BlockJobs blockJobs;                            // audio thread: what the mixer jobs of this block work on
    std::atomic<juce::uint64> parallelBlocks { 0 };
    std::atomic<Plan*> lastPlanSeen { nullptr };    // audio thread: the plan of the previous block (see carryOver)
    HostTransport transport;                          // what the plug-ins are told (filled per block on the audio thread, or per offline render step)
    juce::int64 offlinePos = 0;                       // offline render: the timeline position the plug-ins are told
    MixerWorkers mixerWorkers;
    BackupCopier backupCopier;
    juce::File backupRoot;
    bool waitForAudioThread();                      // true once the audio thread has certainly left the old data; false if it stalled for half a second (then the old data is left alone rather than freed under it)
    void ensurePreRoll();                           // message thread: (re)makes the ring when session mode is on and nothing is recording
    void deliverFromRing (RecordingSession&, juce::int64 from, juce::int64 to);
    std::unique_ptr<Plan> buildPlan() const;
    juce::uint64 latencySignature (bool refresh) const;     // changes whenever any plug-in's delay (or whether it is loaded / bypassed) changes

    Project& project;
    double sampleRate = 48000.0;
    int maxBlock = 512;
    int deviceInputs = 0, deviceOutputs = 0;

    std::atomic<Plan*> plan { nullptr };
    std::atomic<juce::uint64> planLatencySig { 0 };
    std::atomic<RecordingSession*> rec { nullptr };
    std::atomic<PlaybackSession*> playback { nullptr };
    std::atomic<PreRoll*> preRollPtr { nullptr };
    std::atomic<bool> sessionMode { false };
    std::atomic<const MixerState*> auditionMixer { nullptr };
    juce::Uuid offlineMixerId;                       // offline rendering (see setOfflineTarget)
    std::vector<juce::Uuid> offlineTaps, offlinePreTaps;
    std::vector<juce::AudioBuffer<float>>* captureBufs = nullptr;  // only while renderOffline() runs
    std::unique_ptr<PlaybackSession> playSession;   // owned by the message thread
    std::atomic<juce::uint64> blockCounter { 0 };
    std::atomic<bool> running { false };
    std::atomic<juce::int64> samplePosition { 0 };
    std::atomic<float> inputPeak[kMaxInputs];
    std::atomic<float> outputPeak[kMaxInputs];

    std::atomic<double> loadBusy { 0.0 }, loadSpan { 0.0 };   // audio thread adds, message thread takes: seconds spent in the callback / seconds of audio
    std::atomic<float> loadPeakPct { 0.0f };
    std::atomic<int> loadBlocks { 0 }, loadLate { 0 };
    std::atomic<float> recBufPeakPct { 0.0f };

    std::atomic<const void*> scopeSource { nullptr };
    std::vector<float> scopeRing = std::vector<float> ((size_t) (2 * kScopeFrames), 0.0f);
    std::atomic<juce::int64> scopeWrite { 0 };

    LtcLive ltcIn; LtcGenerator ltcGen;
    std::atomic<int> ltcInCh { -1 }, ltcOutCh { -1 }, ltcOutFormat { 1 }, ltcOutRestart { 0 };
    std::atomic<double> ltcOutStart { 0.0 };
    std::atomic<bool> ltcOutRunning { false };
    int ltcSeenRestart = -1;                       // audio thread

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
    std::vector<float> mainCap;                     // audio thread: the processing mixer's main output, left then right, for the cue mixers that follow it
    std::unique_ptr<RecordingSession> session;      // owned by the message thread
    std::atomic<double> recBytesPerSecond { 0.0 };
    juce::File recFolder;
    juce::TimeSliceThread writerThread { "Disk writer" };
    juce::WavAudioFormat wav;
    juce::Uuid currentGroupId;

    juce::CriticalSection planLock;                 // serialises rebuildPlan() calls
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace td
