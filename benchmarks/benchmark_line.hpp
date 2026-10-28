#pragma once

#include "benchmark_output.hpp"

namespace maicord {

// One "label key=value ..." record assembled in a bounded buffer and written
// as a single line.
struct Line {
    char text[1024];
    u32 used = 0;
    void put(const char* value) noexcept {
        while (*value && used + 3 < sizeof(text)) text[used++] = *value++;
    }
    void put(const char* value, u32 count) noexcept {
        for (u32 i = 0; i < count && used + 3 < sizeof(text); ++i) text[used++] = value[i];
    }
    void number(u64 value) noexcept {
        char digits[20];
        const u32 count = decimal_write(digits, value);
        put(digits, count);
    }
    void field(const char* key, u64 value) noexcept { put(" "), put(key), put("="), number(value); }
    void field(const char* key, const char* value) noexcept { put(" "), put(key), put("="), put(value); }
    void digest(const char* key, u64 value) noexcept {
        put(" "), put(key), put("=");
        for (i32 shift = 60; shift >= 0 && used + 3 < sizeof(text); shift -= 4)
            text[used++] = "0123456789abcdef"[(value >> shift) & 15];
    }
    void emit() noexcept {
        text[used++] = '\r', text[used++] = '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, used, &written, nullptr);
        used = 0;
    }
};

} // namespace maicord
