#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/log.hpp"

namespace maicord {

// Updater diagnostics: one log line per phase, request or result,
// "<event> key=value ...", under a category ("update" by default; the plugin
// catalog's fetches use its own through UpdateTransport::log_category).
// Values are numbers, enum ordinals (see the result enums in the update_*.hpp
// headers) and host names only: never request paths, query strings, provider
// base paths, tokens, keys or document contents.
// The executable routes lines to the application log (see
// application_update_entry.cpp), tests capture them, and without a sink they
// are dropped, so the updater core never links the log itself.
using UpdateLogSink = void (*)(LogLevel, const char* category, const char* line) noexcept;
inline UpdateLogSink g_update_log_sink = nullptr;

struct UpdateLog {
    char text[400];
    u32 bytes = 0;
    const char* category = "update";
    __declspec(noinline) explicit UpdateLog(const char* event) noexcept { add(event); }
    // A null category keeps "update".
    __declspec(noinline) UpdateLog(const char* log_category, const char* event) noexcept {
        if (log_category) category = log_category;
        add(event);
    }
    __declspec(noinline) UpdateLog& add(const char* s) noexcept {
        while (s && *s && bytes + 1 < sizeof(text)) text[bytes++] = *s++;
        return *this;
    }
    __declspec(noinline) UpdateLog& field(const char* key, u64 value) noexcept {
        char digits[20];
        const u32 n = decimal_write(digits, value);
        add(" "); add(key); add("=");
        for (u32 i = 0; i < n && bytes + 1 < sizeof(text); ++i) text[bytes++] = digits[i];
        return *this;
    }
    // 16 lowercase hex digits (plugin owner ids).
    __declspec(noinline) UpdateLog& hex(const char* key, u64 value) noexcept {
        add(" "); add(key); add("=");
        for (u32 shift = 64; shift && bytes + 1 < sizeof(text);) { shift -= 4; text[bytes++] = "0123456789abcdef"[(value >> shift) & 15]; }
        return *this;
    }
    // Host names only; anything but [A-Za-z0-9.-] is dropped.
    __declspec(noinline) UpdateLog& host(const char* key, Utf8View name) noexcept {
        add(" "); add(key); add("=");
        for (u32 i = 0; i < name.bytes && bytes + 1 < sizeof(text); ++i) {
            const char c = name.data[i];
            if (ascii_is_alnum(c) || c == '.' || c == '-') text[bytes++] = c;
        }
        return *this;
    }
    __declspec(noinline) void write(LogLevel level = LogLevel::info) noexcept {
        text[bytes] = 0;
        if (g_update_log_sink) g_update_log_sink(level, category, text);
    }
};

} // namespace maicord
