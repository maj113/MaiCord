#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/sys/linux/abi.hpp"

// Win32 error numbers are the portable error vocabulary (S5): the Linux layer maps errno onto the numbers shared
// code compares (ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED, ...) and passes anything else as kErrorNative | errno,
// so a native_error field always says what failed.

namespace maicord {

inline constexpr u32 kErrorNative = 0x40000000u;

// The Win32 number for a positive errno.
constexpr u32 win32_error_from_errno(int error) noexcept {
    switch (error) {
    case 0:
        return ERROR_SUCCESS;
    case kErrorPermission:
    case kErrorAccess:
        return ERROR_ACCESS_DENIED;
    case kErrorNoEntry:
        return ERROR_FILE_NOT_FOUND;
    case kErrorNotDirectory:
        return ERROR_PATH_NOT_FOUND;
    case kErrorNoMemory:
        return ERROR_NOT_ENOUGH_MEMORY;
    case kErrorExists:
        return ERROR_ALREADY_EXISTS;
    case kErrorInvalid:
        return ERROR_INVALID_PARAMETER;
    case kErrorTimedOut:
        return ERROR_TIMEOUT;
    default:
        return kErrorNative | static_cast<u32>(error);
    }
}

// The Win32 number for a failed raw system call result (a negated errno).
constexpr u32 win32_error_from_result(long result) noexcept {
    return win32_error_from_errno(static_cast<int>(-result));
}

// The calling thread's last error, as the Windows TEB keeps one. The Linux layer sets it where a Windows call
// would (set_thread_last_error); libc's errno is never read.
inline u32& thread_last_error_slot() noexcept {
    static __thread u32 value;
    return value;
}

inline u32 thread_last_error() noexcept {
    return thread_last_error_slot();
}

inline void set_thread_last_error(u32 error) noexcept {
    thread_last_error_slot() = error;
}

} // namespace maicord
