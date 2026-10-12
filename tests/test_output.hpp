#pragma once

#include "maicord/core/base.hpp"

#include <Windows.h>

namespace maicord {

// Counts the bytes before the null terminator.
inline u32 test_text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

// Writes text to standard output without a newline.
inline void test_write_text(const char* text) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, test_text_length(text), &written, nullptr);
}

// Writes an unsigned decimal number to standard output.
inline void test_write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

// Writes text and a newline to standard output.
inline void test_write_line(const char* text) noexcept {
    test_write_text(text);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}

// Writes "<label><decimal value>" and a newline to standard output in one write.
inline void test_write_metric(const char* label, u64 value) noexcept {
    char line[160];
    u32 used = 0;
    while (label[used] && used < 128) {
        line[used] = label[used];
        ++used;
    }
    char digits[20];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        line[used++] = digits[--count];
    line[used++] = '\n';
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, used, &written, nullptr);
}

// Counts a failed expectation and reports its label.
inline void test_check(u32* failures, bool valid, const char* label) noexcept {
    if (valid)
        return;
    ++*failures;
    test_write_line(label);
}

// Reports whether every one of count bytes equals value.
inline bool test_filled(const u8* bytes, u32 count, u8 value) noexcept {
    for (u32 i = 0; i != count; ++i)
        if (bytes[i] != value)
            return false;
    return true;
}

// Decodes pairs of lowercase hexadecimal digits; returns the byte count.
inline u32 test_unhex(const char* text, u8* output) noexcept {
    u32 count = 0;
    while (*text) {
        u8 value = 0;
        for (u32 i = 0; i != 2; ++i) {
            const u8 c = static_cast<u8>(*text++);
            value = static_cast<u8>((value << 4) | (c <= '9' ? c - '0' : c - 'a' + 10));
        }
        output[count++] = value;
    }
    return count;
}

} // namespace maicord
