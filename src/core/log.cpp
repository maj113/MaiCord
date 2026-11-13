#include "maicord/core/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"
#include "maicord/core/win32_threadpool.hpp"

#include "log_shared.inc"

namespace maicord {
namespace {

constexpr u32 kPathCapacity = 768;
constexpr u32 kLineCapacity = 1024;
// A line keeps at most this many bytes, its CRLF included when it fits.
constexpr u32 kLineBytes = kLineCapacity - 1;
constexpr u32 kBufferBytes = 16u * 1024u;
constexpr u64 kLogFileMaximumBytes = 16ull * 1024ull * 1024ull;
// Relative due time in 100 ns units for the deferred flush.
constexpr i64 kFlushDelay = -100ll * 10'000ll;
constexpr u32 kStampCharacters = 19; // "YYYY-MM-DD HH:MM:SS"
constexpr u32 kPrefixCharacters = kStampCharacters + 10; // stamp, ".mmm [L] ["

// Lines are formatted in place after the buffered bytes, under the lock: one
// lock and one copy per line, whatever its fields (core/log_event.hpp builds a
// multi-field event on the caller's stack first). Warning and error lines, a
// full buffer and close write synchronously, so the lines before a crash are in
// the file (the process's own buffer dies with it, the system's file cache does
// not); everything else reaches the file within kFlushDelay through one
// thread-pool timer, so external log readers still see lines without further
// log traffic.
struct LogState {
    HANDLE file;
    SRWLOCK lock;
    u64 file_bytes;
    PTP_TIMER flush_timer;
    u64 stamp_second;
    LogCounters counters;
    u32 buffered;
    bool flush_armed;
    char stamp[kStampCharacters];
    char buffer[kBufferBytes];
};

LogState g_log{};

// Caller holds the lock. The local date/time prefix changes once per second.
void copy_stamp_locked(char* line, u64 second) noexcept {
    if (g_log.stamp_second != second) {
        const u64 ticks = second * 10'000'000ull;
        const FILETIME now{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        SYSTEMTIME utc{};
        SYSTEMTIME time{};
        FileTimeToSystemTime(&now, &utc);
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &time)) {
            time = utc;
        }
        put_stamp(g_log.stamp, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        g_log.stamp_second = second;
    }
    copy_bytes(line, g_log.stamp, kStampCharacters);
}

// Caller holds the lock.
void flush_locked() noexcept {
    if (g_log.buffered != 0 && g_log.file && g_log.file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ++g_log.counters.file_writes;
        if (WriteFile(g_log.file, g_log.buffer, g_log.buffered, &written, nullptr)) {
            g_log.file_bytes += written;
            g_log.counters.file_bytes += written;
        }
    }
    g_log.buffered = 0;
}

void CALLBACK flush_timer_callback(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER) noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    flush_locked();
    g_log.flush_armed = false;
    ReleaseSRWLockExclusive(&g_log.lock);
}

LineWriter begin_line(LogLevel level, const char* component) noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const u64 ticks = (static_cast<u64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    AcquireSRWLockExclusive(&g_log.lock);
    if (kBufferBytes - g_log.buffered < kLineBytes) {
        flush_locked();
    }
    char* line = g_log.buffer + g_log.buffered;
    copy_stamp_locked(line, ticks / 10'000'000ull);
    line[kStampCharacters] = '.';
    put_fixed_decimal(line + kStampCharacters + 1, static_cast<u32>(ticks / 10'000ull % 1'000ull), 3);
    copy_bytes(line + kStampCharacters + 4, " [?] [", 6);
    line[kStampCharacters + 6] = level_letter(level);
    LineWriter writer{line + kPrefixCharacters, line + kLineBytes};
    writer.put(component ? component : "unknown");
    writer.put("] ");
    return writer;
}

void commit_line(LineWriter writer, LogLevel level) noexcept {
    writer.put("\r\n");
    const char* line = g_log.buffer + g_log.buffered;
    const u32 length = static_cast<u32>(writer.at - line);
    if (g_log.file && g_log.file != INVALID_HANDLE_VALUE) {
        const u64 pending = g_log.file_bytes + g_log.buffered;
        if (pending <= kLogFileMaximumBytes && length <= kLogFileMaximumBytes - pending) {
            g_log.buffered += length;
            ++g_log.counters.lines;
            if (level >= LogLevel::warning) {
                flush_locked();
            } else if (!g_log.flush_armed) {
                if (!g_log.flush_timer) {
                    g_log.flush_timer = CreateThreadpoolTimer(flush_timer_callback, nullptr, nullptr);
                }
                if (g_log.flush_timer) {
                    ULARGE_INTEGER due{};
                    due.QuadPart = static_cast<u64>(kFlushDelay);
                    FILETIME due_time{due.LowPart, due.HighPart};
                    SetThreadpoolTimer(g_log.flush_timer, &due_time, 0, 0);
                    g_log.flush_armed = true;
                } else {
                    flush_locked();
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

} // namespace

bool log_open() noexcept {
    g_log.file = INVALID_HANDLE_VALUE;

    wchar_t path[kPathCapacity]{};
    const DataPathResult built = data_path(path, kPathCapacity, L"logs\\maicord.log", true);
    if (!built.ok()) {
        return false;
    }
    u32 path_length = built.length;
    const u32 directory_length = path_length - 12; // without "\maicord.log"

    g_log.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    // Leave room for at least one complete record. A file just below the hard
    // limit would open, reject every line, and never reach the rotation check.
    constexpr u64 rotation_threshold = kLogFileMaximumBytes - kLineCapacity;
    if (g_log.file != INVALID_HANDLE_VALUE && GetFileSizeEx(g_log.file, &size) &&
        size.QuadPart >= static_cast<LONGLONG>(rotation_threshold)) {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
        wchar_t previous[kPathCapacity]{};
        for (u32 index = 0; index <= directory_length; ++index) {
            previous[index] = path[index];
        }
        u32 previous_length = directory_length;
        const bool rotated =
            wide_append(previous, kPathCapacity, &previous_length, L"\\maicord.previous.log") &&
            MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (rotated) {
            size.QuadPart = 0;
        } else {
            // Another process may still hold the log open without delete
            // sharing. Keep diagnostics live in a bounded spill file
            // instead of reopening the saturated log.
            path_length = directory_length;
            path[path_length] = 0;
            if (!wide_append(path, kPathCapacity, &path_length, L"\\maicord.overflow.log")) {
                return false;
            }
        }
        g_log.file =
            CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_log.file != INVALID_HANDLE_VALUE && GetFileSizeEx(g_log.file, &size) && size.QuadPart > 0)
        g_log.file_bytes = static_cast<u64>(size.QuadPart);
    return g_log.file != INVALID_HANDLE_VALUE;
}

void log_close() noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    flush_locked();
    if (g_log.file && g_log.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log.file);
        CloseHandle(g_log.file);
    }
    g_log.file = INVALID_HANDLE_VALUE;
    g_log.file_bytes = 0;
    PTP_TIMER timer = g_log.flush_timer;
    g_log.flush_timer = nullptr;
    g_log.flush_armed = false;
    ReleaseSRWLockExclusive(&g_log.lock);
    if (timer) {
        // Without a file a late callback has nothing to write; waiting keeps
        // the timer from being freed under a running callback.
        SetThreadpoolTimer(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }
}

LogCounters log_counters() noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    const LogCounters counters = g_log.counters;
    ReleaseSRWLockExclusive(&g_log.lock);
    return counters;
}

} // namespace maicord
