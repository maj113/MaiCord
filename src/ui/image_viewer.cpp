#include "maicord/ui/image_viewer.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_style.hpp"
namespace maicord {
namespace {
constexpr float kToolbarTarget = 32, kToolbarGap = 8, kToolbarInset = 16, kToolbarTop = 12;
constexpr float kToolbarPitch = kToolbarTarget + kToolbarGap;
// Gap between the view group (fullscreen, fit, zoom) and the item actions.
constexpr float kToolbarGroupGap = 12;
// Media bands: the toolbar row above the body, the caption row below it.
constexpr float kBodyTop = kToolbarTop + kToolbarTarget + 20, kBodyBottom = 40, kBodySide = 24;
constexpr float kChevronTarget = 44, kChevronInset = 16, kPanStep = 40, kSpinnerExtent = 32;
inline bool view_group(u32 index) noexcept {
    return index == ui_image_viewer_fit || index == ui_image_viewer_zoom_in || index == ui_image_viewer_zoom_out ||
           index == ui_image_viewer_fullscreen;
}
// From fit, or actual size when that is smaller (fit upscales on high DPI),
// up to the maximum or actual size when that is larger.
inline float clamp_zoom(const UiImageViewer& v, float zoom) noexcept {
    const float actual = ui_image_viewer_actual_zoom(v);
    return maximum(minimum(1.0f, actual), minimum(maximum(kUiImageViewerMaximumZoom, actual), zoom));
}
inline bool fitted(const UiImageViewer& v) noexcept {
    return v.zoom - 1 < 1e-3f && 1 - v.zoom < 1e-3f;
}
// Nearest device pixel, for crisp 1:1 sampling.
inline float snap(float value, float scale) noexcept {
    const float device = value * scale;
    return static_cast<float>(static_cast<i64>(device >= 0 ? device + .5f : device - .5f)) / scale;
}
void arrange(UiImageViewer* v) noexcept {
    // Images never upscale past one source pixel per DIP; video fills the viewer.
    const float scale = minimum(ui_rect_width(v->body) / v->pixels.width, ui_rect_height(v->body) / v->pixels.height);
    const float fit = v->video ? scale : minimum(1.0f, scale);
    // A zoomed image keeps its absolute scale across resizes; a fitted one refits.
    if (!fitted(*v) && v->fit > 0 && fit != v->fit) {
        v->zoom *= v->fit / fit;
        v->fit = fit;
        v->zoom = clamp_zoom(*v, v->zoom);
    }
    v->fit = fit;
    const float width = v->pixels.width * fit * v->zoom, height = v->pixels.height * fit * v->zoom;
    const float dx = maximum(0.0f, (width - ui_rect_width(v->body)) * .5f);
    const float dy = maximum(0.0f, (height - ui_rect_height(v->body)) * .5f);
    v->pan.x = maximum(-dx, minimum(dx, v->pan.x));
    v->pan.y = maximum(-dy, minimum(dy, v->pan.y));
    float x = (v->body.left + v->body.right - width) * .5f + v->pan.x;
    float y = (v->body.top + v->body.bottom - height) * .5f + v->pan.y;
    const float actual = ui_image_viewer_actual_zoom(*v);
    if (!v->video && v->zoom - actual < actual * 1e-3f && actual - v->zoom < actual * 1e-3f) {
        x = snap(x, v->dpi_scale);
        y = snap(y, v->dpi_scale);
    }
    v->image = {x, y, x + width, y + height};
    v->display = v->image;
    if (v->reveal < 1 && ui_rect_has_area(v->origin)) {
        v->display = ui_rect_lerp(v->origin, v->image, v->reveal);
    } else if (v->reveal < 1) {
        // Without an inline preview to travel to, media grows in from 96%.
        const float inset = (1 - v->reveal) * .02f;
        v->display = {x + width * inset, y + height * inset, x + width * (1 - inset), y + height * (1 - inset)};
    }
}
inline bool shown(const UiImageViewer& v, u32 index) noexcept {
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
// Right-to-left toolbar: Close, Save, Copy, Open | Fit, Zoom in, Zoom out,
// Full screen. Narrow viewports drop the view group from its far end first;
// its keys keep working.
constexpr u32 kToolbarOrder[]{ui_image_viewer_close,         ui_image_viewer_save, ui_image_viewer_copy,
                              ui_image_viewer_open_original, ui_image_viewer_fit,  ui_image_viewer_zoom_in,
                              ui_image_viewer_zoom_out,      ui_image_viewer_fullscreen};
bool place_controls(UiImageViewer* v) noexcept {
    const float width = ui_rect_width(v->viewport);
    u32 wanted = 0;
    for (const u32 index : kToolbarOrder)
        wanted += shown(*v, index);
    const u32 fits = width > 2 * kToolbarInset + kToolbarGroupGap
                         ? static_cast<u32>((width - 2 * kToolbarInset - kToolbarGroupGap + kToolbarGap) /
                                            kToolbarPitch)
                         : 0;
    for (u32 index = 0; index < ui_image_viewer_button_count; ++index) {
        auto* widget = ui_widget_manager_resolve(&v->widgets, v->buttons[index]);
        auto* node = ui_widget_manager_layout_node(&v->widgets, v->buttons[index]);
        if (!node)
            return false;
        bool visible = shown(*v, index);
        auto& style = node->spec.style;
        reset_value(&style.margin);
        if (index == ui_image_viewer_previous || index == ui_image_viewer_next) {
            // Centred on the media body, not the whole viewport.
            style.margin = {kChevronInset, kBodyTop, kChevronInset, kBodyBottom};
        } else if (visible) {
            u32 position = 0;
            for (const u32 candidate : kToolbarOrder) {
                if (candidate == index)
                    break;
                position += shown(*v, candidate);
            }
            visible = position < fits || wanted <= fits;
            style.margin.top = kToolbarTop;
            style.margin.right = kToolbarInset + position * kToolbarPitch + (view_group(index) ? kToolbarGroupGap : 0);
        }
        widget->flags = visible ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
        if (visible)
            style.flags |= ui_layout_node_visible;
        else
            style.flags &= ~ui_layout_node_visible;
    }
    ui_widget_manager_reconcile_input(&v->widgets);
    return ui_widget_manager_layout_rectangle(&v->widgets, v->viewport).ok();
}
void body_for(UiImageViewer* v) noexcept {
    const auto r = v->viewport;
    // Gallery chevrons own the side gutters, so media never sits beneath them.
    const float side = v->item_count > 1 && ui_rect_width(r) >= 480 ? kChevronInset * 2 + kChevronTarget : kBodySide;
    v->body = {r.left + side, r.top + kBodyTop, r.right - side, r.bottom - kBodyBottom};
}
void compose_counter(UiImageViewer* v) noexcept {
    v->counter_bytes = 0;
    if (v->item_count < 2)
        return;
    v->counter_bytes = decimal_write(v->counter, v->item + 1u);
    copy_bytes(v->counter + v->counter_bytes, " / ", 3);
    v->counter_bytes += 3;
    v->counter_bytes += decimal_write(v->counter + v->counter_bytes, v->item_count);
}
// Applies an alpha to the quads painted since first; icon quads keep their
// encoded border.
void fade(PaintStream* paint, u32 first, float alpha) noexcept {
    if (alpha >= 1)
        return;
    for (u32 i = first; i < paint->count; ++i) {
        auto& quad = paint->quads[i];
        quad.fill.alpha *= alpha;
        if (!ui_icon_quad(quad))
            quad.border.alpha *= alpha;
    }
}
// e^x near zero, for the fraction of a wheel notch.
inline float exp_small(float x) noexcept {
    return 1 + x * (1 + x * (.5f + x * (1.0f / 6 + x * (1.0f / 24))));
}
inline UiPoint center(const UiImageViewer& v) noexcept {
    return {(v.body.left + v.body.right) * .5f, (v.body.top + v.body.bottom) * .5f};
}
inline UiRect state_box(const UiImageViewer& v) noexcept {
    const auto c = center(v);
    return {c.x - kSpinnerExtent * .5f, c.y - kSpinnerExtent * .5f, c.x + kSpinnerExtent * .5f,
            c.y + kSpinnerExtent * .5f};
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
    const float dpi_scale = v->dpi_scale > 0 ? v->dpi_scale : 1;
    reset_value(v);
    v->revision = revision;
    v->dpi_scale = dpi_scale;
    constexpr u32 capacity = ui_image_viewer_button_count + 1;
    if (!ui_widget_manager_initialize(&v->widgets, {v->widget_storage, capacity}, {v->layout_storage, capacity}))
        return false;
    auto style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap);
    const auto root = ui_widget_manager_add(
        &v->widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("ImageViewer"), style, {}, 0, 1}});
    if (!root.ok())
        return false;
    const Utf8View labels[]{utf8_literal("Zoom out"),      utf8_literal("Zoom in"),    utf8_literal("Fit image"),
                            utf8_literal("Close image"),   utf8_literal("Save image"), utf8_literal("Open original"),
                            utf8_literal("Copy image"),    utf8_literal("Full screen"), utf8_literal("Previous"),
                            utf8_literal("Next")};
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
    v->chrome = 1;
    v->item_count = 1;
    v->focus = ui_image_viewer_close;
    v->active = ui_image_viewer_layout(v, viewport);
    if (v->active)
        ui_widget_manager_focus(&v->widgets, v->buttons[ui_image_viewer_close]);
    return v->active;
}
bool ui_image_viewer_set_dpi(UiImageViewer* v, u32 dpi) noexcept {
    if (!v || !dpi || dpi > 96 * 8)
        return false;
    const float scale = static_cast<float>(dpi) / 96.0f;
    if (scale == v->dpi_scale)
        return true;
    v->dpi_scale = scale;
    if (v->active) {
        arrange(v);
        ++v->revision;
    }
    return true;
}
bool ui_image_viewer_set_save_available(UiImageViewer* v, bool available) noexcept {
    if (!v || !v->active) return false;
    if (v->can_save == available) return true;
    v->can_save = available;
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
    v->fit = 0;
    reset_value(&v->pan);
    v->dragging = false;
    v->caption_bytes = 0;
    v->media = UiImageViewerMedia::ready;
    compose_counter(v);
    if (chrome) {
        body_for(v);
        if (!place_controls(v))
            return false;
    }
    arrange(v);
    ++v->revision;
    return true;
}
bool ui_image_viewer_set_caption(UiImageViewer* v, Utf8View caption) noexcept {
    if (!v || !v->active)
        return false;
    // Cut at a code point boundary.
    u32 bytes = minimum<u32>(caption.bytes, sizeof(v->caption));
    while (bytes < caption.bytes && bytes && (static_cast<u8>(caption.data[bytes]) & 0xC0) == 0x80)
        --bytes;
    if (bytes == v->caption_bytes && bytes_equal(v->caption, caption.data, bytes))
        return true;
    copy_bytes(v->caption, caption.data, bytes);
    v->caption_bytes = bytes;
    ++v->revision;
    return true;
}
bool ui_image_viewer_set_media(UiImageViewer* v, UiImageViewerMedia media) noexcept {
    if (!v || !v->active)
        return false;
    if (v->media != media) {
        v->media = media;
        ++v->revision;
    }
    return true;
}
bool ui_image_viewer_set_chrome(UiImageViewer* v, float opacity) noexcept {
    if (!v || !v->active || !ui_coordinate_valid(opacity))
        return false;
    opacity = maximum(0.0f, minimum(1.0f, opacity));
    if (v->chrome != opacity) {
        v->chrome = opacity;
        ++v->revision;
    }
    return true;
}
bool ui_image_viewer_set_fullscreen(UiImageViewer* v, bool fullscreen) noexcept {
    if (!v || !v->active)
        return false;
    if (v->fullscreen != fullscreen) {
        v->fullscreen = fullscreen;
        ++v->revision;
    }
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
float ui_image_viewer_actual_zoom(const UiImageViewer& v) noexcept {
    return v.fit > 0 && v.dpi_scale > 0 ? 1.0f / (v.dpi_scale * v.fit) : 1.0f;
}
void ui_image_viewer_zoom(UiImageViewer* v, float factor, UiPoint anchor) noexcept {
    if (!v || !v->active || v->video || !ui_coordinate_valid(factor) || factor <= 0)
        return;
    const float previous = v->zoom;
    v->zoom = clamp_zoom(*v, previous * factor);
    const float ratio = v->zoom / previous;
    const auto c = center(*v);
    v->pan.x = (v->pan.x + c.x - anchor.x) * ratio + anchor.x - c.x;
    v->pan.y = (v->pan.y + c.y - anchor.y) * ratio + anchor.y - c.y;
    arrange(v);
    ++v->revision;
}
void ui_image_viewer_wheel(UiImageViewer* v, i32 delta, UiPoint anchor) noexcept {
    if (!v || !delta)
        return;
    constexpr i32 notch = 120;
    constexpr float log_step = .22314355f; // ln(kUiImageViewerWheelStep)
    float factor = 1;
    const i32 notches = maximum(-64, minimum(64, delta / notch));
    for (i32 i = 0; i < (notches < 0 ? -notches : notches); ++i)
        factor *= notches < 0 ? 1.0f / kUiImageViewerWheelStep : kUiImageViewerWheelStep;
    factor *= exp_small(static_cast<float>(delta % notch) / notch * log_step);
    ui_image_viewer_zoom(v, factor, anchor);
}
void ui_image_viewer_toggle_zoom(UiImageViewer* v, UiPoint anchor) noexcept {
    if (!v || !v->active || v->video)
        return;
    if (!fitted(*v)) {
        ui_image_viewer_apply(v, UiImageViewerCommand::fit);
        return;
    }
    const float actual = ui_image_viewer_actual_zoom(*v);
    ui_image_viewer_zoom(v, (actual > 1.01f ? actual : 2.0f) / v->zoom, anchor);
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
Utf8View ui_image_viewer_tooltip(const UiImageViewer& v, u32 index) noexcept {
    switch (index) {
    case ui_image_viewer_zoom_out: return utf8_literal("Zoom out (-)");
    case ui_image_viewer_zoom_in: return utf8_literal("Zoom in (+)");
    case ui_image_viewer_fit: return utf8_literal("Fit to window (0)");
    case ui_image_viewer_close: return utf8_literal("Close (Esc)");
    case ui_image_viewer_save: return utf8_literal("Save (Ctrl+S)");
    case ui_image_viewer_open_original: return utf8_literal("Open original");
    case ui_image_viewer_copy: return v.copied ? utf8_literal("Copied") : utf8_literal("Copy image (Ctrl+C)");
    case ui_image_viewer_fullscreen:
        return v.fullscreen ? utf8_literal("Exit full screen (F)") : utf8_literal("Full screen (F)");
    case ui_image_viewer_previous: return utf8_literal("Previous (\xE2\x86\x90)");
    case ui_image_viewer_next: return utf8_literal("Next (\xE2\x86\x92)");
    default: return {};
    }
}
float ui_image_viewer_media_opacity(const UiImageViewer& v) noexcept {
    return v.reveal < 1 && !ui_rect_has_area(v.origin) ? v.reveal : 1.0f;
}
bool ui_image_viewer_spinner_visible(const UiImageViewer& v) noexcept {
    return v.active && !v.video && v.media == UiImageViewerMedia::loading && v.reveal > 0;
}
UiImageViewerCommand ui_image_viewer_key_command(const UiImageViewer& v, UiImageViewerKey key,
                                                 UiImageViewerKeyModifiers modifiers) noexcept {
    using Command = UiImageViewerCommand;
    if (!v.active || modifiers.alt)
        return Command::none;
    // Arrows browse the gallery unless a zoomed image needs them to pan.
    const bool browse = v.item_count > 1 && (v.video || v.zoom <= 1.001f);
    if (modifiers.control) {
        if (key == UiImageViewerKey::s)
            return Command::save;
        if (key == UiImageViewerKey::c)
            return v.video ? Command::none : Command::copy;
        return Command::none;
    }
    switch (key) {
    case UiImageViewerKey::escape:
        return v.fullscreen ? Command::exit_fullscreen : Command::dismiss;
    case UiImageViewerKey::f:
        return Command::toggle_fullscreen;
    case UiImageViewerKey::tab:
        return modifiers.shift ? Command::focus_previous : Command::focus_next;
    case UiImageViewerKey::enter:
        return Command::activate;
    case UiImageViewerKey::left:
    case UiImageViewerKey::right:
        if (browse)
            return key == UiImageViewerKey::left ? Command::previous : Command::next;
        if (v.video)
            return Command::video;
        return key == UiImageViewerKey::left ? Command::pan_left : Command::pan_right;
    case UiImageViewerKey::up:
    case UiImageViewerKey::down:
        if (v.video)
            return Command::video;
        return key == UiImageViewerKey::up ? Command::pan_up : Command::pan_down;
    case UiImageViewerKey::space:
        return v.video ? Command::video : Command::activate;
    case UiImageViewerKey::plus:
        return v.video ? Command::none : Command::zoom_in;
    case UiImageViewerKey::minus:
        return v.video ? Command::none : Command::zoom_out;
    case UiImageViewerKey::zero:
    case UiImageViewerKey::home:
        return v.video ? Command::video : Command::fit;
    case UiImageViewerKey::one:
        return v.video ? Command::video : Command::actual_size;
    default:
        return v.video ? Command::video : Command::none;
    }
}
bool ui_image_viewer_apply(UiImageViewer* v, UiImageViewerCommand command) noexcept {
    if (!v || !v->active || v->video)
        return false;
    const auto c = center(*v);
    switch (command) {
    case UiImageViewerCommand::zoom_in:
        ui_image_viewer_zoom(v, kUiImageViewerWheelStep, c);
        return true;
    case UiImageViewerCommand::zoom_out:
        ui_image_viewer_zoom(v, 1.0f / kUiImageViewerWheelStep, c);
        return true;
    case UiImageViewerCommand::fit:
        v->zoom = 1;
        reset_value(&v->pan);
        arrange(v);
        ++v->revision;
        return true;
    case UiImageViewerCommand::actual_size:
        ui_image_viewer_zoom(v, ui_image_viewer_actual_zoom(*v) / v->zoom, c);
        return true;
    case UiImageViewerCommand::pan_left:
    case UiImageViewerCommand::pan_right:
        ui_image_viewer_pan(v, {command == UiImageViewerCommand::pan_left ? kPanStep : -kPanStep, 0});
        return true;
    case UiImageViewerCommand::pan_up:
    case UiImageViewerCommand::pan_down:
        ui_image_viewer_pan(v, {0, command == UiImageViewerCommand::pan_up ? kPanStep : -kPanStep});
        return true;
    default:
        return false;
    }
}
bool ui_image_viewer_paint(const UiImageViewer& v, const Theme& theme, PaintStream* paint) noexcept {
    if (!v.active)
        return true;
    const auto r = v.viewport;
    if (!paint_stream_push_clip(paint, r))
        return false;
    bool ok = paint_stream_push(paint, r, ui_theme_color_alpha(theme.tokens.overlay.media, v.reveal));
    // The media scrim is always black, so its toolbar uses light foregrounds
    // independently of the application's selected appearance.
    const Theme& toolbar_theme = kDefaultTheme;
    static constexpr UiIcon icons[]{UiIcon::zoom_out, UiIcon::zoom_in,       UiIcon::fit,  UiIcon::close,
                             UiIcon::download, UiIcon::external_link, UiIcon::copy, UiIcon::maximize};
    const u32 first = paint->count;
    for (u32 i = 0; ok && i < ui_image_viewer_previous; ++i) {
        if (!ui_rect_has_area(ui_image_viewer_button(v, i))) continue;
        const UiIcon icon = i == ui_image_viewer_copy && v.copied              ? UiIcon::copy_check
                            : i == ui_image_viewer_fullscreen && v.fullscreen ? UiIcon::minimize
                                                                              : icons[i];
        ok = ui_button_icon_paint(paint, v.widgets, v.buttons[i], icon, toolbar_theme, false, v.keyboard_focus);
    }
    // The divider between the view group and the item actions.
    u32 last_view = ui_image_viewer_button_count;
    for (const u32 index : kToolbarOrder)
        if (view_group(index) && ui_rect_has_area(ui_image_viewer_button(v, index)) &&
            last_view == ui_image_viewer_button_count)
            last_view = index;
    if (ok && last_view != ui_image_viewer_button_count) {
        const auto b = ui_image_viewer_button(v, last_view);
        const float x = b.right + (kToolbarGap + kToolbarGroupGap) * .5f - .5f, inset = kToolbarGap - 2;
        ok = paint_stream_push(paint, x, b.top + inset, x + 1, b.bottom - inset, toolbar_theme.tokens.border.strong);
    }
    fade(paint, first, v.reveal * v.chrome);
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_image_viewer_paint_overlay(const UiImageViewer& v, const Theme& theme, PaintStream* paint) noexcept {
    if (!v.active)
        return true;
    const auto r = v.viewport;
    if (!paint_stream_push_clip(paint, r))
        return false;
    bool ok = true;
    const Theme& ink_theme = kDefaultTheme;
    if (!v.video && v.media == UiImageViewerMedia::loading) {
        UiSpinnerStyle style{};
        ok = ui_spinner_paint(paint, state_box(v), ui_theme_color_alpha(ink_theme.text_primary, v.reveal),
                              v.spinner_phase, style);
    } else if (!v.video && v.media == UiImageViewerMedia::failed) {
        const auto box = state_box(v);
        ok = ui_icon_paint(paint, UiIcon::image_off, box,
                           ui_theme_color_alpha(ink_theme.text_secondary, v.reveal));
    }
    const u32 first = paint->count;
    for (u32 i = ui_image_viewer_previous; ok && v.item_count > 1 && i <= ui_image_viewer_next; ++i) {
        const auto button = ui_image_viewer_button(v, i);
        if (!ui_rect_has_area(button)) continue;
        const auto handle = v.buttons[i];
        const bool hover = ui_widget_handle_equal(v.widgets.hovered, handle);
        const bool pressed = hover && ui_widget_handle_equal(v.widgets.captured, handle);
        const bool focus = v.keyboard_focus && ui_widget_handle_equal(v.widgets.focused, handle);
        const float radius = ui_rect_width(button) * .5f;
        const Color fill = hover ? theme.tokens.overlay.scrim_strong : theme.tokens.overlay.scrim_light;
        ok = paint_stream_push_rounded_border(paint, button, fill, focus ? theme.tokens.border.focus : theme.tokens.border.subtle, radius, focus ? 2.0f : 1.0f);
        const float extent = 24, x = (button.left + button.right - extent) * .5f,
                    y = (button.top + button.bottom - extent) * .5f + (pressed ? 1 : 0);
        Color ink = ink_theme.text_primary;
        ink.alpha *= hover ? 1.0f : .86f;
        ok = ok && ui_icon_paint(paint, i == ui_image_viewer_previous ? UiIcon::chevron_left : UiIcon::chevron_right,
                                 {x, y, x + extent, y + extent}, ink);
    }
    fade(paint, first, v.reveal * v.chrome);
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_image_viewer_text_output(const UiImageViewer& v, const Theme&, UiTextOutput* text) noexcept {
    if (!v.active)
        return true;
    const float alpha = v.reveal * v.chrome;
    if (v.counter_bytes) {
        // The counter shares the toolbar row's centre line; it ends where the
        // leftmost toolbar button begins.
        float right = v.viewport.right - kToolbarInset;
        for (u32 i = 0; i < ui_image_viewer_previous; ++i)
            if (const auto b = ui_image_viewer_button(v, i); ui_rect_has_area(b))
                right = minimum(right, b.left - kToolbarInset);
        const UiRect row{v.viewport.left + kToolbarInset, v.viewport.top + kToolbarTop, right,
                         v.viewport.top + kToolbarTop + kToolbarTarget};
        if (ui_rect_has_area(row) &&
            !ui_text_output_push(text, {Utf8View{v.counter, v.counter_bytes}, v.owner, row, v.viewport,
                                        ui_theme_color_alpha(kDefaultTheme.text_secondary, alpha), text_style_none,
                                        UiTextAlignment::start, UiTextAlignment::center, TextWrapping::no_wrap,
                                        TextOverflow::ellipsis}))
            return false;
    }
    if (v.caption_bytes) {
        // The caption spans the media column, so its edges match the media's.
        const UiRect band{v.body.left, v.body.bottom, v.body.right, v.viewport.bottom};
        if (!ui_text_output_push(text, {Utf8View{v.caption, v.caption_bytes}, v.owner, band, v.viewport,
                                        ui_theme_color_alpha(kDefaultTheme.text_primary, alpha), text_style_subtext,
                                        UiTextAlignment::center, UiTextAlignment::center, TextWrapping::no_wrap,
                                        TextOverflow::ellipsis}))
            return false;
    }
    if (!v.video && v.media == UiImageViewerMedia::failed) {
        const auto box = state_box(v);
        const UiRect line{v.body.left, box.bottom + kToolbarGap, v.body.right, box.bottom + kToolbarGap + 20};
        if (!ui_text_output_push(text, {utf8_literal("This image couldn't be loaded."), v.owner, line, v.viewport,
                                        ui_theme_color_alpha(kDefaultTheme.text_secondary, v.reveal),
                                        text_style_subtext, UiTextAlignment::center, UiTextAlignment::center,
                                        TextWrapping::no_wrap, TextOverflow::ellipsis}))
            return false;
    }
    return true;
}
} // namespace maicord
