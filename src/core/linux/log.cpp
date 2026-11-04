// log.hpp on Linux: the same buffered file log as src/core/log.cpp, on open/write/rename and a futex lock. Lines end
// in "\n". Time stamps are local time in the user's zone (os/local_time.hpp: $TZ or the /etc/localtime TZif database;
// UTC when none is readable), as on Windows. The file is <state directory>/logs/maicord.log (D19:
// $XDG_STATE_HOME/maicord, see state_path in data_path.hpp).
//
// Lines are formatted in place after the buffered bytes, under the lock. Error lines, a full buffer and close
// write synchronously; everything else reaches the file within kFlushDelayMilliseconds through one flush thread
// (started with the first buffered line), so external log readers still see lines without further log traffic.
#include "maicord/core/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/local_time.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/path.hpp"

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

// A futex lock: 0 free, 1 held, 2 held with waiters.
struct Lock {
    volatile u32 state;

    void acquire() noexcept {
        u32 expected = 0;
        if (__atomic_compare_exchange_n(&state, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        while (__atomic_exchange_n(&state, 2u, __ATOMIC_ACQUIRE) != 0) {
            sys_futex_wait(&state, 2, nullptr);
        }
    }

    void release() noexcept {
        if (__atomic_exchange_n(&state, 0u, __ATOMIC_RELEASE) == 2) {
            sys_futex_wake(&state, 1);
        }
    }
};

struct LogState {
    int file;
    Lock lock;
    u64 file_bytes;
    // The flush thread sleeps while `flush_wanted` is 0 and flushes once the delay after it became 1.
    unsigned long flush_thread;
    volatile u32 flush_wanted;
    bool flush_thread_started;
    bool stopping;
    u64 stamp_second;
    u32 buffered;
    char stamp[kStampCharacters];
    char buffer[kBufferBytes];
};

LogState g_log{-1};

// Appends to a line, dropping what does not fit.
struct LineWriter {
    char* at;
    char* end;

    void put(const char* text) noexcept {
        for (; *text != 0 && at != end; ++text)
            *at++ = *text;
    }
};

// The low `digits` decimal digits of value, zero-padded.
void put_fixed_decimal(char* output, u32 value, u32 digits) noexcept {
    while (digits != 0) {
        output[--digits] = static_cast<char>('0' + value % 10);
        value /= 10;
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

// Caller holds the lock. The date/time prefix changes once per second.
void copy_stamp_locked(char* line, u64 second) noexcept {
    if (g_log.stamp_second != second) {
        const i64 shifted = static_cast<i64>(second) + local_utc_offset(static_cast<i64>(second));
        const u64 local = shifted > 0 ? static_cast<u64>(shifted) : 0; // the clock is past 1970; a negative reading clamps
        const CivilDate date = civil_date_from_days(static_cast<i64>(local / 86400));
        const u32 seconds_of_day = static_cast<u32>(local % 86400);
        char* stamp = g_log.stamp;
        put_fixed_decimal(stamp, static_cast<u32>(date.year), 4);
        stamp[4] = '-';
        put_fixed_decimal(stamp + 5, date.month, 2);
        stamp[7] = '-';
        put_fixed_decimal(stamp + 8, date.day, 2);
        stamp[10] = ' ';
        put_fixed_decimal(stamp + 11, seconds_of_day / 3600, 2);
        stamp[13] = ':';
        put_fixed_decimal(stamp + 14, seconds_of_day / 60 % 60, 2);
        stamp[16] = ':';
        put_fixed_decimal(stamp + 17, seconds_of_day % 60, 2);
        g_log.stamp_second = second;
    }
    copy_bytes(line, g_log.stamp, kStampCharacters);
}

// Caller holds the lock.
void flush_locked() noexcept {
    if (g_log.buffered != 0 && g_log.file >= 0) {
        const long written = sys_write(g_log.file, g_log.buffer, g_log.buffered);
        if (!syscall_failed(written)) {
            g_log.file_bytes += static_cast<u64>(written);
        }
    }
    g_log.buffered = 0;
}

void* flush_thread_main(void*) {
    for (;;) {
        while (!__atomic_load_n(&g_log.flush_wanted, __ATOMIC_ACQUIRE)) {
            g_log.lock.acquire();
            const bool stop = g_log.stopping;
            g_log.lock.release();
            if (stop) {
                return nullptr;
            }
            sys_futex_wait(&g_log.flush_wanted, 0, nullptr);
        }
        const TimeSpec delay{0, kFlushDelayMilliseconds * 1'000'000};
        sys_nanosleep(&delay);
        g_log.lock.acquire();
        flush_locked();
        __atomic_store_n(&g_log.flush_wanted, 0u, __ATOMIC_RELEASE);
        g_log.lock.release();
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

// Takes the lock and starts a line after the buffered bytes, room for a whole
// line made first: "YYYY-MM-DD HH:MM:SS.mmm [L] [component] ".
LineWriter begin_line(LogLevel level, const char* component) noexcept {
    TimeSpec now{};
    clock_gettime(kClockRealtime, &now);
    g_log.lock.acquire();
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

// Ends the line begun by begin_line and releases the lock.
void commit_line(LineWriter writer, LogLevel level) noexcept {
    writer.put("\n");
    const char* line = g_log.buffer + g_log.buffered;
    const u32 length = static_cast<u32>(writer.at - line);
#if defined(MAICORD_LOG_STDOUT)
    sys_write(1, line, length);
#endif
    if (g_log.file >= 0) {
        const u64 pending = g_log.file_bytes + g_log.buffered;
        if (pending <= kLogFileMaximumBytes && length <= kLogFileMaximumBytes - pending) {
            g_log.buffered += length;
            if (level == LogLevel::error) {
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
    g_log.lock.release();
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
    g_log.file = -1;

    wchar_t path[kPathCapacity]{};
    const DataPathResult built = state_path(path, kPathCapacity, L"logs/maicord.log", true);
    if (!built.ok()) {
        return false;
    }
    u32 path_length = built.length;
    const u32 directory_length = path_length - 12; // without "/maicord.log"

    g_log.file = open_log(path);
    i64 size = 0;
    // Leave room for at least one complete record. A file just below the hard
    // limit would open, reject every line, and never reach the rotation check.
    constexpr u64 rotation_threshold = kLogFileMaximumBytes - kLineCapacity;
    if (g_log.file >= 0 && (size = file_size(g_log.file)) >= static_cast<i64>(rotation_threshold)) {
        sys_close(g_log.file);
        g_log.file = -1;
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
        g_log.file = open_log(path);
    }
    if (g_log.file >= 0 && (size = file_size(g_log.file)) > 0)
        g_log.file_bytes = static_cast<u64>(size);
    return g_log.file >= 0;
}

void log_close() noexcept {
    g_log.lock.acquire();
    flush_locked();
    if (g_log.file >= 0) {
        sys_close(g_log.file);
    }
    g_log.file = -1;
    g_log.file_bytes = 0;
    const bool thread_started = g_log.flush_thread_started;
    g_log.stopping = true;
    g_log.lock.release();
    if (thread_started) {
        // Without a file a late wake has nothing to write; joining keeps the thread from outliving the state.
        __atomic_store_n(&g_log.flush_wanted, 1u, __ATOMIC_RELEASE);
        sys_futex_wake(&g_log.flush_wanted, 1);
        pthread_join(g_log.flush_thread, nullptr);
    }
    g_log.flush_thread_started = false;
    g_log.flush_wanted = 0;
}

void log_write(LogLevel level, const char* component, const char* message) noexcept {
    LineWriter line = begin_line(level, component);
    if (message) {
        line.put(message);
    }
    commit_line(line, level);
}

void log_u64(LogLevel level, const char* component, const char* key, u64 value) noexcept {
    char text[22];
    text[0] = '=';
    text[1 + decimal_write(text + 1, value)] = 0;
    LineWriter line = begin_line(level, component);
    line.put(key ? key : "value");
    line.put(text);
    commit_line(line, level);
}

} // namespace maicord
