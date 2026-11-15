#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"

// Text comparisons of the media URL and playlist parsers. The Utf8View front ends are inline so that a literal
// operand travels as a pointer and a length.
namespace maicord::media_detail {
__declspec(noinline) inline bool equal_chars(const Utf8View& a, const char* b, u32 bytes) noexcept {
    return a.bytes == bytes && (!bytes || bytes_equal(a.data, b, bytes));
}
__forceinline bool equal(const Utf8View& a, const Utf8View& b) noexcept { return equal_chars(a, b.data, b.bytes); }
__declspec(noinline) inline bool prefix_chars(const Utf8View& a, const char* b, u32 bytes) noexcept {
    return a.bytes >= bytes && bytes_equal(a.data, b, bytes);
}
__forceinline bool prefix(const Utf8View& a, const Utf8View& b) noexcept { return prefix_chars(a, b.data, b.bytes); }
} // namespace maicord::media_detail
