#include "maicord/core/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/decimal.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"
#include "maicord/core/win32_threadpool.hpp"

namespace maicord {
namespace {

constexpr u32 kPathCapacity = 768;
constexpr u32 kLineCapacity = 1024;
constexpr u32 kBufferBytes = 16u * 1024u;
constexpr u64 kLogFileMaximumBytes = 16ull * 1024ull * 1024ull;
// Relative due time in 100 ns units for the deferred flush.
constexpr i64 kFlushDelay = -100ll * 10'000ll;
constexpr u32 kStampCharacters = 19; // "YYYY-MM-DD HH:MM:SS"

// Lines are appended to a bounded buffer under the lock. Error lines, a full
// buffer and close write synchronously; everything else reaches the file within
// kFlushDelay through one thread-pool timer, so external log readers still see
// lines without further log traffic.
struct LogState {
    HANDLE file;
    SRWLOCK lock;
    u64 file_bytes;
    PTP_TIMER flush_timer;
    u64 stamp_second;
    u32 buffered;
    bool saturated;
    bool flush_armed;
    char stamp[kStampCharacters];
    char buffer[kBufferBytes];
};

LogState g_log{};

void append_char(char* output, u32 capacity, u32* length, char value) noexcept {
    if (*length + 1 < capacity) {
        output[*length] = value;
        ++*length;
        output[*length] = 0;
    }
}

void append_text(char* output, u32 capacity, u32* length, const char* text) noexcept {
    if (!text) {
        return;
    }
    while (*text != 0 && *length + 1 < capacity) {
        output[*length] = *text;
        ++*length;
        ++text;
    }
    output[*length] = 0;
}

void append_fixed_decimal(char* output, u32 capacity, u32* length, u32 value, u32 digits) noexcept {
    char buffer[10]{};
    for (u32 index = 0; index < digits; ++index) {
        buffer[digits - index - 1] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    for (u32 index = 0; index < digits; ++index) {
        append_char(output, capacity, length, buffer[index]);
    }
}

void append_u64_decimal(char* output, u32 capacity, u32* length, u64 value) noexcept {
    char digits[20];
    const u32 count = decimal_write(digits, value);
    for (u32 index = 0; index < count; ++index) {
        append_char(output, capacity, length, digits[index]);
    }
}

char level_letter(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return 'T';
    case LogLevel::info:
        return 'I';
    case LogLevel::warning:
        return 'W';
    case LogLevel::error:
        return 'E';
    }
    return '?';
}

// The local date/time prefix changes once per second; only milliseconds are
// formatted per line.
u32 begin_line(char* line, u32 capacity, LogLevel level, const char* component) noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const u64 ticks = (static_cast<u64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const u64 second = ticks / 10'000'000ull;
    u32 length = 0;
    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.stamp_second != second) {
        SYSTEMTIME utc{};
        SYSTEMTIME time{};
        FileTimeToSystemTime(&now, &utc);
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &time)) {
            time = utc;
        }
        char stamp[kStampCharacters + 1];
        append_fixed_decimal(stamp, sizeof(stamp), &length, time.wYear, 4);
        append_char(stamp, sizeof(stamp), &length, '-');
        append_fixed_decimal(stamp, sizeof(stamp), &length, time.wMonth, 2);
        append_char(stamp, sizeof(stamp), &length, '-');
        append_fixed_decimal(stamp, sizeof(stamp), &length, time.wDay, 2);
        append_char(stamp, sizeof(stamp), &length, ' ');
        append_fixed_decimal(stamp, sizeof(stamp), &length, time.wHour, 2);
        append_char(stamp, sizeof(stamp), &length, ':');
        append_fixed_decimal(stamp, sizeof(stamp), &length, time.wMinute, 2);
        append_char(stamp, sizeof(stamp), &length, ':');
        append_fixed_decimal(stamp, sizeof(stamp), &length, time.wSecond, 2);
        for (u32 index = 0; index < kStampCharacters; ++index) {
            g_log.stamp[index] = stamp[index];
        }
        g_log.stamp_second = second;
    }
    for (length = 0; length < kStampCharacters; ++length) {
        line[length] = g_log.stamp[length];
    }
    ReleaseSRWLockExclusive(&g_log.lock);
    line[length] = 0;
    append_char(line, capacity, &length, '.');
    append_fixed_decimal(line, capacity, &length, static_cast<u32>(ticks / 10'000ull % 1'000ull), 3);
    append_text(line, capacity, &length, " [");
    append_char(line, capacity, &length, level_letter(level));
    append_text(line, capacity, &length, "] [");
    append_text(line, capacity, &length, component ? component : "unknown");
    append_text(line, capacity, &length, "] ");
    return length;
}

// Caller holds the lock.
void flush_locked() noexcept {
    if (g_log.buffered != 0 && g_log.file && g_log.file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (WriteFile(g_log.file, g_log.buffer, g_log.buffered, &written, nullptr)) {
            g_log.file_bytes += written;
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

void commit_line(char* line, u32* length, LogLevel level) noexcept {
    append_text(line, kLineCapacity, length, "\r\n");
#if defined(MAICORD_LOG_STDOUT)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(output, line, *length, &written, nullptr);
    }
#endif

    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.file && g_log.file != INVALID_HANDLE_VALUE) {
        const u64 pending = g_log.file_bytes + g_log.buffered;
        if (pending <= kLogFileMaximumBytes && *length <= kLogFileMaximumBytes - pending) {
            if (*length > kBufferBytes - g_log.buffered) {
                flush_locked();
            }
            for (u32 index = 0; index < *length; ++index) {
                g_log.buffer[g_log.buffered + index] = line[index];
            }
            g_log.buffered += *length;
            if (level == LogLevel::error) {
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
        } else {
            g_log.saturated = true;
        }
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

} // namespace

bool log_open() noexcept {
    g_log.file = INVALID_HANDLE_VALUE;
    g_log.file_bytes = 0;
    g_log.buffered = 0;
    g_log.saturated = false;

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
    if (g_log.file != INVALID_HANDLE_VALUE && GetFileSizeEx(g_log.file, &size) && size.QuadPart > 0) {
        g_log.file_bytes = static_cast<u64>(size.QuadPart);
        g_log.saturated = g_log.file_bytes >= kLogFileMaximumBytes;
    }
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
    g_log.saturated = false;
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

void log_write(LogLevel level, const char* component, const char* message) noexcept {
    char line[kLineCapacity];
    u32 length = begin_line(line, kLineCapacity, level, component);
    append_text(line, kLineCapacity, &length, message);
    commit_line(line, &length, level);
}

void log_u64(LogLevel level, const char* component, const char* key, u64 value) noexcept {
    char line[kLineCapacity];
    u32 length = begin_line(line, kLineCapacity, level, component);
    append_text(line, kLineCapacity, &length, key ? key : "value");
    append_char(line, kLineCapacity, &length, '=');
    append_u64_decimal(line, kLineCapacity, &length, value);
    commit_line(line, &length, level);
}

} // namespace maicord
