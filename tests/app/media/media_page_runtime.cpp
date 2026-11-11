// Media pages end to end over the real page store, viewer page mode and input routing: open, limit, show and
// hide (zoom and pan kept), pointer routing beside the sidebar and member list, the request through the page's
// own {url, key}, the forced signature refresh, and the unavailable state that no pump asks again until Retry.
#include "../../../src/app/media/media_page.cpp"
#include "../../support/test_output.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "../../support/window_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/ui/core/scene.hpp"

namespace maicord::media_page_runtime_test {
using namespace app_detail;
u32 failures;
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
// What the stand-ins below saw and answer.
u32 requests, resolves, forced_resolves, settles, closed_tabs, navigations, opened_urls, viewer_closes;
// The player's stand-ins (video.cpp): what the page asked of it and what it answers.
u32 video_shows, player_toggles, player_keys;
u8 closed_player;
u16 jumped_track;
bool window_fullscreen;
VideoPagePlayback playback_state = VideoPagePlayback::none;
VideoPageQueue queue_state;
u64 navigated_message;
MediaUrlState url_state = MediaUrlState::empty;
bool url_ready = true;
AccountMediaRequestState answer = AccountMediaRequestState::queued;
AccountMediaError answer_error = AccountMediaError::none;
const char kUrl[] = "https://cdn.discordapp.com/attachments/70/900/cat.png?ex=1&is=1&hm=00";

MediaPageSource image_source(u64 attachment) noexcept {
    MediaPageSource source{};
    source.kind = MediaPageKind::image;
    source.url = {kUrl, sizeof(kUrl) - 1};
    source.title = utf8_literal("cat.png");
    source.source = utf8_literal("#general \xC2\xB7 Fixture \xC2\xB7 Mai");
    source.key = media_asset_attachment(attachment, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
    source.fallback = media_asset_attachment(attachment, MediaAttachmentFormat::png, MediaResolutionTier::px512);
    source.pixels = {1600, 900};
    source.channel_id = 70;
    source.message_id = 900;
    source.revision = 1;
    return source;
}
u32 pump_once() noexcept {
    const u32 before = requests;
    ApplicationMediaPump pump{g_app};
    pump.pass(1000, 2000, 3000, nullptr);
    bool stop = false;
    check(application_media_page_pump(pump, &stop), "pump: the page request never fails the pump");
    return requests - before;
}
bool dispatch(const WindowEvent& event) noexcept {
    return application_media_page_window_event(event);
}

void run() noexcept {
    g_app.window = window_test::open();
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui && ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
              ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                           kGuildChannelShellLayoutMetrics, 1280, 720),
          "fixture: a signed-in shell laid out at 1280 x 720");
    if (!g_app.window || !g_app.account_ui)
        return;
    g_app.renderer.dpi = 96;
    g_app.renderer.width = 1280;
    g_app.renderer.height = 720;
    const UiRect content = application_media_page_rect();
    check(ui_rect_has_area(content) && content.left > 0, "fixture: the content column lies right of the sidebar");

    // Open, duplicates and the limit.
    const u8 first = application_media_page_open(image_source(900));
    check(first == 1 && application_media_page_open(image_source(900)) == first,
          "open: the same media opens once");
    MediaPageSource invalid = image_source(901);
    invalid.url = {};
    check(!application_media_page_open(invalid), "open: a source without a URL is refused");
    u8 last = 0;
    for (u32 i = 1; i < kMediaPageCapacity; ++i)
        last = application_media_page_open(image_source(901 + i));
    check(last == kMediaPageCapacity && !application_media_page_open(image_source(990)),
          "open: eight pages, the ninth is refused");
    application_media_page_close(last);
    check(application_media_page_open(image_source(990)) == last, "open: a closed page's slot is reused");
    check(utf8_equal(application_media_page_title(first), utf8_literal("cat.png")) &&
              application_media_page_kind(first) == MediaPageKind::image && !application_media_page_shown(),
          "open: the page copies its title and does not show by itself");

    // Show, pointer routing, zoom and pan kept across hide/show.
    check(application_media_page_show(first) && application_media_page_shown() == first && viewer_closes == 1 &&
              application_media_page_synchronize() && g_app.renderer.media_page == application_media_page_viewer() &&
              g_app.renderer.media_page && g_app.renderer.media_page->page &&
              ui_rect_equal(g_app.renderer.media_page->viewport, content),
          "show: the page fills the content column, closing a message viewer first");
    const auto& scene = *application_media_page_viewer();
    check(scene.media == UiImageViewerMedia::loading, "show: a picture not yet resident loads");
    check(!dispatch(window_test::pointer_down(content.left - 20, content.top + 100)) &&
              !dispatch(window_test::pointer_up(content.left - 20, content.top + 100)),
          "input: a press on the sidebar routes on");
    const UiPoint middle{(scene.body.left + scene.body.right) * .5f, (scene.body.top + scene.body.bottom) * .5f};
    check(dispatch(window_test::wheel(middle.x, middle.y, 240)) && scene.zoom > 1,
          "input: the wheel over the page zooms its picture");
    check(dispatch(window_test::pointer_down(middle.x, middle.y)) && scene.dragging &&
              dispatch(window_test::pointer_move(middle.x + 30, middle.y + 10, 1)) &&
              dispatch(window_test::pointer_up(middle.x + 30, middle.y + 10)) && !scene.dragging,
          "input: a drag over the picture pans it and ends on release");
    const float before_key = scene.zoom;
    check(dispatch(window_test::key_down(key::add)) && scene.zoom > before_key &&
              !dispatch(window_test::key_down(key::escape)),
          "input: the focused page zooms on + and lets Esc fall through");
    check(!dispatch(window_test::pointer_down(content.left - 20, content.top + 100)) &&
              !dispatch(window_test::key_down(key::add)),
          "input: after a press elsewhere the page no longer takes keys");
    check(dispatch(window_test::pointer_down(middle.x, middle.y)) &&
              dispatch(window_test::pointer_up(middle.x, middle.y)),
          "input: a press on the page gives it the keys back");
    const float zoom = scene.zoom;
    const UiPoint pan = scene.pan;
    check(pan.x != 0, "input: the drag moved the picture");
    check(!dispatch(window_test::pointer_down(content.right + 20, content.top + 100)),
          "input: a press on the member list routes on");
    application_media_page_hide();
    check(!application_media_page_shown() && !g_app.renderer.media_page && application_media_page_synchronize(),
          "hide: the page leaves the content column and the renderer");
    check(application_media_page_show(first) && scene.zoom == zoom && scene.pan.x == pan.x && scene.pan.y == pan.y,
          "show: the picture returns at its zoom and pan");

    // Header actions.
    const auto press = [](u32 button) noexcept {
        const auto rect = ui_image_viewer_button(*application_media_page_viewer(), button);
        const UiPoint at{(rect.left + rect.right) * .5f, (rect.top + rect.bottom) * .5f};
        return ui_rect_has_area(rect) && dispatch(window_test::pointer_down(at.x, at.y)) &&
               dispatch(window_test::pointer_up(at.x, at.y));
    };
    check(application_media_page_synchronize() && press(ui_image_viewer_open_original) && opened_urls == 1,
          "header: Open original opens the page's own URL");
    check(press(ui_image_viewer_close) && closed_tabs == 1, "header: Close tab asks the tabs to close the page");
    check(press(ui_image_viewer_jump) && navigations == 1 && navigated_message == 900 &&
              !application_media_page_shown(),
          "header: Jump to message hides the page and selects its message");

    // The request through the page's own {url, key}.
    const u32 opened_resolves = resolves;
    check(application_media_page_show(first) && pump_once() == 1 && resolves == opened_resolves + 1 &&
              forced_resolves == 0 && settles == 1,
          "pump: the shown image asks once through its resolved URL");
    application_media_page_hide();
    check(pump_once() == 0, "pump: a hidden page asks nothing");
    application_media_page_show(first);
    answer = AccountMediaRequestState::refresh_required;
    check(pump_once() == 1 && !application_media_page_missing(first) && pump_once() == 1 && forced_resolves == 1 &&
              application_media_page_missing(first),
          "pump: a stale signature forces one refresh; stale again, the page is unavailable");
    const u32 resolved = resolves;
    check(pump_once() == 0 && pump_once() == 0 && resolves == resolved,
          "pump: an unavailable page is not asked again (no polling)");
    check(application_media_page_synchronize() && scene.media == UiImageViewerMedia::failed &&
              ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_retry)),
          "unavailable: the page shows Media unavailable with Retry");
    answer = AccountMediaRequestState::queued;
    check(press(ui_image_viewer_retry) && !application_media_page_missing(first) && pump_once() == 1,
          "unavailable: Retry asks once more");
    // A URL refresh that failed for good.
    url_ready = false;
    url_state = MediaUrlState::failed;
    check(pump_once() == 0 && application_media_page_missing(first) && pump_once() == 0,
          "pump: a refresh that fails marks the page unavailable without a request or a retry loop");
    url_ready = true;
    url_state = MediaUrlState::empty;
    answer = AccountMediaRequestState::failed;
    answer_error = AccountMediaError::decode;
    check(application_media_page_synchronize() && press(ui_image_viewer_retry) && pump_once() == 1 &&
              application_media_page_missing(first) && pump_once() == 0,
          "pump: a request that cannot load ends in the unavailable state");
    answer = AccountMediaRequestState::queued;

    // A playlist page hosts the player: a play button until it starts, the queue (the current track in view with its
    // duration, titles as the playlist knows them), row jumps, the wheel, full screen; closing it closes its session.
    MediaPageSource playlist = image_source(950);
    playlist.kind = MediaPageKind::video;
    playlist.flags = media_page_embed | media_page_playlist;
    application_media_page_close(last);
    const u8 player = application_media_page_open(playlist);
    queue_state = {40, 6, 125'000};
    check(player && application_media_page_show(player) && video_shows == 1 && application_media_page_synchronize() &&
              application_media_page_synchronize(),
          "player: a video page shows and asks the player to take it");
    const auto& page = *application_media_page_viewer();
    const auto layout = ui_image_viewer_page_layout(page);
    const u32 rows = ui_image_viewer_queue_rows(page);
    check(page.idle && page.queue && ui_rect_has_area(layout.queue) && rows >= 3,
          "player: a playlist page that has not started shows its play button and its queue");
    check(page.queue && page.queue->count == 40 && page.queue->current == 6 && page.queue->first == 4,
          "player: the queue keeps the current track in view, one row of the past above it");
    check(page.queue && utf8_equal(page.queue->titles[0], utf8_literal("Fixture song")) &&
              utf8_equal(page.queue->titles[1], utf8_literal("Track 6")),
          "player: queue rows are titled as the playlist knows them, numbered until then");
    check(page.queue && utf8_equal(page.queue->duration, utf8_literal("2:05")),
          "player: the current track shows its duration");
    const UiPoint stage{(layout.player.left + layout.player.right) * .5f,
                        (layout.player.top + layout.player.bottom) * .5f};
    check(dispatch(window_test::pointer_down(stage.x, stage.y)) && player_toggles == 1 &&
              dispatch(window_test::pointer_up(stage.x, stage.y)),
          "player: a press on a stage that has not started starts it");
    const UiPoint row{layout.queue.left + 60, layout.queue.top + 40 + 56 * 2.5f};
    check(ui_image_viewer_queue_hit(page, row) == 7 && dispatch(window_test::pointer_move(row.x, row.y, 0)) &&
              page.queue->hovered == 7 && dispatch(window_test::pointer_down(row.x, row.y)) && jumped_track == 7,
          "player: a queue row lights under the pointer and jumps to its track");
    dispatch(window_test::pointer_up(row.x, row.y));
    check(dispatch(window_test::wheel(row.x, row.y, -120)) && application_media_page_synchronize() &&
              page.queue->first == 7,
          "player: the wheel scrolls the queue");
    check(dispatch(window_test::key_down(key::space)) && player_keys == 1, "player: the page hands the player its keys");
    application_media_page_set_fullscreen(true);
    check(window_fullscreen && application_media_page_synchronize() && page.fullscreen &&
              ui_rect_equal(page.viewport, application_viewport_dips()) &&
              !ui_rect_has_area(ui_image_viewer_page_layout(page).queue) &&
              ui_rect_equal(ui_image_viewer_page_layout(page).player, page.viewport),
          "player: full screen makes the page the whole window, all player");
    check(dispatch(window_test::key_down(key::escape)) && !window_fullscreen && application_media_page_synchronize() &&
              ui_rect_equal(page.viewport, content),
          "player: Esc leaves full screen");
    application_media_page_close(player);
    check(closed_player == player, "player: closing the page closes its session");
    application_media_page_show(first);

    // A message viewer over the page, close and reset.
    g_app.image_viewer.scene.active = true;
    check(!application_media_page_shown() && !application_media_page_viewer() &&
              !dispatch(window_test::pointer_down(middle.x, middle.y)),
          "cover: a message viewer over the page takes it out of the strip and the input");
    g_app.image_viewer.scene.active = false;
    application_media_page_close(first);
    check(!application_media_page_shown() && !application_media_page_title(first).bytes,
          "close: closing the shown page hides it and frees it");
    application_media_page_reset();
    check(!application_media_page_title(2).bytes && application_media_page_open(image_source(900)) == 1,
          "reset: every page closes");
    application_media_page_reset();
    ui_scene_release(&g_app.shell_scene);
    window_test::close();
    g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace media_page_runtime_test

namespace maicord {
using namespace media_page_runtime_test;
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    return true;
}
// The playlist knows the titles of its odd tracks.
AccountMediaRequestResult account_module_collection_title(void*, const AccountMediaCollectionTitleRequest* request,
                                                           MediaTrackTitle* title) noexcept {
    AccountMediaRequestResult result{};
    result.state = AccountMediaRequestState::pending;
    if (request->track & 1) {
        copy_bytes(title->text, "Fixture song", 12);
        title->bytes = 12;
        result.state = AccountMediaRequestState::ready;
    }
    return result;
}
Utf8View media_url_resolve(MediaUrlResolver*, Utf8View source, u64, u64, bool force_refresh) noexcept {
    ++resolves;
    forced_resolves += force_refresh;
    return url_ready ? source : Utf8View{};
}
MediaUrlState media_url_resolution_state(const MediaUrlResolver&, Utf8View) noexcept {
    return url_state;
}
MediaUrlEntry* media_url_next(MediaUrlResolver*, u64) noexcept {
    return nullptr;
}
AccountMediaRequestResult account_module_media_attachment_request(void*, const AccountMediaAttachmentRequest* request,
                                                                  u64, u64) noexcept {
    ++requests;
    AccountMediaRequestResult result{};
    result.state = request && request->signed_url.bytes == sizeof(kUrl) - 1 && request->proxy_width ? answer
                                                                                                   : AccountMediaRequestState::invalid;
    result.error = answer_error;
    return result;
}
namespace app_detail {
bool ApplicationMediaPump::asset_backed_off(const MediaAssetKey&) const noexcept {
    return false;
}
CdnImageType ApplicationMediaPump::cdn_image_type(MediaAttachmentFormat) noexcept {
    return {};
}
bool ApplicationMediaPump::settle_media_request(const AccountMediaRequestResult&, const MediaAssetKey&, bool*,
                                                bool) noexcept {
    ++settles;
    return true;
}
bool application_tabs_close_page(u8 id) noexcept {
    ++closed_tabs;
    return id != 0;
}
void application_video_page_show(u8) noexcept {
    ++video_shows;
}
void application_video_page_close(u8 id) noexcept {
    closed_player = id;
}
VideoPagePlayback application_video_page_playback(u8) noexcept {
    return playback_state;
}
bool application_video_page_toggle(u8) noexcept {
    ++player_toggles;
    return true;
}
bool application_video_page_key(u8, u32 code, bool, bool) noexcept {
    player_keys += code == key::space;
    return code == key::space;
}
bool application_video_page_pointer(u8, VideoPointer, UiPoint) noexcept {
    return false;
}
VideoPageQueue application_video_page_queue(u8) noexcept {
    return queue_state;
}
bool application_video_page_track(u8, u16 track) noexcept {
    jumped_track = track;
    return true;
}
bool application_video_viewer_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1280, 720};
}
bool application_window_set_fullscreen(bool on) noexcept {
    window_fullscreen = on;
    return true;
}
bool application_navigate_known_message(u64, u64 message_id) noexcept {
    ++navigations;
    navigated_message = message_id;
    return true;
}
bool application_channel_navigable(u64 channel_id) noexcept {
    return channel_id == 70;
}
bool application_open_content_url(const Utf8View& url) noexcept {
    opened_urls += url.bytes == sizeof(kUrl) - 1;
    return true;
}
void application_image_viewer_close() noexcept {
    ++viewer_closes;
}
UiImageViewerKey application_image_viewer_key(u32 code) noexcept {
    return code == key::add ? UiImageViewerKey::plus : code == key::escape ? UiImageViewerKey::escape
                                                                             : UiImageViewerKey::other;
}
bool application_image_copy(const MediaAssetKey&, const MediaAssetKey&, Utf8View) noexcept {
    return false;
}
bool application_search_close_panel() noexcept {
    return true;
}
void application_thread_browser_close() noexcept {}
void application_composer_text_input_suspend() noexcept {}
bool application_downloads_available(u64, u64, bool) noexcept {
    return false;
}
bool application_downloads_begin(u64, u64, bool) noexcept {
    return false;
}
bool application_downloads_embed_available(u64, u32, bool) noexcept {
    return false;
}
bool application_downloads_begin_embed(u64, u32, bool) noexcept {
    return false;
}
bool application_settings_active() noexcept {
    return false;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_media_page_runtime_entry() noexcept {
    maicord::media_page_runtime_test::run();
    ExitProcess(maicord::media_page_runtime_test::failures ? 1 : 0);
}
