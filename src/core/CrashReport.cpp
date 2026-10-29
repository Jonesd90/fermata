#include "CrashReport.h"
#include <atomic>
#include <cwchar>
#include <cstdlib>

// The Windows part is compiled on Windows only. FERMATA_CPU_WINDOWS_STUBS lets the development check compile it against stand-in headers.
#if JUCE_WINDOWS || defined (FERMATA_CPU_WINDOWS_STUBS)
 #define FERMATA_CRASH_WINDOWS 1
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <tlhelp32.h>
 #ifdef _MSC_VER
  #pragma warning (push)
  #pragma warning (disable: 4091)
 #endif
 #include <dbghelp.h>
 #ifdef _MSC_VER
  #pragma warning (pop)
 #endif
#else
 #define FERMATA_CRASH_WINDOWS 0
#endif

namespace td
{
namespace crashreport
{
static juce::File& folderRef() { static juce::File f; return f; }
juce::File folder() { return folderRef(); }

#if FERMATA_CRASH_WINDOWS
// MINIDUMP_EXCEPTION_INFORMATION and the MiniDumpWriteDump call, written out so that dbghelp.h is not needed (dbghelp.h packs it to 4 bytes)
#pragma pack(push, 4)
struct DumpExceptionInfo { DWORD threadId; void* exceptionPointers; BOOL clientPointers; };
#pragma pack(pop)
using MiniDumpWriteDumpFn = BOOL (WINAPI*) (HANDLE, DWORD, HANDLE, int, const DumpExceptionInfo*, const void*, const void*);
static std::atomic<MiniDumpWriteDumpFn> writeDumpFn { nullptr };

// prepared at start-up, so that writing a report during a crash needs no memory from the (possibly damaged) heap
static wchar_t folderPath[1024] = {};
static wchar_t versionText[64] = {};
static std::atomic<bool> writing { false };

/** The layouts of EXCEPTION_RECORD / EXCEPTION_POINTERS, as Windows defines them. */
struct ExRecord { DWORD code; DWORD flags; ExRecord* inner; void* address; DWORD numberParameters; ULONG_PTR information[15]; };
struct ExPointers { ExRecord* record; void* context; };
#endif

void install (const juce::File& f, const juce::String& version)
{
    folderRef() = f;
#if FERMATA_CRASH_WINDOWS
    f.createDirectory();
    const auto path = f.getFullPathName();
    const auto* w = path.toWideCharPointer();
    size_t i = 0;
    for (; w[i] != 0 && i + 1 < sizeof (folderPath) / sizeof (folderPath[0]); ++i) folderPath[i] = (wchar_t) w[i];
    folderPath[i] = 0;
    const auto* v = version.toWideCharPointer();
    for (i = 0; v[i] != 0 && i + 1 < sizeof (versionText) / sizeof (versionText[0]); ++i) versionText[i] = (wchar_t) v[i];
    versionText[i] = 0;
    if (writeDumpFn.load() == nullptr)
        if (auto dbg = LoadLibraryW (L"dbghelp.dll"))
            writeDumpFn.store (reinterpret_cast<MiniDumpWriteDumpFn> ((void*) GetProcAddress (dbg, "MiniDumpWriteDump")));
#else
    juce::ignoreUnused (version);
#endif
}

#if FERMATA_CRASH_WINDOWS
/** The report's full name, made without the heap: "<folder>\Fermata <version> <why> <date> <time>.dmp". */
static void reportName (wchar_t* name, size_t size, const char* why)
{
    SYSTEMTIME t {};
    GetLocalTime (&t);
    wchar_t whyW[16] = {};
    for (int i = 0; why != nullptr && why[i] != 0 && i < 15; ++i) whyW[i] = (wchar_t) why[i];
    std::swprintf (name, size, L"%ls\\Fermata %ls %ls %04u-%02u-%02u %02u%02u%02u.dmp", folderPath, versionText, whyW,
                   (unsigned) t.wYear, (unsigned) t.wMonth, (unsigned) t.wDay, (unsigned) t.wHour, (unsigned) t.wMinute, (unsigned) t.wSecond);
}
/** The kind of minidump written: every thread's stack and registers, the memory they point at, the modules (loaded and unloaded), the threads' times. */
static constexpr int kDumpType = 0x00000040 /* WithIndirectlyReferencedMemory */ | 0x00000020 /* WithUnloadedModules */ | 0x00001000 /* WithThreadInfo */
                                | 0x00000100 /* WithProcessThreadData */;
#endif

juce::File write (void* exceptionPointers, const char* why)
{
#if FERMATA_CRASH_WINDOWS
    auto fn = writeDumpFn.load();
    if (fn == nullptr || folderPath[0] == 0) return {};
    if (writing.exchange (true)) return {};                                  // (one at a time: a crash while a freeze report is being written)
    wchar_t name[1300] = {};
    reportName (name, sizeof (name) / sizeof (name[0]), why);
    HANDLE file = CreateFileW (name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE)
    {
        DumpExceptionInfo info { GetCurrentThreadId(), exceptionPointers, FALSE };
        ok = fn (GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, exceptionPointers != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle (file);
        if (! ok) DeleteFileW (name);
    }
    writing.store (false);
    return ok ? juce::File (juce::String (name)) : juce::File();
#else
    juce::ignoreUnused (exceptionPointers, why);
    return {};
#endif
}

juce::String describeException (void* exceptionPointers)
{
#if FERMATA_CRASH_WINDOWS
    auto* ep = static_cast<ExPointers*> (exceptionPointers);
    if (ep == nullptr || ep->record == nullptr) return {};
    const auto& r = *ep->record;
    juce::String what;
    switch (r.code)
    {
        case 0xC0000005u:
            what = "access violation";
            if (r.numberParameters >= 2)
                what << (r.information[0] == 1 ? " writing " : r.information[0] == 8 ? " executing " : " reading ") << "0x" << juce::String::toHexString ((juce::int64) r.information[1]);
            break;
        case 0xC00000FDu: what = "stack overflow"; break;
        case 0xC0000094u: what = "integer divide by zero"; break;
        case 0xC000001Du: what = "illegal instruction"; break;
        case 0xC0000409u: what = "stack buffer overrun / fast fail"; break;
        case 0xC0000374u: what = "heap corruption"; break;
        case 0x80000003u: what = "breakpoint"; break;
        case 0xE06D7363u: what = "C++ exception not caught"; break;
        default:          what = "exception 0x" + juce::String::toHexString ((juce::int64) r.code); break;
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExW (0x4 /* FROM_ADDRESS */ | 0x2 /* UNCHANGED_REFCOUNT */, (LPCWSTR) r.address, &mod) && mod != nullptr)
    {
        wchar_t path[1024] = {};
        GetModuleFileNameW (mod, path, 1023);
        const auto offset = (juce::int64) ((const char*) r.address - (const char*) mod);
        what << " at " << juce::File (juce::String (path)).getFileName() << "+0x" << juce::String::toHexString (offset);
    }
    else
        what << " at 0x" << juce::String::toHexString ((juce::int64) (juce::pointer_sized_int) r.address);
    return what;
#else
    juce::ignoreUnused (exceptionPointers);
    return {};
#endif
}

// ------------------------------------------------------------------------------------------------ where every thread is (3.17.0)
#if FERMATA_CRASH_WINDOWS && (defined (_M_X64) || defined (__x86_64__))
 #define FERMATA_CAN_WALK_THREADS 1
/** The return addresses of a stopped thread, innermost first, from its registers: the x64 unwind tables say how each function's frame is laid out.
    Nothing here uses the heap or takes a lock the stopped thread could be holding (apart from what the system needs to find a module's table). */
static int unwindStack (const CONTEXT& start, DWORD64* frames, int maxFrames)
{
    CONTEXT ctx = start;
    int n = 0;
    for (; n < maxFrames && ctx.Rip != 0; ++n)
    {
        frames[n] = ctx.Rip;
        DWORD64 imageBase = 0;
        auto* fe = RtlLookupFunctionEntry (ctx.Rip, &imageBase, nullptr);
        if (fe == nullptr)                                        // a leaf function: the return address is at the top of the stack
        {
            if (ctx.Rsp == 0 || (ctx.Rsp & 7) != 0) { ++n; break; }
            ctx.Rip = *reinterpret_cast<const DWORD64*> (ctx.Rsp);
            ctx.Rsp += 8;
        }
        else
        {
            void* handlerData = nullptr; DWORD64 establisher = 0;
            RtlVirtualUnwind (0 /* UNW_FLAG_NHANDLER */, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
        }
    }
    return n;
}
/** The same, safe against a broken stack: a bad address read ends the walk instead of ending the program. */
static int unwindStackSafely (const CONTEXT& ctx, DWORD64* frames, int maxFrames)
{
   #ifdef _MSC_VER
    __try { return unwindStack (ctx, frames, maxFrames); }
    __except (1 /* EXCEPTION_EXECUTE_HANDLER */) { return 0; }
   #else
    return unwindStack (ctx, frames, maxFrames);
   #endif
}
#else
 #define FERMATA_CAN_WALK_THREADS 0
#endif

#if FERMATA_CRASH_WINDOWS
using GetThreadDescriptionFn2 = LONG (WINAPI*) (HANDLE, wchar_t**);
static juce::String nameOfThread (DWORD id, DWORD mainId)
{
    if (id == mainId) return "main thread";
    static GetThreadDescriptionFn2 fn = [] { auto k = GetModuleHandleW (L"kernel32.dll"); return k != nullptr ? reinterpret_cast<GetThreadDescriptionFn2> ((void*) GetProcAddress (k, "GetThreadDescription")) : nullptr; }();
    juce::String name;
    if (fn != nullptr)
        if (HANDLE h = OpenThread (THREAD_QUERY_LIMITED_INFORMATION, FALSE, id))
        {
            wchar_t* desc = nullptr;
            if (fn (h, &desc) >= 0 && desc != nullptr) { name = juce::String (desc); LocalFree (desc); }
            CloseHandle (h);
        }
    return name.isNotEmpty() ? "'" + name + "'" : "unnamed thread " + juce::String ((juce::int64) id);
}
/** "td::Project::undoReset (Project.cpp:2201)" or "Fermata.exe+0x1a2b3c". */
static juce::String describeAddress (DWORD64 addr)
{
    juce::String where;
    HANDLE process = GetCurrentProcess();
    alignas (SYMBOL_INFO) char buf[sizeof (SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*> (buf);
    sym->SizeOfStruct = sizeof (SYMBOL_INFO); sym->MaxNameLen = 500;
    DWORD64 disp = 0;
    if (SymFromAddr (process, addr, &disp, sym))
    {
        where = juce::String (sym->Name);
        IMAGEHLP_LINE64 line {}; line.SizeOfStruct = sizeof (line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64 (process, addr, &lineDisp, &line) && line.FileName != nullptr)
            where << " (" << juce::File (juce::String (line.FileName)).getFileName() << ":" << (int) line.LineNumber << ")";
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExW (0x4 | 0x2, (LPCWSTR) addr, &mod) && mod != nullptr)
    {
        wchar_t path[1024] = {};
        GetModuleFileNameW (mod, path, 1023);
        const auto modText = juce::File (juce::String (path)).getFileName() + "+0x" + juce::String::toHexString ((juce::int64) (addr - (DWORD64) (juce::pointer_sized_int) mod));
        where = where.isEmpty() ? modText : where + "   [" + modText + "]";
    }
    return where.isNotEmpty() ? where : "0x" + juce::String::toHexString ((juce::int64) addr);
}
#endif

juce::String describeThreads (unsigned long mainThreadId, int mainFrames, int otherFrames, unsigned long onlyThread)
{
#if FERMATA_CAN_WALK_THREADS
    // 1. the threads of this process (the list is made before any thread is held)
    std::vector<DWORD> ids;
    {
        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return {};
        const DWORD self = GetCurrentProcessId();
        THREADENTRY32 te {}; te.dwSize = sizeof (te);
        for (BOOL more = Thread32First (snap, &te); more; more = Thread32Next (snap, &te))
            if (te.th32OwnerProcessID == self && te.th32ThreadID != GetCurrentThreadId()
                && (onlyThread == 0 || te.th32ThreadID == (DWORD) onlyThread || te.th32ThreadID == (DWORD) mainThreadId)) ids.push_back (te.th32ThreadID);
        CloseHandle (snap);
    }
    std::stable_partition (ids.begin(), ids.end(), [mainThreadId] (DWORD id) { return id == (DWORD) mainThreadId; });     // the main thread first
    // 2. each thread held for a moment: its registers read and its stack walked into a fixed array, then let go
    struct Walked { DWORD id; int n; DWORD64 frames[64]; };
    std::vector<Walked> walked (ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
    {
        auto& w = walked[i]; w.id = ids[i]; w.n = 0;
        HANDLE h = OpenThread (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
        if (h == nullptr) continue;
        if (SuspendThread (h) != (DWORD) -1)
        {
            CONTEXT ctx {}; ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext (h, &ctx))
                w.n = unwindStackSafely (ctx, w.frames, juce::jlimit (1, 64, ids[i] == (DWORD) mainThreadId ? mainFrames : otherFrames));
            ResumeThread (h);
        }
        CloseHandle (h);
    }
    // 3. only now, with every thread running again: the names
    static bool symbolsReady = false;
    if (! symbolsReady)
    {
        SymSetOptions (SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        const auto exeFolder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getFullPathName();
        SymInitialize (GetCurrentProcess(), exeFolder.toRawUTF8(), TRUE);       // (Fermata.pdb next to Fermata.exe; already done by a crash trace: fine)
        symbolsReady = true;
    }
    SymRefreshModuleList (GetCurrentProcess());                                // (plug-ins loaded since)
    juce::String out;
    for (auto& w : walked)
    {
        out << "  " << nameOfThread (w.id, (DWORD) mainThreadId) << " (id " << (juce::int64) w.id << "):\n";
        if (w.n == 0) { out << "      (its place could not be read)\n"; continue; }
        for (int k = 0; k < w.n; ++k) out << "      " << k << "  " << describeAddress (w.frames[k]) << "\n";
    }
    return out;
#else
    juce::ignoreUnused (mainThreadId, mainFrames, otherFrames, onlyThread);
    return {};
#endif
}

// ------------------------------------------------------------------------------------------------ the freeze report from a helper process (3.17.0)
juce::File writeViaHelper (const char* why, juce::String& error, int waitMs)
{
#if FERMATA_CRASH_WINDOWS
    if (folderPath[0] == 0) { error = "no folder for the reports"; return {}; }
    wchar_t name[1300] = {};
    reportName (name, sizeof (name) / sizeof (name[0]), why);
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    const auto cmd = "\"" + exe + "\" --fermata-dump " + juce::String ((juce::int64) GetCurrentProcessId()) + " \"" + juce::String (name) + "\"";
    const wchar_t* w = cmd.toWideCharPointer();
    std::vector<wchar_t> cmdLine (w, w + std::wcslen (w) + 1);              // (CreateProcessW may write into it)
    STARTUPINFOW si {}; si.cb = sizeof (si);
    PROCESS_INFORMATION pi {};
    if (! CreateProcessW (nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0x08000000 /* CREATE_NO_WINDOW */, nullptr, nullptr, &si, &pi))
    {
        error = "the helper could not be started (Windows error " + juce::String ((int) GetLastError()) + ")";
        return {};
    }
    const DWORD waited = WaitForSingleObject (pi.hProcess, (DWORD) juce::jmax (1000, waitMs));
    DWORD code = 1;
    if (waited == 0 /* WAIT_OBJECT_0 */) GetExitCodeProcess (pi.hProcess, &code);
    else TerminateProcess (pi.hProcess, 2);
    CloseHandle (pi.hThread); CloseHandle (pi.hProcess);
    const juce::File f { juce::String (name) };
    if (waited != 0) { error = "the helper did not finish in " + juce::String (waitMs / 1000) + " s"; f.deleteFile(); return {}; }
    if (code != 0 || f.getSize() <= 0) { error = "the helper could not write it (code " + juce::String ((int) code) + ")"; f.deleteFile(); return {}; }
    return f;
#else
    juce::ignoreUnused (why, waitMs);
    error = "freeze reports are written on Windows only";
    return {};
#endif
}

int helperMain (const juce::StringArray& args)
{
    const int at = args.indexOf ("--fermata-dump");
    if (at < 0 || at + 2 >= args.size()) return 3;
    const auto pid = (unsigned long) args[at + 1].getLargeIntValue();
    const auto path = args[at + 2].unquoted();
    if (pid == 0 || path.isEmpty()) return 3;
#if FERMATA_CRASH_WINDOWS
    auto dbg = LoadLibraryW (L"dbghelp.dll");
    auto fn = dbg != nullptr ? reinterpret_cast<MiniDumpWriteDumpFn> ((void*) GetProcAddress (dbg, "MiniDumpWriteDump")) : nullptr;
    if (fn == nullptr) return 4;
    HANDLE process = OpenProcess (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | 0x0040 /* PROCESS_DUP_HANDLE */, FALSE, (DWORD) pid);
    if (process == nullptr) return 5;
    juce::File (path).getParentDirectory().createDirectory();
    HANDLE file = CreateFileW (path.toWideCharPointer(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { CloseHandle (process); return 6; }
    const bool ok = fn (process, (DWORD) pid, file, kDumpType, nullptr, nullptr, nullptr) != FALSE;
    CloseHandle (file); CloseHandle (process);
    if (! ok) { DeleteFileW (path.toWideCharPointer()); return 7; }
    return 0;
#else
    return 8;
#endif
}

#if FERMATA_CRASH_WINDOWS
using SetThreadDescriptionFn = LONG (WINAPI*) (HANDLE, const wchar_t*);
static SetThreadDescriptionFn setDescriptionFn() noexcept
{
    static SetThreadDescriptionFn fn = [] { auto k = GetModuleHandleW (L"kernel32.dll"); return k != nullptr ? reinterpret_cast<SetThreadDescriptionFn> ((void*) GetProcAddress (k, "SetThreadDescription")) : nullptr; }();
    return fn;
}
#endif

void nameThisThread (const juce::String& name)
{
#if FERMATA_CRASH_WINDOWS
    if (name.isNotEmpty()) nameThisThreadW (name.toWideCharPointer());
#else
    juce::ignoreUnused (name);
#endif
}

void nameThisThreadW (const wchar_t* name) noexcept
{
#if FERMATA_CRASH_WINDOWS
    if (auto fn = setDescriptionFn(); fn != nullptr && name != nullptr) fn (GetCurrentThread(), name);
#else
    juce::ignoreUnused (name);
#endif
}

void endThisProcess() noexcept
{
#if FERMATA_CRASH_WINDOWS
    TerminateProcess (GetCurrentProcess(), 0);
#endif
    std::_Exit (0);
}

juce::Array<juce::File> allReports()
{
    juce::Array<juce::File> out;
    if (folderRef() == juce::File() || ! folderRef().isDirectory()) return out;
    out = folderRef().findChildFiles (juce::File::findFiles, false, "*.dmp");
    std::sort (out.begin(), out.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    return out;
}

juce::Array<juce::File> reportsSince (juce::Time since)
{
    juce::Array<juce::File> out;
    for (auto& f : allReports()) if (f.getLastModificationTime() > since) out.add (f);
    return out;
}

void prune (int keep)
{
    auto all = allReports();
    for (int i = juce::jmax (0, keep); i < all.size(); ++i) all.getReference (i).deleteFile();
}
} // namespace crashreport
} // namespace td
