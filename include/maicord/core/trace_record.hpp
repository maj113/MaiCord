#pragma once
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
// Bounded stack record. Callers supply static labels and non-sensitive values;
// no allocations or output on polling paths unless a state actually changes.
// text is NUL-terminated at used; the bytes after it are never read.
struct TraceRecord {
    char text[768];
    u32 used = 0;
    __declspec(noinline) explicit TraceRecord(const char* event) noexcept {
        append(event);
        field("pid", process_id());
    }
    void append(const char* value) noexcept {
        u32 cursor = used;
        while (*value && cursor + 1 < sizeof(text))
            text[cursor++] = *value++;
        text[cursor] = 0;
        used = cursor;
    }
    __declspec(noinline) void field(const char* key, const char* value) noexcept {
        append(" ");
        append(key);
        append("=");
        append(value);
    }
    __declspec(noinline) void field(const char* key, u64 value) noexcept {
        char digits[20];
        const u32 count = decimal_write(digits, value);
        append(" ");
        append(key);
        append("=");
        for (u32 index = 0; index < count && used + 1 < sizeof(text); ++index)
            text[used++] = digits[index];
        text[used] = 0;
    }
    void write(const char* component) const noexcept {
        log_write(LogLevel::info, component, text);
    }
};
} // namespace maicord
