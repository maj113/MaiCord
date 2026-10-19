#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace {
float pitch(const UiSettingsTokens& tokens) noexcept {
    return tokens.metric.color_cell + tokens.metric.color_gap;
}
} // namespace

float ui_segments_width(const UiSettingsTokens& tokens, u32 count) noexcept {
    return count ? count * tokens.metric.segment_size + (count - 1) * tokens.metric.segment_gap : 0;
}

UiRect ui_segments_option(const UiSettingsTokens& tokens, UiRect group, u32 count, u32 index) noexcept {
    if (index >= count)
        return {};
    const float size = tokens.metric.segment_size;
    const float left = group.left + index * (size + tokens.metric.segment_gap);
    const float top = (group.top + group.bottom - size) * .5f;
    return {left, top, left + size, top + size};
}

u32 ui_segments_hit(const UiSettingsTokens& tokens, UiRect group, u32 count, float x, float y) noexcept {
    // Gaps resolve to the nearer option so the group has no dead pixels.
    const float half = tokens.metric.segment_gap * .5f;
    for (u32 i = 0; i < count; ++i) {
        const auto option = ui_segments_option(tokens, group, count, i);
        if (ui_rect_contains({option.left - half, option.top - half, option.right + half, option.bottom + half}, x,
                             y))
            return i;
    }
    return ~0u;
}

bool ui_segments_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect group, u32 count, u32 selected,
                       u32 hovered, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha, radius = tokens.metric.control_radius * .75f;
    bool ok = true;
    for (u32 i = 0; ok && i < count; ++i) {
        const auto r = ui_segments_option(tokens, group, count, i);
        const bool on = i == selected, hover = state.enabled && i == hovered;
        const bool pressed = hover && state.pressed;
        const Color fill = on ? c.brand : pressed ? c.mod_strong : hover ? c.mod_normal : c.input_background;
        const Color border = on ? ui_settings_color_alpha(c.border_subtle, .5f) : c.input_border;
        ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom,
                                              ui_settings_color_alpha(fill, alpha),
                                              ui_settings_color_alpha(border, alpha), radius, 1);
    }
    if (ok && state.focused && count) {
        const u32 ring = selected < count ? selected : 0;
        ok = ui_settings_focus_paint(paint, tokens, ui_segments_option(tokens, group, count, ring), radius);
    }
    return ok;
}

float ui_tristate_width(const UiSettingsTokens& tokens) noexcept {
    return tokens.metric.tristate_segment * kUiTristateCount;
}

float ui_tristate_radius(const UiSettingsTokens& tokens) noexcept {
    return tokens.metric.control_radius * .5f;
}

UiRect ui_tristate_option(const UiSettingsTokens& tokens, UiRect group, u32 index) noexcept {
    if (index >= kUiTristateCount)
        return {};
    const float width = tokens.metric.tristate_segment, left = group.left + index * width;
    return {left, group.top, left + width, group.bottom};
}

u32 ui_tristate_hit(const UiSettingsTokens& tokens, UiRect group, float x, float y) noexcept {
    if (!ui_rect_contains(group, x, y) || !(tokens.metric.tristate_segment > 0))
        return ~0u;
    const u32 index = static_cast<u32>((x - group.left) / tokens.metric.tristate_segment);
    return index < kUiTristateCount ? index : kUiTristateCount - 1;
}

bool ui_tristate_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect group, u32 selected,
                       u32 hovered, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha, radius = ui_tristate_radius(tokens);
    // One bordered group; segment fills sit inside the border on the
    // concentric inner shape, so the end segments keep its corners.
    bool ok = paint_stream_push_rounded_border(paint, group.left, group.top, group.right, group.bottom,
                                               ui_settings_color_alpha(c.input_background, alpha),
                                               ui_settings_color_alpha(c.border_subtle, alpha), radius, 1);
    const UiRect inner{group.left + 1, group.top + 1, group.right - 1, group.bottom - 1};
    const float inner_radius = maximum(0.f, radius - 1);
    const Color positive = ui_settings_color_mix(c.positive, {0, 0, 0, 1}, tokens.light ? 0 : .35f);
    for (u32 i = 0; ok && i < kUiTristateCount; ++i) {
        const auto option = ui_tristate_option(tokens, group, i);
        const bool on = i == selected, hover = state.enabled && i == hovered;
        Color fill{};
        if (on)
            fill = i == 0 ? c.critical : i == 2 ? positive : c.mod_strong;
        else if (hover)
            fill = state.pressed ? c.mod_strong : c.mod_normal;
        if (fill.alpha > 0) {
            // End segments clip the inner shape to their span, keeping its
            // outer corners; the middle one is a plain rectangle.
            fill = ui_settings_color_alpha(fill, alpha);
            const float left = i == 0 ? inner.left : option.left, right = i == 2 ? inner.right : option.right;
            ok = i == 1 ? paint_stream_push(paint, left, inner.top, right, inner.bottom, fill)
                        : paint_stream_push_clip(paint, left, inner.top, right, inner.bottom) &&
                              paint_stream_push_rounded(paint, inner.left, inner.top, inner.right, inner.bottom, fill,
                                                        inner_radius) &&
                              paint_stream_pop_clip(paint);
        }
        const Color glyph = on ? (i == 1 ? c.text_strong : c.on_brand)
                            : i == 0 ? c.critical_text
                            : i == 2 ? c.positive
                                     : c.text_subtle;
        const float size = tokens.metric.segment_size * .5f, cx = (option.left + option.right) * .5f,
                    cy = (option.top + option.bottom) * .5f;
        const UiRect icon{cx - size * .5f, cy - size * .5f, cx + size * .5f, cy + size * .5f};
        if (!ok)
            break;
        if (i == 1)
            ok = paint_stream_push_line(paint, {icon.right - 2, icon.top + 2}, {icon.left + 2, icon.bottom - 2}, 2,
                                        ui_settings_color_alpha(glyph, alpha));
        else
            ok = ui_icon_paint(paint, i == 0 ? UiIcon::close : UiIcon::check, icon,
                               ui_settings_color_alpha(glyph, alpha));
    }
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, tokens, group, radius);
    return ok;
}

u32 ui_color_grid_columns(const UiSettingsTokens& tokens, float width) noexcept {
    return maximum(1u, static_cast<u32>((width + tokens.metric.color_gap) / pitch(tokens)));
}

namespace {
// Room around the outer cells for the selection ring.
float ring_room(const UiSettingsTokens& tokens) noexcept {
    return tokens.metric.ring_gap + tokens.metric.ring_width;
}
} // namespace

float ui_color_grid_height(const UiSettingsTokens& tokens, u32 count, float width) noexcept {
    const u32 columns = ui_color_grid_columns(tokens, width);
    const u32 rows = (count + columns - 1) / columns;
    return rows ? rows * pitch(tokens) - tokens.metric.color_gap + ring_room(tokens) * 2 : 0;
}

UiRect ui_color_grid_cell(const UiSettingsTokens& tokens, UiRect field, u32 index) noexcept {
    const float room = ring_room(tokens);
    const u32 columns = ui_color_grid_columns(tokens, ui_rect_width(field) - room * 2);
    const float left = field.left + room + (index % columns) * pitch(tokens);
    const float top = field.top + room + (index / columns) * pitch(tokens);
    return {left, top, left + tokens.metric.color_cell, top + tokens.metric.color_cell};
}

u32 ui_color_grid_hit(const UiSettingsTokens& tokens, UiRect field, u32 count, float x, float y) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const auto cell = ui_color_grid_cell(tokens, field, i);
        if (ui_rect_contains({cell.left - 2, cell.top - 2, cell.right + 2, cell.bottom + 2}, x, y))
            return i;
    }
    return ~0u;
}

bool ui_color_grid_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect cell, Color fill, bool selected,
                         UiControlState state) noexcept {
    const auto& c = tokens.color;
    const auto& m = tokens.metric;
    const float alpha = state.enabled ? 1.f : tokens.disabled_alpha, radius = m.control_radius * .5f;
    bool ok = paint_stream_push_rounded_border(paint, cell.left, cell.top, cell.right, cell.bottom,
                                               ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(c.border_subtle, alpha), radius, 1);
    // Selection and hover share one concentric ring; focus wraps it directly,
    // since packed cells leave no room for another gap.
    if (ok && (selected || state.hovered))
        ok = ui_settings_ring_paint(paint, cell, radius, m.ring_gap, selected ? m.ring_width : 1,
                                    ui_settings_color_alpha(selected ? c.text_strong : c.border_normal, alpha));
    if (ok && state.focused)
        ok = ui_settings_ring_paint(paint, cell, radius, ring_room(tokens), m.focus_width, c.focus_ring);
    if (ok && selected) {
        // Dark check on light fills and light check on dark fills.
        const float luma = fill.red * .299f + fill.green * .587f + fill.blue * .114f;
        const float inset = ui_rect_width(cell) * .2f;
        ok = ui_icon_paint(paint, UiIcon::check,
                           {cell.left + inset, cell.top + inset, cell.right - inset, cell.bottom - inset},
                           luma > .6f ? Color{0, 0, 0, .85f} : Color{1, 1, 1, 1});
    }
    return ok;
}

} // namespace maicord
