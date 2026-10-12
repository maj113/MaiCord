#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"

namespace maicord {

// Updater diagnostics: one "update" log line per phase, request or result,
// "<event> key=value ...". Values are numbers, enum ordinals (see the result
// enums in the update_*.hpp headers) and host names only: never request
// paths, query strings, tokens, keys or file contents.
// The executable routes lines to the application log (see
// application_update_entry.cpp), tests capture them, and without a sink they
// are dropped, so the updater core never links the log itself.
using UpdateLogSink = void (*)(LogLevel, const char* line) noexcept;
inline UpdateLogSink g_update_log_sink = nullptr;
// Opens (true) or closes (false) the application log around startup work that
// runs before the application opens it.
inline void (*g_update_log_session)(bool open) noexcept = nullptr;

struct UpdateLog {
    char text[400];
    u32 bytes = 0;
    explicit UpdateLog(const char* event) noexcept { add(event); }
    UpdateLog& add(const char* s) noexcept {
        while (s && *s && bytes + 1 < sizeof(text)) text[bytes++] = *s++;
        return *this;
    }
    UpdateLog& field(const char* key, u64 value) noexcept {
        char digits[20];
        const u32 n = decimal_write(digits, value);
        add(" "); add(key); add("=");
        for (u32 i = 0; i < n && bytes + 1 < sizeof(text); ++i) text[bytes++] = digits[i];
        return *this;
    }
    // Host names only; anything but [A-Za-z0-9.-] is dropped.
    UpdateLog& host(const char* key, Utf8View name) noexcept {
        add(" "); add(key); add("=");
        for (u32 i = 0; i < name.bytes && bytes + 1 < sizeof(text); ++i) {
            const char c = name.data[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') text[bytes++] = c;
        }
        return *this;
    }
    void write(LogLevel level = LogLevel::info) noexcept {
        text[bytes] = 0;
        if (g_update_log_sink) g_update_log_sink(level, text);
    }
};

} // namespace maicord
