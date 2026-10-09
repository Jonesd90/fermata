// Stand-in for <windows.h>, only for tools/check-windows-code.sh: it declares just the calls CoreReservation.cpp makes, with the signatures of the real Windows SDK,
// so that the Windows part of that file can be compiled (syntax only) on Linux. It is never used in the real build.
#pragma once
#include <cstdint>
#include <cstddef>
typedef unsigned long ULONG; typedef ULONG* PULONG; typedef unsigned long DWORD; typedef DWORD* LPDWORD; typedef int BOOL; typedef long LONG; typedef unsigned short WORD; typedef unsigned char BYTE;
typedef void* HANDLE; typedef void* LPVOID; typedef void* PVOID; typedef const wchar_t* LPCWSTR; typedef void* HMODULE;
#define WINAPI
#define NTAPI
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
HANDLE CreateToolhelp32Snapshot (DWORD, DWORD); BOOL Process32FirstW (HANDLE, PROCESSENTRY32W*); BOOL Process32NextW (HANDLE, PROCESSENTRY32W*);
DWORD GetActiveProcessorCount (WORD);
