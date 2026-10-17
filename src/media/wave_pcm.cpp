#include "maicord/media/wave_pcm.hpp"

namespace maicord {
namespace {
u16 le16(const u8* p) noexcept {
    return static_cast<u16>(u16{p[0]} | (u16{p[1]} << 8));
}
u32 le32(const u8* p) noexcept {
    return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}
bool span(const void* p, u64 bytes) noexcept {
    return (!bytes || p) && bytes <= ~usize{0} - reinterpret_cast<usize>(p);
}
bool overlap(const void* a, u64 an, const void* b, u64 bn) noexcept {
    if (!an || !bn)
        return false;
    const auto av = reinterpret_cast<usize>(a), bv = reinterpret_cast<usize>(b);
    return !span(a, an) || !span(b, bn) || (av < bv + bn && bv < av + an);
}
bool valid(const WavePcmSource& s) noexcept {
    if (!s.ready || !s.encoded.count || s.encoded.count > kWavePcmMaximumEncodedBytes ||
        !span(s.encoded.data, s.encoded.count) || !s.data.count || !span(s.data.data, s.data.count) ||
        (s.channels != 1 && s.channels != 2) || s.sample_rate < 8000 || s.sample_rate > 192000 ||
        (s.container_bits != 8 && s.container_bits != 16 && s.container_bits != 24 &&
         s.container_bits != 32) ||
        !s.valid_bits || s.valid_bits > s.container_bits ||
        s.block_align != s.channels * (s.container_bits / 8) || s.data.count % s.block_align ||
        s.total_frames != s.data.count / s.block_align ||
        s.duration_ms != s.total_frames * 1000 / s.sample_rate ||
        (s.channels == 1 ? (s.channel_mask != 0 && s.channel_mask != 4)
                         : (s.channel_mask != 0 && s.channel_mask != 3)) ||
        (s.encoding != WavePcmEncoding::integer_pcm && s.encoding != WavePcmEncoding::ieee_float) ||
        (s.encoding == WavePcmEncoding::ieee_float && (s.container_bits != 32 || s.valid_bits != 32)))
        return false;
    const auto start = reinterpret_cast<usize>(s.encoded.data), data = reinterpret_cast<usize>(s.data.data);
    return data >= start && data - start <= s.encoded.count &&
           s.data.count <= s.encoded.count - (data - start);
}
WavePcmError format(WavePcmSource& s, const u8* p, u32 bytes) noexcept {
    if (bytes < 16)
        return WavePcmError::malformed;
    u16 tag = le16(p);
    const u16 channels = le16(p + 2);
    s.sample_rate = le32(p + 4);
    s.block_align = le16(p + 12);
    s.container_bits = le16(p + 14);
    s.valid_bits = s.container_bits;
    if (bytes != 16 && (bytes < 18 || u32{le16(p + 16)} != bytes - 18))
        return WavePcmError::malformed;
    if (tag == 0xfffe) {
        if (bytes < 40 || le16(p + 16) < 22)
            return WavePcmError::malformed;
        s.valid_bits = le16(p + 18);
        s.channel_mask = le32(p + 20);
        static constexpr u8 guid_tail[12]{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (!bytes_equal(p + 28, guid_tail, 12) || le32(p + 24) > 65535)
            return WavePcmError::unsupported;
        tag = static_cast<u16>(le32(p + 24));
    }
    if (tag != 1 && tag != 3)
        return WavePcmError::unsupported;
    if ((channels != 1 && channels != 2) || s.sample_rate < 8000 || s.sample_rate > 192000 ||
        (s.container_bits != 8 && s.container_bits != 16 && s.container_bits != 24 &&
         s.container_bits != 32) ||
        (tag == 3 && s.container_bits != 32))
        return WavePcmError::unsupported;
    s.channels = static_cast<u8>(channels);
    s.encoding = tag == 3 ? WavePcmEncoding::ieee_float : WavePcmEncoding::integer_pcm;
    if (!s.valid_bits || s.valid_bits > s.container_bits || (tag == 3 && s.valid_bits != 32) ||
        s.block_align != channels * (s.container_bits / 8) || le32(p + 8) != s.sample_rate * s.block_align)
        return WavePcmError::malformed;
    if (channels == 1 ? (s.channel_mask && s.channel_mask != 4) : (s.channel_mask && s.channel_mask != 3))
        return WavePcmError::unsupported;
    return WavePcmError::none;
}
u32 sample_bits(const u8* p, u32 bytes) noexcept {
    u32 v = 0;
    for (u32 i = 0; i < bytes; ++i)
        v |= u32{p[i]} << (8 * i);
    return v;
}
} // namespace
WavePcmError wave_pcm_parse(WavePcmSource* output, ConstSlice<u8> encoded) noexcept {
    if (!output || !encoded.data || !span(encoded.data, encoded.count) ||
        overlap(output, sizeof(*output), encoded.data, encoded.count))
        return WavePcmError::invalid_input;
    if (encoded.count > kWavePcmMaximumEncodedBytes)
        return WavePcmError::capacity;
    if (encoded.count < 12)
        return WavePcmError::truncated;
    if (!bytes_equal(encoded.data, "RIFF", 4) || !bytes_equal(encoded.data + 8, "WAVE", 4))
        return WavePcmError::unsupported;
    const u64 end = u64{le32(encoded.data + 4)} + 8;
    if (end > encoded.count)
        return WavePcmError::truncated;
    if (end != encoded.count || end < 12)
        return WavePcmError::malformed;
    WavePcmSource candidate{};
    candidate.encoded = encoded;
    bool fmt = false, data = false;
    u32 offset = 12, chunks = 0;
    while (offset < encoded.count) {
        if (++chunks > kWavePcmMaximumChunks)
            return WavePcmError::capacity;
        if (encoded.count - offset < 8)
            return WavePcmError::truncated;
        const auto* chunk = encoded.data + offset;
        const u32 size = le32(chunk + 4);
        const u64 next = u64{offset} + 8 + size + (size & 1u);
        if (next > encoded.count)
            return WavePcmError::truncated;
        if (bytes_equal(chunk, "fmt ", 4)) {
            if (fmt || data)
                return WavePcmError::malformed;
            const auto result = format(candidate, chunk + 8, size);
            if (result != WavePcmError::none)
                return result;
            fmt = true;
        } else if (bytes_equal(chunk, "data", 4)) {
            if (!fmt || data || !size || size % candidate.block_align)
                return WavePcmError::malformed;
            candidate.data = {chunk + 8, size};
            data = true;
        }
        offset = static_cast<u32>(next);
    }
    if (!fmt || !data)
        return WavePcmError::malformed;
    candidate.total_frames = candidate.data.count / candidate.block_align;
    candidate.duration_ms = candidate.total_frames * 1000 / candidate.sample_rate;
    candidate.ready = true;
    *output = candidate;
    return WavePcmError::none;
}
WavePcmDecodeResult wave_pcm_decode(const WavePcmSource& source, u64 first_frame, u32 requested,
                                    Slice<float> output) noexcept {
    if (!valid(source) || !requested || requested > kWavePcmMaximumDecodeFrames ||
        first_frame > source.total_frames || !span(output.data, u64{output.count} * sizeof(float)) ||
        overlap(output.data, u64{output.count} * sizeof(float), source.encoded.data,
                source.encoded.count) ||
        overlap(output.data, u64{output.count} * sizeof(float), &source, sizeof(source)))
        return {0, WavePcmError::invalid_input};
    if (first_frame == source.total_frames)
        return {0, WavePcmError::end_of_stream};
    const u32 frames = static_cast<u32>(minimum<u64>(requested, source.total_frames - first_frame));
    const u32 samples = frames * source.channels, bytes = source.container_bits / 8;
    if (samples > output.count)
        return {0, WavePcmError::capacity};
    const u8* p = source.data.data + first_frame * source.block_align;
    const u32 shift = source.container_bits - source.valid_bits;
    const u32 padding = (u32{1} << shift) - 1;
    for (u32 i = 0; i < samples; ++i) {
        const u32 bits = sample_bits(p + i * bytes, bytes);
        if (source.encoding == WavePcmEncoding::ieee_float ? ((bits & 0x7f800000u) == 0x7f800000u)
                                                           : ((bits & padding) != 0))
            return {0, WavePcmError::malformed};
    }
    for (u32 i = 0; i < samples; ++i) {
        const u32 bits = sample_bits(p + i * bytes, bytes);
        float value = 0;
        if (source.encoding == WavePcmEncoding::ieee_float) {
            copy_bytes(&value, &bits, sizeof(value));
            if (value > 1)
                value = 1;
            if (value < -1)
                value = -1;
        } else {
            const u32 magnitude = bits >> shift;
            const u64 sign = u64{1} << (source.valid_bits - 1);
            const i64 sample =
                source.container_bits == 8
                    ? static_cast<i64>(magnitude) - static_cast<i64>(sign)
                    : ((magnitude & sign) ? static_cast<i64>(magnitude) - static_cast<i64>(sign * 2)
                                          : static_cast<i64>(magnitude));
            value = static_cast<float>(sample) / static_cast<float>(sign);
        }
        output[i] = value;
    }
    return {frames, WavePcmError::none};
}
} // namespace maicord
