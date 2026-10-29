#include "CoreReservation.h"
#include "CrashReport.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

// The Windows part is compiled on Windows only (the planning above it everywhere). FERMATA_CPU_WINDOWS_STUBS lets the development check compile it against stand-in headers.
#if JUCE_WINDOWS || defined (FERMATA_CPU_WINDOWS_STUBS)
 #define FERMATA_CPU_WINDOWS 1
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <tlhelp32.h>
#else
 #define FERMATA_CPU_WINDOWS 0
#endif

namespace td
{
// ======================================================================================================= planning (everywhere)
juce::String CoreReservationSettings::reservedText() const
{
    juce::StringArray a;
    for (int c : reserved) a.add (juce::String (c));
    return a.joinIntoString (",");
}

void CoreReservationSettings::setReservedText (const juce::String& text)
{
    std::set<int> s;
    for (auto& t : juce::StringArray::fromTokens (text, ",; ", ""))
        if (t.containsOnly ("0123456789") && t.isNotEmpty()) s.insert (t.getIntValue());
    reserved.assign (s.begin(), s.end());
}

static bool contains (const std::vector<int>& v, int x) { return std::find (v.begin(), v.end(), x) != v.end(); }

bool isHybridCpu (const std::vector<CpuCore>& cores)
{
    for (auto& c : cores) if (c.efficiency != cores.front().efficiency) return true;
    return false;
}

static int bestEfficiency (const std::vector<CpuCore>& cores)
{
    int best = 0;
    for (auto& c : cores) best = std::max (best, c.efficiency);
    return best;
}

int maxFastCoresForAudio (const std::vector<CpuCore>& cores)
{
    if (! isHybridCpu (cores)) return (int) cores.size();
    const int best = bestEfficiency (cores);
    int fast = 0;
    for (auto& c : cores) if (c.efficiency == best) ++fast;
    return fast / 2;
}

CorePlan makeCorePlan (const std::vector<CpuCore>& cores, const CoreReservationSettings& s, bool limitFastCores)
{
    CorePlan p;
    if (cores.size() < 2) { p.problem = "This PC has only one CPU core, so there is nothing to keep apart."; return p; }
    // the big cores that may be kept for the audio (3.13.0): on a hybrid CPU at most half of them, the higher-numbered first (Windows tends to put its own work on the low ones)
    std::vector<int> ticked;
    for (auto& c : cores) if (contains (s.reserved, c.number)) ticked.push_back (c.number);
    std::vector<int> leftFast;
    if (limitFastCores && isHybridCpu (cores))
    {
        const int best = bestEfficiency (cores), allowed = maxFastCoresForAudio (cores);
        std::vector<int> fastTicked;
        for (auto& c : cores) if (c.efficiency == best && contains (ticked, c.number)) fastTicked.push_back (c.number);
        std::sort (fastTicked.begin(), fastTicked.end(), [] (int a, int b) { return a > b; });
        for (size_t i = (size_t) std::max (0, allowed); i < fastTicked.size(); ++i) leftFast.push_back (fastTicked[i]);
        std::sort (leftFast.begin(), leftFast.end());
    }
    for (auto& c : cores)
    {
        if (contains (ticked, c.number) && ! contains (leftFast, c.number)) { p.audioCores.push_back (c.number); p.audioLogical.insert (p.audioLogical.end(), c.logical.begin(), c.logical.end()); }
        else                                                                { p.generalCores.push_back (c.number); p.generalLogical.insert (p.generalLogical.end(), c.logical.begin(), c.logical.end()); }
    }
    if (ticked.empty())         { p.problem = "No core is ticked: tick at least one core to keep for the audio."; return p; }
    if (p.audioCores.empty())   { p.problem = "This CPU has too few big (fast) cores to keep one for the audio: Windows and the other programs need them. Leave this off."; return p; }
    if (p.generalCores.empty()) { p.problem = "Every core is ticked: leave at least one core for Windows and the rest of the program."; return p; }
    p.valid = true;
    if (! leftFast.empty())
    {
        juce::StringArray a; for (int c : leftFast) a.add (juce::String (c));
        p.warning = "This CPU has few big (fast) cores, so at most " + juce::String (maxFastCoresForAudio (cores)) + " of them can be kept for the audio: core"
                    + (a.size() == 1 ? " " : "s ") + a.joinIntoString (", ") + (a.size() == 1 ? " is" : " are") + " left to Windows and the other programs, or the whole PC would crawl.";
    }
    else if (p.generalCores.size() < 2 && cores.size() >= 4)
        p.warning = "Only one core is left for Windows and everything else. The program and the PC may feel slow; reserve fewer cores if they do.";
    else if (p.audioCores.size() * 2 > cores.size() + 1)
        p.warning = "More than half of the cores are reserved. The audio engine cannot use that many; reserving one or two is usually enough.";
    return p;
}

int suggestedReserveCount (int physicalCores)
{
    if (physicalCores >= 12) return 3;
    if (physicalCores >= 8) return 2;
    return 1;
}

std::vector<int> pickQuietCores (const std::vector<CpuCore>& cores, const std::vector<double>& loadOfLogical, int count)
{
    struct Cand { int number; double noise; int efficiency; };
    std::vector<Cand> cands;
    int bestEff = 0;
    for (auto& c : cores) bestEff = std::max (bestEff, c.efficiency);
    for (auto& c : cores)
    {
        double noise = 0.0;
        for (int l : c.logical) if (l >= 0 && (size_t) l < loadOfLogical.size()) noise = std::max (noise, loadOfLogical[(size_t) l]);
        cands.push_back ({ c.number, noise, c.efficiency });
    }
    // performance cores first (a hybrid CPU's small cores are slower and are left to Windows), then the quietest, then the higher number
    std::sort (cands.begin(), cands.end(), [bestEff] (const Cand& a, const Cand& b)
    {
        const bool pa = a.efficiency == bestEff, pb = b.efficiency == bestEff;
        if (pa != pb) return pa;
        if (std::abs (a.noise - b.noise) > 1.0e-9) return a.noise < b.noise;
        return a.number > b.number;
    });
    // a hybrid CPU (3.13.0): big cores only, and at most half of them - the small ones are too slow for the audio, and the other big ones are needed by everything else
    const bool hybrid = isHybridCpu (cores);
    const int fastLimit = maxFastCoresForAudio (cores);
    std::vector<int> out;
    int fastTaken = 0;
    for (size_t i = 0; i < cands.size() && (int) out.size() < count && (int) out.size() < (int) cores.size() - 1; ++i)     // never every core
    {
        if (hybrid && (cands[i].efficiency != bestEff || fastTaken >= fastLimit)) continue;
        if (cands[i].efficiency == bestEff) ++fastTaken;
        out.push_back (cands[i].number);
    }
    std::sort (out.begin(), out.end());
    return out;
}

// ======================================================================================================= the system part
struct CoreReservation::Impl
{
    std::vector<CpuCore> coreList;
    CoreReservationSettings current;
    CorePlan plan;
    bool active = false;
    juce::String note;                                  // why nothing is active (shown to the user)
    std::atomic<unsigned> generation { 1 };
    juce::CriticalSection threadsLock;
    std::vector<juce::Thread::ThreadID> threads;

#if FERMATA_CPU_WINDOWS
    // --- the Windows calls are looked up by name, so the program also starts on a system that lacks one of them
    using GetSystemCpuSetInformationFn = BOOL (WINAPI*) (void*, ULONG, PULONG, HANDLE, ULONG);
    using SetProcessDefaultCpuSetsFn   = BOOL (WINAPI*) (HANDLE, const ULONG*, ULONG);
    using GetProcessDefaultCpuSetsFn   = BOOL (WINAPI*) (HANDLE, PULONG, ULONG, PULONG);
    using SetThreadSelectedCpuSetsFn   = BOOL (WINAPI*) (HANDLE, const ULONG*, ULONG);
    using SetProcessInformationFn      = BOOL (WINAPI*) (HANDLE, int, LPVOID, DWORD);
    using AvSetMmThreadCharacteristicsWFn = HANDLE (WINAPI*) (LPCWSTR, LPDWORD);
    using NtQuerySystemInformationFn   = LONG (NTAPI*) (int, PVOID, ULONG, PULONG);

    GetSystemCpuSetInformationFn getSystemCpuSetInformation = nullptr;
    SetProcessDefaultCpuSetsFn   setProcessDefaultCpuSets = nullptr;
    GetProcessDefaultCpuSetsFn   getProcessDefaultCpuSets = nullptr;
    SetThreadSelectedCpuSetsFn   setThreadSelectedCpuSets = nullptr;
    SetProcessInformationFn      setProcessInformation = nullptr;
    AvSetMmThreadCharacteristicsWFn avSetMmThreadCharacteristics = nullptr;
    NtQuerySystemInformationFn   ntQuerySystemInformation = nullptr;
    bool available = false;

    std::vector<ULONG> idOfLogical;                     // CPU set id of each logical processor (index = logical processor number)
    juce::SpinLock idsLock;                             // guards audioIds / generalIds (read by the audio thread)
    std::array<ULONG, 64> audioIds {}, generalIds {};
    int numAudioIds = 0, numGeneralIds = 0;

    /** The layout of Windows' SYSTEM_CPU_SET_INFORMATION (32 bytes), written out so it does not depend on which SDK the program is built with. */
    struct CpuSetInfo
    {
        ULONG size; ULONG type;                         // type 0 = a CPU set
        ULONG id; WORD group; BYTE logicalProcessorIndex, coreIndex, lastLevelCacheIndex, numaNodeIndex, efficiencyClass, flags;
        ULONG reserved; unsigned long long allocationTag;
    };
    /** SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION (48 bytes) */
    struct ProcPerf { long long idle, kernel, user, dpc, interrupt; ULONG interruptCount; ULONG pad; };

    /** apply: programs with no CPU set of their own are given 'ids'. Otherwise: programs that have exactly those cores are set free again.
        (A program that someone else - Process Lasso, say - has given its own cores is never touched.) Returns how many programs were changed. */
    int sweepPrograms (const std::vector<ULONG>& ids, bool apply, juce::Thread* stopper)
    {
        if (getProcessDefaultCpuSets == nullptr || setProcessDefaultCpuSets == nullptr || ids.empty()) return 0;
        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        int changed = 0;
        PROCESSENTRY32W pe {}; pe.dwSize = sizeof (pe);
        const DWORD self = GetCurrentProcessId();
        for (BOOL more = Process32FirstW (snap, &pe); more; more = Process32NextW (snap, &pe))
        {
            if (apply && stopper != nullptr && stopper->threadShouldExit()) break;
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4 || pe.th32ProcessID == self) continue;
            HANDLE h = OpenProcess (PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h == nullptr) continue;                          // protected, or another user's: leave it alone
            ULONG need = 0;
            std::vector<ULONG> cur;
            bool ok = getProcessDefaultCpuSets (h, nullptr, 0, &need) != FALSE;
            if (! ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER && need > 0 && need < 1024)
            {
                cur.resize (need);
                ok = getProcessDefaultCpuSets (h, cur.data(), need, &need) != FALSE;
                cur.resize (need);
                std::sort (cur.begin(), cur.end());
            }
            else if (ok) need = 0;
            if (ok)
            {
                if (apply && need == 0)                    { if (setProcessDefaultCpuSets (h, ids.data(), (ULONG) ids.size())) ++changed; }
                else if (! apply && need > 0 && cur == ids) { if (setProcessDefaultCpuSets (h, nullptr, 0)) ++changed; }
            }
            CloseHandle (h);
        }
        CloseHandle (snap);
        return changed;
    }

    /** Where a running Fermata writes down the cores it gave the other programs (3.13.0), so that the next start can set them free after a crash. */
    static juce::File fenceNoteFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Fermata").getChildFile ("cpu-fence.txt");
    }
    static juce::String idsText (const std::vector<ULONG>& ids)
    {
        juce::StringArray a; for (auto id : ids) a.add (juce::String ((juce::uint64) id));
        return a.joinIntoString (",");
    }
    static std::vector<ULONG> idsFromText (const juce::String& t)
    {
        std::vector<ULONG> v;
        for (auto& x : juce::StringArray::fromTokens (t, ",; \r\n", "")) if (x.isNotEmpty() && x.containsOnly ("0123456789")) v.push_back ((ULONG) x.getLargeIntValue());
        std::sort (v.begin(), v.end());
        return v;
    }

    /** Holds the other programs off the reserved cores for as long as it runs (looks again every few seconds, for programs started later). */
    struct Fence : public juce::Thread
    {
        Fence (Impl& o, std::vector<ULONG> general) : juce::Thread ("CPU ring-fence"), owner (o), ids (std::move (general))
        {
            std::sort (ids.begin(), ids.end());
            const auto f = fenceNoteFile();                      // written down first: if Fermata dies from here on, the next start sets the programs free
            f.getParentDirectory().createDirectory();
            f.replaceWithText (idsText (ids));
            startThread (juce::Thread::Priority::low);
        }
        ~Fence() override
        {
            stopThread (4000);
            owner.sweepPrograms (ids, false, nullptr);
            fenceNoteFile().deleteFile();
        }
        void run() override
        {
            td::crashreport::nameThisThread (getThreadName());                // (3.17.0: named for the log)
            while (! threadShouldExit()) { owner.sweepPrograms (ids, true, this); wait (5000); }
        }
        Impl& owner;
        std::vector<ULONG> ids;
    };
    std::unique_ptr<Fence> fence;

    Impl()
    {
        auto k32 = GetModuleHandleW (L"kernel32.dll");
        if (k32 != nullptr)
        {
            getSystemCpuSetInformation = reinterpret_cast<GetSystemCpuSetInformationFn> ((void*) GetProcAddress (k32, "GetSystemCpuSetInformation"));
            setProcessDefaultCpuSets   = reinterpret_cast<SetProcessDefaultCpuSetsFn> ((void*) GetProcAddress (k32, "SetProcessDefaultCpuSets"));
            getProcessDefaultCpuSets   = reinterpret_cast<GetProcessDefaultCpuSetsFn> ((void*) GetProcAddress (k32, "GetProcessDefaultCpuSets"));
            setThreadSelectedCpuSets   = reinterpret_cast<SetThreadSelectedCpuSetsFn> ((void*) GetProcAddress (k32, "SetThreadSelectedCpuSets"));
            setProcessInformation      = reinterpret_cast<SetProcessInformationFn> ((void*) GetProcAddress (k32, "SetProcessInformation"));
        }
        if (auto nt = GetModuleHandleW (L"ntdll.dll"))
            ntQuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFn> ((void*) GetProcAddress (nt, "NtQuerySystemInformation"));
        if (auto av = LoadLibraryW (L"avrt.dll"))
            avSetMmThreadCharacteristics = reinterpret_cast<AvSetMmThreadCharacteristicsWFn> ((void*) GetProcAddress (av, "AvSetMmThreadCharacteristicsW"));

        if (getSystemCpuSetInformation == nullptr || setProcessDefaultCpuSets == nullptr || setThreadSelectedCpuSets == nullptr)
        {
            note = "Reserving cores needs Windows 10 or later.";
            return;
        }
        ULONG len = 0;
        getSystemCpuSetInformation (nullptr, 0, &len, GetCurrentProcess(), 0);
        if (len == 0) { note = "Windows did not list the CPU cores."; return; }
        std::vector<char> buf (len);
        if (! getSystemCpuSetInformation (buf.data(), len, &len, GetCurrentProcess(), 0)) { note = "Windows did not list the CPU cores."; return; }

        struct Raw { int core; int logical; ULONG id; int eff; };
        std::vector<Raw> raw;
        bool otherGroups = false;
        for (ULONG off = 0; off + sizeof (CpuSetInfo) <= len;)
        {
            const auto* e = reinterpret_cast<const CpuSetInfo*> (buf.data() + off);
            if (e->size == 0) break;
            if (e->type == 0)
            {
                if (e->group != 0) otherGroups = true;
                else raw.push_back ({ (int) e->coreIndex, (int) e->logicalProcessorIndex, e->id, (int) e->efficiencyClass });
            }
            off += e->size;
        }
        if (otherGroups || raw.empty() || raw.size() > 64) { note = "This PC has more than 64 logical processors, which is not supported here."; return; }

        std::set<int> coreIdx; int maxLogical = 0;
        for (auto& r : raw) { coreIdx.insert (r.core); maxLogical = std::max (maxLogical, r.logical); }
        idOfLogical.assign ((size_t) maxLogical + 1, 0);
        int n = 0;
        for (int ci : coreIdx)
        {
            CpuCore c; c.number = n++;
            for (auto& r : raw) if (r.core == ci) { c.logical.push_back (r.logical); c.efficiency = r.eff; idOfLogical[(size_t) r.logical] = r.id; }
            std::sort (c.logical.begin(), c.logical.end());
            coreList.push_back (c);
        }
        available = true;
    }

    void copyIds (const std::vector<int>& logical, std::array<ULONG, 64>& dst, int& count) const
    {
        count = 0;
        for (int l : logical)
            if (l >= 0 && (size_t) l < idOfLogical.size() && count < (int) dst.size()) dst[(size_t) count++] = idOfLogical[(size_t) l];
    }

    /** Puts this thread on the audio cores (or lets it go again). Returns false when the ids were being changed at that moment (try again next block). */
    bool applyToThread (HANDLE thread, bool audio)
    {
        if (setThreadSelectedCpuSets == nullptr) return true;
        std::array<ULONG, 64> ids {}; int n = 0;
        {
            const juce::SpinLock::ScopedTryLockType sl (idsLock);
            if (! sl.isLocked()) return false;
            if (active && audio) { ids = audioIds; n = numAudioIds; }
        }
        setThreadSelectedCpuSets (thread, n > 0 ? ids.data() : nullptr, (ULONG) n);
        return true;
    }

    void applyToRegisteredThreads()
    {
        const juce::ScopedLock sl (threadsLock);
        for (auto id : threads)
            if (HANDLE h = OpenThread (THREAD_SET_LIMITED_INFORMATION, FALSE, (DWORD) (uintptr_t) id))
            {
                applyToThread (h, true);
                CloseHandle (h);
            }
    }

    void setPowerAndPriority (bool on)
    {
        struct PowerThrottling { ULONG version, controlMask, stateMask; } st { 1, 0, 0 };
        if (on) st.controlMask = 0x1 | 0x4;               // execution speed + timer resolution: switched off (the program is not slowed down to save power)
        if (setProcessInformation != nullptr) setProcessInformation (GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, (DWORD) sizeof (st));
        // the whole of Fermata (its windows too) at high priority starves everything else on a laptop's few fast cores (3.13.0): above normal there.
        // The audio threads do not depend on it: they are in Windows' "Pro Audio" class (MMCSS) either way.
        if (on && CoreReservation::isLaptop()) SetPriorityClass (GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        else juce::Process::setPriority (on ? juce::Process::HighPriority : juce::Process::NormalPriority);
    }

    void applyNow()
    {
        fence.reset();
        plan = (current.enabled && available) ? makeCorePlan (coreList, current) : CorePlan();
        const bool wasActive = active;
        active = plan.valid;
        {
            const juce::SpinLock::ScopedLockType sl (idsLock);
            numAudioIds = numGeneralIds = 0;
            if (active) { copyIds (plan.audioLogical, audioIds, numAudioIds); copyIds (plan.generalLogical, generalIds, numGeneralIds); }
        }
        if (setProcessDefaultCpuSets != nullptr && (active || wasActive))
        {
            std::array<ULONG, 64> g {}; int ng = 0;
            { const juce::SpinLock::ScopedLockType sl (idsLock); g = generalIds; ng = numGeneralIds; }
            setProcessDefaultCpuSets (GetCurrentProcess(), ng > 0 ? g.data() : nullptr, (ULONG) ng);
        }
        if (active || wasActive) setPowerAndPriority (active);
        generation.fetch_add (1, std::memory_order_release);        // the audio callback moves itself at its next block
        applyToRegisteredThreads();
        if (active && current.keepOthersOff)
            fence = std::make_unique<Fence> (*this, std::vector<ULONG> (generalIds.begin(), generalIds.begin() + numGeneralIds));
    }

    std::vector<double> measureLoad (int ms) const
    {
        if (ntQuerySystemInformation == nullptr || idOfLogical.empty()) return {};
        const size_t n = (size_t) GetActiveProcessorCount (ALL_PROCESSOR_GROUPS);
        if (n == 0 || n > 64) return {};
        std::vector<ProcPerf> a (n), b (n);
        ULONG got = 0;
        if (ntQuerySystemInformation (8 /* SystemProcessorPerformanceInformation */, a.data(), (ULONG) (n * sizeof (ProcPerf)), &got) != 0) return {};
        juce::Thread::sleep (juce::jmax (200, ms));
        if (ntQuerySystemInformation (8, b.data(), (ULONG) (n * sizeof (ProcPerf)), &got) != 0) return {};
        std::vector<double> out (n, 0.0);
        for (size_t i = 0; i < n; ++i)
        {
            const double total = (double) ((b[i].kernel - a[i].kernel) + (b[i].user - a[i].user));      // (kernel time includes the idle time)
            const double irq = (double) ((b[i].dpc - a[i].dpc) + (b[i].interrupt - a[i].interrupt));
            out[i] = total > 0.0 ? juce::jlimit (0.0, 100.0, 100.0 * irq / total) : 0.0;
        }
        return out;
    }
#endif
};

CoreReservation& CoreReservation::get() noexcept
{
    static CoreReservation instance;
    return instance;
}

CoreReservation::CoreReservation() : impl (std::make_unique<Impl>())
{
#if FERMATA_CPU_WINDOWS
    if (impl->available) { /* the cores were listed by the constructor of Impl */ }
#else
    impl->note = "Reserving cores is a Windows feature.";
#endif
}

CoreReservation::~CoreReservation() { shutdown(); }

bool CoreReservation::supported() const noexcept
{
#if FERMATA_CPU_WINDOWS
    return impl->available;
#else
    return false;
#endif
}

const std::vector<CpuCore>& CoreReservation::cores() const noexcept { return impl->coreList; }

CoreReservationSettings CoreReservation::settings() const { return impl->current; }

void CoreReservation::configure (const CoreReservationSettings& s)
{
    impl->current = s;
#if FERMATA_CPU_WINDOWS
    impl->applyNow();
    isActive.store (impl->active);
    audioLogicalCount.store (impl->active ? (int) impl->plan.audioLogical.size() : 0);
#else
    impl->plan = CorePlan();
    isActive.store (false);
    audioLogicalCount.store (0);
#endif
}

juce::String CoreReservation::statusText() const
{
    if (! supported()) return impl->note.isNotEmpty() ? impl->note : juce::String ("Reserving cores is not available here.");
    if (! impl->current.enabled) return "Off: Windows shares all the cores between everything, as usual.";
    if (! impl->plan.valid) return "Not in force: " + impl->plan.problem;
    juce::StringArray a;
    for (int c : impl->plan.audioCores) a.add (juce::String (c));
    juce::String t = "In force: core " + juce::String (a.size() == 1 ? "" : "s ") + a.joinIntoString (", ") + (a.size() == 1 ? " is " : " are ") + "kept for the audio engine; the rest of Fermata uses the other "
                   + juce::String ((int) impl->plan.generalCores.size()) + (impl->plan.generalCores.size() == 1 ? " core." : " cores.");
    if (impl->current.keepOthersOff) t << " Other programs are asked to stay off " << (a.size() == 1 ? "it." : "them.");
    if (impl->plan.warning.isNotEmpty()) t << "\n" << impl->plan.warning;
    return t;
}

void CoreReservation::audioThreadCheck() noexcept
{
#if FERMATA_CPU_WINDOWS
    thread_local unsigned seen = 0;
    thread_local bool inMmcss = false;
    const unsigned g = impl->generation.load (std::memory_order_acquire);
    if (g == seen) return;
    if (! impl->applyToThread (GetCurrentThread(), true)) return;           // the ids were being changed: try again at the next block
    if (! inMmcss && impl->avSetMmThreadCharacteristics != nullptr)         // whether or not cores are reserved: a mixer helper outside "Pro Audio" can be
    {                                                                       // pre-empted by any busy program while the audio thread waits for it
        DWORD taskIndex = 0;
        impl->avSetMmThreadCharacteristics (L"Pro Audio", &taskIndex);       // (fails harmlessly if the driver has already done it)
        inMmcss = true;
    }
    seen = g;
#endif
}

void CoreReservation::registerAudioThread (juce::Thread::ThreadID id)
{
#if FERMATA_CPU_WINDOWS
    if (id == nullptr) return;
    {
        const juce::ScopedLock sl (impl->threadsLock);
        if (std::find (impl->threads.begin(), impl->threads.end(), id) == impl->threads.end()) impl->threads.push_back (id);
    }
    if (isActive.load())
        if (HANDLE h = OpenThread (THREAD_SET_LIMITED_INFORMATION, FALSE, (DWORD) (uintptr_t) id))
        {
            impl->applyToThread (h, true);
            CloseHandle (h);
        }
#else
    juce::ignoreUnused (id);
#endif
}

void CoreReservation::unregisterAudioThread (juce::Thread::ThreadID id)
{
    const juce::ScopedLock sl (impl->threadsLock);
    impl->threads.erase (std::remove (impl->threads.begin(), impl->threads.end(), id), impl->threads.end());
}

std::vector<double> CoreReservation::measureInterruptLoad (int milliseconds) const
{
#if FERMATA_CPU_WINDOWS
    return impl->measureLoad (milliseconds);
#else
    juce::ignoreUnused (milliseconds);
    return {};
#endif
}

void CoreReservation::shutdown()
{
    if (impl == nullptr) return;
    if (impl->current.enabled || isActive.load())
    {
        auto off = impl->current; off.enabled = false;
        configure (off);
    }
}
int CoreReservation::releaseLeftovers (const CoreReservationSettings& saved)
{
#if FERMATA_CPU_WINDOWS
    if (! impl->available) return 0;
    int freed = 0;
    std::set<std::vector<ULONG>> sets;
    const auto note = Impl::fenceNoteFile();
    if (note.existsAsFile())
    {
        if (auto ids = Impl::idsFromText (note.loadFileAsString()); ! ids.empty()) sets.insert (ids);
        note.deleteFile();
    }
    if (saved.enabled && saved.keepOthersOff)                                  // what an older version (or this one) gave them with these settings
        for (bool limited : { false, true })
        {
            const auto plan = makeCorePlan (impl->coreList, saved, limited);
            if (! plan.valid) continue;
            std::array<ULONG, 64> g {}; int ng = 0;
            impl->copyIds (plan.generalLogical, g, ng);
            std::vector<ULONG> ids (g.begin(), g.begin() + ng);
            std::sort (ids.begin(), ids.end());
            if (! ids.empty()) sets.insert (ids);
        }
    for (auto& ids : sets) freed += impl->sweepPrograms (ids, false, nullptr);
    return freed;
#else
    juce::ignoreUnused (saved);
    return 0;
#endif
}

bool CoreReservation::isLaptop() noexcept
{
#if FERMATA_CPU_WINDOWS
    SYSTEM_POWER_STATUS ps {};
    if (! GetSystemPowerStatus (&ps)) return false;
    return ps.BatteryFlag != 128 && ps.BatteryFlag != 255;                 // 128 = no system battery, 255 = unknown
#else
    return false;
#endif
}

juce::String CoreReservation::describeMachine() const
{
    const auto& c = impl->coreList;
    if (c.empty()) return {};
    int logical = 0; for (auto& x : c) logical += (int) x.logical.size();
    juce::String t; t << (int) c.size() << " cores, " << logical << " processors";
    if (isHybridCpu (c))
    {
        const int best = bestEfficiency (c);
        int big = 0; for (auto& x : c) if (x.efficiency == best) ++big;
        juce::StringArray bigList; for (auto& x : c) if (x.efficiency == best) bigList.add (juce::String (x.number));
        t << " (" << big << " big: core" << (big == 1 ? " " : "s ") << bigList.joinIntoString (", ") << "; " << ((int) c.size() - big) << " small)";
    }
    if (isLaptop()) t << ", laptop";
    if (const auto p = SystemLoad::powerText(); p.isNotEmpty()) t << ", " << p;
    return t;
}

// ======================================================================================================= SystemLoad (3.13.0)
#if FERMATA_CPU_WINDOWS
static std::atomic<HANDLE> messageThreadHandle { nullptr };
static std::atomic<DWORD> messageThreadId { 0 };
static bool messageThreadKnown() noexcept { return messageThreadHandle.load() != nullptr; }
static unsigned long long fileTimeValue (const FILETIME& f) noexcept { return ((unsigned long long) f.dwHighDateTime << 32) | f.dwLowDateTime; }
#else
static bool messageThreadKnown() noexcept { return false; }
#endif

void SystemLoad::rememberMessageThread()
{
#if FERMATA_CPU_WINDOWS
    if (messageThreadHandle.load() == nullptr)
    {
        messageThreadId.store (GetCurrentThreadId());
        messageThreadHandle.store (OpenThread (THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId()));
    }
#endif
}

#if FERMATA_CPU_WINDOWS
using GetThreadDescriptionFn = LONG (WINAPI*) (HANDLE, wchar_t**);
static GetThreadDescriptionFn threadDescriptionFn()
{
    static GetThreadDescriptionFn fn = [] { auto k = GetModuleHandleW (L"kernel32.dll"); return k != nullptr ? reinterpret_cast<GetThreadDescriptionFn> ((void*) GetProcAddress (k, "GetThreadDescription")) : nullptr; }();
    return fn;
}
/** A thread's name: JUCE names its threads ("Mixer helper 2", "Take matching" ...); the main thread is the main thread; others go by their number. */
static juce::String threadName (DWORD id)
{
    if (id == messageThreadId.load()) return "main thread";
    juce::String name;
    if (auto fn = threadDescriptionFn())
        if (HANDLE h = OpenThread (THREAD_QUERY_LIMITED_INFORMATION, FALSE, id))
        {
            wchar_t* desc = nullptr;
            if (fn (h, &desc) >= 0 && desc != nullptr) { name = juce::String (desc); LocalFree (desc); }
            CloseHandle (h);
        }
    return name.isNotEmpty() ? name : "unnamed thread " + juce::String ((juce::int64) id) + " (a driver's or a plug-in's, or the audio callback)";
}
#endif

SystemLoad::Snapshot SystemLoad::take (bool withThreads)
{
    Snapshot s;
    s.atMs = juce::Time::getMillisecondCounterHiRes();
    juce::ignoreUnused (withThreads);
#if FERMATA_CPU_WINDOWS
    FILETIME idle {}, kernel {}, user {};
    if (! GetSystemTimes (&idle, &kernel, &user)) return s;
    s.pcIdle = fileTimeValue (idle);
    s.pcTotal = fileTimeValue (kernel) + fileTimeValue (user);            // (the kernel time includes the idle time)
    FILETIME c {}, e {}, k {}, u {};
    if (GetProcessTimes (GetCurrentProcess(), &c, &e, &k, &u)) s.process = fileTimeValue (k) + fileTimeValue (u);
    if (HANDLE h = messageThreadHandle.load())
        if (GetThreadTimes (h, &c, &e, &k, &u)) s.mainThread = fileTimeValue (k) + fileTimeValue (u);
    if (withThreads)
    {
        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            const DWORD self = GetCurrentProcessId();
            THREADENTRY32 te {}; te.dwSize = sizeof (te);
            for (BOOL more = Thread32First (snap, &te); more; more = Thread32Next (snap, &te))
            {
                if (te.th32OwnerProcessID != self) continue;
                if (HANDLE h = OpenThread (THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID))
                {
                    if (GetThreadTimes (h, &c, &e, &k, &u)) s.threads.push_back ({ (unsigned long) te.th32ThreadID, fileTimeValue (k) + fileTimeValue (u) });
                    CloseHandle (h);
                }
            }
            CloseHandle (snap);
        }
    }
    s.ok = true;
#endif
    return s;
}

juce::String SystemLoad::describe (const Snapshot& a, const Snapshot& b, bool forStall)
{
    if (! a.ok || ! b.ok) return {};
    const double wall = (b.atMs - a.atMs) * 10000.0;                       // milliseconds -> 100 ns units
    if (wall <= 0.0) return {};
    const double total = (double) (b.pcTotal - a.pcTotal), idle = (double) (b.pcIdle - a.pcIdle);
    const double pcBusy = total > 0.0 ? juce::jlimit (0.0, 100.0, 100.0 * (total - idle) / total) : 0.0;
    const double fermataCores = (double) (b.process - a.process) / wall;
    const double mainShare = juce::jlimit (0.0, 100.0, 100.0 * (double) (b.mainThread - a.mainThread) / wall);
    juce::String t;
    if (forStall && messageThreadKnown()) t << "main thread working " << juce::roundToInt (mainShare) << "% of it, ";
    if (! forStall && messageThreadKnown()) t << "main thread " << juce::roundToInt (mainShare) << "% of a core, ";
    t << "Fermata using " << juce::String (fermataCores, 1) << (std::abs (fermataCores - 1.0) < 0.05 ? " core" : " cores") << ", whole PC " << juce::roundToInt (pcBusy) << "% busy";
    if (const auto p = powerText(); p.isNotEmpty()) t << ", " << p;
#if FERMATA_CPU_WINDOWS
    if (! a.threads.empty() && ! b.threads.empty())                        // (3.15.0) the busiest of Fermata's threads, by name
    {
        std::vector<std::pair<double, unsigned long>> busy;
        for (auto& [id, after] : b.threads)
        {
            unsigned long long before = 0; bool found = false;
            for (auto& [id0, t0] : a.threads) if (id0 == id) { before = t0; found = true; break; }
            const double share = 100.0 * (double) (after - (found ? before : 0)) / wall;     // (a thread started meanwhile: all its time is new)
            if (share >= 10.0) busy.push_back ({ share, id });
        }
        std::sort (busy.begin(), busy.end(), [] (const auto& x, const auto& y) { return x.first > y.first; });
        if (! busy.empty())
        {
            t << "; busiest: ";
            for (size_t i = 0; i < busy.size() && i < 4; ++i)
                t << (i > 0 ? ", " : "") << threadName ((DWORD) busy[i].second) << " " << juce::roundToInt (busy[i].first) << "%";
        }
    }
#endif
    if (forStall)
    {
        if (pcBusy >= 85.0)                             t << " - the PC was flat out";
        else if (messageThreadKnown() && mainShare >= 70.0) t << " - Fermata's own drawing or work";
        else if (messageThreadKnown() && mainShare < 30.0)  t << " - the main thread was mostly waiting (for Windows, a driver, the disk or a free core)";
    }
    return t;
}

unsigned long SystemLoad::busiestThread (const Snapshot& a, const Snapshot& b, double& percent)
{
    percent = 0.0;
    const double wall = (b.atMs - a.atMs) * 10000.0;
    if (! a.ok || ! b.ok || wall <= 0.0) return 0;
    unsigned long best = 0;
    for (auto& [id, after] : b.threads)
    {
        unsigned long long before = 0;
        for (auto& [id0, t0] : a.threads) if (id0 == id) { before = t0; break; }
        const double share = 100.0 * (double) (after - before) / wall;
        if (share > percent) { percent = share; best = id; }
    }
    return best;
}

juce::String SystemLoad::powerText()
{
#if FERMATA_CPU_WINDOWS
    SYSTEM_POWER_STATUS ps {};
    if (! GetSystemPowerStatus (&ps) || ps.BatteryFlag == 128 || ps.BatteryFlag == 255) return {};
    juce::String t = ps.ACLineStatus == 0 ? "on battery" : ps.ACLineStatus == 1 ? "on mains" : "";
    if (ps.SystemStatusFlag == 1) t << (t.isNotEmpty() ? ", " : "") << "battery saver on";
    return t;
#else
    return {};
#endif
}

void CoreReservation::makeThisThreadBackground() noexcept
{
#if FERMATA_CPU_WINDOWS
    SetThreadPriority (GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);       // low disk (and CPU) priority for this thread
#endif
}
} // namespace td
