#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord {

constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr u8 ascii_lower(u8 value) noexcept {
    return value >= static_cast<u8>('A') && value <= static_cast<u8>('Z')
               ? static_cast<u8>(value + ('a' - 'A'))
               : value;
}

constexpr wchar_t ascii_lower(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

constexpr bool ascii_is_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

constexpr bool ascii_is_digit(u8 value) noexcept {
    return value >= static_cast<u8>('0') && value <= static_cast<u8>('9');
}

constexpr bool ascii_is_hex_digit(char value) noexcept {
    return ascii_is_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

constexpr bool ascii_is_lower_hex_digit(char value) noexcept {
    return ascii_is_digit(value) || (value >= 'a' && value <= 'f');
}

// Space, tab, CR or LF.
constexpr bool ascii_is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// 0-15 for a hexadecimal digit of either case, otherwise -1.
constexpr i32 ascii_hex_value(char value) noexcept {
    return ascii_is_digit(value)            ? value - '0'
           : value >= 'a' && value <= 'f' ? value - 'a' + 10
           : value >= 'A' && value <= 'F' ? value - 'A' + 10
                                          : -1;
}

// The digit for value & 15.
constexpr char ascii_hex_digit(u32 value) noexcept {
    return "0123456789abcdef"[value & 15];
}

constexpr char ascii_hex_digit_upper(u32 value) noexcept {
    return "0123456789ABCDEF"[value & 15];
}

constexpr bool ascii_is_alnum(char value) noexcept {
    return static_cast<u8>(value - '0') < 10 || static_cast<u8>((static_cast<u8>(value) | 0x20) - 'a') < 26;
}

// Without leading and trailing ASCII whitespace.
constexpr Utf8View ascii_trim(Utf8View text) noexcept {
    u32 first = 0, last = text.bytes;
    while (first < last && ascii_is_space(text.data[first]))
        ++first;
    while (last > first && ascii_is_space(text.data[last - 1]))
        --last;
    return {text.data + first, last - first};
}

__declspec(noinline) constexpr bool ascii_equal_ignore_case(const Utf8View& left, const Utf8View& right) noexcept {
    if (left.bytes != right.bytes)
        return false;
    for (u32 index = 0; index < left.bytes; ++index)
        if (ascii_lower(left.data[index]) != ascii_lower(right.data[index]))
            return false;
    return true;
}

// Bit i: text byte base + i lowers to `lead`, for i below 32 and below starts - base. fold is 0x20 for a lowercase
// letter lead (a byte lowers to it only as itself or with bit 5 clear) and 0 for any other lead. When the text ends
// in the page of the block and a load from the block would cross into the next page, the load ends at the end of
// that page instead. That page exists, so the read outside the text cannot fault. The asm hides the text's bounds
// from the compiler, which could otherwise treat that read as undefined.
MAICORD_HASWELL __forceinline u32 ascii_lead_mask(const Utf8View& text, u32 base, u32 starts, u8 lead,
                                                  u8 fold) noexcept {
    const u32 room = 4096 - (reinterpret_cast<usize>(text.data + base) & 4095);
    const u32 shift = room < 32 && text.bytes - base <= room ? 32 - room : 0;
    const char* block = text.data + base - shift;
    __asm__("" : "+r"(block));
    const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(block));
    const u32 mask = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(
                         _mm256_or_si256(bytes, _mm256_set1_epi8(static_cast<char>(fold))),
                         _mm256_set1_epi8(static_cast<char>(lead))))) >>
                     shift;
    return starts - base < 32 ? mask & ((1u << (starts - base)) - 1) : mask;
}

// True for an empty needle. ascii_lead_mask finds the bytes where the needle can start, 32 at a time.
MAICORD_HASWELL __attribute__((minsize)) __declspec(noinline) inline bool
ascii_contains_ignore_case(const Utf8View& text, const Utf8View& needle) noexcept {
    if (!needle.bytes)
        return true;
    if (needle.bytes > text.bytes)
        return false;
    const u8 lead = ascii_lower(static_cast<u8>(needle.data[0]));
    const u8 fold = static_cast<u8>(lead - 'a') < 26 ? 0x20 : 0;
    const u32 starts = text.bytes - needle.bytes + 1;
    for (u32 base = 0; base < starts; base += 32)
        for (u32 mask = ascii_lead_mask(text, base, starts, lead, fold); mask; mask &= mask - 1)
            if (ascii_equal_ignore_case({text.data + base + __builtin_ctz(mask), needle.bytes}, needle))
                return true;
    return false;
}

constexpr bool ascii_graphic_string(const Utf8View& value, u32 maximum_bytes,
                                    bool require_nonempty = false) noexcept {
    if ((!value.data && value.bytes != 0) || (require_nonempty && value.bytes == 0) ||
        value.bytes > maximum_bytes) {
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        const u8 byte = static_cast<u8>(value.data[index]);
        if (byte < 0x21u || byte > 0x7Eu) {
            return false;
        }
    }
    return true;
}

inline bool ascii_graphic_string(ConstSlice<u8> value, u32 maximum_bytes, bool require_nonempty = false) noexcept {
    return ascii_graphic_string(Utf8View{reinterpret_cast<const char*>(value.data), value.count}, maximum_bytes,
                                require_nonempty);
}

} // namespace maicord
