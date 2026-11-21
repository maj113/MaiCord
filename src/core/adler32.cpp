#include "maicord/core/adler32.hpp"

#include "maicord/sys/simd_haswell.hpp"

namespace maicord {

// A block of 32 bytes adds the sum of its bytes to the first sum. It adds 32 times the first sum and the bytes with
// the weights 32 to 1 to the second sum. At most 173 blocks go between two reductions, so that no lane and no sum
// wraps.
MAICORD_HASWELL u32 adler32_update(u32 adler, ConstSlice<u8> bytes) noexcept {
    static constexpr u8 kWeights[32]{32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                     16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1};
    const __m256i weights = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(kWeights));
    const __m256i zero = _mm256_setzero_si256(), ones = _mm256_set1_epi16(1);
    const u8* data = bytes.data;
    u32 count = bytes.count;
    u64 a = adler & 0xFFFFu, b = adler >> 16;
    while (count >= 32) {
        const u32 blocks = minimum<u32>(count / 32, 173);
        __m256i sums = zero, before = zero, weighted = zero;
        for (u32 i = 0; i != blocks; ++i, data += 32) {
            const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(data));
            before = _mm256_add_epi64(before, sums);
            sums = _mm256_add_epi64(sums, _mm256_sad_epu8(block, zero));
            weighted = _mm256_add_epi32(weighted, _mm256_madd_epi16(_mm256_maddubs_epi16(block, weights), ones));
        }
        u64 wide[12];
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(wide), sums);
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(wide + 4), before);
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(wide + 8), weighted);
        b += u64{blocks} * 32 * a + 32 * (wide[4] + wide[5] + wide[6] + wide[7]);
        for (u32 i = 8; i != 12; ++i) b += (wide[i] & 0xffffffffu) + (wide[i] >> 32);
        a = (a + wide[0] + wide[1] + wide[2] + wide[3]) % 65521;
        b %= 65521;
        count -= blocks * 32;
    }
    // At most 31 bytes: an unrolled or vector copy of this loop costs more bytes than it saves time.
#pragma clang loop unroll(disable) vectorize(disable)
    for (; count; --count) {
        a += *data++;
        b += a;
    }
    return static_cast<u32>(b % 65521) << 16 | static_cast<u32>(a % 65521);
}

} // namespace maicord
