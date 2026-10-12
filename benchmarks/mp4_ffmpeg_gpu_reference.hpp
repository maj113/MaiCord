#pragma once

#include "maicord/platform/mp4_decoder.hpp"

namespace maicord {
// Optional benchmark dependency only. AVFrame owns the borrowed texture until
// the next read/release, just like the MFT sample lease used by the other paths.
struct Mp4FfmpegGpuReference {
    void* state;
    GpuVideoFrame frame;
    i32 status;
};
bool mp4_ffmpeg_gpu_initialize(Mp4FfmpegGpuReference* reference,
    ConstSlice<u8> encoded, ID3D11Device* device) noexcept;
Mp4DecodedFrame mp4_ffmpeg_gpu_read(Mp4FfmpegGpuReference* reference,
    bool loop = false) noexcept;
void mp4_ffmpeg_gpu_release(Mp4FfmpegGpuReference* reference) noexcept;
} // namespace maicord
