#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// Elementary streams and reference pictures from tools/generate_video_fixtures.py (libx264, libx265 and libaom over a
// synthetic moving picture; libavcodec's h264 and libaom decoders as the oracle). Layout in the generator's header.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kVideoH264Baseline[]{
#embed "h264_baseline.bin"
};
inline constexpr u8 kVideoH264MainBFrames[]{
#embed "h264_main_bframes.bin"
};
inline constexpr u8 kVideoH264MainWeighted[]{
#embed "h264_main_weighted.bin"
};
inline constexpr u8 kVideoH264HighCropSlices[]{
#embed "h264_high_crop_slices.bin"
};
inline constexpr u8 kVideoHevcMain[]{
#embed "hevc_main.bin"
};
inline constexpr u8 kVideoAv1Basic[]{
#embed "av1_basic.bin"
};
inline constexpr u8 kVideoAv1Tiles[]{
#embed "av1_tiles.bin"
};
#pragma clang diagnostic pop

// One parsed fixture: the packets in decoding order and the reference pictures in output order.
inline constexpr u32 kVideoFixtureMaxPackets = 16;
struct VideoFixture {
    const char* name;
    u32 codec; // 0 H.264, 1 HEVC, 2 AV1
    u32 width, height;
    u32 packet_count, picture_count;
    const u8* packet[kVideoFixtureMaxPackets];
    u32 packet_bytes[kVideoFixtureMaxPackets];
    bool keyframe[kVideoFixtureMaxPackets];
    const u8* pictures; // picture_count * width * height * 3 / 2 bytes
};

inline u32 video_fixture_load32(const u8* at) noexcept {
    return at[0] | (u32{at[1]} << 8) | (u32{at[2]} << 16) | (u32{at[3]} << 24);
}

inline bool video_fixture_parse(const char* name, const u8* data, usize bytes, VideoFixture* out) noexcept {
    if (bytes < 24 || video_fixture_load32(data) != 0x4449564du)
        return false;
    *out = {};
    out->name = name;
    out->codec = video_fixture_load32(data + 4);
    out->width = video_fixture_load32(data + 8);
    out->height = video_fixture_load32(data + 12);
    out->packet_count = video_fixture_load32(data + 16);
    out->picture_count = video_fixture_load32(data + 20);
    if (out->packet_count > kVideoFixtureMaxPackets)
        return false;
    usize at = 24;
    for (u32 i = 0; i < out->packet_count; ++i) {
        if (at + 8 > bytes)
            return false;
        out->packet_bytes[i] = video_fixture_load32(data + at);
        out->keyframe[i] = (video_fixture_load32(data + at + 4) & 1u) != 0;
        out->packet[i] = data + at + 8;
        at += 8 + ((usize{out->packet_bytes[i]} + 3) & ~usize{3});
    }
    out->pictures = data + at;
    return at + usize{out->picture_count} * out->width * out->height * 3 / 2 == bytes;
}
} // namespace maicord
