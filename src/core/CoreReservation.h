#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>

namespace td
{
/** Reserved CPU cores ("ring-fencing", in the spirit of Pyramix MassCore, but done the way Windows allows a normal program to do it).

    What it does (Windows 10 and later, through "CPU sets"):
      - the audio engine's threads with a deadline every block (the driver's audio callback and the mixer helpers) are placed on the reserved cores only;
        the disk writer and the playback reader, which work seconds ahead, run on the other cores at high priority, out of their way;
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

/** 'limitFastCores' (3.13.0): on a CPU with big (fast) and small (slow) cores, at most half of the big ones are kept for the audio - the others are left
    to Windows, the rest of Fermata and the other programs, whatever is ticked (a laptop CPU with two fast cores keeps one of them for the audio). Without
    this, a laptop with two fast cores had both taken away from everything else, which made the whole PC painfully slow. 'false' only to work out what
    an older version did (to set its programs free again: see CoreReservation::releaseLeftovers). */
CorePlan makeCorePlan (const std::vector<CpuCore>& cores, const CoreReservationSettings& s, bool limitFastCores = true);

/** True when the cores are not all alike (big and small cores: Intel 12th generation and later, Core Ultra). */
bool isHybridCpu (const std::vector<CpuCore>& cores);
/** How many of a hybrid CPU's big cores may be kept for the audio: half of them, rounded down (2 -> 1, 4 -> 2, 6 -> 3). Every core, when the CPU is not hybrid. */
int maxFastCoresForAudio (const std::vector<CpuCore>& cores);

/** The 'count' quietest physical cores, from the busy-ness (percent of time in interrupts and DPCs) of each logical processor. A core is as noisy as its noisiest thread; ties go to the higher core number
    (Windows tends to put its own work on the low numbers). */
std::vector<int> pickQuietCores (const std::vector<CpuCore>& cores, const std::vector<double>& loadOfLogical, int count);
/** (3.13.0) On a hybrid CPU it picks big cores only, and no more than maxFastCoresForAudio (so a laptop with two big cores gets one): fewer than 'count'
    is then the right answer, as the small cores are too slow for the audio. */
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

    /** The top of the audio callback (and of a mixer helper's work). Cheap (one atomic read) unless the settings have just changed, or it is the first
        block on this thread: then the thread is moved to the reserved cores (when any) and joins Windows' "Pro Audio" scheduling class (always: a helper
        that is not in it can be pre-empted by any busy program while the audio thread waits for it). */
    void audioThreadCheck() noexcept;
    /** How many logical processors are reserved for the audio right now (0 = none: the reservation is off or not in force). Any thread. */
    int reservedLogicalProcessors() const noexcept { return audioLogicalCount.load (std::memory_order_relaxed); }
    /** A thread of the audio engine that is not the callback (a mixer helper). Call after it has started; unregister before it is deleted. */
    void registerAudioThread (juce::Thread::ThreadID id);
    void unregisterAudioThread (juce::Thread::ThreadID id);

    /** Blocking (sample time in milliseconds, e.g. 3000): the percent of time each logical processor spent in interrupts and DPCs. Empty if it cannot be measured. Call from a background thread. */
    std::vector<double> measureInterruptLoad (int milliseconds) const;

    /** Puts everything back as it was (the programs that were moved, this program's own settings). Called when Fermata closes. */
    void shutdown();

    /** (3.13.0) Call once at start-up, before configure(). A Fermata that crashed or was ended from the Task Manager while it kept the other programs
        off its cores could not set them free again, and Windows does not do it either: they stayed on the remaining cores until they were restarted.
        This sets them free: the programs that still have exactly the cores a Fermata gave them (written down in "cpu-fence.txt" while it runs, and -
        for versions before 3.13.0, which did not write it down - the cores the saved settings would have given them). Returns how many were set free. */
    int releaseLeftovers (const CoreReservationSettings& saved);

    /** True on a PC with a battery (a laptop). */
    static bool isLaptop() noexcept;
    /** "12 cores (2 big, 10 small), laptop, on battery" - for the log. */
    juce::String describeMachine() const;

    /** The calling thread's disk and CPU work gets Windows' "background" priority (the copy to the second recording drive), so the main drive's
        writes always come first. Does nothing on other systems. */
    static void makeThisThreadBackground() noexcept;

private:
    CoreReservation();
    ~CoreReservation();
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> isActive { false };
    std::atomic<int> audioLogicalCount { 0 };
};
/** What the PC and Fermata were doing with the processor over a stretch of time (3.13.0, for the stall watch and the timing lines): take a snapshot
    before and after, and describe the difference. Windows only; elsewhere the snapshots are empty and the description is too. */
struct SystemLoad
{
    struct Snapshot
    {
        bool ok = false;
        double atMs = 0.0;                                  // juce::Time::getMillisecondCounterHiRes
        unsigned long long pcIdle = 0, pcTotal = 0;         // the whole PC, all cores together (100 ns units)
        unsigned long long process = 0;                     // Fermata, all its threads
        unsigned long long mainThread = 0;                  // Fermata's main (message) thread
        std::vector<std::pair<unsigned long, unsigned long long>> threads;   // (take (true) only) each of Fermata's threads: its id and its working time
    };
    /** Call once on the message thread, so that its working time can be read from other threads. */
    static void rememberMessageThread();
    /** Any thread. 'withThreads' (3.15.0, the timing lines): also each of Fermata's threads, so describe() can name the busiest. */
    static Snapshot take (bool withThreads = false);
    /** "main thread working 95% of it, Fermata using 1.2 cores, whole PC 88% busy, on battery", plus a few words on what that means. Empty if unknown.
        When both snapshots have the threads (3.15.0): the main thread's share too, and the busiest threads by name ("busiest: Mixer helper 2 98%,
        main thread 12%") - every thread using a tenth of a core or more, at most four. */
    static juce::String describe (const Snapshot& before, const Snapshot& after, bool forStall);
    /** "on battery" / "on mains" (+ ", battery saver on"); empty on a desktop or when unknown. */
    static juce::String powerText();
};
} // namespace td
