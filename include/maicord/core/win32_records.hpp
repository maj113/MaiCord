#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

// MaiCord's stand-ins for Win32 records: the SDK's layout and member names
// (src/core/win32_sdk_conformance.cpp checks them); native() passes them to
// Win32.

// SRWLOCK (RTL_SRWLOCK), unlocked when zero-initialized. Like a mutex, it
// locks through const objects.
struct SrwLock {
    void* Ptr;

    SRWLOCK* native() const noexcept {
        return reinterpret_cast<SRWLOCK*>(const_cast<SrwLock*>(this));
    }
};

// POINT.
struct Win32Point {
    LONG x;
    LONG y;

    POINT* native() noexcept {
        return reinterpret_cast<POINT*>(this);
    }
};

// RECT.
struct Win32Rect {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;

    RECT* native() noexcept {
        return reinterpret_cast<RECT*>(this);
    }
    const RECT* native() const noexcept {
        return reinterpret_cast<const RECT*>(this);
    }
};

// FILETIME.
struct Win32FileTime {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;

    FILETIME* native() noexcept {
        return reinterpret_cast<FILETIME*>(this);
    }
    const FILETIME* native() const noexcept {
        return reinterpret_cast<const FILETIME*>(this);
    }
};

// FILE_ID_128 and FILE_ID_INFO.
struct Win32FileId128 {
    u8 Identifier[16];
};
struct Win32FileIdInfo {
    u64 VolumeSerialNumber;
    Win32FileId128 FileId;

    FILE_ID_INFO* native() noexcept {
        return reinterpret_cast<FILE_ID_INFO*>(this);
    }
    const FILE_ID_INFO* native() const noexcept {
        return reinterpret_cast<const FILE_ID_INFO*>(this);
    }
};

} // namespace maicord
