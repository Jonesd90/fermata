#pragma once
#include "Common.h"
#include "LowCut.h"
#include <algorithm>

namespace td
{
/** A processor placed in an insert slot (the VST3 wrapper implements this). */
class InsertProcessor
{
public:
    virtual ~InsertProcessor() = default;
    virtual void prepare (double sampleRate, int maxBlock, int numChannels) = 0;
    virtual void process (juce::AudioBuffer<float>&) = 0;   // audio thread
    virtual juce::String getName() const = 0;
    /** How many samples later this processor's output comes than its input (look-ahead, linear-phase filters ...). Valid after prepare(). */
    virtual int          getLatencySamples() const          { return 0; }
    virtual juce::String getDescription() const  { return {}; }       // for saving
    virtual juce::String getStateBase64() const  { return {}; }       // for saving
    /** For the automatic saves: the last state fetched, when nothing says the plug-in has changed since (it reported no parameter change,
        its window is closed, and it is less than a minute old); otherwise a fresh one. Asking a big plug-in (a convolution reverb) for its whole
        state can take a noticeable moment, and the automatic save asked every one of them every few seconds. */
    virtual juce::String getStateBase64Quick() const { return getStateBase64(); }

    // Automatable parameters (normalised 0..1). setParamNorm may be called from the audio thread (inside the slot's try-lock).
    virtual int          numParams() const                  { return 0; }
    virtual juce::String paramName (int) const              { return {}; }
    virtual bool         paramAutomatable (int) const       { return false; }
    virtual float        getParamNorm (int) const           { return 0.0f; }
    virtual void         setParamNorm (int, float)          {}
    /** How many positions the parameter has when it is a switch or a selector (2 = on / off, as a bypass; 3, 4 ... = a list): its normalised values are then
        0, 1 / (n - 1) ... 1 and nothing between. 0 = a smooth control (also one with more than 100 steps, which no one can tell from smooth). */
    virtual int          paramSteps (int) const             { return 0; }
    /** The plug-in's own words for a normalised value ("Off", "Bypassed", "Peak"), for the automation lanes; empty if it has none. */
    virtual juce::String paramText (int, float) const       { return {}; }
};

/** A fixed delay for a block of channels: used to line up the paths of the mixer when plug-ins delay some of them (delay compensation).
    Made (init) on the message thread when the plan is built; run() on the audio thread never allocates. */
struct PathDelay
{
    int len = 0, ch = 0, pos = 0;
    std::vector<float> data;
    void init (int samples, int channels)
    {
        len = juce::jlimit (0, 1 << 20, samples); ch = channels; pos = 0;
        data.assign ((size_t) len * (size_t) juce::jmax (0, ch), 0.0f);
    }
    void clear() noexcept { std::fill (data.begin(), data.end(), 0.0f); pos = 0; }
    void run (float* const* chans, int numCh, int n) noexcept
    {
        if (len <= 0) return;
        const int nc = juce::jmin (numCh, ch);
        for (int c = 0; c < nc; ++c)
        {
            float* d = data.data() + (size_t) c * (size_t) len;
            float* x = chans[c];
            int p = pos;
            for (int i = 0; i < n; ++i)
            {
                const float o = d[p]; d[p] = x[i]; x[i] = o;
                if (++p == len) p = 0;
            }
        }
        pos = (pos + n) % len;
    }
};

/** One insert slot. The audio thread only ever try-locks, so loading a plugin can never block audio. The message thread's own locks give up after
    two seconds (a plug-in stuck inside its process call would otherwise hang the program along with the audio): set / take / reprepare then do
    nothing and say so. */
class InsertSlot
{
public:
    static constexpr int kLockWaitMs = 2000;

    /** Puts p in the slot (prepared first, outside the lock). False if the slot could not be locked in time: p is dropped and the slot is unchanged. */
    bool set (std::unique_ptr<InsertProcessor> p, double sr, int maxBlock, int numCh) { return setKeeping (p, sr, maxBlock, numCh); }

    /** As set(), but a plug-in the slot could not take stays in p (so the caller can put it back where it came from) instead of being dropped. */
    bool setKeeping (std::unique_ptr<InsertProcessor>& p, double sr, int maxBlock, int numCh)
    {
        if (p != nullptr)
            p->prepare (sr, maxBlock, numCh);
        const int lat = p != nullptr ? juce::jmax (0, p->getLatencySamples()) : 0;
        std::unique_ptr<InsertProcessor> old;
        {
            TimedLock tl (lock);
            if (! tl.locked) { stuck(); return false; }
            old = std::move (proc);
            proc = std::move (p);
            latency.store (lat, std::memory_order_relaxed);
            loadedHint.store (proc != nullptr, std::memory_order_relaxed);
            savedDescription.clear();
            savedState.clear(); savedName.clear();
            scratchSize (proc != nullptr ? maxBlock : 0);
        }
        // 'old' is destroyed here, outside the lock
        return true;
    }

    bool clear (double sr, int maxBlock, int numCh) { return set (nullptr, sr, maxBlock, numCh); }

    /** Message thread: takes the plug-in out of the slot (keeping all its settings) so it can be put in another slot. The slot is left empty.
        Null (and the slot unchanged) if it could not be locked in time. */
    std::unique_ptr<InsertProcessor> take()
    {
        std::unique_ptr<InsertProcessor> p;
        {
            TimedLock tl (lock);
            if (! tl.locked) { stuck(); return nullptr; }
            p = std::move (proc);
            proc = nullptr;
            loadedHint.store (false, std::memory_order_relaxed);
            savedDescription.clear(); savedState.clear(); savedName.clear();
            latency.store (0, std::memory_order_relaxed);
        }
        return p;
    }

    /** The audio device (sample rate / buffer size) changed. Audio simply bypasses this slot meanwhile. */
    void reprepare (double sr, int maxBlock, int numCh)
    {
        TimedLock tl (lock);
        if (! tl.locked) { stuck(); return; }
        if (proc != nullptr) { proc->prepare (sr, maxBlock, numCh); latency.store (juce::jmax (0, proc->getLatencySamples()), std::memory_order_relaxed); scratchSize (maxBlock); }
    }

    /** Message thread: asks the plug-in again how late it is (some change it while they work). Only the message thread ever changes 'proc', so no lock is needed. */
    void refreshLatency() noexcept
    {
        if (auto* q = proc.get()) latency.store (juce::jmax (0, q->getLatencySamples()), std::memory_order_relaxed);
        else latency.store (0, std::memory_order_relaxed);
    }
    /** How many samples this slot delays the signal right now: 0 when empty or bypassed. */
    int latencySamples() const noexcept { return (loadedHint.load (std::memory_order_relaxed) && ! bypass.get()) ? latency.load (std::memory_order_relaxed) : 0; }

    /** Audio thread. A bypassed plug-in still hears the music: it is run on a copy of the block and the copy thrown away, so its reverb, its
        look-ahead and its meters stay in step and switching it back in is clean (it used to be left out altogether while bypassed, so the first
        moment after Bypass was switched off played whatever had been sitting in it: a burst of old reverb). */
    void process (juce::AudioBuffer<float>& b) noexcept
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked() || proc == nullptr) return;
        if (! bypass.get()) { proc->process (b); return; }
        const int nc = juce::jmin (b.getNumChannels(), scratch.getNumChannels()), n = b.getNumSamples();
        if (nc < 1 || n > scratch.getNumSamples()) return;              // (no room: the plug-in misses this block, as before)
        for (int c = 0; c < nc; ++c) scratch.copyFrom (c, 0, b, c, 0, n);
        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), nc, n);
        proc->process (view);
    }

    /** Audio thread: moves one plug-in parameter (0..1). Skipped if the slot is busy being changed: false then (the caller tries again), true when the
        plug-in was given the value. */
    bool setParamNorm (int index, float v) noexcept
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked() || proc == nullptr || index >= proc->numParams()) return false;
        proc->setParamNorm (index, v);
        return true;
    }

    /** Message thread only (like getProcessorUnsafe): it is the only thread that ever changes 'proc', so no lock is taken. (These used to take the
        lock, and the mixer window asked every slot its name three times a second: each time the audio thread's try-lock met it, the plug-in was
        skipped for a block and the sound went past it dry.) */
    bool isLoaded() const noexcept        { return proc != nullptr; }
    /** Lock-free hint for the audio thread: true if something is (or is about to be) loaded in this slot. */
    bool mightBeLoaded() const noexcept { return loadedHint.load (std::memory_order_relaxed); }
    juce::String getName() const { return proc != nullptr ? proc->getName() : juce::String(); }

    /** Message thread only. */
    InsertProcessor* getProcessorUnsafe() const noexcept { return proc.get(); }

    Flag bypass;
    /** How late (samples) the sound coming INTO this slot is, counted from the timeline, because of plug-ins before it on its way (delay
        compensation): written by the engine each time it builds its plan, read by the automation so a parameter move lands on the music drawn
        under it (see AutomationPlan). */
    std::atomic<int> autoLat { 0 };
    juce::String savedDescription, savedState;   // kept when a plugin could not be re-created
    juce::String savedName;                      // its name, for the slot to show as "<name> (not loaded)" (message thread; cleared with the two above)
    /** How many times a message-thread lock gave up because the audio thread never let go (a plug-in stuck in its process call). */
    static std::atomic<int>& stuckCount() noexcept { static std::atomic<int> n { 0 }; return n; }
private:
    /** The message thread's lock: waits up to kLockWaitMs for the audio thread's try-lock to let go. */
    struct TimedLock
    {
        explicit TimedLock (juce::SpinLock& l) : lk (l)
        {
            const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) kLockWaitMs;
            while (! (locked = lk.tryEnter()) && juce::Time::getMillisecondCounter() < until) juce::Thread::sleep (1);
        }
        ~TimedLock() { if (locked) lk.exit(); }
        juce::SpinLock& lk; bool locked = false;
    };
    void stuck() { stuckCount().fetch_add (1); juce::Logger::writeToLog ("A plug-in did not let go of its insert slot for " + juce::String (kLockWaitMs) + " ms (stuck in its process call?): the slot was left as it was"); }
    void scratchSize (int maxBlock) { if (maxBlock <= 0) scratch.setSize (0, 0); else if (scratch.getNumChannels() < kMaxTrackChannels || scratch.getNumSamples() < maxBlock) scratch.setSize (kMaxTrackChannels, maxBlock, false, false, true); }
    mutable juce::SpinLock lock;
    std::unique_ptr<InsertProcessor> proc;
    juce::AudioBuffer<float> scratch;             // the copy a bypassed plug-in is run on (sized on the message thread, in set / reprepare)
    std::atomic<bool> loadedHint { false };
    std::atomic<int> latency { 0 };
};

/** One send: a copy of a channel's signal going to another channel of the same mixer (an audio track, an internal bus or an external bus).
    Turned by a dial on the strip; can be taken before or after the fader and can have up to two VST3 inserts in its own path
    (they never touch the signal that goes through the channel itself). */
struct SendState
{
    juce::Uuid dest;                  // the audio track / bus it goes to (message thread)
    Param gainDb { -100.0f };         // <= kSendOffDb means off
    Flag  pre;                        // true: taken before the channel fader
    Flag  followMute { true };        // true: muting the channel mutes this send too (pre or post fader); false: the send carries on when the channel is muted
    Flag  mute;                       // the send's own MUTE (in its panel): silences this send only, whatever its level; the level is kept
    InsertSlot slots[kSendSlots];
    float lastGain = 0.0f;            // audio thread only
    float lastFeed = 0.0f;            // audio thread only: the gain of a send that does not follow mute, before its own level (fader or 1), last block
    std::atomic<int> autoLat { 0 };   // how late the sound is where this send's level is applied (after its own inserts and delay line): see InsertSlot::autoLat
    /** An edit's automation moves this send's level while it plays: it is kept in the engine's plan even while its level is off, so the automation
        can bring it in (a send was only connected when it was on as the plan was built, so one faded up from off by a lane was never heard).
        Set and cleared by AutomationPlan::begin / end, message thread. */
    std::atomic<bool> autoHeld { false };
    bool active() const noexcept      { return gainDb.get() > kSendOffDb; }
    bool inPlan() const noexcept      { return active() || autoHeld.load (std::memory_order_relaxed); }
};

/** The sends of one channel, by destination. Entries are never removed while the project is open, so the audio thread can hold raw pointers. */
struct SendList
{
    std::vector<std::unique_ptr<SendState>> pool;
    SendState* find (const juce::Uuid& dest) const
    {
        for (auto& s : pool) if (s->dest == dest) return s.get();
        return nullptr;
    }
    SendState* get (const juce::Uuid& dest)
    {
        if (auto* s = find (dest)) return s;
        pool.push_back (std::make_unique<SendState>());
        pool.back()->dest = dest;
        return pool.back().get();
    }
};

/** An Audio Track as it appears in one mixer. */
struct StripState
{
    juce::Uuid trackId;
    Param inGainDb { 0.0f };          // the level going into the strip, before the inserts and the fader (automatable as "gain")
    Param gainDb { 0.0f };
    Param pan { 0.0f };               // mono strips: -1 (left) .. +1 (right)
    Param panL { -1.0f }, panR { 1.0f };   // stereo strips: where the left and the right channel of the track are placed, each -1 .. +1
    Flag  mute, solo;
    Flag  outOn { false };             // the strip's output (after the fader) is connected to outDest
    juce::Uuid outDest = juce::Uuid::null();               // an Int Bus or Ext Bus of this mixer; null = the mixer's main Ext Bus
    SendState outSend;                // how the output is carried (always at unity, after the fader)
    SendList sends;
    LowCut lowCut;                    // the LC key: a high-pass before the inserts (zero latency; linear phase when asked for - see LowCut.h)
    InsertSlot slots[kNumSlots];
    Param meterL, meterR;             // peak since last read (UI resets)
    Param rmsL, rmsR;                 // RMS level (linear, about 300 ms window), updated every block
    std::atomic<juce::uint32> vcaMask { 0 };   // bit n set: this strip is controlled by VCA number n+1 (of this mixer)
    Param livePkL, livePkR;           // peak since the edit window last looked (the edit window's own meters: quick, like the mixer's, and independent of them)
    float lastGain = 0.0f, lastPre = 0.0f, msL = 0.0f, msR = 0.0f;   // audio thread only
    float lastIn = 1.0f;              // audio thread only: the input gain of the previous block (for a smooth change)
    std::atomic<int> autoLatIn { 0 }, autoLatOut { 0 };    // how late the sound is before its inserts (input gain) and after them (pan, fader): see InsertSlot::autoLat
    /** While an edit's automation drives a VCA that controls this strip: the VCA's level as it should be for THIS strip's sound (read at this
        strip's own lateness: see AutomationPlan), used instead of the VCA's own fader for the VCAs whose bit is in vcaAutoMask. */
    Param vcaAutoDb[kMaxVcas];
    std::atomic<juce::uint32> vcaAutoMask { 0 };
    StripState() { outSend.gainDb.set (0.0f); }
};

/** An Int Bus or an Ext Bus as it appears in one mixer. */
struct BusState
{
    juce::Uuid busId;
    Param inGainDb { 0.0f };          // the level going into the bus, before its inserts and fader (automatable "gain")
    Param pan { 0.0f };               // balance of the bus's stereo pair: -1 (left) .. +1 (right); 0 = untouched (automatable "pan")
    float lastIn = 1.0f;              // audio thread only
    Param gainDb { 0.0f };
    Flag  mute, solo;                 // solo: Int Bus only (soloing a bus keeps everything that feeds it open)
    std::atomic<int> outFirst { -1 }; // Ext Bus only: first driver output of its stereo pair (-1 = goes nowhere)
    Flag  isMain { false };           // Ext Bus only: one of this mixer's MAIN outputs. Strips whose output is 'Main' go to every Ext Bus ticked so (several may be),
                                      // which also play whatever is sent to them. None ticked: 'Main' goes nowhere.
    Flag  outOn { false };             // Int Bus only: its output (after the fader) is connected to outDest
    juce::Uuid outDest = juce::Uuid::null();               // Int Bus only: an Int Bus or Ext Bus of this mixer; null = the mixer's main Ext Bus
    SendState outSend;
    SendList sends;                   // Int Bus only
    LowCut lowCut;                    // Int Bus only: the LC key, before the inserts
    InsertSlot slots[kNumSlots];
    Param meterL, meterR;
    Param rmsL, rmsR;
    Param remotePkL, remotePkR;       // Ext Bus: peak since the control port (Stream Deck dials) last looked - its own meter, independent of the mixer window's
    std::atomic<juce::uint32> vcaMask { 0 };   // Int Bus: bit n set = controlled by VCA number n+1 (an Ext Bus is never controlled)
    float lastGain = 0.0f, lastPre = 0.0f, msL = 0.0f, msR = 0.0f;
    std::atomic<int> autoLatIn { 0 }, autoLatOut { 0 };    // see StripState
    Param vcaAutoDb[kMaxVcas];                // see StripState
    std::atomic<juce::uint32> vcaAutoMask { 0 };
    BusState() { outSend.gainDb.set (0.0f); }
};

/** A VCA fader as it appears in one mixer: it carries no audio. Its fader (the only automatable thing) adds its gain to every strip it controls
    (the strips keep their own faders), its mute silences them and its solo keeps them open like a solo on each of them. Which strips it controls is in their vcaMask. */
struct VcaState
{
    Param gainDb { 0.0f };
    Flag  mute, solo;
    std::atomic<int> autoLat { 0 };   // the latest of the channels it controls, at their faders (what its own fader shows while automated; each channel gets its own value: StripState::vcaAutoDb)
};

/** One of the project's independent mixers. All solo/mute/level/routing state lives here, so
    nothing done on one mixer can ever affect another. */
struct MixerState
{
    juce::Uuid id;
    juce::String name { "Mixer" };
    juce::Uuid editId = juce::Uuid::null();                // set: this is the mixer of one Edit (it plays only that Edit, and is made, named and removed with it)
    juce::Uuid mainBus = juce::Uuid::null();               // (projects before 2.65 only: the one main Ext Bus, null = the first; now each BusState has isMain)
    Flag followProcessing { false };                       // a cue mixer only: its own mix rests (kept as it is) and its Ext buses play the processing mixer's main output
    std::atomic<bool> monoCheck { false };                 // audition switch (not saved): the Ext bus outputs carry (L+R)/2 on both sides, to hear mono-compatibility problems
    std::atomic<int> ditherBits { 24 };                    // dither on this mixer's driver outputs: 0 = off, 24 or 16 bit TPDF (what the converter / next device keeps)

    // Pools are never shrunk while the project is open, so the audio thread can hold raw pointers.
    std::vector<std::unique_ptr<StripState>> stripPool;
    std::vector<std::unique_ptr<BusState>>    busPool;
    VcaState vcas[kMaxVcas];                                // by VCA number - 1
    std::atomic<juce::uint32> vcaShown { 0 };               // bit n set: the strip of VCA number n+1 is in this mixer (the processing mixer gets the VCAs of the design, other mixers add them with their + button)
    /** Audio tracks taken out of THIS mixer (right-click a strip's name plate: Hide): no strip in its window, and nothing of the track reaches this
        mixer's buses or outputs (the engine leaves its output and sends out of the plan). Its levels and plug-ins are kept for when it is shown
        again (the + button). Message thread only. */
    std::vector<juce::Uuid> hiddenStrips;
    bool isStripHidden (const juce::Uuid& trackId) const { return std::find (hiddenStrips.begin(), hiddenStrips.end(), trackId) != hiddenStrips.end(); }

    /** Audio thread: what the VCAs in 'mask' do to a strip: their gains multiplied together, or 0 when one of them is muted. For the VCAs in
        'ownMask' the strip's own automated value (own[i]) is used instead of the VCA's fader. */
    float vcaGain (juce::uint32 mask, const Param* own = nullptr, juce::uint32 ownMask = 0) const noexcept
    {
        if (mask == 0) return 1.0f;
        float g = 1.0f;
        for (int i = 0; i < kMaxVcas; ++i)
            if (mask & (1u << i))
            {
                if (vcas[i].mute.get()) return 0.0f;
                g *= dbToGain ((own != nullptr && (ownMask & (1u << i)) != 0) ? own[i].get() : vcas[i].gainDb.get());
            }
        return g;
    }
    /** Audio thread: the VCAs that are soloed, as bits. */
    juce::uint32 vcaSoloMask() const noexcept
    {
        juce::uint32 m = 0;
        for (int i = 0; i < kMaxVcas; ++i) if (vcas[i].solo.get()) m |= 1u << i;
        return m;
    }

    StripState* stripFor (const juce::Uuid& trackId)
    {
        for (auto& s : stripPool) if (s->trackId == trackId) return s.get();
        stripPool.push_back (std::make_unique<StripState>());
        stripPool.back()->trackId = trackId;
        return stripPool.back().get();
    }
    BusState* busFor (const juce::Uuid& busId)
    {
        for (auto& f : busPool) if (f->busId == busId) return f.get();
        busPool.push_back (std::make_unique<BusState>());
        busPool.back()->busId = busId;
        return busPool.back().get();
    }
};
} // namespace td
