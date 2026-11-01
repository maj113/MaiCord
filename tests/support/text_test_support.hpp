#pragma once

#include "maicord/core/slice.hpp"
#include "test_output.hpp"

namespace maicord {

// Shared by the text-shaping test programs: they print `name=value` lines and
// take an optional corpus path on the command line.
inline void print(Utf8View text) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data, text.bytes, &written, nullptr);
}

inline void number(u64 value) noexcept {
    test_write_u64(value);
}

// The command line after the program name (quotes respected), leading blanks skipped.
inline const wchar_t* test_arguments() noexcept {
    const wchar_t* at = GetCommandLineW();
    bool quoted = false;
    for (; *at; ++at) {
        if (*at == '"') {
            quoted = !quoted;
        } else if (!quoted && (*at == ' ' || *at == '\t')) {
            break;
        }
    }
    while (*at == ' ' || *at == '\t') {
        ++at;
    }
    return at;
}

// Reads `arguments` as exactly one, optionally quoted, nonempty path into path[32768].
inline bool test_path_argument(const wchar_t* at, wchar_t* path) noexcept {
    u32 count = 0;
    const bool quoted = *at == '"';
    if (quoted) {
        ++at;
    }
    while (*at && !(quoted ? *at == '"' : (*at == ' ' || *at == '\t'))) {
        if (count + 1 >= 32768) {
            return false;
        }
        path[count++] = *at++;
    }
    if (quoted) {
        if (*at != '"') {
            return false;
        }
        ++at;
    }
    while (*at == ' ' || *at == '\t') {
        ++at;
    }
    if (*at || !count) {
        return false;
    }
    path[count] = 0;
    return true;
}

} // namespace maicord
