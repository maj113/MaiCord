#pragma once

#include "maicord/media/lossy_webp.hpp"

namespace maicord {

// Bound the complete animation separately from each compressed still frame.
// Match the CDN body budget without allocating a decoded canvas per frame.
inline constexpr u32 kAnimatedWebpEncodedMaximumBytes = 16u * 1024u * 1024u;
inline constexpr u32 kAnimatedWebpInlineFrameCapacity = 256;
// Match the playback timeline. Longer animations grow metadata on demand;
// decoded frames still use a single reusable canvas.
inline constexpr u32 kAnimatedWebpFrameCapacity = 0xFFFFu;
inline constexpr u64 kAnimatedWebpFrameFloorUs = 10'000;

enum class AnimatedWebpBlend : u8 {
    over,
    source,
};

enum class AnimatedWebpDispose : u8 {
    none,
    background,
};

enum class AnimatedWebpCodec : u8 {
    vp8l_lossless,
    vp8_lossy,
};

enum class AnimatedWebpError : u8 {
    none,
    invalid_input,
    encoded_limit,
    truncated,
    riff,
    chunk,
    duplicate_chunk,
    order,
    unsupported_feature,
    dimensions,
    frame_count,
    rectangle,
    duration,
    output_capacity,
    frame_decode,
};

struct AnimatedWebpFrame {
    u32 payload_offset;
    u32 payload_bytes;
    u32 duration_ms;
    u16 x_offset;
    u16 y_offset;
    u16 width;
    u16 height;
    AnimatedWebpBlend blend;
    AnimatedWebpDispose dispose;
    bool has_alpha;
    AnimatedWebpCodec codec;
};

// One validated table for one immutable cache lease identity. Encoded payloads
// remain offsets in frames; this record owns no pointer into encoded bytes.
struct AnimatedWebpRetainedMetadata {
    u64 content_identity;
    const AnimatedWebpFrame* frames;
    u32 background_rgba;
    u32 frame_count;
    u32 play_count;
    u32 chunks;
    u32 frame_chunks;
    u16 width;
    u16 height;
    bool has_alpha;
    bool has_color_profile;
    bool lossy_present;
};

struct AnimatedWebpWorkspace {
    AnimatedWebpFrame frames[kAnimatedWebpInlineFrameCapacity];
    Slice<AnimatedWebpFrame> extended_frames;
    // The owner replaces extended_frames, preserving its previous contents.
    // A null callback leaves storage entirely caller supplied.
    bool (*grow_frames)(AnimatedWebpWorkspace*, u32 required) noexcept;
    // Direct parsing and pipeline binding clear identity before table reuse.
    AnimatedWebpRetainedMetadata retained;
};

struct AnimatedWebpImage {
    const AnimatedWebpFrame* frames;
    u32 background_rgba;
    u32 frame_count;
    u32 play_count;
    u16 width;
    u16 height;
    bool has_alpha;
    bool has_color_profile;
};

struct AnimatedWebpParseResult {
    u32 chunks;
    u32 frame_chunks;
    AnimatedWebpError error;

    constexpr bool ok() const noexcept {
        return error == AnimatedWebpError::none;
    }
};

struct AnimatedWebpDecodeResult {
    AnimatedWebpParseResult parse;
    WebpDecodeResult frame_decode;
    LossyWebpDecodeResult lossy_decode;
    WebpImage frame_image;
    AnimatedWebpFrame frame;
    u64 duration_us;
    u32 frame_index;
    AnimatedWebpError error;

    constexpr bool ok() const noexcept {
        return error == AnimatedWebpError::none;
    }
};

AnimatedWebpParseResult animated_webp_parse(
    ConstSlice<u8> encoded, AnimatedWebpImage* image,
    AnimatedWebpWorkspace* workspace) noexcept;
AnimatedWebpDecodeResult animated_webp_decode_frame_rgba8(
    ConstSlice<u8> encoded, u32 frame_index,
    AnimatedWebpImage* image, AnimatedWebpWorkspace* animation_workspace,
    Vp8lWorkspace* vp8l_workspace, Slice<u8> rgba_output) noexcept;
AnimatedWebpDecodeResult animated_webp_decode_frame(
    ConstSlice<u8> encoded, u32 frame_index, AnimatedWebpImage* image,
    AnimatedWebpWorkspace* animation_workspace,
    Vp8lWorkspace* vp8l_workspace,
    LossyWebpWorkspace* lossy_workspace, Vp8Keyframe* vp8_frame,
    Vp8ControlHeader* vp8_header, Slice<u8> rgba_output,
    Slice<u8> alpha_rgba_scratch, Slice<u8> alpha_output) noexcept;
AnimatedWebpDecodeResult animated_webp_decode_parsed_frame(
    ConstSlice<u8> encoded, const AnimatedWebpImage& image,
    u32 frame_index, Vp8lWorkspace* vp8l_workspace,
    LossyWebpWorkspace* lossy_workspace, Vp8Keyframe* vp8_frame,
    Vp8ControlHeader* vp8_header, Slice<u8> rgba_output,
    Slice<u8> alpha_rgba_scratch, Slice<u8> alpha_output) noexcept;

static_assert(sizeof(AnimatedWebpFrame) == 24);
static_assert(sizeof(AnimatedWebpRetainedMetadata) == 48);
static_assert(sizeof(AnimatedWebpWorkspace) == 6'216);
static_assert(sizeof(AnimatedWebpImage) == 32);

} // namespace maicord
