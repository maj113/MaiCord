#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#endif

// The entry function of a test executable (maicord_test ENTRY): no arguments, ends the process itself.
#if defined(_WIN32)
#define MAICORD_TEST_ENTRY(name) extern "C" __declspec(noreturn) void WINAPI name() noexcept
#else
#define MAICORD_TEST_ENTRY(name) extern "C" [[noreturn]] void name() noexcept
#endif

namespace maicord {

// Writes bytes to standard output; false when they were not all written.
inline bool test_write_bytes(const void* data, u32 bytes) noexcept {
#if defined(_WIN32)
    DWORD written = 0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data, bytes, &written, nullptr) && written == bytes;
#else
    return sys_write(1, data, bytes) == static_cast<long>(bytes);
#endif
}

// Ends the process with a status.
[[noreturn]] inline void test_process_exit(u32 status) noexcept {
#if defined(_WIN32)
    ExitProcess(status);
#else
    sys_exit_group(static_cast<int>(status));
#endif
}

// Writes text to standard output without a newline.
inline void test_write_text(const char* text) noexcept {
    test_write_bytes(text, c_string_length(text));
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
    test_write_bytes(output, count);
}

// Writes text and a newline to standard output.
inline void test_write_line(const char* text) noexcept {
    test_write_text(text);
    test_write_bytes("\n", 1);
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
    test_write_bytes(line, used);
}

// Counts a failed expectation and reports its label.
inline void test_check(u32* failures, bool valid, const char* label) noexcept {
    if (valid)
        return;
    ++*failures;
    test_write_line(label);
}

// Reports "PASS <name>" or "FAIL <name>" and ends the test process with its exit code.
[[noreturn]] inline void test_exit(u32 failures, const char* name) noexcept {
    test_write_text(failures ? "FAIL " : "PASS ");
    test_write_line(name);
    test_process_exit(failures ? 1 : 0);
}

// Reports whether every one of count bytes equals value.
inline bool test_filled(const u8* bytes, u32 count, u8 value) noexcept {
    for (u32 i = 0; i != count; ++i)
        if (bytes[i] != value)
            return false;
    return true;
}

// Reports whether needle occurs in haystack; an empty needle never matches.
inline bool test_contains(ConstSlice<u8> haystack, Utf8View needle) noexcept {
    for (u32 i = 0; needle.bytes && needle.bytes <= haystack.count && i <= haystack.count - needle.bytes; ++i)
        if (bytes_equal(haystack.data + i, needle.data, needle.bytes))
            return true;
    return false;
}
inline bool test_contains(Utf8View haystack, Utf8View needle) noexcept {
    return test_contains(ConstSlice<u8>{reinterpret_cast<const u8*>(haystack.data), haystack.bytes}, needle);
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
