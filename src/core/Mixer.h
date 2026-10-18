#pragma once
#include "Common.h"

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

    // Automatable parameters (normalised 0..1). setParamNorm may be called from the audio thread (inside the slot's try-lock).
    virtual int          numParams() const                  { return 0; }
    virtual juce::String paramName (int) const              { return {}; }
    virtual bool         paramAutomatable (int) const       { return false; }
    virtual float        getParamNorm (int) const           { return 0.0f; }
    virtual void         setParamNorm (int, float)          {}
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

/** One insert slot. The audio thread only ever try-locks, so loading a plugin can never block audio. */
class InsertSlot
{
public:
    void set (std::unique_ptr<InsertProcessor> p, double sr, int maxBlock, int numCh)
    {
        if (p != nullptr)
            p->prepare (sr, maxBlock, numCh);
        latency.store (p != nullptr ? juce::jmax (0, p->getLatencySamples()) : 0, std::memory_order_relaxed);
        std::unique_ptr<InsertProcessor> old;
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            old = std::move (proc);
            proc = std::move (p);
            loadedHint.store (proc != nullptr, std::memory_order_relaxed);
            savedDescription.clear();
            savedState.clear();
        }
        // 'old' is destroyed here, outside the lock
    }

    void clear (double sr, int maxBlock, int numCh) { set (nullptr, sr, maxBlock, numCh); }

    /** Message thread: takes the plug-in out of the slot (keeping all its settings) so it can be put in another slot. The slot is left empty. */
    std::unique_ptr<InsertProcessor> take()
    {
        std::unique_ptr<InsertProcessor> p;
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            p = std::move (proc);
            proc = nullptr;
            loadedHint.store (false, std::memory_order_relaxed);
            savedDescription.clear(); savedState.clear();
        }
        latency.store (0, std::memory_order_relaxed);
        return p;
    }

    /** The audio device (sample rate / buffer size) changed. Audio simply bypasses this slot meanwhile. */
    void reprepare (double sr, int maxBlock, int numCh)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (proc != nullptr) { proc->prepare (sr, maxBlock, numCh); latency.store (juce::jmax (0, proc->getLatencySamples()), std::memory_order_relaxed); }
    }

    /** Message thread: asks the plug-in again how late it is (some change it while they work). Only the message thread ever changes 'proc', so no lock is needed. */
    void refreshLatency() noexcept
    {
        if (auto* q = proc.get()) latency.store (juce::jmax (0, q->getLatencySamples()), std::memory_order_relaxed);
        else latency.store (0, std::memory_order_relaxed);
    }
    /** How many samples this slot delays the signal right now: 0 when empty or bypassed. */
    int latencySamples() const noexcept { return (loadedHint.load (std::memory_order_relaxed) && ! bypass.get()) ? latency.load (std::memory_order_relaxed) : 0; }

    void process (juce::AudioBuffer<float>& b) noexcept
    {
        if (bypass.get())
            return;
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (sl.isLocked() && proc != nullptr)
            proc->process (b);
    }

    /** Audio thread: moves one plug-in parameter (0..1). Skipped if the slot is busy being changed. */
    void setParamNorm (int index, float v) noexcept
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (sl.isLocked() && proc != nullptr && index < proc->numParams()) proc->setParamNorm (index, v);
    }

    bool isLoaded() const        { const juce::SpinLock::ScopedLockType sl (lock); return proc != nullptr; }
    /** Lock-free hint for the audio thread: true if something is (or is about to be) loaded in this slot. */
    bool mightBeLoaded() const noexcept { return loadedHint.load (std::memory_order_relaxed); }
    juce::String getName() const { const juce::SpinLock::ScopedLockType sl (lock); return proc != nullptr ? proc->getName() : juce::String(); }

    /** Message thread only. */
    InsertProcessor* getProcessorUnsafe() const noexcept { return proc.get(); }

    Flag bypass;
    juce::String savedDescription, savedState;   // kept when a plugin could not be re-created
private:
    mutable juce::SpinLock lock;
    std::unique_ptr<InsertProcessor> proc;
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
    InsertSlot slots[kSendSlots];
    float lastGain = 0.0f;            // audio thread only
    float lastFeed = 0.0f;            // audio thread only: the gain of a send that does not follow mute, before its own level (fader or 1), last block
    bool active() const noexcept      { return gainDb.get() > kSendOffDb; }
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
    InsertSlot slots[kNumSlots];
    Param meterL, meterR;             // peak since last read (UI resets)
    Param rmsL, rmsR;                 // RMS level (linear, about 300 ms window), updated every block
    std::atomic<juce::uint32> vcaMask { 0 };   // bit n set: this strip is controlled by VCA number n+1 (of this mixer)
    Param livePkL, livePkR;           // peak since the edit window last looked (the edit window's own meters: quick, like the mixer's, and independent of them)
    float lastGain = 0.0f, lastPre = 0.0f, msL = 0.0f, msR = 0.0f;   // audio thread only
    float lastIn = 1.0f;              // audio thread only: the input gain of the previous block (for a smooth change)
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
    InsertSlot slots[kNumSlots];
    Param meterL, meterR;
    Param rmsL, rmsR;
    std::atomic<juce::uint32> vcaMask { 0 };   // Int Bus: bit n set = controlled by VCA number n+1 (an Ext Bus is never controlled)
    float lastGain = 0.0f, lastPre = 0.0f, msL = 0.0f, msR = 0.0f;
    BusState() { outSend.gainDb.set (0.0f); }
};

/** A VCA fader as it appears in one mixer: it carries no audio. Its fader (the only automatable thing) adds its gain to every strip it controls
    (the strips keep their own faders), its mute silences them and its solo keeps them open like a solo on each of them. Which strips it controls is in their vcaMask. */
struct VcaState
{
    Param gainDb { 0.0f };
    Flag  mute, solo;
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

    /** Audio thread: what the VCAs in 'mask' do to a strip: their gains multiplied together, or 0 when one of them is muted. */
    float vcaGain (juce::uint32 mask) const noexcept
    {
        if (mask == 0) return 1.0f;
        float g = 1.0f;
        for (int i = 0; i < kMaxVcas; ++i)
            if (mask & (1u << i)) { if (vcas[i].mute.get()) return 0.0f; g *= dbToGain (vcas[i].gainDb.get()); }
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
