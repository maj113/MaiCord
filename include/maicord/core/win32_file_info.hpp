#pragma once

// kernel32: file information classes, declared as the SDK declares them; see maicord/sys/sdk.hpp.

#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

MAICORD_SYS_TYPES_BEGIN
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
MAICORD_SYS_TYPES_END
