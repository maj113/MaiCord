#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord {
namespace {
constexpr float kSearchIcon = 16, kSearchInset = 12, kSearchEdge = 8, kClearSize = 24, kHintWidth = 44,
                kHintHeight = 20, kCloseGlyph = 20;
// The close buttons' glyph ink and hover plate.
Color close_color(const UiSettingsTokens& tokens, UiControlState state) noexcept {
    return state.hovered || state.pressed || state.focused ? tokens.color.text_strong : tokens.color.text_subtle;
}
Color close_fill(const UiSettingsTokens& tokens, UiControlState state) noexcept {
    return state.pressed ? tokens.color.mod_normal : state.hovered ? tokens.color.mod_subtle : Color{};
}
} // namespace

bool ui_settings_nav_item_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& item, bool selected,
                                bool destructive, UiControlState state) noexcept {
    const auto& c = tokens.color;
    Color fill{};
    if (destructive)
        fill = selected || state.hovered || state.pressed ? c.critical_tint : Color{};
    else
        fill = selected ? c.nav_selected : state.pressed ? c.mod_strong : state.hovered ? c.mod_subtle : Color{};
    const float radius = tokens.metric.nav_item_radius;
    bool ok = fill.alpha <= 0 ||
              paint_stream_push_rounded(paint, item, fill, radius);
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, item, {}, c.focus_ring, radius, 2);
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

UiRect ui_settings_nav_icon_rect(const UiSettingsLayoutTokens& tokens, const UiRect& item) noexcept {
    const float size = tokens.metric.nav_icon, left = item.left + tokens.metric.nav_item_padding;
    const float top = (item.top + item.bottom - size) * .5f;
    return {left, top, left + size, top + size};
}

UiRect ui_settings_nav_label_rect(const UiSettingsLayoutTokens& tokens, const UiRect& item, bool icon) noexcept {
    const float left =
        item.left + tokens.metric.nav_item_padding + (icon ? tokens.metric.nav_icon + tokens.metric.nav_item_padding : 0);
    return {left, item.top, item.right - tokens.metric.nav_item_padding, item.bottom};
}

UiRect ui_settings_nav_badge_rect(const UiRect& item, bool dot, float digits_width) noexcept {
    const float cy = (item.top + item.bottom) * .5f, right = item.right - 10;
    const float width = dot ? 8.f : maximum(18.f, digits_width + 10), height = dot ? 8.f : 18.f;
    return {right - width, cy - height * .5f, right, cy + height * .5f};
}

bool ui_settings_nav_badge_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& badge, bool) noexcept {
    return paint_stream_push_rounded(paint, badge, tokens.color.brand, ui_rect_height(badge) * .5f);
}

bool ui_settings_separator_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& row) noexcept {
    const float middle = (row.top + row.bottom) * .5f;
    const float inset = tokens.metric.nav_item_padding;
    return paint_stream_push(paint, row.left + inset, middle - .5f, row.right - inset, middle + .5f,
                             tokens.color.separator);
}

bool ui_settings_search_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& field, bool has_text,
                              UiControlState state) noexcept {
    const auto& c = tokens.color;
    const float radius = tokens.metric.control_radius;
    const Color fill = ui_settings_color_alpha(c.input_background, state.enabled ? 1.f : .5f);
    const float cy = (field.top + field.bottom) * .5f;
    // The accent outline shows while the field holds the caret; at rest it is the plain input border.
    bool ok = paint_stream_push_rounded_border(paint, field, fill, state.focused ? c.brand : c.input_border, radius,
                                               state.focused ? 2.f : 1.f);
    ok = ok && ui_icon_paint(paint, UiIcon::search,
                             {field.left + kSearchInset, cy - kSearchIcon * .5f, field.left + kSearchInset + kSearchIcon,
                              cy + kSearchIcon * .5f},
                             c.text_subtle);
    if (ok && !has_text) {
        ok = paint_stream_push_rounded_border(paint, ui_settings_search_hint_rect(field), c.mod_subtle, c.border_subtle,
                                              radius * .5f, 1);
    } else if (ok) {
        const auto clear = ui_settings_search_clear_rect(field);
        if (state.hovered)
            ok = paint_stream_push_rounded(paint, clear, c.mod_normal, ui_rect_width(clear) * .5f);
        const float cx = (clear.left + clear.right) * .5f, icon = 14;
        ok = ok && ui_icon_paint(paint, UiIcon::close,
                                 {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                                 state.hovered ? c.text_strong : c.text_subtle);
    }
    return ok;
}

UiRect ui_settings_search_text_rect(const UiRect& field) noexcept {
    return {field.left + kSearchInset + kSearchIcon + kSearchEdge, field.top, field.right - kClearSize - kSearchEdge - 4,
            field.bottom};
}

UiRect ui_settings_search_clear_rect(const UiRect& field) noexcept {
    const float cy = (field.top + field.bottom) * .5f;
    return {field.right - kSearchEdge - kClearSize, cy - kClearSize * .5f, field.right - kSearchEdge,
            cy + kClearSize * .5f};
}

UiRect ui_settings_search_hint_rect(const UiRect& field) noexcept {
    const float cy = (field.top + field.bottom) * .5f;
    return {field.right - kSearchEdge - kHintWidth, cy - kHintHeight * .5f, field.right - kSearchEdge,
            cy + kHintHeight * .5f};
}

UiRect ui_settings_close_rect(const UiSettingsLayoutTokens& tokens, float center_x, float top) noexcept {
    const float half = tokens.metric.close_size * .5f;
    return {center_x - half, top, center_x + half, top + tokens.metric.close_size};
}

bool ui_settings_close_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& square,
                             UiControlState state) noexcept {
    const Color fill = close_fill(tokens, state);
    const float radius = tokens.metric.control_radius;
    const float cx = (square.left + square.right) * .5f, cy = (square.top + square.bottom) * .5f;
    bool ok = !fill.alpha || paint_stream_push_rounded(paint, square, fill, radius);
    ok = ok && ui_icon_paint(paint, UiIcon::close,
                             {cx - kCloseGlyph * .5f, cy - kCloseGlyph * .5f, cx + kCloseGlyph * .5f,
                              cy + kCloseGlyph * .5f},
                             close_color(tokens, state));
    return ok && (!state.focused || ui_settings_focus_paint(paint, tokens, square, radius));
}

bool ui_settings_dialog_close_paint(PaintStream* paint, const UiSettingsTokens& tokens, const UiRect& target,
                                    UiControlState state) noexcept {
    const auto& c = tokens.color;
    const Color fill = close_fill(tokens, state);
    const float icon = 24, cx = (target.left + target.right) * .5f, cy = (target.top + target.bottom) * .5f;
    const float half = icon * .5f + 4;
    const UiRect plate{cx - half, cy - half, cx + half, cy + half};
    bool ok = !fill.alpha || paint_stream_push_rounded(paint, plate, fill, tokens.metric.nav_item_radius);
    ok = ok && ui_icon_paint(paint, UiIcon::close, {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                             close_color(tokens, state));
    if (ok && state.focused)
        ok = paint_stream_push_rounded_border(paint, plate.left - 2, plate.top - 2, plate.right + 2, plate.bottom + 2, {},
                                              c.focus_ring, tokens.metric.nav_item_radius + 2, 2);
    return ok;
}

} // namespace maicord
