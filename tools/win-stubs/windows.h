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
