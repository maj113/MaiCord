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

} // namespace maicord
