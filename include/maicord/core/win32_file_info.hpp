#pragma once

// kernel32: file information classes, extended file attributes and extended find, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

#define FIND_FIRST_EX_LARGE_FETCH 0x00000002

MAICORD_SYS_TYPES_BEGIN
enum _FINDEX_INFO_LEVELS {
    FindExInfoStandard,
    FindExInfoBasic,
};
typedef _FINDEX_INFO_LEVELS FINDEX_INFO_LEVELS;
enum _FINDEX_SEARCH_OPS {
    FindExSearchNameMatch,
};
typedef _FINDEX_SEARCH_OPS FINDEX_SEARCH_OPS;
enum _GET_FILEEX_INFO_LEVELS {
    GetFileExInfoStandard,
};
typedef _GET_FILEEX_INFO_LEVELS GET_FILEEX_INFO_LEVELS;
// <fileapi.h>
struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
};
typedef _WIN32_FILE_ATTRIBUTE_DATA WIN32_FILE_ATTRIBUTE_DATA;
// <winbase.h>
struct _FILE_BASIC_INFO {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    DWORD FileAttributes;
};
typedef _FILE_BASIC_INFO FILE_BASIC_INFO;
struct _FILE_STANDARD_INFO {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    DWORD NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
};
typedef _FILE_STANDARD_INFO FILE_STANDARD_INFO;
struct _FILE_RENAME_INFO {
    union {
        BOOLEAN ReplaceIfExists;
        DWORD Flags;
    };
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
};
typedef _FILE_RENAME_INFO FILE_RENAME_INFO;
struct _FILE_ATTRIBUTE_TAG_INFO {
    DWORD FileAttributes;
    DWORD ReparseTag;
};
typedef _FILE_ATTRIBUTE_TAG_INFO FILE_ATTRIBUTE_TAG_INFO;
MAICORD_SYS_TYPES_END

extern "C" __declspec(dllimport) HANDLE WINAPI FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
extern "C" __declspec(dllimport) BOOL WINAPI GetFileAttributesExW(LPCWSTR lpFileName,
    GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
