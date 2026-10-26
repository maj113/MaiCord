#include "maicord/ui/paint.hpp"
#include "maicord/ui/background_presentation.hpp"

namespace maicord {
bool paint_stream_push_focus(PaintStream* stream, const UiWidgetManager& owner,
                             const Color& color) noexcept {
    if (!stream)
        return false;
    UiRect bounds{};
    if (!ui_widget_manager_context_anchor(owner, &bounds))
        return true;
    const float half_extent = minimum(ui_rect_width(bounds), ui_rect_height(bounds)) * 0.5f;
    if (!paint_stream_push_clip(stream, bounds))
        return false;
    const bool painted = paint_stream_push_rounded_border(stream, bounds, Color{}, color, minimum(4.0f, half_extent), minimum(2.0f, half_extent));
    const bool popped = paint_stream_pop_clip(stream);
    return painted && popped;
}

bool paint_stream_initialize(PaintStream* stream, u32 initial_capacity) noexcept {
    if (!stream || stream->quad_storage.arena.base || stream->quads || initial_capacity == 0 ||
        initial_capacity > kMaxPaintQuads ||
        !stable_storage_initialize(&stream->quad_storage, kMaxPaintQuads, initial_capacity,
                                   sizeof(PaintQuad), alignof(PaintQuad))) {
        return false;
    }
    stream->quads = stable_storage_data<PaintQuad>(stream->quad_storage);
    stream->quad_capacity = stream->quad_storage.capacity;
    return true;
}

void paint_stream_release(PaintStream* stream) noexcept {
    if (!stream) {
        return;
    }
    stable_storage_release(&stream->quad_storage);
    stream->quads = nullptr;
    stream->quad_capacity = 0;
    stream->count = 0;
    stream->clip_count = 0;
    stream->batch_count = 0;
    stream->sealed_batches = 0;
    stream->clip_depth = 0;
    stream->rejected = 0;
    stream->content_recorder = nullptr;
}

bool paint_stream_bind_content_recorder(PaintStream* stream, UiPaintContentRecorder* recorder,
                                        Slice<UiPaintContentBounds> storage) noexcept {
    if (!stream || !recorder || !storage.data || storage.count == 0) {
        return false;
    }
    *recorder = UiPaintContentRecorder{storage, 0, 0};
    stream->content_recorder = recorder;
    return true;
}

bool paint_stream_reset(PaintStream* stream, float right, float bottom) noexcept {
    if (!stream || (!stream->quads && !paint_stream_initialize(stream))) {
        return false;
    }
    stream->count = 0;
    stream->clip_count = 1;
    stream->batch_count = 0;
    stream->sealed_batches = 0;
    stream->clip_depth = 1;
    stream->rejected = 0;
    if (stream->content_recorder) {
        stream->content_recorder->count = 0;
        stream->content_recorder->dropped = 0;
    }
    const bool valid =
        paint_coordinate_valid(right) && paint_coordinate_valid(bottom) && right > 0.0f && bottom > 0.0f;
    stream->clips[0] = PaintClip{0.0f, 0.0f, valid ? right : 0.0f, valid ? bottom : 0.0f};
    stream->clip_stack[0] = 0;
    return true;
}

bool paint_stream_push_clip(PaintStream* stream, const UiRect& rectangle) noexcept {
    if (!stream || !paint_coordinate_valid(rectangle.left) || !paint_coordinate_valid(rectangle.top) ||
        !paint_coordinate_valid(rectangle.right) || !paint_coordinate_valid(rectangle.bottom) ||
        stream->clip_depth == 0 || stream->clip_depth >= kMaxPaintClipDepth ||
        stream->clip_count >= kMaxPaintClips) {
        if (stream) {
            ++stream->rejected;
        }
        return false;
    }
    const PaintClip& parent = stream->clips[stream->clip_stack[stream->clip_depth - 1]];
    PaintClip clip{maximum(rectangle.left, parent.left), maximum(rectangle.top, parent.top),
                   minimum(rectangle.right, parent.right), minimum(rectangle.bottom, parent.bottom)};
    if (clip.right < clip.left) {
        clip.right = clip.left;
    }
    if (clip.bottom < clip.top) {
        clip.bottom = clip.top;
    }
    const u16 index = static_cast<u16>(stream->clip_count++);
    stream->clips[index] = clip;
    stream->clip_stack[stream->clip_depth++] = index;
    return true;
}

bool paint_stream_push_clip(PaintStream* stream, float left, float top, float right, float bottom) noexcept {
    return paint_stream_push_clip(stream, UiRect{left, top, right, bottom});
}

bool paint_stream_pop_clip(PaintStream* stream) noexcept {
    if (!stream || stream->clip_depth <= 1) {
        if (stream) {
            ++stream->rejected;
        }
        return false;
    }
    --stream->clip_depth;
    return true;
}

u32 paint_stream_seal(PaintStream* stream) noexcept {
    if (!stream)
        return 0;
    stream->sealed_batches = stream->batch_count;
    return stream->batch_count;
}

bool paint_stream_push_rounded_border(PaintStream* stream, const UiRect& rectangle, const Color& fill,
                                      const Color& border, float radius, float border_width) noexcept {
    const float left = rectangle.left, top = rectangle.top, right = rectangle.right, bottom = rectangle.bottom;
    if (!stream || !stream->quads || !paint_coordinate_valid(left) || !paint_coordinate_valid(top) ||
        !paint_coordinate_valid(right) || !paint_coordinate_valid(bottom) ||
        !paint_coordinate_valid(radius) || !paint_coordinate_valid(border_width) || radius < 0.0f ||
        border_width < 0.0f || stream->clip_depth == 0) {
        if (stream) {
            ++stream->rejected;
        }
        return false;
    }
    if (left >= right || top >= bottom) {
        return true;
    }
    const u16 clip_index = stream->clip_stack[stream->clip_depth - 1];
    const PaintClip& clip = stream->clips[clip_index];
    if (clip.left >= clip.right || clip.top >= clip.bottom || right <= clip.left || bottom <= clip.top ||
        left >= clip.right || top >= clip.bottom) {
        return true;
    }
    if (stream->count >= stream->quad_capacity &&
        !stable_storage_grow(&stream->quad_storage, stream->count + 1u)) {
        ++stream->rejected;
        return false;
    }
    stream->quad_capacity = stream->quad_storage.capacity;

    PaintBatch* batch = stream->batch_count != 0 ? &stream->batches[stream->batch_count - 1] : nullptr;
    if (!batch || batch->clip_index != clip_index || stream->batch_count <= stream->sealed_batches ||
        batch->first_quad + batch->quad_count != stream->count) {
        if (stream->batch_count >= kMaxPaintBatches) {
            ++stream->rejected;
            return false;
        }
        batch = &stream->batches[stream->batch_count++];
        *batch = PaintBatch{stream->count, 0, clip_index, 0};
    }

    const float maximum_radius = minimum((right - left) * 0.5f, (bottom - top) * 0.5f);
    PaintQuad& quad = stream->quads[stream->count++];
    quad.left = left;
    quad.top = top;
    quad.right = right;
    quad.bottom = bottom;
    quad.fill = fill;
    quad.border = border;
    quad.radius = minimum(radius, maximum_radius);
    quad.border_width = minimum(border_width, maximum_radius);
    quad.reserved_x = quad.reserved_y = 0.0f;
    ++batch->quad_count;
    return true;
}

bool paint_stream_push_rounded_border(PaintStream* stream, float left, float top, float right, float bottom,
                                      const Color& fill, const Color& border, float radius,
                                      float border_width) noexcept {
    return paint_stream_push_rounded_border(stream, UiRect{left, top, right, bottom}, fill, border, radius,
                                            border_width);
}

bool paint_stream_push_rounded(PaintStream* stream, const UiRect& rectangle, const Color& color,
                               float radius) noexcept {
    return paint_stream_push_rounded_border(stream, rectangle, color, color, radius, 0.0f);
}

bool paint_stream_push_rounded(PaintStream* stream, float left, float top, float right, float bottom,
                               const Color& color, float radius) noexcept {
    return paint_stream_push_rounded_border(stream, UiRect{left, top, right, bottom}, color, color, radius, 0.0f);
}

bool paint_stream_push(PaintStream* stream, const UiRect& rectangle, const Color& color) noexcept {
    return paint_stream_push_rounded_border(stream, rectangle, color, color, 0.0f, 0.0f);
}

bool paint_stream_push(PaintStream* stream, float left, float top, float right, float bottom,
                       const Color& color) noexcept {
    return paint_stream_push_rounded_border(stream, UiRect{left, top, right, bottom}, color, color, 0.0f, 0.0f);
}

bool paint_stream_push_line(PaintStream* stream, UiPoint a, UiPoint b, float width,
                            const Color& color) noexcept {
    if (!stream || !paint_coordinate_valid(a.x) || !paint_coordinate_valid(a.y) ||
        !paint_coordinate_valid(b.x) || !paint_coordinate_valid(b.y) || !paint_coordinate_valid(width) ||
        width <= 0)
        return false;
    const float r = width * .5f, pad = r + 1;
    const u32 first = stream->count;
    if (!paint_stream_push(stream, minimum(a.x, b.x) - pad, minimum(a.y, b.y) - pad,
                           maximum(a.x, b.x) + pad, maximum(a.y, b.y) + pad, color))
        return false;
    if (stream->count != first) {
        auto& quad = stream->quads[first];
        quad.radius = r;
        quad.border_width = -1;
        quad.reserved_x = (b.x - a.x) * .5f;
        quad.reserved_y = (b.y - a.y) * .5f;
    }
    return true;
}

bool paint_stream_push_edge_strip(PaintStream* stream, const UiRect& shape, float width, const Color& color,
                                  float radius) noexcept {
    if (!stream || !paint_coordinate_valid(width) || !paint_coordinate_valid(radius) || !(width > 0) ||
        radius < 0) {
        if (stream)
            ++stream->rejected;
        return false;
    }
    const u32 before = stream->count;
    if (!paint_stream_push(stream, shape.left, shape.top, minimum(shape.left + width, shape.right), shape.bottom,
                           color))
        return false;
    if (stream->count != before) {
        auto& quad = stream->quads[before];
        quad.radius = minimum(radius, minimum(shape.right - shape.left, shape.bottom - shape.top) * .5f);
        quad.border_width = kPaintEdgeStripMode;
        quad.reserved_x = shape.right - shape.left;
    }
    return true;
}

bool paint_stream_push_sv_square(PaintStream* stream, const UiRect& rectangle, const Color& hue, float radius) noexcept {
    const u32 before = stream ? stream->count : 0;
    if (!paint_stream_push_rounded(stream, rectangle, hue, radius))
        return false;
    if (stream->count != before)
        stream->quads[before].border_width = -4;
    return true;
}

bool paint_stream_push_hue_strip(PaintStream* stream, const UiRect& rectangle, float radius) noexcept {
    const u32 before = stream ? stream->count : 0;
    if (!paint_stream_push_rounded(stream, rectangle, {1, 1, 1, 1}, radius))
        return false;
    if (stream->count != before)
        stream->quads[before].border_width = -5;
    return true;
}

bool paint_stream_push_palette_gradient(PaintStream* stream, const UiRect& rectangle, const BackgroundTheme& theme,
                                        float radius) noexcept {
    if (!stream || !background_theme_valid(theme))
        return false;
    const auto color = [](u32 rgb) noexcept -> Color {
        return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1};
    };
    Color from = color(theme.colors[0]);
    if (theme.color_count == 1)
        return paint_stream_push_rounded(stream, rectangle, from, radius);
    float direction_x{}, direction_y{};
    if (!ui_background_direction(theme.angle_degrees, &direction_x, &direction_y))
        return false;
    const u32 before = stream->count, batches = stream->batch_count;
    const u32 tail_count = batches ? stream->batches[batches - 1].quad_count : 0;
    for (u32 i = 0; i + 1 < theme.color_count; ++i) {
        const Color to = color(theme.colors[i + 1]);
        const u32 next = stream->count;
        if (!paint_stream_push_rounded(stream, rectangle, from, radius)) {
            stream->count = before;
            stream->batch_count = batches;
            if (batches)
                stream->batches[batches - 1].quad_count = tail_count;
            return false;
        }
        if (stream->count != next) {
            auto& quad = stream->quads[next];
            quad.border = to;
            quad.fill.alpha = static_cast<float>(i) / (theme.color_count - 1);
            quad.border.alpha = static_cast<float>(i + 1) / (theme.color_count - 1);
            quad.border_width = -6;
            quad.reserved_x = direction_x;
            quad.reserved_y = direction_y;
        }
        from = to;
    }
    return true;
}

bool paint_stream_push_gradient(PaintStream* stream, const UiRect& rectangle, const Color& first, const Color& last,
                                float radius) noexcept {
    const u32 before = stream ? stream->count : 0;
    if (!paint_stream_push_rounded(stream, rectangle, first, radius))
        return false;
    if (stream->count != before) {
        auto& quad = stream->quads[before];
        quad.border = last;
        quad.border_width = -2;
    }
    return true;
}

} // namespace maicord
