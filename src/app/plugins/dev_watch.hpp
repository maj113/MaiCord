#pragma once

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"

// Developer Mode's folder watcher (plugin design 9.2): one change notification on a dev folder, joined to the
// UI wait set, and a settle delay so a linker that writes a DLL in several steps is read once, after the last
// write. UI thread only; zero-initialized, caller-owned.
namespace maicord::app_detail {

inline constexpr u64 kDevWatchSettleMilliseconds = 300;

struct DevWatch {
    HANDLE handle;
    u64 settle_at_ms; // 0: nothing pending
};

// Watches folder (which must exist) for files created, renamed, resized or written. False when it can't.
inline bool dev_watch_open(DevWatch* watch, const wchar_t* folder) noexcept {
    if (watch->handle) return true;
    const HANDLE handle = FindFirstChangeNotificationW(folder, FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    watch->handle = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    watch->settle_at_ms = 0;
    return watch->handle != nullptr;
}

inline void dev_watch_close(DevWatch* watch) noexcept {
    if (watch->handle) FindCloseChangeNotification(watch->handle);
    *watch = {};
}

// True once, kDevWatchSettleMilliseconds after the folder's last change.
inline bool dev_watch_settled(DevWatch* watch, u64 now_ms) noexcept {
    if (!watch->handle) return false;
    if (WaitForSingleObject(watch->handle, 0) == WAIT_OBJECT_0) {
        FindNextChangeNotification(watch->handle);
        watch->settle_at_ms = now_ms + kDevWatchSettleMilliseconds;
    }
    if (!watch->settle_at_ms || now_ms < watch->settle_at_ms) return false;
    watch->settle_at_ms = 0;
    return true;
}

// Milliseconds until a pending change settles (INFINITE: none pending).
inline u32 dev_watch_timeout(const DevWatch& watch, u64 now_ms) noexcept {
    if (!watch.settle_at_ms) return INFINITE;
    return watch.settle_at_ms <= now_ms ? 0 : static_cast<u32>(watch.settle_at_ms - now_ms);
}

} // namespace maicord::app_detail
