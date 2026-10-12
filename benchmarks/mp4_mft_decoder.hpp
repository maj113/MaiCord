#pragma once

#include "maicord/platform/mp4_decoder.hpp"

struct IMFTransform;

namespace maicord {

// Custom ISO-BMFF pipeline with a directly hosted H.264 transform. This skips
// MF's byte stream, media source, resolver, source reader and video processor.
// The transform is only the codec primitive; MaiCord owns demux, timestamps,
// looping, surface reuse and YUV presentation.
struct Mp4DirectDecoder {
    void* state;
    void* rgba8;
    const GpuVideoFrame* gpu;
    u32 rgba8_capacity;
    u32 width;
    u32 height;
    u32 row_pitch;
    bool ready;
    u8 reserved[7];
};

struct Mp4DirectOutputLayout {
    u32 storage_width;
    u32 storage_height;
    u32 pitch;
    u32 crop_x;
    u32 crop_y;
    u32 width;
    u32 height;
};

// A non-null device requests GPU output (never a hidden pixel readback).
// A device shared with presentation must have multithread protection enabled
// before this call. The public wrapper owns software negotiation fallback.
bool mp4_direct_decoder_initialize(Mp4DirectDecoder* decoder,
                                   ConstSlice<u8> encoded,
                                   ID3D11Device* video_device = nullptr) noexcept;
Mp4DecodedFrame mp4_direct_decoder_read(Mp4DirectDecoder* decoder,
                                        bool loop_at_end = true) noexcept;
void mp4_direct_decoder_release(Mp4DirectDecoder* decoder) noexcept;
i32 mp4_direct_decoder_last_status(const Mp4DirectDecoder* decoder) noexcept;
u32 mp4_direct_decoder_color_matrix(const Mp4DirectDecoder* decoder) noexcept;
u32 mp4_direct_decoder_nominal_range(const Mp4DirectDecoder* decoder) noexcept;
Mp4DirectOutputLayout mp4_direct_decoder_output_layout(
    const Mp4DirectDecoder* decoder) noexcept;
// Optional diagnostic snapshot of the most recently decoded native sample.
// This is not called by playback and is discarded from the shipping link.
bool mp4_direct_decoder_copy_i420(const Mp4DirectDecoder* decoder,
                                  Slice<u8> destination) noexcept;
// Borrowed diagnostic access, never retained beyond the decoder lifetime.
// Benchmark-only callers can inspect codec attributes without putting probe
// switches, environment reads or timing calls in the shipping decode loop.
IMFTransform* mp4_direct_decoder_transform(const Mp4DirectDecoder* decoder) noexcept;

static_assert(sizeof(Mp4DirectDecoder) <= 48);

} // namespace maicord
