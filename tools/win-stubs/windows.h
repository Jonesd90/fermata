// Stand-in for <windows.h>, only for tools/check-windows-code.sh: it declares just the calls CoreReservation.cpp makes, with the signatures of the real Windows SDK,
// so that the Windows part of that file can be compiled (syntax only) on Linux. It is never used in the real build.
#pragma once
#include <cstdint>
#include <cstddef>
typedef unsigned long ULONG; typedef ULONG* PULONG; typedef unsigned long DWORD; typedef DWORD* LPDWORD; typedef int BOOL; typedef long LONG; typedef unsigned short WORD; typedef unsigned char BYTE;
typedef void* HANDLE; typedef void* LPVOID; typedef void* PVOID; typedef const wchar_t* LPCWSTR; typedef void* HMODULE;
#define WINAPI
#define NTAPI
// the real header leaves these behind (windef.h / rpcndr.h): a file that includes windows.h must cope with them, as FileSync.h does
#define near
#define far
#define pascal
#define small char
#define hyper long long
#ifndef NOMINMAX
 #define min(a, b) (((a) < (b)) ? (a) : (b))
 #define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE) (intptr_t) -1)
#define ERROR_INSUFFICIENT_BUFFER 122L
#define TH32CS_SNAPPROCESS 0x2
#define PROCESS_SET_LIMITED_INFORMATION 0x2000
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define THREAD_SET_LIMITED_INFORMATION 0x0400
#define ALL_PROCESSOR_GROUPS 0xffff
struct PROCESSENTRY32W { DWORD dwSize, cntUsage, th32ProcessID; uintptr_t th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; wchar_t szExeFile[260]; };
typedef int (*FARPROC_STUB)();
HMODULE GetModuleHandleW (LPCWSTR); HMODULE LoadLibraryW (LPCWSTR); FARPROC_STUB GetProcAddress (HMODULE, const char*);
HANDLE GetCurrentProcess(); HANDLE GetCurrentThread(); DWORD GetCurrentProcessId(); DWORD GetLastError(); BOOL CloseHandle (HANDLE);
HANDLE OpenProcess (DWORD, BOOL, DWORD); HANDLE OpenThread (DWORD, BOOL, DWORD);
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
BOOL SetThreadPriority (HANDLE, int);
HANDLE CreateToolhelp32Snapshot (DWORD, DWORD); BOOL Process32FirstW (HANDLE, PROCESSENTRY32W*); BOOL Process32NextW (HANDLE, PROCESSENTRY32W*);
DWORD GetActiveProcessorCount (WORD);
// processor times, power and priority (3.13.0)
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
typedef FILETIME* LPFILETIME;
BOOL GetSystemTimes (LPFILETIME, LPFILETIME, LPFILETIME);
BOOL GetProcessTimes (HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
BOOL GetThreadTimes (HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
DWORD GetCurrentThreadId();
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
struct SYSTEM_POWER_STATUS { BYTE ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag; DWORD BatteryLifeTime, BatteryFullLifeTime; };
typedef SYSTEM_POWER_STATUS* LPSYSTEM_POWER_STATUS;
BOOL GetSystemPowerStatus (LPSYSTEM_POWER_STATUS);
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
// crash reports (3.14.0)
typedef unsigned long long ULONG_PTR; typedef wchar_t* LPWSTR;
#define CREATE_ALWAYS 2
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
void GetLocalTime (SYSTEMTIME*);
BOOL DeleteFileW (LPCWSTR);
BOOL GetModuleHandleExW (DWORD, LPCWSTR, HMODULE*);
DWORD GetModuleFileNameW (HMODULE, LPWSTR, DWORD);
// the threads of the process (3.15.0)
#define TH32CS_SNAPTHREAD 0x4
struct THREADENTRY32 { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; };
BOOL Thread32First (HANDLE, THREADENTRY32*); BOOL Thread32Next (HANDLE, THREADENTRY32*);
HANDLE LocalFree (HANDLE);
// thread stacks and the helper process (3.17.0)
typedef unsigned long long DWORD64; typedef DWORD64* PDWORD64; typedef unsigned long long ULONG64; typedef char CHAR; typedef wchar_t* LPWSTR2;
struct CONTEXT { DWORD ContextFlags; DWORD64 Rip, Rsp; };
#define CONTEXT_FULL 0x10000B
struct RUNTIME_FUNCTION { DWORD BeginAddress, EndAddress, UnwindData; }; typedef RUNTIME_FUNCTION* PRUNTIME_FUNCTION;
struct UNWIND_HISTORY_TABLE; struct KNONVOLATILE_CONTEXT_POINTERS;
PRUNTIME_FUNCTION RtlLookupFunctionEntry (DWORD64, PDWORD64, UNWIND_HISTORY_TABLE*);
void* RtlVirtualUnwind (DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, CONTEXT*, void**, PDWORD64, KNONVOLATILE_CONTEXT_POINTERS*);
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_QUERY_INFORMATION 0x0040
DWORD SuspendThread (HANDLE); DWORD ResumeThread (HANDLE); BOOL GetThreadContext (HANDLE, CONTEXT*);
struct STARTUPINFOW { DWORD cb; void* rest[12]; };
struct PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; };
BOOL CreateProcessW (LPCWSTR, wchar_t*, void*, void*, BOOL, DWORD, void*, LPCWSTR, STARTUPINFOW*, PROCESS_INFORMATION*);
DWORD WaitForSingleObject (HANDLE, DWORD); BOOL GetExitCodeProcess (HANDLE, LPDWORD); BOOL TerminateProcess (HANDLE, unsigned int);
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_VM_READ 0x0010
BOOL SetPriorityClass (HANDLE, DWORD);
// file API (src/core/FileSync.h)
typedef void* LPSECURITY_ATTRIBUTES;
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
HANDLE CreateFileW (LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL FlushFileBuffers (HANDLE);
