#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

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

static_assert(sizeof(AnimatedWebpFrame) == 24);

} // namespace maicord
