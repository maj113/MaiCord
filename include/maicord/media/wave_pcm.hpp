#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kWavePcmMaximumEncodedBytes = 64u * 1024u * 1024u;
inline constexpr u32 kWavePcmMaximumChunks = 4096;
inline constexpr u32 kWavePcmMaximumDecodeFrames = 8192;
enum class WavePcmError : u8 {
    none,
    invalid_input,
    malformed,
    unsupported,
    truncated,
    capacity,
    end_of_stream
};
enum class WavePcmEncoding : u8 {
    integer_pcm,
    ieee_float
};
struct WavePcmSource {
    ConstSlice<u8> encoded{}, data{};
    u64 total_frames{}, duration_ms{};
    u32 sample_rate{}, channel_mask{};
    u16 block_align{}, container_bits{}, valid_bits{};
    u8 channels{};
    WavePcmEncoding encoding{};
    bool ready{};
};
struct WavePcmDecodeResult {
    u32 frames{};
    WavePcmError error{};
    bool ok() const noexcept {
        return error == WavePcmError::none;
    }
};
// Complete little-endian RIFF/WAVE only. The source borrows immutable encoded
// storage until the last decode. No index, scratch allocation, or host API is used.
// One fmt and one nonempty data chunk, exact RIFF length and word padding required.
// PCM 8/16/24/32 or float32, mono/stereo, 8..192 kHz. Extensible PCM valid bits
// are left aligned; mono unspecified/front-center and stereo unspecified/FL+FR.
// Failure preserves output. Float finiteness/PCM padding are checked per block.
WavePcmError wave_pcm_parse(WavePcmSource* output, ConstSlice<u8> encoded) noexcept;
// Frames are per channel. Decode at most requested_frames (8192 max), clipping
// only at EOF; output contains interleaved float samples. PCM is normalized by
// 2^(valid_bits-1); finite IEEE samples are clamped to [-1,1]. Capacity, alias,
// invalid padding, and nonfinite sample failures write nothing. Source unchanged.
WavePcmDecodeResult wave_pcm_decode(const WavePcmSource& source, u64 first_frame, u32 requested_frames,
                                    Slice<float> output) noexcept;
} // namespace maicord
