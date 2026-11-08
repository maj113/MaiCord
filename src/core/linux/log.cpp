// log.hpp on Linux: the same buffered file log as src/core/log.cpp, on open/write/rename and an SRW lock (the futex
// twin of os/linux/srwlock.hpp). Lines end in "\n". Time stamps are local time in the user's zone (os/local_time.hpp:
// $TZ or the /etc/localtime TZif database; UTC when none is readable), as on Windows. The file is <state
// directory>/logs/maicord.log (D19: $XDG_STATE_HOME/maicord, see state_path in data_path.hpp).
//
// Lines are formatted in place after the buffered bytes, under the lock: one lock and one copy per line, whatever its
// fields (core/log_event.hpp builds a multi-field event on the caller's stack first). Warning and error lines, a full
// buffer and close write synchronously, so the lines before a crash are in the file (the process's own buffer dies
// with it, the page cache does not); everything else reaches the file within kFlushDelayMilliseconds through one
// flush thread (started with the first buffered line), so external log readers still see lines without further log
// traffic.
#include "maicord/core/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/local_time.hpp"
#include "maicord/os/linux/srwlock.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/path.hpp"

#include "../log_shared.inc"

namespace maicord {
namespace {

constexpr u32 kPathCapacity = 768;
constexpr u32 kLineCapacity = 1024;
// A line keeps at most this many bytes, its newline included when it fits.
constexpr u32 kLineBytes = kLineCapacity - 1;
constexpr u32 kBufferBytes = 16u * 1024u;
constexpr u64 kLogFileMaximumBytes = 16ull * 1024ull * 1024ull;
constexpr i64 kFlushDelayMilliseconds = 10;
constexpr u32 kStampCharacters = 19; // "YYYY-MM-DD HH:MM:SS"
constexpr u32 kPrefixCharacters = kStampCharacters + 10; // stamp, ".mmm [L] ["

struct LogState {
    int file_plus_one; // the descriptor plus one; 0 without a file, so that the whole state is zero (.bss, not .data)
    SRWLOCK lock;
    u64 file_bytes;
    // The flush thread sleeps while `flush_wanted` is 0 and flushes once the delay after it became 1.
    unsigned long flush_thread;
    volatile u32 flush_wanted;
    bool flush_thread_started;
    bool stopping;
    u64 stamp_second;
    LogCounters counters;
    u32 buffered;
    char stamp[kStampCharacters];
    char buffer[kBufferBytes];
};

LogState g_log;

// The log file's descriptor, or -1.
int log_file() noexcept {
    return g_log.file_plus_one - 1;
}

void set_log_file(int descriptor) noexcept {
    g_log.file_plus_one = descriptor + 1;
}

// Caller holds the lock. The date/time prefix changes once per second.
void copy_stamp_locked(char* line, u64 second) noexcept {
    if (g_log.stamp_second != second) {
        const i64 shifted = static_cast<i64>(second) + local_utc_offset(static_cast<i64>(second));
        const u64 local = shifted > 0 ? static_cast<u64>(shifted) : 0; // the clock is past 1970; a negative reading clamps
        const CivilDate date = civil_date_from_days(static_cast<i64>(local / 86400));
        const u32 seconds_of_day = static_cast<u32>(local % 86400);
        put_stamp(g_log.stamp, static_cast<u32>(date.year), date.month, date.day, seconds_of_day / 3600,
                  seconds_of_day / 60 % 60, seconds_of_day % 60);
        g_log.stamp_second = second;
    }
    copy_bytes(line, g_log.stamp, kStampCharacters);
}

// Caller holds the lock.
void flush_locked() noexcept {
    if (g_log.buffered != 0 && log_file() >= 0) {
        ++g_log.counters.file_writes;
        const long written = sys_write(log_file(), g_log.buffer, g_log.buffered);
        if (!syscall_failed(written)) {
            g_log.file_bytes += static_cast<u64>(written);
            g_log.counters.file_bytes += static_cast<u64>(written);
        }
    }
    g_log.buffered = 0;
}

void* flush_thread_main(void*) {
    for (;;) {
        while (!__atomic_load_n(&g_log.flush_wanted, __ATOMIC_ACQUIRE)) {
            AcquireSRWLockExclusive(&g_log.lock);
            const bool stop = g_log.stopping;
            ReleaseSRWLockExclusive(&g_log.lock);
            if (stop) {
                return nullptr;
            }
            sys_futex_wait(&g_log.flush_wanted, 0, nullptr);
        }
        const TimeSpec delay{0, kFlushDelayMilliseconds * 1'000'000};
        sys_nanosleep(&delay);
        AcquireSRWLockExclusive(&g_log.lock);
        flush_locked();
        __atomic_store_n(&g_log.flush_wanted, 0u, __ATOMIC_RELEASE);
        ReleaseSRWLockExclusive(&g_log.lock);
    }
}

// Caller holds the lock. False when the flush thread could not be started.
bool start_flush_thread_locked() noexcept {
    if (!g_log.flush_thread_started) {
        g_log.stopping = false;
        g_log.flush_thread_started = pthread_create(&g_log.flush_thread, nullptr, flush_thread_main, nullptr) == 0;
    }
    return g_log.flush_thread_started;
}

LineWriter begin_line(LogLevel level, const char* component) noexcept {
    TimeSpec now{};
    clock_gettime(kClockRealtime, &now);
    AcquireSRWLockExclusive(&g_log.lock);
    if (kBufferBytes - g_log.buffered < kLineBytes) {
        flush_locked();
    }
    char* line = g_log.buffer + g_log.buffered;
    copy_stamp_locked(line, static_cast<u64>(now.seconds));
    line[kStampCharacters] = '.';
    put_fixed_decimal(line + kStampCharacters + 1, static_cast<u32>(now.nanoseconds / 1'000'000), 3);
    copy_bytes(line + kStampCharacters + 4, " [?] [", 6);
    line[kStampCharacters + 6] = level_letter(level);
    LineWriter writer{line + kPrefixCharacters, line + kLineBytes};
    writer.put(component ? component : "unknown");
    writer.put("] ");
    return writer;
}

void commit_line(LineWriter writer, LogLevel level) noexcept {
    writer.put("\n");
    const char* line = g_log.buffer + g_log.buffered;
    const u32 length = static_cast<u32>(writer.at - line);
#if defined(MAICORD_LOG_STDOUT)
    sys_write(1, line, length);
#endif
    if (log_file() >= 0) {
        const u64 pending = g_log.file_bytes + g_log.buffered;
        if (pending <= kLogFileMaximumBytes && length <= kLogFileMaximumBytes - pending) {
            g_log.buffered += length;
            ++g_log.counters.lines;
            if (level >= LogLevel::warning) {
                flush_locked();
            } else if (!__atomic_load_n(&g_log.flush_wanted, __ATOMIC_ACQUIRE)) {
                if (start_flush_thread_locked()) {
                    __atomic_store_n(&g_log.flush_wanted, 1u, __ATOMIC_RELEASE);
                    sys_futex_wake(&g_log.flush_wanted, 1);
                } else {
                    flush_locked();
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

// Opens `path` (UTF-16) for appending; the descriptor, or -1.
int open_log(const wchar_t* path) noexcept {
    char text[kPathCapacity * 3];
    if (path_to_utf8(path, text, sizeof(text)) == 0) {
        return -1;
    }
    const long file = sys_open(text, kOpenWriteOnly | kOpenCreate | kOpenAppend, 0600);
    return syscall_failed(file) ? -1 : static_cast<int>(file);
}

// The file's size, or -1.
i64 file_size(int file) noexcept {
    const long size = sys_seek(file, 0, kSeekEnd);
    return syscall_failed(size) ? -1 : size;
}

} // namespace

bool log_open() noexcept {
    set_log_file(-1);

    wchar_t path[kPathCapacity]{};
    const DataPathResult built = state_path(path, kPathCapacity, L"logs/maicord.log", true);
    if (!built.ok()) {
        return false;
    }
    u32 path_length = built.length;
    const u32 directory_length = path_length - 12; // without "/maicord.log"

    set_log_file(open_log(path));
    i64 size = 0;
    // Leave room for at least one complete record. A file just below the hard
    // limit would open, reject every line, and never reach the rotation check.
    constexpr u64 rotation_threshold = kLogFileMaximumBytes - kLineCapacity;
    if (log_file() >= 0 && (size = file_size(log_file())) >= static_cast<i64>(rotation_threshold)) {
        sys_close(log_file());
        set_log_file(-1);
        wchar_t previous[kPathCapacity]{};
        for (u32 index = 0; index <= directory_length; ++index) {
            previous[index] = path[index];
        }
        u32 previous_length = directory_length;
        char from[kPathCapacity * 3];
        char to[kPathCapacity * 3];
        const bool rotated = wide_append(previous, kPathCapacity, &previous_length, L"/maicord.previous.log") &&
                             path_to_utf8(path, from, sizeof(from)) != 0 &&
                             path_to_utf8(previous, to, sizeof(to)) != 0 && !syscall_failed(sys_rename(from, to));
        if (rotated) {
            size = 0;
        } else {
            // The file could not be moved aside. Keep diagnostics live in a bounded spill file
            // instead of reopening the saturated log.
            path_length = directory_length;
            path[path_length] = 0;
            if (!wide_append(path, kPathCapacity, &path_length, L"/maicord.overflow.log")) {
                return false;
            }
        }
        set_log_file(open_log(path));
    }
    if (log_file() >= 0 && (size = file_size(log_file())) > 0)
        g_log.file_bytes = static_cast<u64>(size);
    return log_file() >= 0;
}

void log_flush() noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    flush_locked();
    ReleaseSRWLockExclusive(&g_log.lock);
}

void log_close() noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    flush_locked();
    if (log_file() >= 0) {
        sys_close(log_file());
    }
    set_log_file(-1);
    g_log.file_bytes = 0;
    const bool thread_started = g_log.flush_thread_started;
    g_log.stopping = true;
    ReleaseSRWLockExclusive(&g_log.lock);
    if (thread_started) {
        // Without a file a late wake has nothing to write; joining keeps the thread from outliving the state.
        __atomic_store_n(&g_log.flush_wanted, 1u, __ATOMIC_RELEASE);
        sys_futex_wake(&g_log.flush_wanted, 1);
        pthread_join(g_log.flush_thread, nullptr);
    }
    g_log.flush_thread_started = false;
    g_log.flush_wanted = 0;
}

LogCounters log_counters() noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    const LogCounters counters = g_log.counters;
    ReleaseSRWLockExclusive(&g_log.lock);
    return counters;
}

} // namespace maicord
