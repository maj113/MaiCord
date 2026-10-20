#pragma once

// kernel32: process creation, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#define CREATE_NO_WINDOW 0x08000000
#define STARTF_USESHOWWINDOW 0x00000001
#define PROC_THREAD_ATTRIBUTE_NUMBER 0x0000FFFF
#define PROC_THREAD_ATTRIBUTE_THREAD 0x00010000
#define PROC_THREAD_ATTRIBUTE_INPUT 0x00020000
#define PROC_THREAD_ATTRIBUTE_ADDITIVE 0x00040000
#define ProcThreadAttributeValue(Number, Thread, Input, Additive) (((Number) & PROC_THREAD_ATTRIBUTE_NUMBER) | ((Thread != FALSE) ? PROC_THREAD_ATTRIBUTE_THREAD : 0) | ((Input != FALSE) ? PROC_THREAD_ATTRIBUTE_INPUT : 0) | ((Additive != FALSE) ? PROC_THREAD_ATTRIBUTE_ADDITIVE : 0))
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST ProcThreadAttributeValue (ProcThreadAttributeHandleList, FALSE, TRUE, FALSE)
typedef struct _PROC_THREAD_ATTRIBUTE_LIST* LPPROC_THREAD_ATTRIBUTE_LIST;

MAICORD_SYS_TYPES_BEGIN
// <processthreadsapi.h>
struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
};
typedef _PROCESS_INFORMATION PROCESS_INFORMATION;
typedef _PROCESS_INFORMATION* LPPROCESS_INFORMATION;
struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
};
typedef _STARTUPINFOW STARTUPINFOW;
typedef _STARTUPINFOW* LPSTARTUPINFOW;
// <winbase.h>
struct _STARTUPINFOEXW {
    STARTUPINFOW StartupInfo;
    LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList;
};
typedef _STARTUPINFOEXW STARTUPINFOEXW;
enum _PROC_THREAD_ATTRIBUTE_NUM {
    ProcThreadAttributeParentProcess = 0,
    ProcThreadAttributeHandleList = 2,
    ProcThreadAttributeGroupAffinity = 3,
    ProcThreadAttributePreferredNode = 4,
    ProcThreadAttributeIdealProcessor = 5,
    ProcThreadAttributeUmsThread = 6,
    ProcThreadAttributeMitigationPolicy = 7,
    ProcThreadAttributeSecurityCapabilities = 9,
    ProcThreadAttributeProtectionLevel = 11,
    ProcThreadAttributeJobList = 13,
    ProcThreadAttributeChildProcessPolicy = 14,
    ProcThreadAttributeAllApplicationPackagesPolicy = 15,
    ProcThreadAttributeWin32kFilter = 16,
    ProcThreadAttributeSafeOpenPromptOriginClaim = 17,
    ProcThreadAttributeDesktopAppPolicy = 18,
    ProcThreadAttributePseudoConsole = 22,
    ProcThreadAttributeMitigationAuditPolicy = 24,
    ProcThreadAttributeMachineType = 25,
    ProcThreadAttributeComponentFilter = 26,
    ProcThreadAttributeEnableOptionalXStateFeatures = 27,
    ProcThreadAttributeTrustedApp = 29,
    ProcThreadAttributeSveVectorLength = 30
};
MAICORD_SYS_TYPES_END

extern "C" __declspec(dllimport) BOOL WINAPI CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
    DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation);
extern "C" __declspec(dllimport) void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo);
extern "C" __declspec(dllimport) BOOL WINAPI InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    DWORD dwAttributeCount, DWORD dwFlags, PSIZE_T lpSize);
extern "C" __declspec(dllimport) void WINAPI DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList);
extern "C" __declspec(dllimport) BOOL WINAPI UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    DWORD dwFlags, DWORD_PTR Attribute, PVOID lpValue, SIZE_T cbSize, PVOID lpPreviousValue, PSIZE_T lpReturnSize);
