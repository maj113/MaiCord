#pragma once

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_types.hpp"

// The kernel32 functions and constants MaiCord calls, declared exactly as the
// Windows SDK declares them (x64) so <Windows.h> may come before or after.
// Sources that need nothing else from the SDK include this instead of
// <Windows.h>. src/core/win32_sdk_conformance.cpp checks every declaration
// against the SDK; records held by value are in win32_records.hpp.

typedef unsigned char BYTE;
typedef short SHORT;
typedef unsigned int UINT32;
typedef unsigned long ULONG;
typedef __int64 LONGLONG;
typedef unsigned __int64 ULONGLONG;
typedef unsigned __int64 ULONG64;
typedef __int64 LONG_PTR;
typedef __int64 INT_PTR;
typedef ULONG_PTR SIZE_T;
typedef ULONG_PTR DWORD_PTR;
typedef INT_PTR(WINAPI* FARPROC)();
typedef DWORD(WINAPI* PTHREAD_START_ROUTINE)(void* lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;
struct _SECURITY_ATTRIBUTES;
typedef _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;
struct _OVERLAPPED;
typedef _OVERLAPPED OVERLAPPED;
union _LARGE_INTEGER;
typedef _LARGE_INTEGER LARGE_INTEGER;
union _ULARGE_INTEGER;
typedef _ULARGE_INTEGER ULARGE_INTEGER;
struct _SYSTEM_INFO;
typedef _SYSTEM_INFO SYSTEM_INFO;
struct _MEMORY_BASIC_INFORMATION;
typedef _MEMORY_BASIC_INFORMATION MEMORY_BASIC_INFORMATION;
struct _BY_HANDLE_FILE_INFORMATION;
typedef _BY_HANDLE_FILE_INFORMATION BY_HANDLE_FILE_INFORMATION;
struct _SYSTEMTIME;
typedef _SYSTEMTIME SYSTEMTIME;
struct _TIME_ZONE_INFORMATION;
typedef _TIME_ZONE_INFORMATION TIME_ZONE_INFORMATION;
struct _TP_CALLBACK_INSTANCE;
typedef _TP_CALLBACK_INSTANCE TP_CALLBACK_INSTANCE;
typedef TP_CALLBACK_INSTANCE* PTP_CALLBACK_INSTANCE;
struct _TP_CALLBACK_ENVIRON_V3;
typedef _TP_CALLBACK_ENVIRON_V3 TP_CALLBACK_ENVIRON;
typedef TP_CALLBACK_ENVIRON* PTP_CALLBACK_ENVIRON;
struct _TP_TIMER;
typedef _TP_TIMER TP_TIMER;
typedef TP_TIMER* PTP_TIMER;
typedef void(WINAPI TP_TIMER_CALLBACK)(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER);
typedef TP_TIMER_CALLBACK* PTP_TIMER_CALLBACK;
struct _TP_WORK;
typedef _TP_WORK TP_WORK;
typedef TP_WORK* PTP_WORK;
typedef void(WINAPI TP_WORK_CALLBACK)(PTP_CALLBACK_INSTANCE, void*, PTP_WORK);
typedef TP_WORK_CALLBACK* PTP_WORK_CALLBACK;

#define MAX_PATH 260
#define MAXDWORD 0xffffffff
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define CopyMemory RtlCopyMemory
#define RtlCopyMemory(Destination,Source,Length) memcpy((Destination),(Source),(Length))
#define ZeroMemory RtlZeroMemory
#define RtlZeroMemory(Destination,Length) memset((Destination),0,(Length))
extern "C" void* __cdecl memcpy(void* _Dst, void const* _Src, unsigned __int64 _Size);
extern "C" void* __cdecl memset(void* _Dst, int _Val, unsigned __int64 _Size);

// Errors.
#define _HRESULT_TYPEDEF_(_sc) ((HRESULT)_sc)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_NOTIMPL _HRESULT_TYPEDEF_(0x80004001L)
#define E_NOINTERFACE _HRESULT_TYPEDEF_(0x80004002L)
#define E_POINTER _HRESULT_TYPEDEF_(0x80004003L)
#define E_FAIL _HRESULT_TYPEDEF_(0x80004005L)
#define E_UNEXPECTED _HRESULT_TYPEDEF_(0x8000FFFFL)
#define E_OUTOFMEMORY _HRESULT_TYPEDEF_(0x8007000EL)
#define E_INVALIDARG _HRESULT_TYPEDEF_(0x80070057L)
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_DATA 13L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_WRITE_FAULT 29L
#define ERROR_READ_FAULT 30L
#define ERROR_GEN_FAILURE 31L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_HANDLE_EOF 38L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_DISK_FULL 112L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_BAD_PATHNAME 161L
#define ERROR_BUSY 170L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_FILE_TOO_LARGE 223L
#define ERROR_MORE_DATA 234L
#define ERROR_DIRECTORY 267L
#define ERROR_ARITHMETIC_OVERFLOW 534L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_PENDING 997L
#define ERROR_FILE_INVALID 1006L
#define ERROR_PROCESS_ABORTED 1067L
#define ERROR_NOT_FOUND 1168L
#define ERROR_CANCELLED 1223L
#define ERROR_RETRY 1237L
#define ERROR_REVISION_MISMATCH 1306L
#define ERROR_TIMEOUT 1460L
#define ERROR_INVALID_STATE 5023L
#define WSAEMSGSIZE 10040L
extern "C" __declspec(dllimport) DWORD WINAPI GetLastError();
extern "C" __declspec(dllimport) void WINAPI SetLastError(DWORD dwErrCode);

// Handles.
#define DUPLICATE_SAME_ACCESS 0x00000002
#define HANDLE_FLAG_INHERIT 0x00000001
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002
extern "C" __declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE hObject);
extern "C" __declspec(dllimport) BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
    HANDLE hTargetProcessHandle, HANDLE* lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle,
    DWORD dwOptions);
extern "C" __declspec(dllimport) BOOL WINAPI GetHandleInformation(HANDLE hObject, DWORD* lpdwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);

// Memory.
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_IMAGE 0x01000000
#define SECTION_MAP_WRITE 0x0002
#define SECTION_MAP_READ 0x0004
#define FILE_MAP_WRITE SECTION_MAP_WRITE
#define FILE_MAP_READ SECTION_MAP_READ
#define HEAP_ZERO_MEMORY 0x00000008
extern "C" __declspec(dllimport) void* WINAPI VirtualAlloc(void* lpAddress, SIZE_T dwSize, DWORD flAllocationType,
    DWORD flProtect);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualProtect(void* lpAddress, SIZE_T dwSize, DWORD flNewProtect,
    DWORD* lpflOldProtect);
extern "C" __declspec(dllimport) SIZE_T WINAPI VirtualQuery(const void* lpAddress,
    MEMORY_BASIC_INFORMATION* lpBuffer, SIZE_T dwLength);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualLock(void* lpAddress, SIZE_T dwSize);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualUnlock(void* lpAddress, SIZE_T dwSize);
extern "C" __declspec(dllimport) void WINAPI GetSystemInfo(SYSTEM_INFO* lpSystemInfo);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFileMappingW(HANDLE hFile,
    SECURITY_ATTRIBUTES* lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
    const wchar_t* lpName);
extern "C" __declspec(dllimport) HANDLE WINAPI OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle,
    const wchar_t* lpName);
extern "C" __declspec(dllimport) void* WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
extern "C" __declspec(dllimport) BOOL WINAPI UnmapViewOfFile(const void* lpBaseAddress);
extern "C" __declspec(dllimport) HANDLE WINAPI GetProcessHeap();
extern "C" __declspec(dllimport) __declspec(allocator) void* WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags,
    SIZE_T dwBytes);
extern "C" __declspec(dllimport) BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, void* lpMem);

// Files.
#define GENERIC_READ (0x80000000L)
#define GENERIC_WRITE (0x40000000L)
#define DELETE (0x00010000L)
#define SYNCHRONIZE (0x00100000L)
#define FILE_READ_ATTRIBUTES ( 0x0080 )
#define FILE_WRITE_ATTRIBUTES ( 0x0100 )
#define FILE_APPEND_DATA ( 0x0004 )
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#define FILE_BEGIN 0
#define FILE_END 2
#define FILE_TYPE_DISK 0x0001
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_WRITE_THROUGH 0x00000008
#define FILE_NAME_NORMALIZED 0x0
#define VOLUME_NAME_DOS 0x0
#define DRIVE_FIXED 3
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFileW(const wchar_t* lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
extern "C" __declspec(dllimport) BOOL WINAPI ReadFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead,
    DWORD* lpNumberOfBytesRead, OVERLAPPED* lpOverlapped);
extern "C" __declspec(dllimport) BOOL WINAPI WriteFile(HANDLE hFile, const void* lpBuffer,
    DWORD nNumberOfBytesToWrite, DWORD* lpNumberOfBytesWritten, OVERLAPPED* lpOverlapped);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileSizeEx(HANDLE hFile, LARGE_INTEGER* lpFileSize);
extern "C" __declspec(dllimport) BOOL WINAPI FlushFileBuffers(HANDLE hFile);
extern "C" __declspec(dllimport) BOOL WINAPI SetEndOfFile(HANDLE hFile);
extern "C" __declspec(dllimport) DWORD WINAPI GetFileType(HANDLE hFile);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileInformationByHandle(HANDLE hFile,
    BY_HANDLE_FILE_INFORMATION* lpFileInformation);
extern "C" __declspec(dllimport) DWORD WINAPI GetFinalPathNameByHandleW(HANDLE hFile, wchar_t* lpszFilePath,
    DWORD cchFilePath, DWORD dwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI DeleteFileW(const wchar_t* lpFileName);
extern "C" __declspec(dllimport) BOOL WINAPI MoveFileExW(const wchar_t* lpExistingFileName,
    const wchar_t* lpNewFileName, DWORD dwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI CopyFileW(const wchar_t* lpExistingFileName,
    const wchar_t* lpNewFileName, BOOL bFailIfExists);
extern "C" __declspec(dllimport) DWORD WINAPI GetFileAttributesW(const wchar_t* lpFileName);
extern "C" __declspec(dllimport) BOOL WINAPI SetFileAttributesW(const wchar_t* lpFileName, DWORD dwFileAttributes);
extern "C" __declspec(dllimport) BOOL WINAPI CreateDirectoryW(const wchar_t* lpPathName,
    SECURITY_ATTRIBUTES* lpSecurityAttributes);
extern "C" __declspec(dllimport) BOOL WINAPI RemoveDirectoryW(const wchar_t* lpPathName);
extern "C" __declspec(dllimport) DWORD WINAPI GetTempPathW(DWORD nBufferLength, wchar_t* lpBuffer);
extern "C" __declspec(dllimport) UINT WINAPI GetTempFileNameW(const wchar_t* lpPathName,
    const wchar_t* lpPrefixString, UINT uUnique, wchar_t* lpTempFileName);
extern "C" __declspec(dllimport) UINT WINAPI GetDriveTypeW(const wchar_t* lpRootPathName);
extern "C" __declspec(dllimport) BOOL WINAPI GetDiskFreeSpaceExW(const wchar_t* lpDirectoryName,
    ULARGE_INTEGER* lpFreeBytesAvailableToCaller, ULARGE_INTEGER* lpTotalNumberOfBytes,
    ULARGE_INTEGER* lpTotalNumberOfFreeBytes);

// Synchronization and threads.
#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchange _InterlockedExchange
#define InterlockedExchangeAdd _InterlockedExchangeAdd
#define InterlockedCompareExchange _InterlockedCompareExchange
#define InterlockedOr _InterlockedOr
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#define InterlockedIncrement64 _InterlockedIncrement64
#define InterlockedExchange64 _InterlockedExchange64
#define InterlockedExchangeAdd64 _InterlockedExchangeAdd64
#define YieldProcessor _mm_pause
extern "C" void _mm_pause(void);
#define INFINITE 0xFFFFFFFF
#define STATUS_WAIT_0 ((DWORD )0x00000000L)
#define STATUS_ABANDONED_WAIT_0 ((DWORD )0x00000080L)
#define WAIT_OBJECT_0 ((STATUS_WAIT_0 ) + 0 )
#define WAIT_ABANDONED ((STATUS_ABANDONED_WAIT_0 ) + 0 )
#define WAIT_TIMEOUT 258L
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_BASE_PRIORITY_LOWRT 15
#define THREAD_BASE_PRIORITY_MAX 2
#define THREAD_PRIORITY_HIGHEST THREAD_BASE_PRIORITY_MAX
#define THREAD_PRIORITY_TIME_CRITICAL THREAD_BASE_PRIORITY_LOWRT
extern "C" __declspec(dllimport) void WINAPI InitializeSRWLock(SRWLOCK* SRWLock);
extern "C" __declspec(dllimport) void WINAPI AcquireSRWLockExclusive(SRWLOCK* SRWLock);
extern "C" __declspec(dllimport) void WINAPI ReleaseSRWLockExclusive(SRWLOCK* SRWLock);
extern "C" __declspec(dllimport) void WINAPI AcquireSRWLockShared(SRWLOCK* SRWLock);
extern "C" __declspec(dllimport) void WINAPI ReleaseSRWLockShared(SRWLOCK* SRWLock);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES* lpEventAttributes,
    BOOL bManualReset, BOOL bInitialState, const wchar_t* lpName);
extern "C" __declspec(dllimport) BOOL WINAPI SetEvent(HANDLE hEvent);
extern "C" __declspec(dllimport) BOOL WINAPI ResetEvent(HANDLE hEvent);
extern "C" __declspec(dllimport) DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
extern "C" __declspec(dllimport) DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles,
    BOOL bWaitAll, DWORD dwMilliseconds);
extern "C" __declspec(dllimport) void WINAPI Sleep(DWORD dwMilliseconds);
extern "C" __declspec(dllimport) BOOL WINAPI SwitchToThread();
extern "C" __declspec(dllimport) HANDLE WINAPI CreateThread(SECURITY_ATTRIBUTES* lpThreadAttributes,
    SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, void* lpParameter, DWORD dwCreationFlags,
    DWORD* lpThreadId);
extern "C" __declspec(dllimport) BOOL WINAPI GetExitCodeThread(HANDLE hThread, DWORD* lpExitCode);
extern "C" __declspec(dllimport) BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
extern "C" __declspec(dllimport) HANDLE WINAPI GetCurrentThread();
extern "C" __declspec(dllimport) DWORD WINAPI GetCurrentThreadId();
extern "C" __declspec(dllimport) PTP_TIMER WINAPI CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti, void* pv,
    PTP_CALLBACK_ENVIRON pcbe);
extern "C" __declspec(dllimport) void WINAPI SetThreadpoolTimer(PTP_TIMER pti, FILETIME* pftDueTime,
    DWORD msPeriod, DWORD msWindowLength);
extern "C" __declspec(dllimport) void WINAPI WaitForThreadpoolTimerCallbacks(PTP_TIMER pti,
    BOOL fCancelPendingCallbacks);
extern "C" __declspec(dllimport) void WINAPI CloseThreadpoolTimer(PTP_TIMER pti);
extern "C" __declspec(dllimport) PTP_WORK WINAPI CreateThreadpoolWork(PTP_WORK_CALLBACK pfnwk, void* pv,
    PTP_CALLBACK_ENVIRON pcbe);
extern "C" __declspec(dllimport) void WINAPI SubmitThreadpoolWork(PTP_WORK pwk);
extern "C" __declspec(dllimport) void WINAPI WaitForThreadpoolWorkCallbacks(PTP_WORK pwk,
    BOOL fCancelPendingCallbacks);
extern "C" __declspec(dllimport) void WINAPI CloseThreadpoolWork(PTP_WORK pwk);

// Processes, modules and the environment.
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT (0x00000002)
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x00000004)
extern "C" __declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT uExitCode);
extern "C" __declspec(dllimport) BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode);
extern "C" __declspec(dllimport) HANDLE WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) DWORD WINAPI GetCurrentProcessId();
extern "C" __declspec(dllimport) BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, DWORD* lpExitCode);
extern "C" __declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
extern "C" __declspec(dllimport) wchar_t* WINAPI GetCommandLineW();
extern "C" __declspec(dllimport) DWORD WINAPI GetEnvironmentVariableW(const wchar_t* lpName, wchar_t* lpBuffer,
    DWORD nSize);
extern "C" __declspec(dllimport) BOOL WINAPI SetEnvironmentVariableW(const wchar_t* lpName,
    const wchar_t* lpValue);
extern "C" __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const wchar_t* lpModuleName);
extern "C" __declspec(dllimport) BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, const wchar_t* lpModuleName,
    HMODULE* phModule);
extern "C" __declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HMODULE hModule, wchar_t* lpFilename,
    DWORD nSize);
extern "C" __declspec(dllimport) FARPROC WINAPI GetProcAddress(HMODULE hModule, const char* lpProcName);
extern "C" __declspec(dllimport) HMODULE WINAPI LoadLibraryExW(const wchar_t* lpLibFileName, HANDLE hFile,
    DWORD dwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI FreeLibrary(HMODULE hLibModule);
extern "C" __declspec(dllimport) BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule);
extern "C" __declspec(dllimport) void WINAPI OutputDebugStringA(const char* lpOutputString);

// Time.
extern "C" __declspec(dllimport) BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
extern "C" __declspec(dllimport) BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
extern "C" __declspec(dllimport) DWORD WINAPI GetTickCount();
extern "C" __declspec(dllimport) ULONGLONG WINAPI GetTickCount64();
extern "C" __declspec(dllimport) void WINAPI GetSystemTimeAsFileTime(FILETIME* lpSystemTimeAsFileTime);
extern "C" __declspec(dllimport) void WINAPI GetLocalTime(SYSTEMTIME* lpSystemTime);
extern "C" __declspec(dllimport) BOOL WINAPI FileTimeToSystemTime(const FILETIME* lpFileTime,
    SYSTEMTIME* lpSystemTime);
extern "C" __declspec(dllimport) BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime,
    FILETIME* lpFileTime);
extern "C" __declspec(dllimport) BOOL WINAPI SystemTimeToTzSpecificLocalTime(
    const TIME_ZONE_INFORMATION* lpTimeZoneInformation, const SYSTEMTIME* lpUniversalTime,
    SYSTEMTIME* lpLocalTime);

// Strings.
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_ERR_INVALID_CHARS 0x00000080
#define CSTR_EQUAL 2
extern "C" __declspec(dllimport) int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags,
    const char* lpMultiByteStr, int cbMultiByte, wchar_t* lpWideCharStr, int cchWideChar);
extern "C" __declspec(dllimport) int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags,
    const wchar_t* lpWideCharStr, int cchWideChar, char* lpMultiByteStr, int cbMultiByte,
    const char* lpDefaultChar, BOOL* lpUsedDefaultChar);
extern "C" __declspec(dllimport) int WINAPI CompareStringOrdinal(const wchar_t* lpString1, int cchCount1,
    const wchar_t* lpString2, int cchCount2, BOOL bIgnoreCase);

namespace maicord {

// Records these functions fill: the SDK's layout and member names.

// LARGE_INTEGER.
struct Win32LargeInteger {
    LONGLONG QuadPart;

    LARGE_INTEGER* native() noexcept {
        return reinterpret_cast<LARGE_INTEGER*>(this);
    }
};

// SYSTEM_INFO.
struct Win32SystemInfo {
    DWORD dwOemId;
    DWORD dwPageSize;
    void* lpMinimumApplicationAddress;
    void* lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;

    SYSTEM_INFO* native() noexcept {
        return reinterpret_cast<SYSTEM_INFO*>(this);
    }
};

// MEMORY_BASIC_INFORMATION.
struct Win32MemoryInformation {
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;

    MEMORY_BASIC_INFORMATION* native() noexcept {
        return reinterpret_cast<MEMORY_BASIC_INFORMATION*>(this);
    }
};

} // namespace maicord
