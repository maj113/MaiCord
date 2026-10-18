#pragma once
#include "application_state.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord::app_detail {
// True when region holds enough content-text pixels matching their own glyph
// quad colors, so a blank or stale frame cannot pass.
inline bool probe_glyph_ink(const D3D11_MAPPED_SUBRESOURCE& mapped, UiRect region) noexcept {
    const auto& r = g_app.renderer;
    if (!r.content_text || !ui_rect_has_area(region))
        return false;
    const float scale = r.dpi / 96.f;
    u32 pixels{};
    for (u32 i = 0; i < r.content_text->quad_count; ++i) {
        const auto& quad = r.content_text->quads[i];
        const auto clip = ui_rect_intersection(region, {quad.left, quad.top, quad.right, quad.bottom});
        if (!ui_rect_has_area(clip))
            continue;
        const i32 left = maximum(0, static_cast<i32>(clip.left * scale));
        const i32 top = maximum(0, static_cast<i32>(clip.top * scale));
        const i32 right = minimum(static_cast<i32>(r.width), static_cast<i32>(clip.right * scale + 1));
        const i32 bottom = minimum(static_cast<i32>(r.height), static_cast<i32>(clip.bottom * scale + 1));
        const i32 expected[]{static_cast<i32>(quad.color.blue * 255.f + .5f),
                             static_cast<i32>(quad.color.green * 255.f + .5f),
                             static_cast<i32>(quad.color.red * 255.f + .5f)};
        for (i32 y = top; y < bottom; ++y)
            for (i32 x = left; x < right; ++x) {
                const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                i32 difference{};
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 delta = pixel[channel] - expected[channel];
                    difference += delta < 0 ? -delta : delta;
                }
                if (difference <= 48 && ++pixels >= 24)
                    return true;
            }
    }
    return false;
}

// Inspect only pixels inside actual retained glyph rectangles. A blank frame
// or a frame from the previous controller phase cannot satisfy these checks.
inline bool probe_ink(const D3D11_MAPPED_SUBRESOURCE& mapped, UiRect region, Color wanted,
                      bool dim = false) noexcept {
    const auto& r = g_app.renderer;
    const auto* quads = r.content_text ? r.content_text->quads : r.glyph_text.title_quads;
    const u32 count = r.content_text ? r.content_text->quad_count : r.glyph_text.title_quad_count;
    const float scale = r.dpi / 96.f;
    const float opacity = dim ? .45f : 1.f;
    const i32 colors[]{static_cast<i32>(wanted.blue * opacity * 255 + .5f),
                       static_cast<i32>(wanted.green * opacity * 255 + .5f),
                       static_cast<i32>(wanted.red * opacity * 255 + .5f)};
    u32 pixels{};
    for (u32 i = 0; i < count; ++i) {
        const auto& q = quads[i];
        if (q.color.red != wanted.red || q.color.green != wanted.green || q.color.blue != wanted.blue)
            continue;
        const auto bounds = ui_rect_intersection(region, {q.left, q.top, q.right, q.bottom});
        if (!ui_rect_has_area(bounds))
            continue;
        for (i32 y = maximum(0, static_cast<i32>(bounds.top * scale));
             y < minimum(static_cast<i32>(r.height), static_cast<i32>(bounds.bottom * scale + 1)); ++y)
            for (i32 x = maximum(0, static_cast<i32>(bounds.left * scale));
                 x < minimum(static_cast<i32>(r.width), static_cast<i32>(bounds.right * scale + 1)); ++x) {
                const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                i32 distance{};
                for (u32 c = 0; c < 3; ++c) {
                    const i32 d = pixel[c] - colors[c];
                    distance += d < 0 ? -d : d;
                }
                if (distance <= 36 && ++pixels >= 16)
                    return true;
            }
    }
    return false;
}
} // namespace maicord::app_detail
