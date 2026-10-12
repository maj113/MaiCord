#pragma once
#include "maicord/core/log.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
// Bounded stack record. Callers supply static labels and non-sensitive values;
// no allocations or output on polling paths unless a state actually changes.
struct TraceRecord {
    char text[768]{};
    u32 used = 0;
    explicit TraceRecord(const char* event) noexcept {
        append(event);
        field("pid", process_id());
    }
    void append(const char* value) noexcept {
        u32 cursor = used;
        while (*value && cursor + 1 < sizeof(text))
            text[cursor++] = *value++;
        used = cursor;
    }
    __declspec(noinline) void field(const char* key, const char* value) noexcept {
        append(" ");
        append(key);
        append("=");
        append(value);
    }
    __declspec(noinline) void field(const char* key, u64 value) noexcept {
        char digits[21];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        append(" ");
        append(key);
        append("=");
        while (count && used + 1 < sizeof(text))
            text[used++] = digits[--count];
    }
    void write(const char* component) const noexcept {
        log_write(LogLevel::info, component, text);
    }
};
} // namespace maicord
