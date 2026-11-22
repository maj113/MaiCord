#pragma once

// The AVX2 case-insensitive search, apart from ascii.hpp so its includers do not parse the SIMD declarations.

#include "maicord/core/ascii.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord {

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

} // namespace maicord
