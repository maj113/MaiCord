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
                       u32 hovered, bool tristate, UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float alpha = state.enabled ? 1.f : .45f, radius = tokens.metric.control_radius * .75f;
    bool ok = true;
    for (u32 i = 0; ok && i < count; ++i) {
        const auto r = ui_segments_option(tokens, group, count, i);
        const bool on = i == selected, hover = state.enabled && i == hovered;
        Color tone = c.brand;
        if (tristate)
            tone = i == 0 ? c.critical : i == 2 ? ui_settings_color_mix(c.positive, {0, 0, 0, 1}, tokens.light ? 0 : .35f)
                                                : c.mod_strong;
        const Color fill = on ? tone : hover ? c.mod_normal : c.input_background;
        const Color border = on ? ui_settings_color_alpha(c.border_subtle, .5f) : c.input_border;
        ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom,
                                              ui_settings_color_alpha(fill, alpha),
                                              ui_settings_color_alpha(border, alpha), radius, 1);
        if (!ok || !tristate)
            continue;
        const Color glyph = on ? (i == 1 ? c.text_strong : c.on_brand)
                            : i == 0  ? c.critical_text
                            : i == 2  ? c.positive
                                      : c.text_subtle;
        const float inset = ui_rect_width(r) * .25f;
        const UiRect icon{r.left + inset, r.top + inset, r.right - inset, r.bottom - inset};
        if (i == 1)
            ok = paint_stream_push_line(paint, {icon.right - 1, icon.top + 1}, {icon.left + 1, icon.bottom - 1}, 2,
                                        ui_settings_color_alpha(glyph, alpha));
        else
            ok = ui_icon_paint(paint, i == 0 ? UiIcon::close : UiIcon::check, icon,
                               ui_settings_color_alpha(glyph, alpha));
    }
    if (ok && state.focused && count) {
        const u32 ring = selected < count ? selected : 0;
        const auto r = ui_segments_option(tokens, group, count, ring);
        ok = paint_stream_push_rounded_border(paint, r.left - 3, r.top - 3, r.right + 3, r.bottom + 3, {}, c.focus_ring,
                                              radius + 3, 2);
    }
    return ok;
}

u32 ui_color_grid_columns(const UiSettingsTokens& tokens, float width) noexcept {
    return maximum(1u, static_cast<u32>((width + tokens.metric.color_gap) / pitch(tokens)));
}

float ui_color_grid_height(const UiSettingsTokens& tokens, u32 count, float width) noexcept {
    const u32 columns = ui_color_grid_columns(tokens, width);
    const u32 rows = (count + columns - 1) / columns;
    // Room for the selection ring above and below the outer rows.
    return rows ? rows * pitch(tokens) - tokens.metric.color_gap + 8 : 0;
}

UiRect ui_color_grid_cell(const UiSettingsTokens& tokens, UiRect field, u32 index) noexcept {
    const u32 columns = ui_color_grid_columns(tokens, ui_rect_width(field) - 8);
    const float left = field.left + 4 + (index % columns) * pitch(tokens);
    const float top = field.top + 4 + (index / columns) * pitch(tokens);
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
    const float alpha = state.enabled ? 1.f : .45f, radius = 4;
    bool ok = paint_stream_push_rounded_border(paint, cell.left, cell.top, cell.right, cell.bottom,
                                               ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(c.border_subtle, alpha), radius, 1);
    if (ok && (selected || state.hovered))
        ok = paint_stream_push_rounded_border(paint, cell.left - 3, cell.top - 3, cell.right + 3, cell.bottom + 3, {},
                                              selected ? c.text_strong : c.border_normal, radius + 3,
                                              selected ? 2.f : 1.f);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, cell.left - 6, cell.top - 6, cell.right + 6, cell.bottom + 6, {},
                                              c.focus_ring, radius + 6, 2);
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
