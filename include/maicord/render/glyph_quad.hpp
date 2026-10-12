#pragma once

#include "maicord/ui/theme.hpp"

namespace maicord {

struct GlyphQuad {
    float left;
    float top;
    float right;
    float bottom;
    float u0;
    float v0;
    float u1;
    float v1;
    Color color;
};

static_assert(sizeof(GlyphQuad) == 48);

} // namespace maicord
