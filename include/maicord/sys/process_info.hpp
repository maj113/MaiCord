#pragma once

// Process information: PSAPI, Tool Help and process snapshots, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <processsnapshot.h>
#endif

// <psapi.h>
#define PSAPI_VERSION 2
#define GetProcessMemoryInfo K32GetProcessMemoryInfo
#define QueryWorkingSetEx K32QueryWorkingSetEx

// <tlhelp32.h>
#define TH32CS_SNAPPROCESS 0x00000002

struct HPSS__;
struct HPSSWALK__;
typedef struct HPSS__* HPSS;
typedef struct HPSSWALK__* HPSSWALK;

MAICORD_SYS_TYPES_BEGIN
// <psapi.h>
struct _MODULEINFO {
    LPVOID lpBaseOfDll;
    DWORD SizeOfImage;
    LPVOID EntryPoint;
};
typedef _MODULEINFO MODULEINFO;
typedef _MODULEINFO* LPMODULEINFO;
union _PSAPI_WORKING_SET_EX_BLOCK {
    ULONG_PTR Flags;
    union {
        struct {
            ULONG_PTR Valid : 1;
            ULONG_PTR ShareCount : 3;
            ULONG_PTR Win32Protection : 11;
            ULONG_PTR Shared : 1;
            ULONG_PTR Node : 6;
            ULONG_PTR Locked : 1;
            ULONG_PTR LargePage : 1;
            ULONG_PTR Reserved : 7;
            ULONG_PTR Bad : 1;
            ULONG_PTR ReservedUlong : 32;
        };
        struct {
            ULONG_PTR Valid : 1;
            ULONG_PTR Reserved0 : 14;
            ULONG_PTR Shared : 1;
            ULONG_PTR Reserved1 : 15;
            ULONG_PTR Bad : 1;
            ULONG_PTR ReservedUlong : 32;
        } Invalid;
    };
};
typedef _PSAPI_WORKING_SET_EX_BLOCK PSAPI_WORKING_SET_EX_BLOCK;
struct _PSAPI_WORKING_SET_EX_INFORMATION {
    PVOID VirtualAddress;
    PSAPI_WORKING_SET_EX_BLOCK VirtualAttributes;
};
typedef _PSAPI_WORKING_SET_EX_INFORMATION PSAPI_WORKING_SET_EX_INFORMATION;
struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
};
typedef _PROCESS_MEMORY_COUNTERS PROCESS_MEMORY_COUNTERS;
typedef PROCESS_MEMORY_COUNTERS* PPROCESS_MEMORY_COUNTERS;
struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
};
typedef _PROCESS_MEMORY_COUNTERS_EX PROCESS_MEMORY_COUNTERS_EX;
struct _PROCESS_MEMORY_COUNTERS_EX2 {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
typedef _PROCESS_MEMORY_COUNTERS_EX2 PROCESS_MEMORY_COUNTERS_EX2;
// <tlhelp32.h>
struct tagPROCESSENTRY32W {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[260];
};
typedef tagPROCESSENTRY32W PROCESSENTRY32W;
typedef PROCESSENTRY32W* LPPROCESSENTRY32W;
// <processsnapshot.h>
typedef enum {
    PSS_HANDLE_HAVE_NAME = 0x02,
    PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION = 0x08
} PSS_HANDLE_FLAGS;
typedef enum {
    PSS_OBJECT_TYPE_THREAD = 2,
} PSS_OBJECT_TYPE;
typedef enum {
    PSS_CAPTURE_HANDLES = 0x00000004,
    PSS_CAPTURE_HANDLE_NAME_INFORMATION = 0x00000008,
    PSS_CAPTURE_HANDLE_BASIC_INFORMATION = 0x00000010,
    PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION = 0x00000020,
    PSS_CREATE_USE_VM_ALLOCATIONS = 0x20000000,
    PSS_CREATE_RELEASE_SECTION = 0x80000000
} PSS_CAPTURE_FLAGS;
typedef enum {
    PSS_WALK_HANDLES = 2,
} PSS_WALK_INFORMATION_CLASS;
typedef struct {
    HANDLE Handle;
    PSS_HANDLE_FLAGS Flags;
    PSS_OBJECT_TYPE ObjectType;
    FILETIME CaptureTime;
    DWORD Attributes;
    DWORD GrantedAccess;
    DWORD HandleCount;
    DWORD PointerCount;
    DWORD PagedPoolCharge;
    DWORD NonPagedPoolCharge;
    FILETIME CreationTime;
    WORD TypeNameLength;
    wchar_t const* TypeName;
    WORD ObjectNameLength;
    wchar_t const* ObjectName;
    union {
        struct {
            DWORD ExitStatus;
            void* PebBaseAddress;
            ULONG_PTR AffinityMask;
            LONG BasePriority;
            DWORD ProcessId;
            DWORD ParentProcessId;
            DWORD Flags;
        } Process;
        struct {
            DWORD ExitStatus;
            void* TebBaseAddress;
            DWORD ProcessId;
            DWORD ThreadId;
            ULONG_PTR AffinityMask;
            int Priority;
            int BasePriority;
            void* Win32StartAddress;
        } Thread;
        struct {
            LONG CurrentCount;
            BOOL Abandoned;
            DWORD OwnerProcessId;
            DWORD OwnerThreadId;
        } Mutant;
        struct {
            BOOL ManualReset;
            BOOL Signaled;
        } Event;
        struct {
            void* BaseAddress;
            DWORD AllocationAttributes;
            LARGE_INTEGER MaximumSize;
        } Section;
        struct {
            LONG CurrentCount;
            LONG MaximumCount;
        } Semaphore;
    } TypeSpecificInformation;
} PSS_HANDLE_ENTRY;
typedef struct {
    void* Context;
    void* (__stdcall* AllocRoutine)(void* Context, DWORD Size);
    void(__stdcall* FreeRoutine)(void* Context, void* Address);
} PSS_ALLOCATOR;
MAICORD_SYS_TYPES_END

// <psapi.h>
extern "C" BOOL WINAPI K32EnumProcessModules(HANDLE hProcess, HMODULE* lphModule, DWORD cb, LPDWORD lpcbNeeded);
extern "C" DWORD WINAPI K32GetModuleBaseNameW(HANDLE hProcess, HMODULE hModule, LPWSTR lpBaseName, DWORD nSize);
extern "C" DWORD WINAPI K32GetModuleFileNameExW(HANDLE hProcess, HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
extern "C" BOOL WINAPI K32GetModuleInformation(HANDLE hProcess, HMODULE hModule, LPMODULEINFO lpmodinfo, DWORD cb);
extern "C" DWORD WINAPI K32GetMappedFileNameW(HANDLE hProcess, LPVOID lpv, LPWSTR lpFilename, DWORD nSize);
extern "C" BOOL WINAPI K32QueryWorkingSetEx(HANDLE hProcess, PVOID pv, DWORD cb);
extern "C" BOOL WINAPI K32GetProcessMemoryInfo(HANDLE Process, PPROCESS_MEMORY_COUNTERS ppsmemCounters, DWORD cb);

// <tlhelp32.h>
extern "C" HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
extern "C" BOOL WINAPI Process32FirstW(HANDLE hSnapshot, LPPROCESSENTRY32W lppe);
extern "C" BOOL WINAPI Process32NextW(HANDLE hSnapshot, LPPROCESSENTRY32W lppe);

// <processsnapshot.h>
extern "C" DWORD WINAPI PssCaptureSnapshot(HANDLE ProcessHandle, PSS_CAPTURE_FLAGS CaptureFlags,
    DWORD ThreadContextFlags, HPSS* SnapshotHandle);
extern "C" DWORD WINAPI PssFreeSnapshot(HANDLE ProcessHandle, HPSS SnapshotHandle);
extern "C" DWORD WINAPI PssWalkSnapshot(HPSS SnapshotHandle, PSS_WALK_INFORMATION_CLASS InformationClass,
    HPSSWALK WalkMarkerHandle, void* Buffer, DWORD BufferLength);
extern "C" DWORD WINAPI PssWalkMarkerCreate(PSS_ALLOCATOR const* Allocator, HPSSWALK* WalkMarkerHandle);
extern "C" DWORD WINAPI PssWalkMarkerFree(HPSSWALK WalkMarkerHandle);
