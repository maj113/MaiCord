#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

struct PaintQuad {
    float left;
    float top;
    float right;
    float bottom;
    Color fill;
    Color border;
    float radius;
    float border_width;
    float reserved_x;
    float reserved_y;
};
static_assert(sizeof(PaintQuad) == 64);

inline constexpr u32 kMaxPaintQuads = 4096;
inline constexpr u32 kInitialPaintQuads = 64;
// Whole-frame bounds: the shell plus every overlay share one stream, and an
// overflow rejects the frame. Overlays clip per element (a profile popout's
// role pills and badges, a 48-row Roles menu's dots and checks), so the clip
// limit must stay well above one overlay's count.
inline constexpr u32 kMaxPaintClips = 256;
inline constexpr u32 kMaxPaintBatches = 256;
inline constexpr u32 kMaxPaintClipDepth = 16;
inline constexpr float kMaximumPaintCoordinate = 1'048'576.0f;

inline bool paint_coordinate_valid(float value) noexcept {
    return value == value && value >= -kMaximumPaintCoordinate && value <= kMaximumPaintCoordinate;
}

struct PaintClip {
    float left;
    float top;
    float right;
    float bottom;
};

struct PaintBatch {
    u32 first_quad;
    u32 quad_count;
    u16 clip_index;
    u16 reserved;
};

struct UiPaintContentBounds {
    UiWidgetReference owner;
    UiRect rectangle;
    UiLayoutContentKind kind;
    u16 flags;
};

struct UiPaintContentRecorder {
    Slice<UiPaintContentBounds> storage;
    u32 count;
    u32 dropped;
};

// Stable-address, demand-committed geometry stream: starts at
// kInitialPaintQuads and grows to kMaxPaintQuads without moving existing
// geometry or changing batch indices.
struct PaintStream {
    UiRect scrollbar_viewport;
    bool filter_scrollbars;
    StableStorage quad_storage;
    PaintQuad* quads;

    PaintClip clips[kMaxPaintClips];
    PaintBatch batches[kMaxPaintBatches];
    u16 clip_stack[kMaxPaintClipDepth];
    u32 count;
    u32 clip_count;
    u32 batch_count;
    u32 clip_depth;
    u32 rejected;
    u32 quad_capacity;
    // Batches below this index are closed (paint_stream_seal): a quad never
    // joins them even under the same clip.
    u32 sealed_batches;
    UiPaintContentRecorder* content_recorder;
};

bool paint_stream_initialize(PaintStream* stream, u32 initial_capacity = kInitialPaintQuads) noexcept;
void paint_stream_release(PaintStream* stream) noexcept;
bool paint_stream_bind_content_recorder(PaintStream* stream, UiPaintContentRecorder* recorder,
                                        Slice<UiPaintContentBounds> storage) noexcept;
bool paint_stream_reset(PaintStream* stream, float right = kMaximumPaintCoordinate,
                        float bottom = kMaximumPaintCoordinate) noexcept;
bool paint_stream_record_content(PaintStream* stream, UiWidgetReference owner, const UiRect& rectangle,
                                 UiLayoutContentKind kind,
                                 u16 flags = ui_layout_content_bounds_collision) noexcept;
bool paint_stream_push_clip(PaintStream* stream, float left, float top, float right, float bottom) noexcept;
bool paint_stream_pop_clip(PaintStream* stream) noexcept;
// A layer boundary: closes the current batch so the next quad starts a new one
// even under the same clip, and returns that batch index. Otherwise consecutive
// quads under one clip share a batch, and a later layer (a popout after the
// shell) would land in the earlier batch and draw beneath everything drawn
// between the two ranges.
u32 paint_stream_seal(PaintStream* stream) noexcept;
bool paint_stream_push_rounded_border(PaintStream* stream, float left, float top, float right, float bottom,
                                      const Color& fill, const Color& border, float radius,
                                      float border_width) noexcept;
bool paint_stream_push_rounded(PaintStream* stream, float left, float top, float right, float bottom,
                               const Color& color, float radius) noexcept;
bool paint_stream_push(PaintStream* stream, float left, float top, float right, float bottom,
                       const Color& color) noexcept;
// Two-color gradient encoded in a single 64-byte paint instance.
bool paint_stream_push_gradient(PaintStream* stream, UiRect rectangle, Color first, Color last,
                                float radius = 0) noexcept;
bool paint_stream_push_sv_square(PaintStream*, UiRect, Color hue_rgb, float radius = 0) noexcept;
bool paint_stream_push_hue_strip(PaintStream*, UiRect, float radius = 0) noexcept;
// Static directional preview of the theme's literal colors, evenly spaced. Uses
// at most four paint instances; no texture, background constant or animation.
bool paint_stream_push_palette_gradient(PaintStream*, UiRect, const BackgroundTheme&,
                                        float radius = 0) noexcept;
bool paint_stream_push_line(PaintStream* stream, UiPoint a, UiPoint b, float width,
                            const Color& color) noexcept;
// The leading `width` DIPs of the rounded rectangle `shape` (an accent border
// that follows its corners): one instance over the strip, whose shader mode
// clips it to the shape's outline (reserved_x carries the shape's width).
inline constexpr float kPaintEdgeStripMode = -10;
bool paint_stream_push_edge_strip(PaintStream* stream, UiRect shape, float width, const Color& color,
                                  float radius) noexcept;
// A single clipped, transparent focus outline. No geometry for ineligible focus.
bool paint_stream_push_focus(PaintStream* stream, const UiWidgetManager& owner,
                             const Color& color) noexcept;

} // namespace maicord
