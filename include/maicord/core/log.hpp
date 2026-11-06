#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

enum class LogLevel : u8 {
    trace,
    info,
    warning,
    error,
};

bool log_open() noexcept;
void log_close() noexcept;
#if !defined(_WIN32)
// Writes the buffered lines to the file now. The Linux updater's relaunch replaces the process image (execve), which
// leaves no later close to do it.
void log_flush() noexcept;
#endif
// One line: "YYYY-MM-DD HH:MM:SS.mmm [L] [component] message". Several values of one event go on one line through
// LogEvent (core/log_event.hpp), never a call per value. Warning and error lines reach the file before the call
// returns; trace and info lines within a few milliseconds.
void log_write(LogLevel level, const char* component, const char* message) noexcept;
// A line "key=value", for an event that is one number.
void log_u64(LogLevel level, const char* component, const char* key, u64 value) noexcept;

// What the log has done since the process started: lines accepted, file writes (system calls) and bytes written.
struct LogCounters {
    u64 lines;
    u64 file_writes;
    u64 file_bytes;
};
LogCounters log_counters() noexcept;

} // namespace maicord
