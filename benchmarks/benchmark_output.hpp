#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {

inline void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, c_string_length(value), &written, nullptr);
}

inline void write_u64(u64 value) noexcept {
    char digits[20];
    const u32 count = decimal_write(digits, value);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), digits, count, &written, nullptr);
}

inline void write_metric(const char* name, u64 value, const char* unit) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" ");
    write_text(unit);
    write_text("\r\n");
}

} // namespace maicord
