#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// Movies and what libavformat and libavcodec decode from them, from tools/generate_mp4_fixtures.py (libx264 and the
// native AAC encoder muxed by the mp4 muxer; layout of the .ref files in the generator's header).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kMp4H264AacBFrames[]{
#embed "h264_aac_bframes.mp4"
};
inline constexpr u8 kMp4H264AacBFramesRef[]{
#embed "h264_aac_bframes.ref"
};
inline constexpr u8 kMp4H264BaselineGops[]{
#embed "h264_baseline_gops.mp4"
};
inline constexpr u8 kMp4H264BaselineGopsRef[]{
#embed "h264_baseline_gops.ref"
};
inline constexpr u8 kMp4H264HighCropPyramid[]{
#embed "h264_high_crop_pyramid.mp4"
};
inline constexpr u8 kMp4H264HighCropPyramidRef[]{
#embed "h264_high_crop_pyramid.ref"
};
inline constexpr u8 kMp4H264Fragmented[]{
#embed "h264_fragmented.mp4"
};
inline constexpr u8 kMp4H264FragmentedRef[]{
#embed "h264_fragmented.ref"
};
inline constexpr u8 kMp4HevcMainAac[]{
#embed "hevc_main_aac.mp4"
};
#pragma clang diagnostic pop

// One parsed .ref: the pictures (output order, each with its presentation time and duration) and the PCM.
struct Mp4Reference {
    u32 width, height, frames, rate, channels, samples;
    const u8* frame_data; // frames records of 16 + width * height * 3 / 2 bytes
    const u8* pcm;        // samples * channels int16, little endian
};

inline u32 mp4_reference_load32(const u8* at) noexcept {
    return at[0] | (u32{at[1]} << 8) | (u32{at[2]} << 16) | (u32{at[3]} << 24);
}

inline u32 mp4_reference_record_bytes(const Mp4Reference& r) noexcept {
    return 16u + r.width * r.height * 3u / 2u;
}

inline i64 mp4_reference_time(const Mp4Reference& r, u32 index) noexcept {
    const u8* at = r.frame_data + usize{index} * mp4_reference_record_bytes(r);
    return static_cast<i64>(u64{mp4_reference_load32(at)} | (u64{mp4_reference_load32(at + 4)} << 32));
}

inline u64 mp4_reference_duration(const Mp4Reference& r, u32 index) noexcept {
    const u8* at = r.frame_data + usize{index} * mp4_reference_record_bytes(r) + 8;
    return u64{mp4_reference_load32(at)} | (u64{mp4_reference_load32(at + 4)} << 32);
}

inline const u8* mp4_reference_picture(const Mp4Reference& r, u32 index) noexcept {
    return r.frame_data + usize{index} * mp4_reference_record_bytes(r) + 16;
}

inline bool mp4_reference_parse(const u8* data, usize bytes, Mp4Reference* out) noexcept {
    if (bytes < 28 || mp4_reference_load32(data) != 0x3450324du)
        return false;
    *out = {};
    out->width = mp4_reference_load32(data + 4);
    out->height = mp4_reference_load32(data + 8);
    out->frames = mp4_reference_load32(data + 12);
    out->rate = mp4_reference_load32(data + 16);
    out->channels = mp4_reference_load32(data + 20);
    out->samples = mp4_reference_load32(data + 24);
    out->frame_data = data + 28;
    out->pcm = out->frame_data + usize{out->frames} * mp4_reference_record_bytes(*out);
    return static_cast<usize>(out->pcm - data) + usize{out->samples} * out->channels * 2 == bytes;
}
} // namespace maicord
