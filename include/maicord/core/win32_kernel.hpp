#pragma once

// kernel32: memory, files, synchronization, threads, modules, time and strings, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/sys/intrin.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

// <winnt.h>
#define NTAPI __stdcall
#define __TEXT(quote) L##quote
#define TEXT(quote) __TEXT(quote)
#define STDMETHODCALLTYPE __stdcall
#define MAXDWORD 0xffffffff
#ifdef __has_builtin
#if __has_builtin(__builtin_offsetof)
#define FIELD_OFFSET(type, field) ((LONG)__builtin_offsetof(type, field))
#endif
#endif
#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)(LONG_PTR)&(((type *)0)->field))
#endif
#define LOCALE_NAME_MAX_LENGTH 85
#define STATUS_WAIT_0 ((DWORD )0x00000000L)
#define STATUS_ABANDONED_WAIT_0 ((DWORD )0x00000080L)
#define InterlockedOr _InterlockedOr
#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchange _InterlockedExchange
#define InterlockedExchangeAdd _InterlockedExchangeAdd
#define InterlockedCompareExchange _InterlockedCompareExchange
#define InterlockedIncrement64 _InterlockedIncrement64
#define InterlockedAdd64 _InlineInterlockedAdd64
#define InterlockedExchange64 _InterlockedExchange64
#define InterlockedExchangeAdd64 _InterlockedExchangeAdd64
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#define InterlockedExchangePointer _InterlockedExchangePointer
#define InterlockedCompareExchangePointer _InterlockedCompareExchangePointer
#define YieldProcessor _mm_pause
#define UNW_FLAG_NHANDLER 0x0
#define DELETE (0x00010000L)
#define READ_CONTROL (0x00020000L)
#define SYNCHRONIZE (0x00100000L)
#define STANDARD_RIGHTS_REQUIRED (0x000F0000L)
#define STANDARD_RIGHTS_READ (READ_CONTROL)
#define GENERIC_READ (0x80000000L)
#define GENERIC_WRITE (0x40000000L)
#define THREAD_SUSPEND_RESUME (0x0002)
#define THREAD_GET_CONTEXT (0x0008)
#define THREAD_QUERY_INFORMATION (0x0040)
#define THREAD_BASE_PRIORITY_LOWRT 15
#define QUOTA_LIMITS_HARDWS_MIN_DISABLE 0x00000002
#define QUOTA_LIMITS_HARDWS_MAX_ENABLE 0x00000004
#define TIMER_QUERY_STATE 0x0001
#define TIMER_MODIFY_STATE 0x0002
#define TIMER_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE| TIMER_QUERY_STATE|TIMER_MODIFY_STATE)
#define SECTION_MAP_WRITE 0x0002
#define SECTION_MAP_READ 0x0004
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOMBINE 0x400
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define SEC_IMAGE 0x01000000
#define SEC_NOCACHE 0x10000000
#define SEC_IMAGE_NO_EXECUTE (SEC_IMAGE | SEC_NOCACHE)
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_IMAGE 0x01000000
#define FILE_APPEND_DATA ( 0x0004 )
#define FILE_READ_ATTRIBUTES ( 0x0080 )
#define FILE_WRITE_ATTRIBUTES ( 0x0100 )
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#define DUPLICATE_SAME_ACCESS 0x00000002
#define FAST_FAIL_FATAL_APP_EXIT 7
#define FAST_FAIL_INVALID_ARG 5
#define HEAP_ZERO_MEMORY 0x00000008
#define RtlCopyMemory(Destination,Source,Length) memcpy((Destination),(Source),(Length))
#define RtlFillMemory(Destination,Length,Fill) memset((Destination),(Fill),(Length))
#define RtlZeroMemory(Destination,Length) memset((Destination),0,(Length))
#define RTL_SRWLOCK_INIT {0}
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define KEY_QUERY_VALUE (0x0001)
#define KEY_SET_VALUE (0x0002)
#define KEY_CREATE_SUB_KEY (0x0004)
#define KEY_ENUMERATE_SUB_KEYS (0x0008)
#define KEY_NOTIFY (0x0010)
#define KEY_READ ((STANDARD_RIGHTS_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY) & (~SYNCHRONIZE))
#define REG_CREATED_NEW_KEY (0x00000001L)
#define REG_SZ ( 1ul )
#define REG_BINARY ( 3ul )

// <minwinbase.h>
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory
#define PROCESS_HEAP_REGION 0x0001
#define PROCESS_HEAP_ENTRY_BUSY 0x0004

// <fileapi.h>
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define DeleteFile DeleteFileW

// <handleapi.h>
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

// <synchapi.h>
#define SRWLOCK_INIT RTL_SRWLOCK_INIT
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002

// <memoryapi.h>
#define FILE_MAP_WRITE SECTION_MAP_WRITE
#define FILE_MAP_READ SECTION_MAP_READ

// <libloaderapi.h>
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT (0x00000002)
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x00000004)
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800

// <winbase.h>
#define FILE_BEGIN 0
#define FILE_END 2
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define WAIT_OBJECT_0 ((STATUS_WAIT_0 ) + 0 )
#define WAIT_ABANDONED ((STATUS_ABANDONED_WAIT_0 ) + 0 )
#define SecureZeroMemory RtlSecureZeroMemory
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_INVALID_HANDLE 0x8000
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_PRIORITY_TIME_CRITICAL THREAD_BASE_PRIORITY_LOWRT
#define VOLUME_NAME_DOS 0x0
#define FILE_NAME_NORMALIZED 0x0
#define DRIVE_FIXED 3
#define FILE_TYPE_DISK 0x0001
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define INFINITE 0xFFFFFFFF
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_WRITE_THROUGH 0x00000008

// <winnls.h>
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_ERR_INVALID_CHARS 0x00000080
#define CSTR_EQUAL 2
#define CP_UTF8 65001
#define LOCALE_NAME_USER_DEFAULT NULL

typedef DWORD(__stdcall* PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;
typedef void(__stdcall* PTIMERAPCROUTINE)(LPVOID lpArgToCompletionRoutine, DWORD dwTimerLowValue,
    DWORD dwTimerHighValue);

MAICORD_SYS_TYPES_BEGIN
struct _CONTEXT;
struct _EXCEPTION_POINTERS;
struct _EXCEPTION_RECORD;
struct _EXCEPTION_REGISTRATION_RECORD;
struct _FILE_ATTRIBUTE_TAG_INFO;
struct _FILE_BASIC_INFO;
struct _FILE_RENAME_INFO;
struct _FILE_STANDARD_INFO;
struct _IMAGE_DATA_DIRECTORY;
struct _IMAGE_DELAYLOAD_DESCRIPTOR;
struct _IMAGE_DOS_HEADER;
struct _IMAGE_FILE_HEADER;
struct _IMAGE_IMPORT_BY_NAME;
struct _IMAGE_IMPORT_DESCRIPTOR;
struct _IMAGE_NT_HEADERS64;
struct _IMAGE_OPTIONAL_HEADER64;
struct _IMAGE_RUNTIME_FUNCTION_ENTRY;
struct _IMAGE_THUNK_DATA64;
struct _KNONVOLATILE_CONTEXT_POINTERS;
struct _M128A;
struct _NT_TIB;
struct _PROCESS_INFORMATION;
struct _STARTUPINFOW;
struct _SYSTEMTIME;
struct _TIME_DYNAMIC_ZONE_INFORMATION;
struct _TIME_ZONE_INFORMATION;
struct _TP_CALLBACK_ENVIRON_V3;
struct _WIN32_FILE_ATTRIBUTE_DATA;
struct _XSAVE_FORMAT;

// <excpt.h>
#define EXCEPTION_CONTINUE_SEARCH 0
// <winnt.h>
union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
};
typedef _LARGE_INTEGER LARGE_INTEGER;
typedef LARGE_INTEGER* PLARGE_INTEGER;
union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
};
typedef _ULARGE_INTEGER ULARGE_INTEGER;
struct _LUID {
    DWORD LowPart;
    LONG HighPart;
};
typedef _LUID LUID;
struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
};
typedef _MEMORY_BASIC_INFORMATION MEMORY_BASIC_INFORMATION;
typedef _MEMORY_BASIC_INFORMATION* PMEMORY_BASIC_INFORMATION;
struct _OSVERSIONINFOW {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
};
typedef _OSVERSIONINFOW OSVERSIONINFOW;
// <minwinbase.h>
struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
};
typedef _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;
typedef _SECURITY_ATTRIBUTES* LPSECURITY_ATTRIBUTES;
struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
};
typedef _OVERLAPPED OVERLAPPED;
typedef _OVERLAPPED* LPOVERLAPPED;
struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
};
typedef _WIN32_FIND_DATAW WIN32_FIND_DATAW;
typedef _WIN32_FIND_DATAW* LPWIN32_FIND_DATAW;
enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo,
    FileStandardInfo,
    FileNameInfo,
    FileRenameInfo,
    FileDispositionInfo,
    FileAllocationInfo,
    FileEndOfFileInfo,
    FileStreamInfo,
    FileCompressionInfo,
    FileAttributeTagInfo,
    FileIdBothDirectoryInfo,
    FileIdBothDirectoryRestartInfo,
    FileIoPriorityHintInfo,
    FileRemoteProtocolInfo,
    FileFullDirectoryInfo,
    FileFullDirectoryRestartInfo,
    FileStorageInfo,
    FileAlignmentInfo,
    FileIdInfo,
    FileIdExtdDirectoryInfo,
    FileIdExtdDirectoryRestartInfo,
    FileDispositionInfoEx,
    FileRenameInfoEx,
};
typedef _FILE_INFO_BY_HANDLE_CLASS FILE_INFO_BY_HANDLE_CLASS;
struct _PROCESS_HEAP_ENTRY {
    PVOID lpData;
    DWORD cbData;
    BYTE cbOverhead;
    BYTE iRegionIndex;
    WORD wFlags;
    union {
        struct {
            HANDLE hMem;
            DWORD dwReserved[3];
        } Block;
        struct {
            DWORD dwCommittedSize;
            DWORD dwUnCommittedSize;
            LPVOID lpFirstBlock;
            LPVOID lpLastBlock;
        } Region;
    };
};
typedef _PROCESS_HEAP_ENTRY PROCESS_HEAP_ENTRY;
typedef _PROCESS_HEAP_ENTRY* LPPROCESS_HEAP_ENTRY;
struct _REASON_CONTEXT {
    ULONG Version;
    DWORD Flags;
    union {
        struct {
            HMODULE LocalizedReasonModule;
            ULONG LocalizedReasonId;
            ULONG ReasonStringCount;
            LPWSTR* ReasonStrings;
        } Detailed;
        LPWSTR SimpleReasonString;
    } Reason;
};
typedef _REASON_CONTEXT* PREASON_CONTEXT;
// <fileapi.h>
struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
};
typedef _BY_HANDLE_FILE_INFORMATION BY_HANDLE_FILE_INFORMATION;
typedef _BY_HANDLE_FILE_INFORMATION* LPBY_HANDLE_FILE_INFORMATION;
// <synchapi.h>
typedef RTL_SRWLOCK* PSRWLOCK;
// <sysinfoapi.h>
struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
};
typedef _SYSTEM_INFO SYSTEM_INFO;
typedef _SYSTEM_INFO* LPSYSTEM_INFO;
// <memoryapi.h>
enum _MEMORY_RESOURCE_NOTIFICATION_TYPE {
    LowMemoryResourceNotification,
};
typedef _MEMORY_RESOURCE_NOTIFICATION_TYPE MEMORY_RESOURCE_NOTIFICATION_TYPE;
// <winbase.h>
struct _FILE_DISPOSITION_INFO {
    BOOLEAN DeleteFileW;
};
typedef _FILE_DISPOSITION_INFO FILE_DISPOSITION_INFO;
MAICORD_SYS_TYPES_END

// <processenv.h>
extern "C" __declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
extern "C" __declspec(dllimport) LPWSTR WINAPI GetCommandLineW();
extern "C" __declspec(dllimport) DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
extern "C" __declspec(dllimport) DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);
extern "C" __declspec(dllimport) BOOL WINAPI SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue);

// <fileapi.h>
extern "C" __declspec(dllimport) BOOL WINAPI CreateDirectoryW(LPCWSTR lpPathName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
extern "C" __declspec(dllimport) BOOL WINAPI DeleteFileW(LPCWSTR lpFileName);
extern "C" __declspec(dllimport) BOOL WINAPI FindClose(HANDLE hFindFile);
extern "C" __declspec(dllimport) HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
extern "C" __declspec(dllimport) BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
extern "C" __declspec(dllimport) BOOL WINAPI FlushFileBuffers(HANDLE hFile);
extern "C" __declspec(dllimport) UINT WINAPI GetDriveTypeW(LPCWSTR lpRootPathName);
extern "C" __declspec(dllimport) DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileInformationByHandle(HANDLE hFile,
    LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
extern "C" __declspec(dllimport) DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
extern "C" __declspec(dllimport) DWORD WINAPI GetFileType(HANDLE hFile);
extern "C" __declspec(dllimport) DWORD WINAPI GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath,
    DWORD cchFilePath, DWORD dwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime,
    LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
extern "C" __declspec(dllimport) UINT WINAPI GetTempFileNameW(LPCWSTR lpPathName, LPCWSTR lpPrefixString, UINT uUnique,
    LPWSTR lpTempFileName);
extern "C" __declspec(dllimport) BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
extern "C" __declspec(dllimport) BOOL WINAPI RemoveDirectoryW(LPCWSTR lpPathName);
extern "C" __declspec(dllimport) BOOL WINAPI SetEndOfFile(HANDLE hFile);
extern "C" __declspec(dllimport) BOOL WINAPI SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes);
extern "C" __declspec(dllimport) BOOL WINAPI SetFileInformationByHandle(HANDLE hFile,
    FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize);
extern "C" __declspec(dllimport) BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
    PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod);
extern "C" __declspec(dllimport) BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime,
    const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime);
extern "C" __declspec(dllimport) BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
extern "C" __declspec(dllimport) DWORD WINAPI GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer);

// <handleapi.h>
extern "C" __declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE hObject);
extern "C" __declspec(dllimport) BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
    HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle,
    DWORD dwOptions);
extern "C" __declspec(dllimport) BOOL WINAPI GetHandleInformation(HANDLE hObject, LPDWORD lpdwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);

// <errhandlingapi.h>
extern "C" __declspec(dllimport) DWORD WINAPI GetLastError();
extern "C" __declspec(dllimport) void WINAPI SetLastError(DWORD dwErrCode);
extern "C" __declspec(dllimport) UINT WINAPI SetErrorMode(UINT uMode);

// <profileapi.h>
extern "C" __declspec(dllimport) BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
extern "C" __declspec(dllimport) BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);

// <heapapi.h>
extern "C" __declspec(dllimport) __declspec(allocator) LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags,
    SIZE_T dwBytes);
extern "C" __declspec(dllimport) __declspec(allocator) LPVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags,
    LPVOID lpMem, SIZE_T dwBytes);
extern "C" __declspec(dllimport) BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
extern "C" __declspec(dllimport) HANDLE WINAPI GetProcessHeap();
extern "C" __declspec(dllimport) DWORD WINAPI GetProcessHeaps(DWORD NumberOfHeaps, PHANDLE ProcessHeaps);
extern "C" __declspec(dllimport) BOOL WINAPI HeapLock(HANDLE hHeap);
extern "C" __declspec(dllimport) BOOL WINAPI HeapUnlock(HANDLE hHeap);
extern "C" __declspec(dllimport) BOOL WINAPI HeapWalk(HANDLE hHeap, LPPROCESS_HEAP_ENTRY lpEntry);

// <ioapiset.h>
extern "C" __declspec(dllimport) HANDLE WINAPI CreateIoCompletionPort(HANDLE FileHandle, HANDLE ExistingCompletionPort,
    ULONG_PTR CompletionKey, DWORD NumberOfConcurrentThreads);
extern "C" __declspec(dllimport) BOOL WINAPI GetQueuedCompletionStatus(HANDLE CompletionPort,
    LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey, LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds);
extern "C" __declspec(dllimport) BOOL WINAPI PostQueuedCompletionStatus(HANDLE CompletionPort,
    DWORD dwNumberOfBytesTransferred, ULONG_PTR dwCompletionKey, LPOVERLAPPED lpOverlapped);
extern "C" __declspec(dllimport) BOOL WINAPI DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer,
    DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned,
    LPOVERLAPPED lpOverlapped);
extern "C" __declspec(dllimport) BOOL WINAPI CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
extern "C" __declspec(dllimport) BOOL WINAPI CancelSynchronousIo(HANDLE hThread);

// <synchapi.h>
extern "C" __declspec(dllimport) void WINAPI InitializeSRWLock(PSRWLOCK SRWLock);
extern "C" __declspec(dllimport) void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
extern "C" __declspec(dllimport) void WINAPI ReleaseSRWLockShared(PSRWLOCK SRWLock);
extern "C" __declspec(dllimport) void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock);
extern "C" __declspec(dllimport) void WINAPI AcquireSRWLockShared(PSRWLOCK SRWLock);
extern "C" __declspec(dllimport) BOOL WINAPI SetEvent(HANDLE hEvent);
extern "C" __declspec(dllimport) BOOL WINAPI ResetEvent(HANDLE hEvent);
extern "C" __declspec(dllimport) BOOL WINAPI ReleaseMutex(HANDLE hMutex);
extern "C" __declspec(dllimport) DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
extern "C" __declspec(dllimport) DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds,
    BOOL bAlertable);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes,
    BOOL bInitialOwner, LPCWSTR lpName);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
    BOOL bInitialState, LPCWSTR lpName);
extern "C" BOOL WINAPI SetWaitableTimerEx(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
    PTIMERAPCROUTINE pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, PREASON_CONTEXT WakeContext,
    ULONG TolerableDelay);
extern "C" __declspec(dllimport) BOOL WINAPI SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime,
    LONG lPeriod, PTIMERAPCROUTINE pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, BOOL fResume);
extern "C" __declspec(dllimport) BOOL WINAPI CancelWaitableTimer(HANDLE hTimer);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES lpTimerAttributes,
    LPCWSTR lpTimerName, DWORD dwFlags, DWORD dwDesiredAccess);
extern "C" __declspec(dllimport) void WINAPI Sleep(DWORD dwMilliseconds);
extern "C" __declspec(dllimport) DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles,
    BOOL bWaitAll, DWORD dwMilliseconds);

// <processthreadsapi.h>
extern "C" __declspec(dllimport) BOOL WINAPI GetProcessTimes(HANDLE hProcess, LPFILETIME lpCreationTime,
    LPFILETIME lpExitTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
extern "C" __declspec(dllimport) HANDLE WINAPI GetCurrentProcess();
extern "C" __declspec(dllimport) DWORD WINAPI GetCurrentProcessId();
extern "C" __declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT uExitCode);
extern "C" __declspec(dllimport) BOOL WINAPI SwitchToThread();
extern "C" __declspec(dllimport) HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
    LPDWORD lpThreadId);
extern "C" __declspec(dllimport) HANDLE WINAPI GetCurrentThread();
extern "C" __declspec(dllimport) DWORD WINAPI GetCurrentThreadId();
extern "C" __declspec(dllimport) HANDLE WINAPI OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId);
extern "C" __declspec(dllimport) BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
extern "C" __declspec(dllimport) BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
extern "C" __declspec(dllimport) DWORD WINAPI SuspendThread(HANDLE hThread);
extern "C" __declspec(dllimport) DWORD WINAPI ResumeThread(HANDLE hThread);
extern "C" __declspec(dllimport) DWORD WINAPI GetPriorityClass(HANDLE hProcess);
extern "C" __declspec(dllimport) BOOL WINAPI GetThreadTimes(HANDLE hThread, LPFILETIME lpCreationTime,
    LPFILETIME lpExitTime, LPFILETIME lpKernelTime, LPFILETIME lpUserTime);
extern "C" __declspec(dllimport) BOOL WINAPI GetProcessHandleCount(HANDLE hProcess, PDWORD pdwHandleCount);

// <sysinfoapi.h>
extern "C" __declspec(dllimport) void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
extern "C" __declspec(dllimport) void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
extern "C" __declspec(dllimport) DWORD WINAPI GetTickCount();
extern "C" __declspec(dllimport) ULONGLONG WINAPI GetTickCount64();
extern "C" __declspec(dllimport) UINT WINAPI GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize);
extern "C" __declspec(dllimport) UINT WINAPI GetWindowsDirectoryW(LPWSTR lpBuffer, UINT uSize);
extern "C" __declspec(dllimport) void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo);
extern "C" __declspec(dllimport) void WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime);

// <memoryapi.h>
extern "C" __declspec(dllimport) LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType,
    DWORD flProtect);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect,
    PDWORD lpflOldProtect);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
extern "C" __declspec(dllimport) SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer,
    SIZE_T dwLength);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFileMappingW(HANDLE hFile,
    LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
    LPCWSTR lpName);
extern "C" __declspec(dllimport) HANDLE WINAPI OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle,
    LPCWSTR lpName);
extern "C" __declspec(dllimport) LPVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
extern "C" __declspec(dllimport) BOOL WINAPI UnmapViewOfFile(LPCVOID lpBaseAddress);
extern "C" __declspec(dllimport) BOOL WINAPI GetProcessWorkingSetSizeEx(HANDLE hProcess,
    PSIZE_T lpMinimumWorkingSetSize, PSIZE_T lpMaximumWorkingSetSize, PDWORD Flags);
extern "C" __declspec(dllimport) BOOL WINAPI SetProcessWorkingSetSizeEx(HANDLE hProcess,
    SIZE_T dwMinimumWorkingSetSize, SIZE_T dwMaximumWorkingSetSize, DWORD Flags);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualLock(LPVOID lpAddress, SIZE_T dwSize);
extern "C" __declspec(dllimport) BOOL WINAPI VirtualUnlock(LPVOID lpAddress, SIZE_T dwSize);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateMemoryResourceNotification(MEMORY_RESOURCE_NOTIFICATION_TYPE NotificationType);
extern "C" __declspec(dllimport) BOOL WINAPI QueryMemoryResourceNotification(HANDLE ResourceNotificationHandle,
    PBOOL ResourceState);

// <libloaderapi.h>
extern "C" __declspec(dllimport) BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule);
extern "C" __declspec(dllimport) BOOL WINAPI FreeLibrary(HMODULE hLibModule);
extern "C" __declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
extern "C" __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName);
extern "C" __declspec(dllimport) BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);
extern "C" __declspec(dllimport) FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
extern "C" __declspec(dllimport) HMODULE WINAPI LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
extern "C" __declspec(dllimport) HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo);
extern "C" __declspec(dllimport) LPVOID WINAPI LockResource(HGLOBAL hResData);
extern "C" __declspec(dllimport) DWORD WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo);
extern "C" __declspec(dllimport) HRSRC WINAPI FindResourceW(HMODULE hModule, LPCWSTR lpName, LPCWSTR lpType);
extern "C" __declspec(dllimport) HMODULE WINAPI LoadLibraryW(LPCWSTR lpLibFileName);

// <realtimeapiset.h>
extern "C" __declspec(dllimport) BOOL WINAPI QueryThreadCycleTime(HANDLE ThreadHandle, PULONG64 CycleTime);
extern "C" __declspec(dllimport) BOOL WINAPI QueryProcessCycleTime(HANDLE ProcessHandle, PULONG64 CycleTime);

// <winbase.h>
extern "C" __declspec(dllimport) __declspec(allocator) HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
extern "C" __declspec(dllimport) __declspec(allocator) HGLOBAL WINAPI GlobalReAlloc(HGLOBAL hMem, SIZE_T dwBytes,
    UINT uFlags);
extern "C" __declspec(dllimport) SIZE_T WINAPI GlobalSize(HGLOBAL hMem);
extern "C" __declspec(dllimport) BOOL WINAPI GlobalUnlock(HGLOBAL hMem);
extern "C" __declspec(dllimport) LPVOID WINAPI GlobalLock(HGLOBAL hMem);
extern "C" __declspec(dllimport) UINT WINAPI GlobalFlags(HGLOBAL hMem);
extern "C" __declspec(dllimport) HGLOBAL WINAPI GlobalFree(HGLOBAL hMem);
extern "C" __declspec(dllimport) HLOCAL WINAPI LocalFree(HLOCAL hMem);
extern "C" __declspec(dllimport) BOOL WINAPI GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask,
    PDWORD_PTR lpSystemAffinityMask);
extern "C" __declspec(dllimport) DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
extern "C" __declspec(dllimport) int WINAPI lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2);
extern "C" __declspec(dllimport) int WINAPI lstrlenA(LPCSTR lpString);
extern "C" __declspec(dllimport) BOOL WINAPI CopyFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName,
    BOOL bFailIfExists);
extern "C" __declspec(dllimport) BOOL WINAPI MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName);
extern "C" __declspec(dllimport) BOOL WINAPI MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName,
    DWORD dwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI CreateHardLinkW(LPCWSTR lpFileName, LPCWSTR lpExistingFileName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes);

// <winbase.h>
extern "C" __declspec(dllimport) BOOL WINAPI GetFileInformationByHandleEx(HANDLE hFile,
    FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize);

// <stringapiset.h>
extern "C" __declspec(dllimport) int WINAPI CompareStringOrdinal(LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
    int cchCount2, BOOL bIgnoreCase);
extern "C" __declspec(dllimport) int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr,
    int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
extern "C" __declspec(dllimport) int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr,
    int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar);

// <winnls.h>
extern "C" __declspec(dllimport) int WINAPI GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName);

// Inline definitions; the SDK's own with MAICORD_SDK_INCLUDES.
#if !defined(MAICORD_SDK_INCLUDES)
extern "C" __forceinline
LONG64
_InlineInterlockedAdd64 (
       LONG64 volatile *Addend,
      LONG64 Value
    )
{
    return _InterlockedExchangeAdd64(Addend, Value) + Value;
}
extern "C" __forceinline
LONG
ReadAcquire (
       LONG const volatile *Source
    )
{
    LONG Value;
    Value = *Source;
    return Value;
}
extern "C" __forceinline
void
WriteRelease (
       LONG volatile *Destination,
      LONG Value
    )
{
    *Destination = Value;
    return;
}
extern "C" __forceinline
LONG64
ReadAcquire64 (
       LONG64 const volatile *Source
    )
{
    LONG64 Value;
    Value = *Source;
    return Value;
}
extern "C" __forceinline
PVOID
RtlSecureZeroMemory(
      PVOID ptr,
      SIZE_T cnt
    )
{
    volatile char *vptr = (volatile char *)ptr;
    __stosb((PBYTE )((DWORD64)vptr), 0, cnt);
    return ptr;
}
#endif

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
