#pragma once

#include "maicord/platform/mp4_decoder.hpp"

struct IMFSourceReader;

namespace maicord {

// Benchmark-only MF reference. Never linked into the app or account DLL.
struct Mp4ReferenceDecoder {
    ::IMFSourceReader* reader;
    void* direct_decoder;
    void* rgba8;
    const GpuVideoFrame* gpu;
    u32 rgba8_capacity;
    u32 width;
    u32 height;
    u32 row_pitch;
    u64 fallback_duration_us;
    bool ready;
    u8 reserved[7];
};

bool mp4_reference_thread_start() noexcept;
void mp4_reference_thread_stop() noexcept;
bool mp4_reference_initialize(Mp4ReferenceDecoder* decoder, ConstSlice<u8> encoded,
                            u16 requested_width,
                            u16 requested_height,
                            ID3D11Device* video_device = nullptr) noexcept;
// Explicit Source Reader reference, not a production fallback.
bool mp4_reference_initialize_source_reader(Mp4ReferenceDecoder* decoder,
                                          ConstSlice<u8> encoded,
                                          u16 requested_width,
                                          u16 requested_height) noexcept;
Mp4DecodedFrame mp4_reference_read(Mp4ReferenceDecoder* decoder,
                                 bool loop_at_end = true) noexcept;
void mp4_reference_release(Mp4ReferenceDecoder* decoder) noexcept;

static_assert(sizeof(Mp4ReferenceDecoder) <= 64);

} // namespace maicord
