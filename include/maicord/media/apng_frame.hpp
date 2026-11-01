#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

enum class ApngDispose : u8 {
    none,
    background,
    previous,
};

enum class ApngBlend : u8 {
    source,
    over,
};

struct ApngFrame {
    u32 first_span;
    u32 compressed_bytes;
    u32 width;
    u32 height;
    u32 x_offset;
    u32 y_offset;
    u16 span_count;
    u16 delay_num;
    u16 delay_den;
    ApngDispose dispose;
    ApngBlend blend;
};

static_assert(sizeof(ApngFrame) == 32);

} // namespace maicord
