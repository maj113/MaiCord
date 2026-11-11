#include "media_page.hpp"
#include "downloads.hpp"
#include "image_viewer.hpp"
#include "pump.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/search/search.hpp"
#include "../chat/threads/browser.hpp"
#include "../shell/content_input.hpp"
#include "../shell/cursor.hpp"
#include "../shell/input.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/panel_cover.hpp"
#include "../shell/state.hpp"
#include "../shell/tabs.hpp"
#include "maicord/app/account/account_module_api.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/media_asset_failures.hpp"
#include "maicord/app/media/url_resolver.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/render/media/atlas.hpp"

namespace maicord::app_detail {
namespace {
// One committed page per media tab.
struct MediaPage {
    char url[kCdnMaximumTargetBytes];
    char title[96], source[96];
    MediaAssetKey key, fallback;
    UiSize pixels;
    u64 channel_id, message_id, author_id;
    u32 revision;
    // The image's zoom (relative to fit) and pan, kept while the page is hidden.
    float zoom;
    UiPoint pan;
    u16 url_bytes;
    u8 title_bytes, source_bytes, flags;
    MediaPageKind kind;
    // The media cannot load until Retry. `refreshed`: the request asked for a fresh signature once already.
    bool missing, refreshed;
};
static_assert(sizeof(MediaPage) <= 4096, "one page each");
struct MediaPages {
    MediaPage* pages[kMediaPageCapacity];
    // The shown page's viewer (page mode).
    UiImageViewer scene;
    u64 copied_until_ms;
    u8 shown;
    // Keys go to the page after a press on it (and when it shows), until a press elsewhere.
    bool focused;
};
MediaPages g_pages;

MediaPage* page_of(u8 id) noexcept {
    return id && id <= kMediaPageCapacity ? g_pages.pages[id - 1] : nullptr;
}
MediaPage* shown_page() noexcept {
    return g_pages.scene.active ? page_of(g_pages.shown) : nullptr;
}
Utf8View url_of(const MediaPage& page) noexcept {
    return {page.url, page.url_bytes};
}
// The page answers pointer and keys while it shows and nothing covers the content column.
bool front() noexcept {
    return application_media_page_shown() && application_panel_uncovered();
}
UiImageViewerPage page_info(const MediaPage& page) noexcept {
    static constexpr UiIcon glyphs[]{UiIcon::image, UiIcon::video, UiIcon::music};
    return {{page.title, page.title_bytes},
            {page.source, page.source_bytes},
            glyphs[static_cast<u32>(page.kind)],
            static_cast<UiImageViewerPageKind>(page.kind),
            page.message_id && application_channel_navigable(page.channel_id),
            (page.flags & media_page_playlist) != 0};
}
bool image_resident(const MediaPage& page) noexcept {
    auto* preview = g_app.renderer.preview_media_atlas;
    auto* primary = g_app.renderer.media_atlas;
    return (preview && (media_atlas_peek(preview, page.key).ok() || media_atlas_peek(preview, page.fallback).ok())) ||
           (primary && media_atlas_peek(primary, page.key).ok());
}
void keep_view() noexcept {
    auto* page = shown_page();
    if (!page || page->kind != MediaPageKind::image)
        return;
    page->zoom = g_pages.scene.zoom;
    page->pan = g_pages.scene.pan;
}
// Retry: forgets the failure, the backoff and a failed URL refresh, so the next pump asks once more.
void retry(MediaPage* page) noexcept {
    page->missing = page->refreshed = false;
    media_asset_failure_clear(&g_app.media_failures, page->key);
    const Utf8View url = url_of(*page);
    for (auto& entry : g_app.media_urls.entries)
        if (entry.state == MediaUrlState::failed && entry.original_bytes == url.bytes &&
            bytes_equal(entry.original, url.data, url.bytes)) {
            entry.state = MediaUrlState::queued;
            entry.attempts = 0;
            entry.retry_at_ms = 0;
        }
    application_request_frame();
}
void activate(u32 index) noexcept {
    auto* page = shown_page();
    if (!page)
        return;
    auto& scene = g_pages.scene;
    const bool embed = page->key.kind == MediaAssetKind::embed;
    const bool snapshot = (page->flags & media_page_snapshot) != 0;
    switch (index) {
    case ui_image_viewer_close:
        application_tabs_close_page(g_pages.shown);
        return;
    case ui_image_viewer_jump:
        if (page_info(*page).can_jump) {
            // Selecting the message's channel shows the channel; the page stays as its tab.
            const u64 channel = page->channel_id, message = page->message_id;
            application_media_page_hide();
            application_navigate_known_message(channel, message);
        }
        return;
    case ui_image_viewer_open_original: {
        const u64 now = clock_monotonic_milliseconds();
        const Utf8View resolved = media_url_resolve(&g_app.media_urls, url_of(*page), now,
                                                    clock_unix_milliseconds() / 1000);
        application_open_content_url(resolved.bytes ? resolved : url_of(*page));
        return;
    }
    case ui_image_viewer_save:
        if (embed)
            application_downloads_begin_embed(page->message_id, media_asset_embed_index(page->key), snapshot);
        else
            application_downloads_begin(page->message_id, page->key.id, snapshot);
        return;
    case ui_image_viewer_copy:
        if (application_image_copy(page->key, page->fallback, url_of(*page))) {
            scene.copied = true;
            ++scene.revision;
            g_pages.copied_until_ms = clock_monotonic_milliseconds() + 1500;
            application_request_frame();
        }
        return;
    case ui_image_viewer_retry:
        retry(page);
        return;
    default:
        ui_image_viewer_apply(&scene, index == ui_image_viewer_zoom_out  ? UiImageViewerCommand::zoom_out
                                      : index == ui_image_viewer_zoom_in ? UiImageViewerCommand::zoom_in
                                                                         : UiImageViewerCommand::fit);
    }
}
// A key while the page has the keyboard; false lets it fall through (Ctrl+W, the shell's shortcuts).
bool key_event(const WindowEvent& event) noexcept {
    auto& scene = g_pages.scene;
    if (input_plain_key_up(event)) {
        if (event.key.key != key::space || !scene.keyboard.pressed.manager)
            return false;
        const auto button = ui_button_keyboard_key(&scene.keyboard, &scene.widgets, UiButtonKey::space, false);
        if (button.activate)
            activate(scene.widgets.storage[button.target.slot].id - 10);
        application_request_frame();
        return true;
    }
    const u32 code = event.key.key;
    const bool repeat = input_repeated(event);
    const auto command = ui_image_viewer_key_command(
        scene, application_image_viewer_key(code),
        {application_control_down(), application_shift_down(), application_alt_down()});
    switch (command) {
    case UiImageViewerCommand::none:
    case UiImageViewerCommand::video:
    case UiImageViewerCommand::dismiss:
    case UiImageViewerCommand::exit_fullscreen:
    case UiImageViewerCommand::toggle_fullscreen:
    case UiImageViewerCommand::previous:
    case UiImageViewerCommand::next:
        return false;
    case UiImageViewerCommand::save:
    case UiImageViewerCommand::copy:
        if (!repeat)
            activate(command == UiImageViewerCommand::save ? ui_image_viewer_save : ui_image_viewer_copy);
        break;
    case UiImageViewerCommand::focus_next:
    case UiImageViewerCommand::focus_previous: {
        const auto target = ui_widget_manager_next_focus(scene.widgets, scene.widgets.focused,
                                                         command == UiImageViewerCommand::focus_previous, true);
        ui_widget_manager_focus(&scene.widgets, target);
        scene.focus = static_cast<u8>(scene.widgets.storage[target.slot].id - 10);
        scene.keyboard_focus = true;
        ++scene.revision;
        break;
    }
    case UiImageViewerCommand::activate: {
        const auto button = ui_button_keyboard_key(&scene.keyboard, &scene.widgets,
                                                   code == key::enter ? UiButtonKey::enter : UiButtonKey::space,
                                                   true, repeat);
        if (button.activate)
            activate(scene.widgets.storage[button.target.slot].id - 10);
        break;
    }
    default:
        ui_image_viewer_apply(&scene, command);
        break;
    }
    application_request_frame();
    return true;
}
} // namespace

u8 application_media_page_open(const MediaPageSource& source) noexcept {
    if (static_cast<u32>(source.kind) > static_cast<u32>(MediaPageKind::audio) || !source.url.bytes ||
        source.url.bytes > kCdnMaximumTargetBytes ||
        (source.kind == MediaPageKind::image && (!media_asset_key_valid(source.key) || !ui_size_valid(source.pixels))))
        return 0;
    u8 free = 0;
    for (u8 id = kMediaPageCapacity; id; --id) {
        const auto* page = g_pages.pages[id - 1];
        if (!page)
            free = id;
        else if (media_asset_key_equal(page->key, source.key) && page->url_bytes == source.url.bytes &&
                 bytes_equal(page->url, source.url.data, source.url.bytes))
            return id;
    }
    auto* page = free ? static_cast<MediaPage*>(commit_pages(sizeof(MediaPage))) : nullptr;
    if (!page)
        return 0;
    copy_bytes(page->url, source.url.data, source.url.bytes);
    page->url_bytes = static_cast<u16>(source.url.bytes);
    page->title_bytes = static_cast<u8>(utf8_truncated_length(source.title, sizeof(page->title)));
    copy_bytes(page->title, source.title.data, page->title_bytes);
    page->source_bytes = static_cast<u8>(utf8_truncated_length(source.source, sizeof(page->source)));
    copy_bytes(page->source, source.source.data, page->source_bytes);
    page->key = source.key;
    page->fallback = source.fallback;
    page->pixels = ui_size_valid(source.pixels) ? source.pixels : UiSize{16, 9};
    page->channel_id = source.channel_id;
    page->message_id = source.message_id;
    page->author_id = source.author_id;
    page->revision = source.revision;
    page->flags = source.flags;
    page->kind = source.kind;
    page->zoom = 1;
    g_pages.pages[free - 1] = page;
    return free;
}
bool application_media_page_show(u8 id) noexcept {
    auto* page = page_of(id);
    if (!page || !g_app.account_ui || g_app.login.visible)
        return false;
    if (g_pages.shown == id && g_pages.scene.active)
        return true;
    keep_view();
    // The page replaces whatever covered the chat column; the composer gives up the keyboard.
    application_image_viewer_close();
    application_search_close_panel();
    application_thread_browser_close();
    application_composer_text_input_suspend();
    message_action_bar_close(&g_app.message_action_bar);
    auto& scene = g_pages.scene;
    ui_image_viewer_set_dpi(&scene, g_app.renderer.dpi);
    if (!ui_image_viewer_open(&scene, application_media_page_rect(), page->pixels) ||
        !ui_image_viewer_set_page(&scene, page_info(*page))) {
        scene.active = false;
        return false;
    }
    if (page->kind == MediaPageKind::image)
        ui_image_viewer_set_view(&scene, page->zoom, page->pan);
    g_pages.shown = id;
    g_pages.focused = true;
    application_repaint();
    return true;
}
void application_media_page_hide() noexcept {
    auto& scene = g_pages.scene;
    if (!scene.active)
        return;
    keep_view();
    const bool capture = scene.dragging || scene.widgets.captured.generation;
    scene.dragging = false;
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
    ui_button_keyboard_cancel(&scene.keyboard);
    if (capture)
        application_release_capture();
    scene.active = false;
    ++scene.revision;
    g_pages.shown = 0;
    g_pages.focused = false;
    g_app.renderer.media_page = nullptr;
    application_repaint();
}
void application_media_page_close(u8 id) noexcept {
    auto* page = page_of(id);
    if (!page)
        return;
    if (g_pages.shown == id)
        application_media_page_hide();
    pages_release(page);
    g_pages.pages[id - 1] = nullptr;
}
void application_media_page_reset() noexcept {
    for (u8 id = 1; id <= kMediaPageCapacity; ++id)
        application_media_page_close(id);
}
u8 application_media_page_shown() noexcept {
    return shown_page() && !g_app.image_viewer.scene.active ? g_pages.shown : 0;
}
MediaPageKind application_media_page_kind(u8 id) noexcept {
    const auto* page = page_of(id);
    return page ? page->kind : MediaPageKind::image;
}
Utf8View application_media_page_title(u8 id) noexcept {
    const auto* page = page_of(id);
    return page ? Utf8View{page->title, page->title_bytes} : Utf8View{};
}
bool application_media_page_missing(u8 id) noexcept {
    const auto* page = page_of(id);
    return page && page->missing;
}
UiRect application_media_page_rect() noexcept {
    return ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
}
const UiImageViewer* application_media_page_viewer() noexcept {
    return application_media_page_shown() ? &g_pages.scene : nullptr;
}
UiImageViewer* application_media_page_spinner() noexcept {
    return application_media_page_shown() && ui_image_viewer_spinner_visible(g_pages.scene) ? &g_pages.scene
                                                                                            : nullptr;
}
bool application_media_page_key_current(const MediaAssetKey& key) noexcept {
    const auto* page = application_media_page_shown() ? shown_page() : nullptr;
    return page && !page->missing && (media_asset_key_equal(key, page->key) || media_asset_key_equal(key, page->fallback));
}

bool application_media_page_synchronize() noexcept {
    auto& renderer = g_app.renderer;
    auto& scene = g_pages.scene;
    auto* page = shown_page();
    if (page && (!g_app.account_ui || g_app.login.visible)) {
        application_media_page_hide();
        page = nullptr;
    }
    if (!page || !ui_image_viewer_set_dpi(&scene, renderer.dpi) ||
        !ui_image_viewer_layout(&scene, application_media_page_rect())) {
        // Too small a content column draws no page until it grows back.
        renderer.media_page = nullptr;
        return !g_app.account_ui || !renderer.interaction_text_media_page_revision ||
               renderer_rebuild_content_text(&renderer, application_theme());
    }
    ui_image_viewer_set_page(&scene, page_info(*page));
    const bool image = page->kind == MediaPageKind::image;
    ui_image_viewer_set_media(&scene, page->missing                  ? UiImageViewerMedia::failed
                                      : !image || image_resident(*page) ? UiImageViewerMedia::ready
                                                                       : UiImageViewerMedia::loading);
    ui_image_viewer_set_actions(&scene, ui_image_viewer_action_open | (image ? ui_image_viewer_action_copy : 0));
    const bool snapshot = (page->flags & media_page_snapshot) != 0;
    ui_image_viewer_set_save_available(
        &scene, page->message_id && (page->key.kind == MediaAssetKind::embed
                                         ? application_downloads_embed_available(
                                               page->message_id, media_asset_embed_index(page->key), snapshot)
                                         : page->key.kind == MediaAssetKind::attachment &&
                                               application_downloads_available(page->message_id, page->key.id,
                                                                               snapshot)));
    if (scene.copied && clock_monotonic_milliseconds() >= g_pages.copied_until_ms) {
        scene.copied = false;
        ++scene.revision;
    } else if (scene.copied) {
        application_request_frame();
    }
    const bool focus = g_app.keyboard_focus_visible && g_pages.focused;
    if (scene.keyboard_focus != focus) {
        scene.keyboard_focus = focus;
        ++scene.revision;
    }
    renderer.media_page = &scene;
    renderer.media_page_key = page->key;
    renderer.media_page_fallback = page->fallback;
    return renderer.interaction_text_media_page_revision == scene.revision ||
           renderer_rebuild_content_text(&renderer, application_theme());
}

bool application_media_page_window_event(const WindowEvent& event) noexcept {
    auto& scene = g_pages.scene;
    if (!front())
        return false;
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event)) {
        scene.dragging = false;
        if (ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {}).changed)
            ++scene.revision;
        return false;
    }
    if (event.type == EventType::pointer_leave) {
        if (ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, {-1, -1}).changed) {
            ++scene.revision;
            application_request_frame();
        }
        return false;
    }
    if (input_plain_key(event))
        return g_pages.focused && key_event(event);
    if (event.type == EventType::cursor_query) {
        UiPoint cursor{};
        if (!client_cursor_dip(event, &cursor) || !ui_rect_contains(scene.viewport, cursor.x, cursor.y))
            return false;
        application_set_cursor(UiCursor::arrow);
        return true;
    }
    if (event.type == EventType::context_menu) {
        const UiPoint point = client_context_dip(event);
        return (event.flags & ev_keyboard) ? g_pages.focused : ui_rect_contains(scene.viewport, point.x, point.y);
    }
    if (!input_pointer(event))
        return false;
    const UiPoint point = event.type == EventType::wheel ? client_wheel_dip(event)
                                                          : UiPoint{client_dip_x(event), client_dip_y(event)};
    const bool inside = ui_rect_contains(scene.viewport, point.x, point.y);
    const bool captured = scene.dragging || scene.widgets.captured.generation;
    if (input_press(event) || input_double(event) || input_down(event, PointerButton::right) ||
        input_down(event, PointerButton::middle))
        g_pages.focused = inside;
    if (!inside && !captured)
        return false;
    const bool image = !scene.video;
    const bool over_image = image && ui_rect_contains(scene.image, point.x, point.y) &&
                            ui_rect_contains(scene.body, point.x, point.y);
    if (input_double(event) && over_image &&
        !ui_widget_manager_hit_test(scene.widgets, point.x, point.y).generation) {
        ui_image_viewer_toggle_zoom(&scene, point);
        scene.dragging = true;
        scene.pointer = point;
        window_capture_pointer(g_app.window);
        application_request_frame();
        return true;
    }
    if (input_down(event)) {
        scene.keyboard_focus = false;
        if (const auto button = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, point);
            button.handled) {
            scene.focus = static_cast<u8>(scene.widgets.storage[button.target.slot].id - 10);
            ++scene.revision;
            window_capture_pointer(g_app.window);
        } else if (over_image) {
            scene.dragging = true;
            scene.pointer = point;
            window_capture_pointer(g_app.window);
        }
        application_request_frame();
        return true;
    }
    if (input_move(event)) {
        if (ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, point).changed)
            ++scene.revision;
        if (scene.dragging) {
            ui_image_viewer_pan(&scene, {point.x - scene.pointer.x, point.y - scene.pointer.y});
            scene.pointer = point;
        }
        application_request_frame();
        return true;
    }
    if (input_release(event)) {
        const auto button = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, point);
        scene.dragging = false;
        if (captured)
            application_release_capture();
        if (button.changed)
            ++scene.revision;
        if (button.activate)
            activate(scene.widgets.storage[button.target.slot].id - 10);
        application_request_frame();
        return true;
    }
    if (input_vertical_wheel(event) && image) {
        ui_image_viewer_wheel(&scene, static_cast<i16>(event.wheel.dy120), point);
        application_request_frame();
    }
    // The page swallows the rest of what happens over it (other buttons, horizontal wheels).
    return true;
}

bool application_media_page_pump(ApplicationMediaPump& pump, bool* stop) noexcept {
    *stop = false;
    auto* page = application_media_page_shown() ? shown_page() : nullptr;
    // Video and audio pages play through the session (phase 3); a page that cannot load waits for Retry.
    if (!page || page->kind != MediaPageKind::image || page->missing || pump.visibility_only || image_resident(*page) ||
        pump.asset_backed_off(page->key))
        return true;
    auto& application = pump.application;
    const Utf8View source = url_of(*page);
    const Utf8View url =
        media_url_resolve(&application.media_urls, source, pump.now_ms, pump.now_seconds, page->refreshed);
    if (!url.bytes) {
        // Refreshing: the REST pump services the resolver. A refresh that failed for good leaves the page
        // unavailable.
        if (media_url_resolution_state(application.media_urls, source) == MediaUrlState::failed) {
            page->missing = true;
            application_request_frame();
        } else {
            pump.result.history_requested =
                pump.result.history_requested || media_url_next(&application.media_urls, pump.now_ms) != nullptr;
        }
        return true;
    }
    const bool embed = page->key.kind == MediaAssetKind::embed;
    const auto format = embed ? media_asset_embed_format(page->key) : media_asset_attachment_format(page->key);
    const u16 cap = media_preview_extent_cap(format, (page->key.flags & media_asset_flag_animated) != 0);
    const float extent = static_cast<float>(cap) * 96.0f / static_cast<float>(maximum(application.renderer.dpi, 1u));
    const auto proxy = media_presentation_request(static_cast<u32>(page->pixels.width),
                                                  static_cast<u32>(page->pixels.height), extent, extent,
                                                  application.renderer.dpi, cap);
    const AccountMediaAttachmentRequest request{url,
                                                page->key,
                                                page->message_id,
                                                page->revision,
                                                proxy.width,
                                                proxy.height,
                                                pump.cdn_image_type(format),
                                                {0, {0, 0}},
                                                application.renderer.graphics.device};
    const auto requested =
        account_module_media_attachment_request(application.account.instance, &request, pump.now_ms, pump.now_seconds);
    const bool stale = requested.state == AccountMediaRequestState::refresh_required ||
                       (requested.state == AccountMediaRequestState::failed &&
                        requested.error == AccountMediaError::target);
    // A stale signature asks for one forced refresh; a second, or a request that cannot load, ends in the
    // unavailable state.
    if (stale || requested.state == AccountMediaRequestState::failed ||
        requested.state == AccountMediaRequestState::invalid ||
        requested.state == AccountMediaRequestState::unsupported) {
        if (stale && !page->refreshed)
            page->refreshed = true;
        else
            page->missing = true;
        application_request_frame();
        return true;
    }
    return pump.settle_media_request(requested, page->key, stop, true);
}
} // namespace maicord::app_detail
