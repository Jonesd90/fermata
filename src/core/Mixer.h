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
    virtual juce::String getDescription() const  { return {}; }       // for saving
    virtual juce::String getStateBase64() const  { return {}; }       // for saving

    // Automatable parameters (normalised 0..1). setParamNorm may be called from the audio thread (inside the slot's try-lock).
    virtual int          numParams() const                  { return 0; }
    virtual juce::String paramName (int) const              { return {}; }
    virtual bool         paramAutomatable (int) const       { return false; }
    virtual float        getParamNorm (int) const           { return 0.0f; }
    virtual void         setParamNorm (int, float)          {}
};

/** One insert slot. The audio thread only ever try-locks, so loading a plugin can never block audio. */
class InsertSlot
{
public:
    void set (std::unique_ptr<InsertProcessor> p, double sr, int maxBlock, int numCh)
    {
        if (p != nullptr)
            p->prepare (sr, maxBlock, numCh);
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

    /** The audio device (sample rate / buffer size) changed. Audio simply bypasses this slot meanwhile. */
    void reprepare (double sr, int maxBlock, int numCh)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (proc != nullptr) proc->prepare (sr, maxBlock, numCh);
    }

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
};

/** One send: a copy of a channel's signal going to another channel of the same mixer (an audio track, an internal bus or an external bus).
    Turned by a dial on the strip; can be taken before or after the fader and can have up to two VST3 inserts in its own path
    (they never touch the signal that goes through the channel itself). */
struct SendState
{
    juce::Uuid dest;                  // the audio track / bus it goes to (message thread)
    Param gainDb { -100.0f };         // <= kSendOffDb means off
    Flag  pre;                        // true: taken before the channel fader
    InsertSlot slots[kSendSlots];
    float lastGain = 0.0f;            // audio thread only
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
    float lastGain = 0.0f, lastPre = 0.0f, msL = 0.0f, msR = 0.0f;   // audio thread only
    StripState() { outSend.gainDb.set (0.0f); }
};

/** An Int Bus or an Ext Bus as it appears in one mixer. */
struct BusState
{
    juce::Uuid busId;
    Param gainDb { 0.0f };
    Flag  mute;
    std::atomic<int> outFirst { -1 }; // Ext Bus only: first driver output of its stereo pair (-1 = goes nowhere)
    Flag  outOn { false };             // Int Bus only: its output (after the fader) is connected to outDest
    juce::Uuid outDest = juce::Uuid::null();               // Int Bus only: an Int Bus or Ext Bus of this mixer; null = the mixer's main Ext Bus
    SendState outSend;
    SendList sends;                   // Int Bus only
    InsertSlot slots[kNumSlots];
    Param meterL, meterR;
    Param rmsL, rmsR;
    float lastGain = 0.0f, lastPre = 0.0f, msL = 0.0f, msR = 0.0f;
    BusState() { outSend.gainDb.set (0.0f); }
};

/** One of the project's independent mixers. All solo/mute/level/routing state lives here, so
    nothing done on one mixer can ever affect another. */
struct MixerState
{
    juce::Uuid id;
    juce::String name { "Mixer" };
    juce::Uuid mainBus = juce::Uuid::null();               // the Ext Bus designated as this mixer's main output (null or not found = the first Ext Bus)
    std::atomic<bool> monoCheck { false };                 // audition switch (not saved): the Ext bus outputs carry (L+R)/2 on both sides, to hear mono-compatibility problems
    std::atomic<int> ditherBits { 24 };                    // dither on this mixer's driver outputs: 0 = off, 24 or 16 bit TPDF (what the converter / next device keeps)

    // Pools are never shrunk while the project is open, so the audio thread can hold raw pointers.
    std::vector<std::unique_ptr<StripState>> stripPool;
    std::vector<std::unique_ptr<BusState>>    busPool;

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
