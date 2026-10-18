// MaiCord's own records that stand for SDK records (Win32Rect for RECT and
// others; see maicord/core/win32_records.hpp): the SDK's layout and member
// names. The SDK-free headers themselves are checked by the units
// tools/generate_sys_conformance.py generates.
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/core/win32_user.hpp"

#include <Windows.h>
#include <stddef.h>

namespace maicord {

#define MAICORD_SAME_FIELD(ours, theirs, field)                                                                \
    static_assert(offsetof(ours, field) == offsetof(theirs, field) &&                                            \
                  sizeof(ours::field) == sizeof(theirs::field))
#define MAICORD_SAME_RECORD(ours, theirs)                                                                     \
    static_assert(sizeof(ours) == sizeof(theirs) && alignof(ours) == alignof(theirs))

MAICORD_SAME_RECORD(SrwLock, SRWLOCK);
MAICORD_SAME_RECORD(Win32Point, POINT);
MAICORD_SAME_FIELD(Win32Point, POINT, x);
MAICORD_SAME_FIELD(Win32Point, POINT, y);
MAICORD_SAME_RECORD(Win32Rect, RECT);
MAICORD_SAME_FIELD(Win32Rect, RECT, left);
MAICORD_SAME_FIELD(Win32Rect, RECT, top);
MAICORD_SAME_FIELD(Win32Rect, RECT, right);
MAICORD_SAME_FIELD(Win32Rect, RECT, bottom);
MAICORD_SAME_RECORD(Win32FileTime, FILETIME);
MAICORD_SAME_FIELD(Win32FileTime, FILETIME, dwLowDateTime);
MAICORD_SAME_FIELD(Win32FileTime, FILETIME, dwHighDateTime);
MAICORD_SAME_RECORD(Win32FileIdInfo, FILE_ID_INFO);
MAICORD_SAME_FIELD(Win32FileIdInfo, FILE_ID_INFO, VolumeSerialNumber);
MAICORD_SAME_FIELD(Win32FileIdInfo, FILE_ID_INFO, FileId);

MAICORD_SAME_RECORD(Win32LargeInteger, LARGE_INTEGER);
MAICORD_SAME_FIELD(Win32LargeInteger, LARGE_INTEGER, QuadPart);
MAICORD_SAME_RECORD(Win32SystemInfo, SYSTEM_INFO);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, dwOemId);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, dwPageSize);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, lpMinimumApplicationAddress);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, lpMaximumApplicationAddress);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, dwActiveProcessorMask);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, dwNumberOfProcessors);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, dwProcessorType);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, dwAllocationGranularity);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, wProcessorLevel);
MAICORD_SAME_FIELD(Win32SystemInfo, SYSTEM_INFO, wProcessorRevision);
MAICORD_SAME_RECORD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, BaseAddress);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, AllocationBase);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, AllocationProtect);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, PartitionId);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, RegionSize);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, State);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, Protect);
MAICORD_SAME_FIELD(Win32MemoryInformation, MEMORY_BASIC_INFORMATION, Type);
MAICORD_SAME_RECORD(Win32TrackMouseEvent, TRACKMOUSEEVENT);
MAICORD_SAME_FIELD(Win32TrackMouseEvent, TRACKMOUSEEVENT, cbSize);
MAICORD_SAME_FIELD(Win32TrackMouseEvent, TRACKMOUSEEVENT, dwFlags);
MAICORD_SAME_FIELD(Win32TrackMouseEvent, TRACKMOUSEEVENT, hwndTrack);
MAICORD_SAME_FIELD(Win32TrackMouseEvent, TRACKMOUSEEVENT, dwHoverTime);

} // namespace maicord
