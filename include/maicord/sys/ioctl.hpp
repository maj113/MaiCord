#pragma once

// Device I/O control codes, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <winioctl.h>
#endif

// <winioctl.h>
#define FILE_DEVICE_FILE_SYSTEM 0x00000009
#define CTL_CODE( DeviceType, Function, Method, Access ) ( ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) )
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0
#define FILE_SPECIAL_ACCESS (FILE_ANY_ACCESS)
#define FSCTL_SET_REPARSE_POINT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
