#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Renderer-neutral path commands. The 28-byte command layout is fixed: shader
// uploads depend on it.
enum class GlyphPathCommandKind : u8 {
    begin_filled, begin_hollow, line, cubic, end_open, end_closed,
};

enum class GlyphFillRule : u8 { alternate, winding };

struct GlyphPathCommand {
    float x1, y1, x2, y2, x3, y3;
    GlyphPathCommandKind kind;
    u8 reserved[3];
};

struct GlyphOutline {
    GlyphPathCommand* commands;
    u32 capacity;
    u32 count;
    u32 rejected;
    float minimum_x, minimum_y, maximum_x, maximum_y;
    GlyphFillRule fill_rule;
    bool has_bounds;
};

static_assert(sizeof(GlyphPathCommand) == 28);
static_assert(sizeof(GlyphOutline) == 40);

} // namespace maicord
