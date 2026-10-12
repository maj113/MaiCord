#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/encoded_video_limits.hpp"

namespace maicord {
inline constexpr u32 kOggOpusMaximumPacketBytes = 61'440;
inline constexpr u32 kOggOpusSampleRate = 48'000;
inline constexpr u32 kOggOpusSeekPrerollFrames = 3'840;
enum class OggOpusError : u8 {
    none,
    invalid_input,
    malformed,
    unsupported,
    truncated,
    checksum,
    capacity,
    cancelled,
    end_of_stream
};
using OggOpusCancelled = bool (*)(void*) noexcept;
struct OggOpusCursor {
    u32 page_offset{}, data_offset{}, segment_index{}, packet_index{};
    // Geometry belongs only to page_offset. Zero end means not yet cached.
    u32 page_body{}, page_end{};
    u64 decoded_frame{};
};
struct OggOpusDemuxer {
    ConstSlice<u8> encoded{};
    OggOpusCursor cursor{};
    u64 granule_origin{}, decoded_frames{}, total_frames{};
    u32 serial{}, first_audio_offset{}, packet_count{}, input_sample_rate{};
    u16 pre_skip{};
    i16 output_gain_q8{};
    u8 channels{};
    bool ready{};
};
struct OggOpusPacket {
    ConstSlice<u8> bytes{};
    u64 decoded_first_frame{};
    u32 duration_frames{}, trim_front_frames{}, trim_back_frames{}, packet_index{};
    OggOpusError error{};
    bool ok() const noexcept {
        return error == OggOpusError::none;
    }
};
struct OggOpusSeekPoint {
    u64 decoded_first_frame{}, target_frame{}, discard_frames{};
    u32 packet_index{};
};
// RFC 3533 page checksum over a complete page; its checksum field counts as zero.
u32 ogg_page_checksum(ConstSlice<u8> page) noexcept;
// One complete immutable, borrowed family-0 mono/stereo stream, at most 64 MiB.
// Initialization validates every page and packet. No index or heap is allocated.
// Scratch is caller-owned and required only when a header/audio packet crosses
// pages. A 61,440-byte slice covers all supported packets, including OpusTags.
// Larger metadata packets fail explicitly with capacity. Failure preserves out.
OggOpusError ogg_opus_demux_initialize(OggOpusDemuxer* out, ConstSlice<u8> encoded, Slice<u8> scratch,
                                       OggOpusCancelled cancelled = nullptr,
                                       void* context = nullptr) noexcept;
// Decode the complete packet, then retain [trim_front,duration-trim_back).
// Contiguous bytes borrow encoded. Continued bytes borrow scratch until reuse.
// Apply output_gain_q8 once in addition to user volume. input_sample_rate is
// metadata, never the output clock. Times use frames per channel at 48 kHz.
OggOpusPacket ogg_opus_demux_next(OggOpusDemuxer*, Slice<u8> scratch) noexcept;
void ogg_opus_demux_reset(OggOpusDemuxer*) noexcept;
// Relative audible-frame target in [0,total_frames], not absolute granules.
// Select a packet at/before target minus 80 ms, or stream start with pre-skip.
// Caller resets its decoder and discards decoded samples through the target.
// No decoder delay is inferred. Failure preserves cursor and seek-point output.
OggOpusError ogg_opus_demux_seek(OggOpusDemuxer*, u64 target_frame, OggOpusSeekPoint*, Slice<u8> scratch,
                                 OggOpusCancelled cancelled = nullptr, void* context = nullptr) noexcept;
} // namespace maicord
