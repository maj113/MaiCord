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
void log_write(LogLevel level, const char* component, const char* message) noexcept;
void log_u64(LogLevel level, const char* component, const char* key, u64 value) noexcept;

} // namespace maicord
