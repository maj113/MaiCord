#include "maicord/app/voice/mixer.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord {
// The vector loops give the same samples as the scalar tails beside them. Each handles 16-bit values, so a
// multiplication by ceil(2^21 / 100) and a shift divide by 100 exactly (for values below 43,690).
MAICORD_HASWELL void voice_mixer_accumulate(Slice<i32> mix, ConstSlice<i16> pcm, u32 gain_percent) noexcept {
    const i32 gain = static_cast<i32>(minimum(gain_percent, kVoiceGainMaximumPercent));
    const u32 count = minimum(mix.count, pcm.count);
    if (!gain || !mix.data || !pcm.data)
        return;
    i32* out = mix.data;
    const i16* in = pcm.data;
    u32 i = 0;
    if (gain == 100) {
        for (; i + 8 <= count; i += 8) {
            const __m256i sample = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(in + i)));
            auto* const sum = reinterpret_cast<__m256i_u*>(out + i);
            _mm256_storeu_si256(sum, _mm256_add_epi32(_mm256_loadu_si256(sum), sample));
        }
    } else {
        const __m256i hundredth = _mm256_set1_epi32(20972), hundred = _mm256_set1_epi32(100);
        const __m256i factor = _mm256_set1_epi32(gain);
        for (; i + 8 <= count; i += 8) {
            // magnitude = 100 * whole + rest, so magnitude * gain / 100 = whole * gain + rest * gain / 100.
            const __m256i sample = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(in + i)));
            const __m256i magnitude = _mm256_abs_epi32(sample);
            const __m256i whole = _mm256_srli_epi32(_mm256_mulhi_epu16(magnitude, hundredth), 5);
            const __m256i rest = _mm256_sub_epi32(magnitude, _mm256_mullo_epi16(whole, hundred));
            const __m256i part = _mm256_srli_epi32(_mm256_mulhi_epu16(_mm256_mullo_epi16(rest, factor), hundredth), 5);
            const __m256i scaled = _mm256_add_epi32(_mm256_madd_epi16(whole, factor), part);
            auto* const sum = reinterpret_cast<__m256i_u*>(out + i);
            _mm256_storeu_si256(sum, _mm256_add_epi32(_mm256_loadu_si256(sum), _mm256_sign_epi32(scaled, sample)));
        }
    }
#pragma clang loop vectorize(disable) unroll(disable)
    for (; i < count; ++i)
        out[i] += in[i] * gain / 100;
}

i16 voice_mixer_soft_clip(i32 sample) noexcept {
    const i64 magnitude = sample < 0 ? -static_cast<i64>(sample) : sample;
    if (magnitude <= kVoiceSoftClipKnee)
        return static_cast<i16>(sample);
    // Rational knee: unit slope at the knee, asymptotic to 32767, monotonic.
    constexpr i64 range = 32767 - kVoiceSoftClipKnee;
    const i64 excess = magnitude - kVoiceSoftClipKnee;
    const i64 bent = kVoiceSoftClipKnee + range * excess / (excess + range);
    return static_cast<i16>(sample < 0 ? -bent : bent);
}

namespace {
// voice_mixer_soft_clip for eight samples. With range = 32767 - knee, range * excess / (excess + range) is
// range - 1 - (range * range - 1) / (excess + range). A float holds the dividend exactly, and the float quotient
// is the exact one or one beside it: the remainder tells which.
MAICORD_HASWELL __m256i soft_clip(__m256i sample) noexcept {
    constexpr i32 range = 32767 - kVoiceSoftClipKnee, dividend = range * range - 1;
    const __m256i knee = _mm256_set1_epi32(kVoiceSoftClipKnee);
    // The curve is flat long before 2^30, and with this limit each sum stays in 32 bits.
    const __m256i magnitude = _mm256_min_epu32(_mm256_abs_epi32(sample), _mm256_set1_epi32(1 << 30));
    const __m256i excess = _mm256_max_epi32(_mm256_sub_epi32(magnitude, knee), _mm256_setzero_si256());
    const __m256i divisor = _mm256_add_epi32(excess, _mm256_set1_epi32(range));
    const __m256i quotient = _mm256_cvttps_epi32(
        _mm256_div_ps(_mm256_set1_ps(static_cast<float>(dividend)), _mm256_cvtepi32_ps(divisor)));
    const __m256i remainder =
        _mm256_sub_epi32(_mm256_set1_epi32(dividend), _mm256_mullo_epi32(quotient, divisor));
    // The first term is -1 for a quotient that is one too large. The second is -1 unless it is one too small.
    const __m256i below = _mm256_add_epi32(
        _mm256_add_epi32(quotient, _mm256_srai_epi32(remainder, 31)), _mm256_cmpgt_epi32(divisor, remainder));
    const __m256i bent = _mm256_sub_epi32(
        _mm256_add_epi32(_mm256_min_epi32(magnitude, knee), _mm256_set1_epi32(range - 2)), below);
    return _mm256_sign_epi32(bent, sample);
}
} // namespace

MAICORD_HASWELL void voice_mixer_finish(ConstSlice<i32> mix, Slice<i16> output) noexcept {
    const u32 count = minimum(mix.count, output.count);
    const i32* in = mix.data;
    i16* out = output.data;
    const __m256i knee = _mm256_set1_epi16(kVoiceSoftClipKnee);
    const __m256i span = _mm256_set1_epi16(static_cast<i16>(2 * kVoiceSoftClipKnee));
    u32 i = 0;
    for (; i + 16 <= count; i += 16) {
        const __m256i low = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(in + i));
        const __m256i high = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(in + i + 8));
        // Mixed voices rarely reach the knee. A sample that the pack saturates is also beyond the knee.
        __m256i packed = _mm256_packs_epi32(low, high);
        const __m256i beyond = _mm256_subs_epu16(_mm256_add_epi16(packed, knee), span);
        if (!_mm256_testz_si256(beyond, beyond))
            packed = _mm256_packs_epi32(soft_clip(low), soft_clip(high));
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(out + i), _mm256_permute4x64_epi64(packed, 0xd8));
    }
    for (; i < count; ++i)
        out[i] = voice_mixer_soft_clip(in[i]);
}

MAICORD_HASWELL u32 voice_pcm_level(ConstSlice<i16> pcm) noexcept {
    if (!pcm.data || !pcm.count)
        return 0;
    // The magnitudes are 16-bit unsigned values. All their bytes add up in 64-bit lanes, and so do the high bytes.
    const __m256i zero = _mm256_setzero_si256();
    __m256i bytes = zero, high = zero;
    u32 i = 0;
    for (; i + 16 <= pcm.count; i += 16) {
        const __m256i magnitude =
            _mm256_abs_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(pcm.data + i)));
        bytes = _mm256_add_epi64(bytes, _mm256_sad_epu8(magnitude, zero));
        high = _mm256_add_epi64(high, _mm256_sad_epu8(_mm256_srli_epi16(magnitude, 8), zero));
    }
    const __v4du tops = static_cast<__v4du>(high), lanes = static_cast<__v4du>(bytes) + (tops << 8) - tops;
    u64 sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
#pragma clang loop vectorize(disable) unroll(disable)
    for (; i < pcm.count; ++i)
        sum += static_cast<u32>(pcm[i] < 0 ? -static_cast<i32>(pcm[i]) : pcm[i]);
    return static_cast<u32>(sum / pcm.count);
}
} // namespace maicord
