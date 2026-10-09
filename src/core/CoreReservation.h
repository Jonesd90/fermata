#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>

namespace td
{
/** Reserved CPU cores ("ring-fencing", in the spirit of Pyramix MassCore, but done the way Windows allows a normal program to do it).

    What it does (Windows 10 and later, through "CPU sets"):
      - the audio engine's own threads (the driver's audio callback, the disk writer, the playback readers) are placed on the reserved cores only;
      - everything else in Fermata (windows, plug-in scanning, exports ...) is kept on the other cores;
      - optionally, the other programs on the PC are asked to stay off the reserved cores as well (what Process Lasso does);
      - the audio callback joins the Windows "Pro Audio" scheduling class (MMCSS), and Fermata is told not to be throttled for power saving.

    What it cannot do: Windows kernel work (DPCs and interrupts from drivers such as network, USB, graphics, Wi-Fi) runs on whichever core the driver's interrupt was sent to, whatever any program asks.
    That is why the cores are chosen by measurement (quietest first), and why the cores that handle the most interrupts are best left to Windows.

    The planning (which cores are the audio cores, which the rest) is plain code and is tested everywhere; the Windows calls are in CoreReservation.cpp. On other systems nothing is changed. */

/** One physical core and the logical processors (hyper-threads) that belong to it. */
struct CpuCore
{
    int number = 0;                    // 0, 1, 2 ... in the order Windows lists them
    std::vector<int> logical;          // the logical processor numbers on this core (one, or two with hyper-threading)
    int efficiency = 0;                // Windows' efficiency class: on a CPU with big and small cores the big ones have the higher number
};

struct CoreReservationSettings
{
    bool enabled = false;
    bool keepOthersOff = false;        // also ask the other programs to stay off the reserved cores
    std::vector<int> reserved;         // physical core numbers

    juce::String reservedText() const;                       // "2,3"
    void setReservedText (const juce::String& text);
    bool operator== (const CoreReservationSettings& o) const { return enabled == o.enabled && keepOthersOff == o.keepOthersOff && reserved == o.reserved; }
};

/** Which logical processors are for the audio, which for the rest. */
struct CorePlan
{
    bool valid = false;
    juce::String problem;              // why it is not valid, in words the user can act on
    juce::String warning;              // valid, but worth saying
    std::vector<int> audioCores, generalCores;          // physical core numbers
    std::vector<int> audioLogical, generalLogical;      // logical processor numbers
};

CorePlan makeCorePlan (const std::vector<CpuCore>& cores, const CoreReservationSettings& s);

/** The 'count' quietest physical cores, from the busy-ness (percent of time in interrupts and DPCs) of each logical processor. A core is as noisy as its noisiest thread; ties go to the higher core number
    (Windows tends to put its own work on the low numbers). */
std::vector<int> pickQuietCores (const std::vector<CpuCore>& cores, const std::vector<double>& loadOfLogical, int count);
/** How many cores are sensible to reserve on a PC with this many physical cores. */
int suggestedReserveCount (int physicalCores);

class CoreReservation
{
public:
    static CoreReservation& get() noexcept;

    /** True on Windows 10 or later (CPU sets are available); false elsewhere. */
    bool supported() const noexcept;
    /** The physical cores of this PC (empty if not supported). */
    const std::vector<CpuCore>& cores() const noexcept;

    /** Message thread. Puts the settings into effect (or takes them away again when not enabled). Safe to call again with new settings. */
    void configure (const CoreReservationSettings&);
    CoreReservationSettings settings() const;
    bool active() const noexcept { return isActive.load(); }
    /** A sentence for the user: what is in force now, or why nothing is. */
    juce::String statusText() const;

    /** The top of the audio callback. Cheap (one atomic read) unless the settings have just changed, or it is the first block on this thread. */
    void audioThreadCheck() noexcept;
    /** A thread of the audio engine that is not the callback (disk writer, playback reader). Call after it has started; unregister before it is deleted. */
    void registerAudioThread (juce::Thread::ThreadID id);
    void unregisterAudioThread (juce::Thread::ThreadID id);

    /** Blocking (sample time in milliseconds, e.g. 3000): the percent of time each logical processor spent in interrupts and DPCs. Empty if it cannot be measured. Call from a background thread. */
    std::vector<double> measureInterruptLoad (int milliseconds) const;

    /** Puts everything back as it was (the programs that were moved, this program's own settings). Called when Fermata closes. */
    void shutdown();

private:
    CoreReservation();
    ~CoreReservation();
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> isActive { false };
};
} // namespace td
