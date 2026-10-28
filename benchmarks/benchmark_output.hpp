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

inline void write_value(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\r\n");
}

inline void write_ops(const char* name, u64 operations_per_second) noexcept {
    write_metric(name, operations_per_second, "ops/s");
}

// Operations (or bytes) per second; zero while the clock has not advanced.
inline u64 per_second(u64 count, u64 elapsed_us) noexcept {
    return elapsed_us == 0 ? 0 : count * 1'000'000ull / elapsed_us;
}

} // namespace maicord
