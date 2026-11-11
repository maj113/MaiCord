#include "maicord/core/crc32.hpp"

#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {
// The reflected Barrett step: the low 32 bits times x^32 mod P plus the high 32 bits (quotient by x^64 / P, then P).
__forceinline u64 barrett(u64 value) noexcept {
    const __m128i constants = _mm_set_epi32(1, static_cast<int>(0xF7011641u), 1, static_cast<int>(0xDB710641u));
    const __m128i quotient = _mm_clmulepi64_si128(_mm_cvtsi32_si128(static_cast<int>(value)), constants, 0x10);
    const __m128i product = _mm_clmulepi64_si128(_mm_blend_epi16(_mm_setzero_si128(), quotient, 3), constants, 0x00);
    return (value ^ static_cast<u64>(_mm_cvtsi128_si64(product))) >> 32;
}
} // namespace

// Carry-less multiplication (PCLMULQDQ is in the -mpclmul baseline): 16-byte folds, one reduction to 32 bits, then
// Barrett steps of 4 bytes and of the last 1 to 3. The constants are x^160 and x^96 mod P for the folds and x^64 mod P
// for the last 64 bits, bit-reflected and shifted left by one as in zlib and the kernel.
u32 crc32_extend(u32 crc, const void* bytes, u32 count) noexcept {
    const auto* data = static_cast<const u8*>(bytes);
    u64 state = ~crc;
    while (count) {
        u64 value;
        if (count >= 16) {
            const __m128i fold = _mm_set_epi32(0, static_cast<int>(0xCCAA009Eu), 1, 0x751997D0);
            __m128i x = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(data)),
                                      _mm_cvtsi32_si128(static_cast<int>(state)));
            for (data += 16, count -= 16; count >= 16; data += 16, count -= 16)
                x = _mm_xor_si128(
                    _mm_xor_si128(_mm_clmulepi64_si128(x, fold, 0x00), _mm_clmulepi64_si128(x, fold, 0x11)),
                    _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(data)));
            x = _mm_xor_si128(_mm_clmulepi64_si128(x, fold, 0x10), _mm_srli_si128(x, 8));
            x = _mm_xor_si128(_mm_clmulepi64_si128(_mm_blend_epi16(_mm_setzero_si128(), x, 3),
                                                   _mm_cvtsi64_si128(0x163CD6124ll), 0x00),
                              _mm_srli_si128(x, 4));
            value = static_cast<u64>(_mm_cvtsi128_si64(x));
        } else {
            const u32 take = minimum(count, 4u);
            u32 word = 0;
            for (u32 index = 0; index < take; ++index)
                word |= static_cast<u32>(data[index]) << (8 * index);
            value = (state ^ word) << (32 - 8 * take);
            data += take;
            count -= take;
        }
        state = barrett(value);
    }
    return ~static_cast<u32>(state);
}

} // namespace maicord
