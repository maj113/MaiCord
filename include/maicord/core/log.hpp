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
void log_write(LogLevel level, const char* component, const char* message) noexcept;
void log_u64(LogLevel level, const char* component, const char* key, u64 value) noexcept;

} // namespace maicord
