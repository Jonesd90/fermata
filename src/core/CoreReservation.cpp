#include "CoreReservation.h"
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

CorePlan makeCorePlan (const std::vector<CpuCore>& cores, const CoreReservationSettings& s)
{
    CorePlan p;
    if (cores.size() < 2) { p.problem = "This PC has only one CPU core, so there is nothing to keep apart."; return p; }
    for (auto& c : cores)
    {
        if (contains (s.reserved, c.number)) { p.audioCores.push_back (c.number); p.audioLogical.insert (p.audioLogical.end(), c.logical.begin(), c.logical.end()); }
        else                                 { p.generalCores.push_back (c.number); p.generalLogical.insert (p.generalLogical.end(), c.logical.begin(), c.logical.end()); }
    }
    if (p.audioCores.empty())   { p.problem = "No core is ticked: tick at least one core to keep for the audio."; return p; }
    if (p.generalCores.empty()) { p.problem = "Every core is ticked: leave at least one core for Windows and the rest of the program."; return p; }
    p.valid = true;
    if (p.generalCores.size() < 2 && cores.size() >= 4)
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
    std::vector<int> out;
    for (int i = 0; i < count && i < (int) cands.size() && i < (int) cores.size() - 1; ++i) out.push_back (cands[(size_t) i].number);     // never every core
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

    /** Holds the other programs off the reserved cores for as long as it runs (looks again every few seconds, for programs started later). */
    struct Fence : public juce::Thread
    {
        Fence (Impl& o, std::vector<ULONG> general) : juce::Thread ("CPU ring-fence"), owner (o), ids (std::move (general))
        { std::sort (ids.begin(), ids.end()); startThread (juce::Thread::Priority::low); }
        ~Fence() override { stopThread (4000); sweep (false); }
        void run() override
        {
            while (! threadShouldExit()) { sweep (true); wait (5000); }
        }
        /** apply: programs with no CPU set of their own are given the general cores. Otherwise: programs that have exactly those cores are set free again.
            (A program that someone else - Process Lasso, say - has given its own cores is never touched.) */
        void sweep (bool apply)
        {
            if (owner.getProcessDefaultCpuSets == nullptr || owner.setProcessDefaultCpuSets == nullptr || ids.empty()) return;
            HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return;
            PROCESSENTRY32W pe {}; pe.dwSize = sizeof (pe);
            const DWORD self = GetCurrentProcessId();
            for (BOOL more = Process32FirstW (snap, &pe); more; more = Process32NextW (snap, &pe))
            {
                if (apply && threadShouldExit()) break;
                if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4 || pe.th32ProcessID == self) continue;
                HANDLE h = OpenProcess (PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h == nullptr) continue;                          // protected, or another user's: leave it alone
                ULONG need = 0;
                std::vector<ULONG> cur;
                bool ok = owner.getProcessDefaultCpuSets (h, nullptr, 0, &need) != FALSE;
                if (! ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER && need > 0 && need < 1024)
                {
                    cur.resize (need);
                    ok = owner.getProcessDefaultCpuSets (h, cur.data(), need, &need) != FALSE;
                    std::sort (cur.begin(), cur.end());
                }
                else if (ok) need = 0;
                if (ok)
                {
                    if (apply && need == 0) owner.setProcessDefaultCpuSets (h, ids.data(), (ULONG) ids.size());
                    else if (! apply && need > 0 && cur == ids) owner.setProcessDefaultCpuSets (h, nullptr, 0);
                }
                CloseHandle (h);
            }
            CloseHandle (snap);
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
        juce::Process::setPriority (on ? juce::Process::HighPriority : juce::Process::NormalPriority);
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
#else
    impl->plan = CorePlan();
    isActive.store (false);
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
    if (isActive.load() && ! inMmcss && impl->avSetMmThreadCharacteristics != nullptr)
    {
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
} // namespace td
