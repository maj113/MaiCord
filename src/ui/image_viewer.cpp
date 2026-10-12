#include "maicord/ui/image_viewer.hpp"
namespace maicord {
namespace {
constexpr float kToolbarTarget = 32, kToolbarPitch = 40, kToolbarInset = 16, kToolbarTop = 12;
// Gap between the view (zoom) group and the item actions.
constexpr float kToolbarGroupGap = 12;
bool zoom_group(u32 index) noexcept {
    return index == ui_image_viewer_fit || index == ui_image_viewer_zoom_in || index == ui_image_viewer_zoom_out;
}
constexpr float kChevronTarget = 44, kChevronInset = 16;
UiRect lerp_rect(UiRect from, UiRect to, float t) noexcept {
    return {from.left + (to.left - from.left) * t, from.top + (to.top - from.top) * t,
            from.right + (to.right - from.right) * t, from.bottom + (to.bottom - from.bottom) * t};
}
void arrange(UiImageViewer* v) noexcept {
    // Images never upscale past their pixels; video fills the viewer.
    const float scale = minimum(ui_rect_width(v->body) / v->pixels.width, ui_rect_height(v->body) / v->pixels.height);
    const float fit = v->video ? scale : minimum(1.0f, scale);
    const float width = v->pixels.width * fit * v->zoom, height = v->pixels.height * fit * v->zoom;
    const float dx = maximum(0.0f, (width - ui_rect_width(v->body)) * .5f);
    const float dy = maximum(0.0f, (height - ui_rect_height(v->body)) * .5f);
    v->pan.x = maximum(-dx, minimum(dx, v->pan.x));
    v->pan.y = maximum(-dy, minimum(dy, v->pan.y));
    const float x = (v->body.left + v->body.right - width) * .5f + v->pan.x;
    const float y = (v->body.top + v->body.bottom - height) * .5f + v->pan.y;
    v->image = {x, y, x + width, y + height};
    v->display = v->reveal < 1 && ui_rect_has_area(v->origin) ? lerp_rect(v->origin, v->image, v->reveal)
                                                                : v->image;
}
bool shown(const UiImageViewer& v, u32 index) noexcept {
    switch (index) {
    case ui_image_viewer_zoom_out:
    case ui_image_viewer_zoom_in:
    case ui_image_viewer_fit:
        return !v.video;
    case ui_image_viewer_save:
        return v.can_save;
    case ui_image_viewer_open_original:
        return (v.actions & ui_image_viewer_action_open) != 0;
    case ui_image_viewer_copy:
        return (v.actions & ui_image_viewer_action_copy) != 0 && !v.video;
    case ui_image_viewer_previous:
    case ui_image_viewer_next:
        return v.item_count > 1;
    default:
        return true;
    }
}
// Right-to-left toolbar: Close, Save, Copy, Open | Fit, Zoom in, Zoom out.
// Narrow viewports drop the zoom group first; keyboard zoom keeps working.
bool place_controls(UiImageViewer* v) noexcept {
    constexpr u32 order[]{ui_image_viewer_close, ui_image_viewer_save, ui_image_viewer_copy,
                          ui_image_viewer_open_original, ui_image_viewer_fit, ui_image_viewer_zoom_in,
                          ui_image_viewer_zoom_out};
    const float width = ui_rect_width(v->viewport);
    u32 wanted = 0;
    for (const u32 index : order)
        wanted += shown(*v, index);
    const u32 fits = width > 2 * kToolbarInset + kToolbarGroupGap
                         ? static_cast<u32>((width - 2 * kToolbarInset - kToolbarGroupGap + kToolbarPitch -
                                             kToolbarTarget) /
                                            kToolbarPitch)
                         : 0;
    u32 slot = 0;
    for (u32 index = 0; index < ui_image_viewer_button_count; ++index) {
        auto* widget = ui_widget_manager_resolve(&v->widgets, v->buttons[index]);
        auto* node = widget ? ui_layout_engine_resolve(&v->widgets.layout, widget->layout) : nullptr;
        if (!node)
            return false;
        bool visible = shown(*v, index);
        auto& style = node->spec.style;
        if (index == ui_image_viewer_previous || index == ui_image_viewer_next) {
            style.margin = {};
            style.margin.left = style.margin.right = kChevronInset;
        } else if (visible) {
            u32 position = 0;
            for (const u32 candidate : order) {
                if (candidate == index)
                    break;
                position += shown(*v, candidate);
            }
            visible = position < fits || (wanted <= fits);
            style.margin = {};
            style.margin.top = kToolbarTop;
            style.margin.right =
                kToolbarInset + position * kToolbarPitch + (zoom_group(index) ? kToolbarGroupGap : 0);
            slot += visible;
        }
        widget->flags = visible ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
        if (visible)
            style.flags |= ui_layout_node_visible;
        else
            style.flags &= ~ui_layout_node_visible;
    }
    (void)slot;
    ui_widget_manager_reconcile_input(&v->widgets);
    return ui_widget_manager_layout_rectangle(&v->widgets, v->viewport).ok();
}
void body_for(UiImageViewer* v) noexcept {
    const auto r = v->viewport;
    // Gallery chevrons own the side gutters, so media never sits beneath them.
    const float side = v->item_count > 1 && ui_rect_width(r) >= 480 ? kChevronInset * 2 + kChevronTarget : 24;
    v->body = {r.left + side, r.top + 64, r.right - side, r.bottom - 40};
}
void append(UiImageViewer* v, Utf8View text) noexcept {
    const u32 count = minimum<u32>(text.bytes, sizeof(v->footer) - v->footer_bytes);
    copy_bytes(v->footer + v->footer_bytes, text.data, count);
    v->footer_bytes += count;
}
void append_number(UiImageViewer* v, u32 value) noexcept {
    char digits[10];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && count < 10);
    while (count && v->footer_bytes < sizeof(v->footer))
        v->footer[v->footer_bytes++] = digits[--count];
}
void compose_footer(UiImageViewer* v) noexcept {
    v->footer_bytes = 0;
    if (v->item_count > 1) {
        append_number(v, v->item + 1u);
        append(v, utf8_literal(" / "));
        append_number(v, v->item_count);
        append(v, utf8_literal(" \xC2\xB7 \xE2\x86\x90 \xE2\x86\x92 to browse \xC2\xB7 "));
    }
    if (v->video)
        append(v, utf8_literal("Space to play \xC2\xB7 Esc to close"));
    else if (v->can_save)
        append(v, utf8_literal("Scroll to zoom \xC2\xB7 Drag to pan \xC2\xB7 Ctrl+S to save \xC2\xB7 Esc to close"));
    else
        append(v, utf8_literal("Scroll to zoom \xC2\xB7 Drag to pan \xC2\xB7 Esc to close"));
}
} // namespace
bool ui_image_viewer_layout(UiImageViewer* v, UiRect viewport) noexcept {
    if (!v || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 240 || ui_rect_height(viewport) < 160)
        return false;
    if (ui_rect_equal(v->viewport, viewport))
        return true;
    v->viewport = viewport;
    body_for(v);
    if (!place_controls(v))
        return false;
    arrange(v);
    ++v->revision;
    return true;
}
bool ui_image_viewer_open(UiImageViewer* v, UiRect viewport, UiSize pixels) noexcept {
    if (!v || !ui_size_valid(pixels))
        return false;
    const u32 revision = v->revision + 1;
    *v = {};
    v->revision = revision;
    constexpr u32 capacity = ui_image_viewer_button_count + 1;
    if (!ui_widget_manager_initialize(&v->widgets, {v->widget_storage, capacity}, {v->layout_storage, capacity}))
        return false;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags = ui_layout_node_visible | ui_layout_node_allow_overlap;
    const auto root = ui_widget_manager_add(
        &v->widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("ImageViewer"), style, {}, 0, 1}});
    if (!root.ok())
        return false;
    const Utf8View labels[]{utf8_literal("Zoom out"),      utf8_literal("Zoom in"),    utf8_literal("Fit image"),
                            utf8_literal("Close image"),   utf8_literal("Save image"), utf8_literal("Open original"),
                            utf8_literal("Copy image"),    utf8_literal("Previous"),   utf8_literal("Next")};
    for (u32 i = 0; i < ui_image_viewer_button_count; ++i) {
        const bool chevron = i == ui_image_viewer_previous || i == ui_image_viewer_next;
        auto button_style = ui_layout_style();
        button_style.width = ui_layout_fixed(chevron ? kChevronTarget : kToolbarTarget);
        button_style.height = ui_layout_fixed(chevron ? kChevronTarget : kToolbarTarget);
        button_style.horizontal_alignment =
            i == ui_image_viewer_previous ? UiLayoutAlignment::start : UiLayoutAlignment::end;
        button_style.vertical_alignment = chevron ? UiLayoutAlignment::center : UiLayoutAlignment::start;
        const auto button = ui_widget_manager_add(
            &v->widgets, root.handle,
            {10 + i, UiWidgetKind::button, ui_widget_none, {labels[i], button_style, {}, 0, 10 + i}});
        if (!button.ok())
            return false;
        v->buttons[i] = button.handle;
    }
    v->owner = {&v->widgets, root.handle};
    v->pixels = pixels;
    v->zoom = 1;
    v->reveal = 1;
    v->item_count = 1;
    v->focus = ui_image_viewer_close;
    compose_footer(v);
    v->active = ui_image_viewer_layout(v, viewport);
    if (v->active)
        ui_widget_manager_focus(&v->widgets, v->buttons[ui_image_viewer_close]);
    return v->active;
}
bool ui_image_viewer_set_save_available(UiImageViewer* v, bool available) noexcept {
    if (!v || !v->active) return false;
    if (v->can_save == available) return true;
    v->can_save = available;
    compose_footer(v);
    if (!place_controls(v)) return false;
    ++v->revision;
    return true;
}
bool ui_image_viewer_set_actions(UiImageViewer* v, u8 actions) noexcept {
    if (!v || !v->active) return false;
    actions &= ui_image_viewer_action_open | ui_image_viewer_action_copy;
    if (v->actions == actions) return true;
    v->actions = actions;
    if (!place_controls(v)) return false;
    ++v->revision;
    return true;
}
bool ui_image_viewer_set_item(UiImageViewer* v, UiSize pixels, u32 item, u32 count, bool video) noexcept {
    if (!v || !v->active || !ui_size_valid(pixels) || !count || count > 255 || item >= count)
        return false;
    const bool chrome = v->item_count != count || v->video != video;
    v->pixels = pixels;
    v->item = static_cast<u8>(item);
    v->item_count = static_cast<u8>(count);
    v->video = video;
    v->zoom = 1;
    v->pan = {};
    v->dragging = false;
    compose_footer(v);
    if (chrome) {
        body_for(v);
        if (!place_controls(v))
            return false;
    }
    arrange(v);
    ++v->revision;
    return true;
}
void ui_image_viewer_set_reveal(UiImageViewer* v, float reveal, UiRect origin) noexcept {
    if (!v || !v->active || !ui_coordinate_valid(reveal))
        return;
    reveal = maximum(0.0f, minimum(1.0f, reveal));
    if (reveal == v->reveal && ui_rect_equal(origin, v->origin))
        return;
    v->reveal = reveal;
    v->origin = origin;
    arrange(v);
    ++v->revision;
}
void ui_image_viewer_zoom(UiImageViewer* v, float factor, UiPoint anchor) noexcept {
    if (!v || !v->active || v->video || !ui_coordinate_valid(factor) || factor <= 0)
        return;
    const float previous = v->zoom;
    v->zoom = maximum(1.0f, minimum(16.0f, previous * factor));
    const float ratio = v->zoom / previous;
    const float cx = (v->body.left + v->body.right) * .5f, cy = (v->body.top + v->body.bottom) * .5f;
    v->pan.x = (v->pan.x + cx - anchor.x) * ratio + anchor.x - cx;
    v->pan.y = (v->pan.y + cy - anchor.y) * ratio + anchor.y - cy;
    arrange(v);
    ++v->revision;
}
void ui_image_viewer_pan(UiImageViewer* v, UiPoint delta) noexcept {
    if (!v || !v->active || !ui_coordinate_valid(delta.x) || !ui_coordinate_valid(delta.y))
        return;
    v->pan.x += delta.x;
    v->pan.y += delta.y;
    arrange(v);
    ++v->revision;
}
UiRect ui_image_viewer_button(const UiImageViewer& v, u32 index) noexcept {
    if (index >= ui_image_viewer_button_count || !ui_widget_manager_input_enabled(v.widgets, v.buttons[index]))
        return {};
    const auto* node = ui_widget_manager_layout_node(v.widgets, v.buttons[index]);
    return node ? node->rectangle : UiRect{};
}
bool ui_image_viewer_paint(const UiImageViewer& v, const Theme& theme, PaintStream* paint) noexcept {
    if (!v.active)
        return true;
    const auto r = v.viewport;
    if (!paint_stream_push_clip(paint, r.left, r.top, r.right, r.bottom))
        return false;
    bool ok = paint_stream_push(paint, r.left, r.top, r.right, r.bottom,
                                ui_theme_color_alpha(theme.tokens.overlay.media, v.reveal));
    // The media scrim is always black, so its toolbar uses light foregrounds
    // independently of the application's selected appearance.
    const Theme& toolbar_theme = kDefaultTheme;
    constexpr UiIcon icons[]{UiIcon::zoom_out, UiIcon::zoom_in,       UiIcon::fit,
                             UiIcon::close,    UiIcon::download,      UiIcon::external_link,
                             UiIcon::copy};
    for (u32 i = 0; ok && i < ui_image_viewer_previous; ++i) {
        if (!ui_rect_has_area(ui_image_viewer_button(v, i))) continue;
        ok = ui_button_icon_paint(paint, v.widgets, v.buttons[i],
                                  i == ui_image_viewer_copy && v.copied ? UiIcon::copy_check : icons[i],
                                  toolbar_theme, false, v.keyboard_focus);
    }
    const auto fit = ui_image_viewer_button(v, ui_image_viewer_fit);
    if (ok && ui_rect_has_area(fit)) {
        const float x = fit.right + (kToolbarPitch - kToolbarTarget + kToolbarGroupGap) * .5f - .5f;
        ok = paint_stream_push(paint, x, fit.top + 6, x + 1, fit.bottom - 6, {1, 1, 1, .16f * v.reveal});
    }
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_image_viewer_paint_overlay(const UiImageViewer& v, const Theme& theme, PaintStream* paint) noexcept {
    if (!v.active || v.item_count < 2)
        return true;
    const auto r = v.viewport;
    if (!paint_stream_push_clip(paint, r.left, r.top, r.right, r.bottom))
        return false;
    bool ok = true;
    for (u32 i = ui_image_viewer_previous; ok && i <= ui_image_viewer_next; ++i) {
        const auto button = ui_image_viewer_button(v, i);
        if (!ui_rect_has_area(button)) continue;
        const auto handle = v.buttons[i];
        const bool hover = v.widgets.hovered.slot == handle.slot && v.widgets.hovered.generation == handle.generation;
        const bool pressed = hover && v.widgets.captured.slot == handle.slot &&
                             v.widgets.captured.generation == handle.generation;
        const bool focus = v.keyboard_focus && v.widgets.focused.slot == handle.slot &&
                           v.widgets.focused.generation == handle.generation;
        const float radius = ui_rect_width(button) * .5f;
        const Color fill = ui_theme_color_alpha(
            hover ? theme.tokens.overlay.scrim_strong : theme.tokens.overlay.scrim_light, v.reveal);
        ok = paint_stream_push_rounded_border(paint, button.left, button.top, button.right, button.bottom, fill,
                                              focus ? theme.tokens.border.focus : ui_theme_color_alpha(theme.tokens.border.subtle, v.reveal), radius,
                                              focus ? 2.0f : 1.0f);
        const float extent = 24, x = (button.left + button.right - extent) * .5f,
                    y = (button.top + button.bottom - extent) * .5f + (pressed ? 1 : 0);
        Color ink = kDefaultTheme.text_primary;
        ink.alpha *= (hover ? 1.0f : .86f) * v.reveal;
        ok = ok && ui_icon_paint(paint, i == ui_image_viewer_previous ? UiIcon::chevron_left : UiIcon::chevron_right,
                                 {x, y, x + extent, y + extent}, ink);
    }
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_image_viewer_text_output(const UiImageViewer& v, const Theme&, UiTextOutput* text) noexcept {
    if (!v.active || !v.footer_bytes)
        return true;
    const UiRect footer{v.viewport.left + 24, v.body.bottom + 8, v.viewport.right - 24, v.viewport.bottom};
    Color ink = kDefaultTheme.text_primary;
    ink.alpha *= v.reveal;
    return ui_text_output_push(text, {Utf8View{v.footer, v.footer_bytes}, v.owner, footer, v.viewport, ink,
                                      text_style_none, UiTextAlignment::center, UiTextAlignment::center,
                                      UiTextWrapping::no_wrap, UiTextOverflow::ellipsis});
}
} // namespace maicord

namespace maicord {
bool ui_image_viewer_text(const UiImageViewer& v, const Theme& theme, UiTextCommandStream* text) noexcept {
    UiTextOutput output{text, nullptr};
    return ui_image_viewer_text_output(v, theme, &output);
}
} // namespace maicord
