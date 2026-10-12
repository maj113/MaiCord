#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kMp3MaximumFrames = 1152;
inline constexpr u32 kMp3DecoderStateAlignment = 16;
inline constexpr u32 kMp3DefaultSeekPoints = 256;
enum class Mp3Error : u8 {
    none,
    invalid_input,
    malformed,
    unsupported,
    truncated,
    capacity,
    cancelled,
    end_of_stream,
    need_preroll,
};
using Mp3Cancelled = bool (*)(void*) noexcept;
struct Mp3Stream {
    ConstSlice<u8> encoded{};
    u64 raw_frames{}, total_frames{}, cursor_frame{}, discard_before{};
    u32 sample_rate{}, audio_begin{}, audio_end{}, cursor_offset{}, frame_count{};
    u32 delay_frames{}, padding_frames{};
    u16 frames_per_packet{};
    u8 channels{}, mpeg_version{};
    bool gapless{}, ready{};
};
struct Mp3SeekPoint {
    u64 first_frame{}, main_data_bytes{};
    u32 offset{}, frame_index{};
};
struct Mp3SeekIndex {
    Slice<Mp3SeekPoint> points{};
    const u8* source{};
    u32 source_bytes{}, point_count{}, frame_count{};
};
struct Mp3Packet {
    ConstSlice<u8> bytes{};
    u64 decoded_first_frame{};
    u32 duration_frames{}, trim_front_frames{}, trim_back_frames{};
    Mp3Error error{};
    bool ok() const noexcept {
        return error == Mp3Error::none;
    }
};
struct Mp3Decoder {
    void* state{};
    u32 state_bytes{}, sample_rate{};
    u8 channels{};
    bool initialized{};
};
struct Mp3DecodeResult {
    Mp3Error error{};
    u32 frames{};
    bool ok() const noexcept {
        return error == Mp3Error::none;
    }
};

// Borrowed Layer-III stream of at most 64MiB. Header scan counts actual PCM
// frames; VBR tags are checked metadata, never used for proportional-byte seeking.
// No allocation, decoder, device or whole-file PCM. Failure preserves output.
Mp3Error mp3_stream_initialize(Mp3Stream*, ConstSlice<u8>, Mp3Cancelled = nullptr,
                               void* = nullptr) noexcept;
Mp3Error mp3_stream_build_index(const Mp3Stream&, Mp3SeekIndex*, Slice<Mp3SeekPoint>,
                                Mp3Cancelled = nullptr, void* = nullptr) noexcept;
Mp3Packet mp3_stream_next(Mp3Stream*) noexcept;
// Relative audible frame target. Caller resets decoder and decodes/discards all
// preroll packets; checkpoints cover 511B reservoir before two DSP frames.
// Null index uses the stream start, keeping seeks exact without an allocation.
Mp3Error mp3_stream_seek(Mp3Stream*, u64 target_frame, const Mp3SeekIndex* = nullptr,
                         Mp3Cancelled = nullptr, void* = nullptr) noexcept;
u32 mp3_decoder_storage_bytes() noexcept;
Mp3Error mp3_decoder_initialize(Mp3Decoder*, Slice<u8> state, u32 sample_rate, u32 channels) noexcept;
Mp3Error mp3_decoder_reset(Mp3Decoder*) noexcept;
// Decode one validated frame into caller interleaved float PCM, at most
// 1152 frames/channel. need_preroll is allowed only when caller discards the whole
// packet; never fabricate silence for a missing reservoir or corrupt side info.
Mp3DecodeResult mp3_decoder_decode(Mp3Decoder*, ConstSlice<u8>, Slice<float>) noexcept;
void mp3_decoder_release(Mp3Decoder*) noexcept;
} // namespace maicord
