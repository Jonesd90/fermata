// Stand-in for <dbghelp.h>, only for tools/check-windows-code.sh (the symbol calls CrashReport.cpp makes, with the real signatures). Never used in the real build.
#pragma once
#include "windows.h"
struct SYMBOL_INFO { ULONG SizeOfStruct, TypeIndex; ULONG64 Reserved[2]; ULONG Index, Size; ULONG64 ModBase; ULONG Flags; ULONG64 Value, Address; ULONG Register, Scope, Tag, NameLen, MaxNameLen; CHAR Name[1]; };
struct IMAGEHLP_LINE64 { DWORD SizeOfStruct; void* Key; DWORD LineNumber; char* FileName; DWORD64 Address; };
#define SYMOPT_UNDNAME 0x2
#define SYMOPT_DEFERRED_LOADS 0x4
#define SYMOPT_LOAD_LINES 0x10
DWORD SymSetOptions (DWORD); DWORD SymGetOptions();
BOOL SymInitialize (HANDLE, const char*, BOOL);
BOOL SymRefreshModuleList (HANDLE);
BOOL SymFromAddr (HANDLE, DWORD64, PDWORD64, SYMBOL_INFO*);
BOOL SymGetLineFromAddr64 (HANDLE, DWORD64, LPDWORD, IMAGEHLP_LINE64*);
