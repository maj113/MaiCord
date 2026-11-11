#include "maicord/ui/media/image_viewer.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/widgets/button.hpp"
#include "maicord/ui/voice/video_scene.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/text/style.hpp"
namespace maicord {
namespace {
constexpr float kToolbarTarget = 32, kToolbarGap = 8, kToolbarInset = 16, kToolbarTop = 12;
constexpr float kToolbarPitch = kToolbarTarget + kToolbarGap;
// Gap between the view group (fullscreen, fit, zoom) and the item actions.
constexpr float kToolbarGroupGap = 12;
// Media bands: the toolbar row above the body, the caption row below it.
constexpr float kBodyTop = kToolbarTop + kToolbarTarget + 20, kBodyBottom = 40, kBodySide = 24;
constexpr float kChevronTarget = 44, kChevronInset = 16, kPanStep = 40, kSpinnerExtent = 32;
// Page mode: the header band (glyph, title over source, toolbar), the inset around the player, the queue column (its
// heading, then the scrolled list of rows: thumbnail, title over artist, duration) and the Retry button. The queue
// column takes a third of a wide page within its bounds.
constexpr float kPageHeader = 48, kPageToolbarTop = (kPageHeader - kToolbarTarget) * .5f, kPageInset = 16;
constexpr float kPageGlyph = 20, kPageTextLeft = kPageInset + kPageGlyph + 12, kPageStageGlyph = 48;
constexpr float kPageQueue = 320, kPageQueueWide = 420, kPageQueueMinimum = 720, kRetryWidth = 80;
constexpr float kQueueHeading = 40, kQueueRow = kUiImageViewerQueueRow, kQueueThumb = 40, kQueuePad = 8;
constexpr float kQueueDuration = 48;
inline bool view_group(u32 index) noexcept {
    return index == ui_image_viewer_fit || index == ui_image_viewer_zoom_in || index == ui_image_viewer_zoom_out ||
           index == ui_image_viewer_fullscreen;
}
UiImageViewerPageLayout page_layout(const UiImageViewer& v) noexcept {
    const auto r = v.viewport;
    UiImageViewerPageLayout out{};
    if (v.fullscreen) {
        out.player = r;
        return out;
    }
    out.header = {r.left, r.top, r.right, r.top + kPageHeader};
    const UiRect content{r.left + kPageInset, out.header.bottom + kPageInset * .5f, r.right - kPageInset,
                         r.bottom - kPageInset};
    if (v.page_info.kind == UiImageViewerPageKind::image) {
        // The caption band stays under the picture, as in the viewer.
        out.player = {content.left, content.top, content.right, r.bottom - kBodyBottom};
        return out;
    }
    out.player = content;
    if (!v.page_info.queue)
        return out;
    if (ui_rect_width(content) >= kPageQueueMinimum) {
        const float width = minimum(kPageQueueWide, maximum(kPageQueue, ui_rect_width(content) * .34f));
        out.queue = {content.right - width, content.top, content.right, content.bottom};
        out.player.right = out.queue.left - kPageInset;
        return out;
    }
    // A narrow page stacks the queue under a 16:9 player, leaving it at least three rows.
    const float split = minimum(content.top + ui_rect_width(content) * 9 / 16,
                                content.bottom - kQueueHeading - 3 * kQueueRow - kPageInset);
    if (split > content.top + kQueueRow) {
        out.player.bottom = split;
        out.queue = {content.left, split + kPageInset, content.right, content.bottom};
    }
    return out;
}
// The queue's list: the column under its heading.
UiRect queue_body(const UiImageViewerPageLayout& layout) noexcept {
    return ui_rect_has_area(layout.queue)
               ? UiRect{layout.queue.left, layout.queue.top + kQueueHeading, layout.queue.right,
                        layout.queue.bottom - kQueuePad}
               : UiRect{};
}
// The queue's rows[n] (track first + n + 1), placed by the list's scroll; and its thumbnail square.
UiRect queue_row(const UiImageViewer& v, const UiImageViewerPageLayout& layout, u32 n) noexcept {
    const auto body = queue_body(layout);
    const float top = body.top + static_cast<float>(v.queue->first + n) * kQueueRow - v.queue->scroll;
    return {body.left + kQueuePad, top, body.right - kQueuePad, top + kQueueRow};
}
UiRect queue_thumb(const UiRect& row) noexcept {
    return {row.left + kQueuePad, row.top + kQueuePad, row.left + kQueuePad + kQueueThumb,
            row.top + kQueuePad + kQueueThumb};
}
// The audio page's artwork square (empty when the stage has no room for it); the page's picture draws into it.
UiRect page_art(const UiImageViewer& v) noexcept {
    return v.page_info.kind == UiImageViewerPageKind::audio && !v.fullscreen
               ? ui_audio_page_layout(page_layout(v).player, kDefaultUiAudioControlsStyle).art
               : UiRect{};
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
    // An audio page's artwork fills its square; images never upscale past one source pixel per DIP, video fills the
    // viewer.
    if (v->page && v->page_info.kind == UiImageViewerPageKind::audio) {
        v->image = v->display = v->body;
        return;
    }
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
    // A full screen page is all player.
    if (v.page && v.fullscreen)
        return false;
    // An unavailable page has nothing to zoom, copy or save.
    const bool inert = v.page && v.media == UiImageViewerMedia::failed;
    switch (index) {
    case ui_image_viewer_zoom_out:
    case ui_image_viewer_zoom_in:
    case ui_image_viewer_fit:
        return !v.video && !inert;
    case ui_image_viewer_save:
        return v.can_save && !inert;
    case ui_image_viewer_open_original:
        return (v.actions & ui_image_viewer_action_open) != 0;
    case ui_image_viewer_new_tab:
        return (v.actions & ui_image_viewer_action_tab) != 0 && !v.page;
    case ui_image_viewer_copy:
        return (v.actions & ui_image_viewer_action_copy) != 0 && !v.video && !inert;
    case ui_image_viewer_fullscreen:
        return !v.page;
    case ui_image_viewer_previous:
    case ui_image_viewer_next:
        return v.item_count > 1;
    case ui_image_viewer_jump:
        return v.page;
    case ui_image_viewer_retry:
        return inert;
    default:
        return true;
    }
}
// Right-to-left toolbar: Close, Save, Copy, Open, Open in new tab, Jump (pages) | Fit, Zoom in,
// Zoom out, Full screen. Narrow viewports drop the view group from its far end
// first; its keys keep working.
constexpr u32 kToolbarOrder[]{ui_image_viewer_close,  ui_image_viewer_save,    ui_image_viewer_copy,
                              ui_image_viewer_open_original, ui_image_viewer_new_tab, ui_image_viewer_jump,
                              ui_image_viewer_fit, ui_image_viewer_zoom_in, ui_image_viewer_zoom_out,
                              ui_image_viewer_fullscreen};
// The unavailable state's icon, under it its line of text, then Retry.
constexpr float kStateText = 20;
bool place_controls(UiImageViewer* v) noexcept {
    // A page keeps room for its title beside the toolbar.
    const float width = ui_rect_width(v->viewport) - (v->page ? kPageTextLeft + 160 : 0);
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
        } else if (index == ui_image_viewer_retry) {
            const float x = (v->body.left + v->body.right - kRetryWidth) * .5f;
            const float y = (v->body.top + v->body.bottom + kSpinnerExtent) * .5f + kToolbarGap * 2 + kStateText;
            style.margin = {x - v->viewport.left, y - v->viewport.top, 0, 0};
        } else if (visible) {
            u32 position = 0;
            for (const u32 candidate : kToolbarOrder) {
                if (candidate == index)
                    break;
                position += shown(*v, candidate);
            }
            visible = position < fits || wanted <= fits;
            style.margin.top = v->page ? kPageToolbarTop : kToolbarTop;
            style.margin.right = kToolbarInset + position * kToolbarPitch + (view_group(index) ? kToolbarGroupGap : 0);
        }
        // Jump to message without its message stays visible but inert ("Message unavailable" on hover).
        widget->flags = !visible ? ui_widget_none
                        : index == ui_image_viewer_jump && !v->page_info.can_jump
                            ? ui_widget_hit_test
                            : ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
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
    if (v->page) {
        const UiRect art = page_art(*v);
        v->body = ui_rect_has_area(art) ? art : page_layout(*v).player;
        return;
    }
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
bool ui_image_viewer_layout(UiImageViewer* v, const UiRect& viewport) noexcept {
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
bool ui_image_viewer_open(UiImageViewer* v, const UiRect& viewport, UiSize pixels) noexcept {
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
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("ImageViewer"), style, {}, 1}});
    if (!root.ok())
        return false;
    const Utf8View labels[]{utf8_literal("Zoom out"),      utf8_literal("Zoom in"),    utf8_literal("Fit image"),
                            utf8_literal("Close image"),   utf8_literal("Save image"), utf8_literal("Open original"),
                            utf8_literal("Copy image"),    utf8_literal("Full screen"), utf8_literal("Previous"),
                            utf8_literal("Next"),          utf8_literal("Jump to message"), utf8_literal("Retry"),
                            utf8_literal("Open in new tab")};
    for (u32 i = 0; i < ui_image_viewer_button_count; ++i) {
        const bool chevron = i == ui_image_viewer_previous || i == ui_image_viewer_next;
        const bool retry = i == ui_image_viewer_retry;
        auto button_style = ui_layout_style();
        button_style.width = ui_layout_fixed(chevron ? kChevronTarget : retry ? kRetryWidth : kToolbarTarget);
        button_style.height = ui_layout_fixed(chevron ? kChevronTarget : kToolbarTarget);
        button_style.horizontal_alignment =
            i == ui_image_viewer_previous || retry ? UiLayoutAlignment::start : UiLayoutAlignment::end;
        button_style.vertical_alignment = chevron ? UiLayoutAlignment::center : UiLayoutAlignment::start;
        const auto button = ui_widget_manager_add(
            &v->widgets, root.handle,
            {10 + i, UiWidgetKind::button, ui_widget_none, {labels[i], button_style, {}, 10 + i}});
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
    actions &= ui_image_viewer_action_open | ui_image_viewer_action_copy | ui_image_viewer_action_tab;
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
    const u32 bytes = utf8_truncated_length(caption, sizeof(v->caption));
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
        const bool retry = shown(*v, ui_image_viewer_retry);
        v->media = media;
        if (retry != shown(*v, ui_image_viewer_retry) && !place_controls(v))
            return false;
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
        // A page's regions and toolbar follow it; its viewport usually changes with it too.
        if (v->page) {
            body_for(v);
            if (!place_controls(v))
                return false;
            arrange(v);
        }
        ++v->revision;
    }
    return true;
}
void ui_image_viewer_set_reveal(UiImageViewer* v, float reveal, const UiRect& origin) noexcept {
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
bool ui_image_viewer_set_page(UiImageViewer* v, const UiImageViewerPage& page) noexcept {
    if (!v || !v->active)
        return false;
    auto& info = v->page_info;
    const bool same_text = info.title.data == page.title.data && info.title.bytes == page.title.bytes &&
                           info.source.data == page.source.data && info.source.bytes == page.source.bytes;
    const bool same_shape = v->page && info.kind == page.kind && info.queue == page.queue;
    if (same_shape && same_text && info.glyph == page.glyph && info.can_jump == page.can_jump)
        return true;
    info = page;
    if (!same_shape) {
        v->page = true;
        // The page's root owns its hosted player (the audio card needs an enabled owner); it takes no hits.
        if (auto* root = ui_widget_manager_resolve(&v->widgets, v->owner.handle))
            root->flags = ui_widget_enabled;
        v->video = page.kind != UiImageViewerPageKind::image;
        v->item = 0;
        v->item_count = 1;
        v->counter_bytes = 0;
        v->fullscreen = false;
        body_for(v);
    }
    if (!place_controls(v))
        return false;
    arrange(v);
    ++v->revision;
    return true;
}
UiImageViewerPageLayout ui_image_viewer_page_layout(const UiImageViewer& v) noexcept {
    return v.active && v.page ? page_layout(v) : UiImageViewerPageLayout{};
}
UiRect ui_image_viewer_queue_body(const UiImageViewer& v) noexcept {
    return queue_body(ui_image_viewer_page_layout(v));
}
u32 ui_image_viewer_queue_rows(const UiImageViewer& v) noexcept {
    const float room = ui_rect_height(ui_image_viewer_queue_body(v));
    return room > 0 ? minimum(kUiImageViewerQueueRows, static_cast<u32>(room / kQueueRow) + 3) : 0;
}
u16 ui_image_viewer_queue_hit(const UiImageViewer& v, UiPoint point) noexcept {
    const auto body = ui_image_viewer_queue_body(v);
    if (!v.queue || !ui_rect_contains(body, point.x, point.y))
        return 0;
    const u32 track = static_cast<u32>((point.y - body.top + v.queue->scroll) / kQueueRow) + 1;
    return track <= v.queue->count ? static_cast<u16>(track) : 0;
}
UiRect ui_image_viewer_queue_row(const UiImageViewer& v, u32 n) noexcept {
    return v.queue ? queue_row(v, ui_image_viewer_page_layout(v), n) : UiRect{};
}
UiRect ui_image_viewer_queue_thumb(const UiImageViewer& v, u32 n) noexcept {
    return v.queue ? queue_thumb(ui_image_viewer_queue_row(v, n)) : UiRect{};
}
void ui_image_viewer_set_view(UiImageViewer* v, float zoom, UiPoint pan) noexcept {
    if (!v || !v->active || v->video || !ui_coordinate_valid(zoom) || zoom <= 0 || !ui_coordinate_valid(pan.x) ||
        !ui_coordinate_valid(pan.y))
        return;
    v->zoom = clamp_zoom(*v, zoom);
    v->pan = pan;
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
    // A page's Jump to message keeps its place while its message is unavailable.
    if (index >= ui_image_viewer_button_count ||
        (!ui_widget_manager_input_enabled(v.widgets, v.buttons[index]) &&
         !(v.page && index == ui_image_viewer_jump)))
        return {};
    const auto* node = ui_widget_manager_layout_node(v.widgets, v.buttons[index]);
    return node && (node->spec.style.flags & ui_layout_node_visible) ? node->rectangle : UiRect{};
}
Utf8View ui_image_viewer_tooltip(const UiImageViewer& v, u32 index) noexcept {
    switch (index) {
    case ui_image_viewer_zoom_out: return utf8_literal("Zoom out (-)");
    case ui_image_viewer_zoom_in: return utf8_literal("Zoom in (+)");
    case ui_image_viewer_fit: return utf8_literal("Fit to window (0)");
    case ui_image_viewer_close: return v.page ? utf8_literal("Close tab (Ctrl+W)") : utf8_literal("Close (Esc)");
    case ui_image_viewer_save: return utf8_literal("Save (Ctrl+S)");
    case ui_image_viewer_open_original: return utf8_literal("Open original");
    case ui_image_viewer_copy: return v.copied ? utf8_literal("Copied") : utf8_literal("Copy image (Ctrl+C)");
    case ui_image_viewer_fullscreen:
        return v.fullscreen ? utf8_literal("Exit full screen (F)") : utf8_literal("Full screen (F)");
    case ui_image_viewer_previous: return utf8_literal("Previous (\xE2\x86\x90)");
    case ui_image_viewer_next: return utf8_literal("Next (\xE2\x86\x92)");
    case ui_image_viewer_jump:
        return v.page_info.can_jump ? utf8_literal("Jump to message") : utf8_literal("Message unavailable");
    case ui_image_viewer_new_tab: return utf8_literal("Open in new tab");
    case ui_image_viewer_retry: return utf8_literal("Retry");
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
    // An unavailable page has nothing to zoom, copy or save.
    const bool inert = v.page && v.media == UiImageViewerMedia::failed;
    if (modifiers.control) {
        if (key == UiImageViewerKey::s)
            return inert ? Command::none : Command::save;
        if (key == UiImageViewerKey::c)
            return v.video || inert ? Command::none : Command::copy;
        return Command::none;
    }
    switch (key) {
    case UiImageViewerKey::escape:
        // A page is not dismissed: Esc only leaves full screen, else falls through.
        return v.fullscreen ? Command::exit_fullscreen : v.page ? Command::none : Command::dismiss;
    case UiImageViewerKey::f:
        // A page's player owns full screen (an image page has none).
        return !v.page ? Command::toggle_fullscreen : v.video ? Command::video : Command::none;
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
        return v.video || inert ? Command::none : Command::zoom_in;
    case UiImageViewerKey::minus:
        return v.video || inert ? Command::none : Command::zoom_out;
    case UiImageViewerKey::zero:
    case UiImageViewerKey::home:
        return v.video ? Command::video : inert ? Command::none : Command::fit;
    case UiImageViewerKey::one:
        return v.video ? Command::video : inert ? Command::none : Command::actual_size;
    default:
        return v.video ? Command::video : Command::none;
    }
}
bool ui_image_viewer_apply(UiImageViewer* v, UiImageViewerCommand command) noexcept {
    if (!v || !v->active)
        return false;
    // Tab and Shift+Tab move the keyboard focus over the buttons, a video's too.
    if (command == UiImageViewerCommand::focus_next || command == UiImageViewerCommand::focus_previous) {
        const auto target = ui_widget_manager_next_focus(v->widgets, v->widgets.focused,
                                                         command == UiImageViewerCommand::focus_previous, true);
        ui_widget_manager_focus(&v->widgets, target);
        v->focus = static_cast<u8>(v->widgets.storage[target.slot].id - 10);
        v->keyboard_focus = true;
        ++v->revision;
        return true;
    }
    if (v->video)
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
    UiPaint p{paint, theme};
    if (!p.clip(v.viewport))
        return false;
    // A page sits in the content column in the application's colours. The viewer's media scrim is always black,
    // so its toolbar uses light foregrounds independently of the application's selected appearance.
    const Theme& toolbar_theme = v.page ? theme : kDefaultTheme;
    if (v.page) {
        const auto layout = page_layout(v);
        const bool audio = v.page_info.kind == UiImageViewerPageKind::audio;
        p.fill(v.viewport, v.fullscreen ? theme.tokens.overlay.media : theme.tokens.background.primary);
        if (!v.fullscreen) {
            const float glyph_top = layout.header.top + (kPageHeader - kPageGlyph) * .5f;
            p.rule_below(layout.header)
                .icon(v.page_info.glyph,
                      {layout.header.left + kPageInset, glyph_top, layout.header.left + kPageInset + kPageGlyph,
                       glyph_top + kPageGlyph},
                      theme.text_secondary);
            // Pictures play on a dark stage; the audio card sits on the page itself.
            if (!audio)
                p.rounded(layout.player, theme.tokens.overlay.media, theme.scale.radius_md);
        }
        // Until the player draws over it, an audio stage shows its artwork tile (the picture draws over it when
        // the page has one) and another video stage the page's glyph.
        const auto c = center(v);
        const UiRect art = page_art(v);
        if (ui_rect_has_area(art))
            p.rounded(art, ui_theme_color_alpha(theme.tokens.accent.brand, .16f), theme.scale.radius_md)
                .icon(UiIcon::music, ui_rect_shrink(art, ui_rect_width(art) * .3f), theme.tokens.accent.brand);
        else if (v.video && !v.idle && v.media != UiImageViewerMedia::failed)
            p.icon(v.page_info.glyph,
                   {c.x - kPageStageGlyph * .5f, c.y - kPageStageGlyph * .5f, c.x + kPageStageGlyph * .5f,
                    c.y + kPageStageGlyph * .5f},
                   audio ? theme.text_secondary : kDefaultTheme.text_secondary);
        // The queue's list, clipped under its heading: the current track on the selected fill, the row under the
        // pointer on the hover fill, a thin rule between rows, and each row's thumbnail well (its picture draws over
        // it; the equaliser over the current one is overlay).
        if (v.queue && ui_rect_has_area(layout.queue)) {
            const auto& queue = *v.queue;
            p.rounded(layout.queue, theme.tokens.background.secondary, theme.scale.radius_md).clip(queue_body(layout));
            for (u32 n = 0, rows = ui_image_viewer_queue_rows(v); n < rows && queue.first + n < queue.count; ++n) {
                const u32 track = queue.first + n + 1;
                const auto row = queue_row(v, layout, n);
                const auto tile = queue_thumb(row);
                if (track == queue.current || track == queue.hovered)
                    p.rounded(row, track == queue.current ? theme.tokens.state.row_selected : theme.tokens.state.row_hover,
                              theme.scale.radius_sm);
                else if (track < queue.count)
                    p.separator({tile.left, row.bottom - 1, row.right - kQueuePad, row.bottom});
                p.rounded(tile, theme.tokens.fill.muted, theme.scale.radius_sm)
                    .icon(UiIcon::music, ui_rect_shrink(tile, 10), theme.text_secondary);
            }
            p.unclip();
        }
    } else {
        p.fill(v.viewport, ui_theme_color_alpha(theme.tokens.overlay.media, v.reveal));
    }
    static constexpr UiIcon icons[]{UiIcon::zoom_out, UiIcon::zoom_in,       UiIcon::fit,  UiIcon::close,
                             UiIcon::download, UiIcon::external_link, UiIcon::copy, UiIcon::maximize};
    const u32 first = paint->count;
    for (u32 i = 0; p && i < ui_image_viewer_previous; ++i) {
        if (!ui_rect_has_area(ui_image_viewer_button(v, i))) continue;
        const UiIcon icon = i == ui_image_viewer_copy && v.copied              ? UiIcon::copy_check
                            : i == ui_image_viewer_fullscreen && v.fullscreen ? UiIcon::minimize
                                                                              : icons[i];
        if (!ui_button_icon_paint(paint, v.widgets, v.buttons[i], icon, toolbar_theme, false, v.keyboard_focus))
            p.fail();
    }
    if (p && ui_rect_has_area(ui_image_viewer_button(v, ui_image_viewer_jump))) {
        const u32 jump = paint->count;
        if (!ui_button_icon_paint(paint, v.widgets, v.buttons[ui_image_viewer_jump], UiIcon::arrow_right,
                                  toolbar_theme, false, v.keyboard_focus))
            p.fail();
        // Inert while its message is unavailable.
        if (!v.page_info.can_jump)
            fade(paint, jump, .4f);
    }
    if (p && ui_rect_has_area(ui_image_viewer_button(v, ui_image_viewer_new_tab)) &&
        !ui_button_icon_paint(paint, v.widgets, v.buttons[ui_image_viewer_new_tab], UiIcon::plus, toolbar_theme, false,
                              v.keyboard_focus))
        p.fail();
    // The divider between the view group and the item actions.
    u32 last_view = ui_image_viewer_button_count;
    for (const u32 index : kToolbarOrder)
        if (view_group(index) && ui_rect_has_area(ui_image_viewer_button(v, index))) {
            last_view = index;
            break;
        }
    if (p && last_view != ui_image_viewer_button_count) {
        const auto b = ui_image_viewer_button(v, last_view);
        const float x = b.right + (kToolbarGap + kToolbarGroupGap) * .5f - .5f, inset = kToolbarGap - 2;
        p.fill({x, b.top + inset, x + 1, b.bottom - inset}, toolbar_theme.tokens.border.strong);
    }
    fade(paint, first, v.reveal * v.chrome);
    return p.end();
}
bool ui_image_viewer_paint_overlay(const UiImageViewer& v, const Theme& theme, PaintStream* paint) noexcept {
    if (!v.active)
        return true;
    UiPaint p{paint, theme};
    if (!p.clip(v.viewport))
        return false;
    // A video page's stage is dark like the viewer; an audio page shows its state on the page itself.
    const Theme& ink_theme = v.page && v.page_info.kind == UiImageViewerPageKind::audio ? theme : kDefaultTheme;
    if (!v.video && v.media == UiImageViewerMedia::loading)
        p.spinner(state_box(v), ui_theme_color_alpha(ink_theme.text_primary, v.reveal), v.spinner_phase);
    else if ((!v.video || v.page) && v.media == UiImageViewerMedia::failed)
        p.icon(UiIcon::image_off, state_box(v), ui_theme_color_alpha(ink_theme.text_secondary, v.reveal));
    if (const auto retry = ui_image_viewer_button(v, ui_image_viewer_retry); ui_rect_has_area(retry)) {
        const auto handle = v.buttons[ui_image_viewer_retry];
        const bool focus = v.keyboard_focus && ui_widget_handle_equal(v.widgets.focused, handle);
        p.rounded_border(retry,
                         ui_widget_handle_equal(v.widgets.hovered, handle) ? theme.tokens.overlay.scrim_strong
                                                                           : theme.tokens.overlay.scrim_light,
                         focus ? theme.tokens.border.focus : theme.tokens.border.subtle, theme.scale.radius_sm,
                         focus ? 2.0f : 1.0f);
    }
    // A video or audio page whose player has not started: its play button, over the poster or the artwork.
    if (v.page && v.video && v.idle) {
        const UiRect art = page_art(v), stage = ui_rect_has_area(art) ? art : page_layout(v).player;
        if (p && !ui_video_play_affordance_paint(stage, stage, {}, theme, paint))
            p.fail();
    }
    // The queue's current track: the equaliser on a scrim over its thumbnail.
    if (const u32 n = v.page && v.queue ? v.queue->current - 1u - v.queue->first : kUiImageViewerQueueRows;
        n < ui_image_viewer_queue_rows(v) && v.queue->current) {
        const auto tile = ui_image_viewer_queue_thumb(v, n);
        p.clip(ui_image_viewer_queue_body(v))
            .rounded(tile, theme.tokens.overlay.scrim_strong, theme.scale.radius_sm)
            .icon(UiIcon::audio_lines, ui_rect_shrink(tile, 10), kDefaultTheme.text_primary)
            .unclip();
    }
    const u32 first = paint->count;
    for (u32 i = ui_image_viewer_previous; p && v.item_count > 1 && i <= ui_image_viewer_next; ++i) {
        const auto button = ui_image_viewer_button(v, i);
        if (!ui_rect_has_area(button)) continue;
        const auto handle = v.buttons[i];
        const bool hover = ui_widget_handle_equal(v.widgets.hovered, handle);
        const bool pressed = hover && ui_widget_handle_equal(v.widgets.captured, handle);
        const bool focus = v.keyboard_focus && ui_widget_handle_equal(v.widgets.focused, handle);
        const float extent = 24, x = (button.left + button.right - extent) * .5f,
                    y = (button.top + button.bottom - extent) * .5f + (pressed ? 1 : 0);
        Color ink = ink_theme.text_primary;
        ink.alpha *= hover ? 1.0f : .86f;
        p.rounded_border(button, hover ? theme.tokens.overlay.scrim_strong : theme.tokens.overlay.scrim_light,
                         focus ? theme.tokens.border.focus : theme.tokens.border.subtle, ui_rect_width(button) * .5f,
                         focus ? 2.0f : 1.0f)
            .icon(i == ui_image_viewer_previous ? UiIcon::chevron_left : UiIcon::chevron_right,
                  {x, y, x + extent, y + extent}, ink);
    }
    fade(paint, first, v.reveal * v.chrome);
    return p.end();
}
bool ui_image_viewer_text_output(const UiImageViewer& v, const Theme& theme, UiTextOutput* out) noexcept {
    if (!v.active)
        return true;
    const float alpha = v.reveal * v.chrome;
    UiTextPen<UiTextClip::label> text{out, v.widgets, v.viewport};
    auto label = text.in(v.viewport);
    // The counter (and a page's title) ends where the leftmost toolbar button begins.
    float right = v.viewport.right - kToolbarInset;
    for (u32 i = 0; i < ui_image_viewer_button_count; ++i)
        if (const auto b = ui_image_viewer_button(v, i);
            ui_rect_has_area(b) && i != ui_image_viewer_previous && i != ui_image_viewer_next &&
            i != ui_image_viewer_retry)
            right = minimum(right, b.left - kToolbarInset);
    if (v.page && !v.fullscreen) {
        // Title over the source line, beside the kind glyph.
        const float left = v.viewport.left + kPageTextLeft, top = v.viewport.top + kPageToolbarTop - 2;
        label(v.owner.handle, v.page_info.title, {left, top, right, top + 20}, theme.text_primary,
              text_style_bold);
        label(v.owner.handle, v.page_info.source, {left, top + 20, right, top + 36}, theme.text_secondary,
              text_style_subtext);
        const auto layout = page_layout(v);
        // An audio page's title and source line under its artwork.
        const auto audio = v.page_info.kind == UiImageViewerPageKind::audio
                               ? ui_audio_page_layout(layout.player, kDefaultUiAudioControlsStyle)
                               : UiAudioPageLayout{};
        if (ui_rect_has_area(audio.art)) {
            label(v.owner.handle, v.page_info.title, audio.title, theme.text_primary, text_style_bold, ui_text_center);
            label(v.owner.handle, v.page_info.source, audio.source, theme.text_secondary, text_style_subtext,
                  ui_text_center);
        }
        // The queue's heading, then each row's title over its artist, and its duration at the end; every row alike,
        // so the current track changes no text.
        if (v.queue && ui_rect_has_area(layout.queue)) {
            const auto& queue = *v.queue;
            // The heading names the playlist (the page's title) and where the queue is in it ("6 of 24").
            const float inset = layout.queue.left + kQueuePad * 2, end = layout.queue.right - kQueuePad * 2;
            const float title_end = end - (queue.position.bytes ? 72 : 0);
            label(v.owner.handle, v.page_info.title, {inset, layout.queue.top, title_end, layout.queue.top + kQueueHeading},
                  theme.text_primary, text_style_bold, ui_text_middle);
            label(v.owner.handle, queue.position, {title_end, layout.queue.top, end, layout.queue.top + kQueueHeading},
                  theme.text_secondary, text_style_subtext, ui_text_middle | ui_text_end);
            auto list = text.in(queue_body(layout));
            for (u32 n = 0, rows = ui_image_viewer_queue_rows(v); n < rows && queue.first + n < queue.count; ++n) {
                const auto row = queue_row(v, layout, n);
                const auto& item = queue.rows[n];
                const float left = row.left + kQueuePad * 2 + kQueueThumb;
                const float right = row.right - kQueuePad - (item.duration.bytes ? kQueueDuration : 0);
                // Without an artist line the title centres in its row.
                list(v.owner.handle, item.title,
                     {left, row.top + kQueuePad, right,
                      item.detail.bytes ? row.top + kQueueRow * .5f : row.bottom - kQueuePad},
                     theme.text_primary, text_style_bold, ui_text_middle);
                list(v.owner.handle, item.detail, {left, row.top + kQueueRow * .5f, right, row.bottom - kQueuePad},
                     theme.text_secondary, text_style_subtext, ui_text_middle);
                list(v.owner.handle, item.duration, {right, row.top, row.right - kQueuePad, row.bottom},
                     theme.text_secondary, text_style_subtext, ui_text_middle | ui_text_end);
            }
        }
    } else if (v.counter_bytes) {
        // The counter shares the toolbar row's centre line.
        const UiRect row{v.viewport.left + kToolbarInset, v.viewport.top + kToolbarTop, right,
                         v.viewport.top + kToolbarTop + kToolbarTarget};
        if (ui_rect_has_area(row))
            label(v.owner.handle, Utf8View{v.counter, v.counter_bytes}, row,
                  ui_theme_color_alpha(kDefaultTheme.text_secondary, alpha));
    }
    // The caption spans the media column, so its edges match the media's.
    if (v.caption_bytes)
        label(v.owner.handle, Utf8View{v.caption, v.caption_bytes},
              {v.body.left, v.body.bottom, v.body.right, v.viewport.bottom},
              ui_theme_color_alpha(v.page ? theme.text_primary : kDefaultTheme.text_primary, alpha),
              text_style_subtext, ui_text_center);
    if ((!v.video || v.page) && v.media == UiImageViewerMedia::failed) {
        const Theme& ink = v.page && v.page_info.kind == UiImageViewerPageKind::audio ? theme : kDefaultTheme;
        const auto box = state_box(v);
        label(v.owner.handle,
              v.page ? utf8_literal("Media unavailable") : utf8_literal("This image couldn't be loaded."),
              {v.body.left, box.bottom + kToolbarGap, v.body.right, box.bottom + kToolbarGap + kStateText},
              ui_theme_color_alpha(ink.text_secondary, v.reveal), text_style_subtext, ui_text_center);
        if (const auto retry = ui_image_viewer_button(v, ui_image_viewer_retry); ui_rect_has_area(retry))
            label(v.owner.handle, utf8_literal("Retry"), retry, kDefaultTheme.text_primary, text_style_subtext,
                  ui_text_center | ui_text_middle);
    }
    return text.ok();
}
} // namespace maicord
