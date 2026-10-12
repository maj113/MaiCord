#include "maicord/ui/controls/settings_controls.hpp"

namespace maicord {

bool ui_settings_nav_item_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect item, bool selected,
                                bool destructive, UiControlState state) noexcept {
    const auto& c = tokens.color;
    Color fill{};
    if (destructive)
        fill = selected || state.hovered || state.pressed ? c.critical_tint : Color{};
    else
        fill = selected ? c.nav_selected : state.pressed ? c.mod_strong : state.hovered ? c.mod_subtle : Color{};
    const float radius = tokens.metric.nav_item_radius;
    bool ok = fill.alpha <= 0 ||
              paint_stream_push_rounded(paint, item.left, item.top, item.right, item.bottom, fill, radius);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, item.left, item.top, item.right, item.bottom, {},
                                              c.focus_ring, radius, 2);
    return ok;
}

Color ui_settings_nav_item_text(const UiSettingsTokens& tokens, bool selected, bool destructive,
                                UiControlState state) noexcept {
    const auto& c = tokens.color;
    if (!state.enabled)
        return ui_settings_color_alpha(c.text_subtle, .5f);
    if (destructive)
        return c.critical_text;
    return selected || state.hovered ? c.text_strong : c.text_subtle;
}

UiRect ui_settings_nav_icon_rect(const UiSettingsTokens& tokens, UiRect item) noexcept {
    const float size = tokens.metric.nav_icon, left = item.left + tokens.metric.nav_item_padding;
    const float top = (item.top + item.bottom - size) * .5f;
    return {left, top, left + size, top + size};
}

UiRect ui_settings_nav_label_rect(const UiSettingsTokens& tokens, UiRect item, bool icon) noexcept {
    const float left =
        item.left + tokens.metric.nav_item_padding + (icon ? tokens.metric.nav_icon + tokens.metric.nav_item_padding : 0);
    return {left, item.top, item.right - tokens.metric.nav_item_padding, item.bottom};
}

bool ui_settings_separator_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect row) noexcept {
    const float middle = (row.top + row.bottom) * .5f;
    const float inset = tokens.metric.nav_item_padding;
    return paint_stream_push(paint, row.left + inset, middle - .5f, row.right - inset, middle + .5f,
                             tokens.color.border_subtle);
}

bool ui_settings_search_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect field, bool has_text,
                              UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float radius = tokens.metric.control_radius;
    const Color fill = ui_settings_color_alpha(c.input_background, state.enabled ? 1.f : .5f);
    bool ok = paint_stream_push_rounded_border(paint, field.left, field.top, field.right, field.bottom, fill,
                                               state.focused ? c.brand : c.input_border, radius,
                                               state.focused ? 2.f : 1.f);
    const float icon = 16, cy = (field.top + field.bottom) * .5f;
    if (ok && !has_text) {
        const float x = field.right - 8 - icon;
        ok = ui_icon_paint(paint, UiIcon::search, {x, cy - icon * .5f, x + icon, cy + icon * .5f}, c.text_subtle);
    } else if (ok) {
        const auto clear = ui_settings_search_clear_rect(tokens, field);
        if (state.hovered)
            ok = paint_stream_push_rounded(paint, clear.left, clear.top, clear.right, clear.bottom, c.mod_normal,
                                           ui_rect_width(clear) * .5f);
        const float cx = (clear.left + clear.right) * .5f;
        ok = ok && ui_icon_paint(paint, UiIcon::close, {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f,
                                                        cy + icon * .5f},
                                 state.hovered ? c.text_strong : c.text_subtle);
    }
    return ok;
}

UiRect ui_settings_search_text_rect(const UiSettingsTokens&, UiRect field) noexcept {
    return {field.left + 10, field.top, field.right - 32, field.bottom};
}

UiRect ui_settings_search_clear_rect(const UiSettingsTokens&, UiRect field) noexcept {
    const float size = 24, cy = (field.top + field.bottom) * .5f;
    return {field.right - 4 - size, cy - size * .5f, field.right - 4, cy + size * .5f};
}

UiRect ui_settings_close_rect(const UiSettingsTokens& tokens, float center_x, float top) noexcept {
    const float half = tokens.metric.close_size * .5f;
    return {center_x - half, top, center_x + half, top + tokens.metric.close_size};
}

UiRect ui_settings_close_caption_rect(const UiSettingsTokens& tokens, UiRect circle) noexcept {
    const float top = circle.bottom + tokens.metric.close_caption_gap;
    return {circle.left - 12, top, circle.right + 12, top + kUiSettingsCaptionLine};
}

Color ui_settings_close_color(const UiSettingsTokens& tokens, UiControlState state) noexcept {
    return state.hovered || state.pressed || state.focused ? tokens.color.text_strong : tokens.color.text_subtle;
}

bool ui_settings_close_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect circle,
                             UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float offset = state.pressed ? 1.f : 0.f;
    const Color ink = ui_settings_close_color(tokens, state);
    const Color fill = state.pressed ? c.mod_normal : state.hovered ? c.mod_subtle : Color{};
    const float radius = ui_rect_width(circle) * .5f;
    bool ok = paint_stream_push_rounded_border(paint, circle.left, circle.top + offset, circle.right,
                                               circle.bottom + offset, fill, ink, radius, 2);
    const float icon = 18, cx = (circle.left + circle.right) * .5f, cy = (circle.top + circle.bottom) * .5f + offset;
    ok = ok && ui_icon_paint(paint, UiIcon::close, {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                             ink);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, circle.left - 3, circle.top - 3, circle.right + 3,
                                              circle.bottom + 3, {}, c.focus_ring, radius + 3, 2);
    return ok;
}

bool ui_settings_dialog_close_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect target,
                                    UiControlState state) noexcept {
    const auto& c = tokens.color;
    const Color ink = state.hovered || state.pressed || state.focused ? c.text_strong : c.text_subtle;
    const Color fill = state.pressed ? c.mod_normal : state.hovered ? c.mod_subtle : Color{};
    const float icon = 24, cx = (target.left + target.right) * .5f, cy = (target.top + target.bottom) * .5f;
    const float half = icon * .5f + 4;
    const UiRect plate{cx - half, cy - half, cx + half, cy + half};
    bool ok = !fill.alpha || paint_stream_push_rounded(paint, plate.left, plate.top, plate.right, plate.bottom, fill,
                                                       tokens.metric.nav_item_radius);
    ok = ok && ui_icon_paint(paint, UiIcon::close, {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                             ink);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, plate.left - 2, plate.top - 2, plate.right + 2, plate.bottom + 2, {},
                                              c.focus_ring, tokens.metric.nav_item_radius + 2, 2);
    return ok;
}

} // namespace maicord
