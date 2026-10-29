#include "CrashReport.h"
#include <atomic>
#include <cwchar>

// The Windows part is compiled on Windows only. FERMATA_CPU_WINDOWS_STUBS lets the development check compile it against stand-in headers.
#if JUCE_WINDOWS || defined (FERMATA_CPU_WINDOWS_STUBS)
 #define FERMATA_CRASH_WINDOWS 1
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
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

juce::File write (void* exceptionPointers, const char* why)
{
#if FERMATA_CRASH_WINDOWS
    auto fn = writeDumpFn.load();
    if (fn == nullptr || folderPath[0] == 0) return {};
    if (writing.exchange (true)) return {};                                  // (one at a time: a crash while a freeze report is being written)
    SYSTEMTIME t {};
    GetLocalTime (&t);
    wchar_t whyW[16] = {};
    for (int i = 0; why != nullptr && why[i] != 0 && i < 15; ++i) whyW[i] = (wchar_t) why[i];
    wchar_t name[1300] = {};
    std::swprintf (name, sizeof (name) / sizeof (name[0]), L"%ls\\Fermata %ls %ls %04u-%02u-%02u %02u%02u%02u.dmp", folderPath, versionText, whyW,
                   (unsigned) t.wYear, (unsigned) t.wMonth, (unsigned) t.wDay, (unsigned) t.wHour, (unsigned) t.wMinute, (unsigned) t.wSecond);
    HANDLE file = CreateFileW (name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE)
    {
        DumpExceptionInfo info { GetCurrentThreadId(), exceptionPointers, FALSE };
        // every thread's stack and registers, the memory they point at, the modules (loaded and unloaded) and the threads' times: a few megabytes
        const int type = 0x00000040 /* WithIndirectlyReferencedMemory */ | 0x00000020 /* WithUnloadedModules */ | 0x00001000 /* WithThreadInfo */
                       | 0x00000100 /* WithProcessThreadData */;
        ok = fn (GetCurrentProcess(), GetCurrentProcessId(), file, type, exceptionPointers != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
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
