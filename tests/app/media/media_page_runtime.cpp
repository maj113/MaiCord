// Media pages end to end over the real page store, viewer page mode and input routing: open, limit, show and
// hide (zoom and pan kept), pointer routing beside the sidebar and member list, the request through the page's
// own {url, key}, the forced signature refresh, the unavailable state that no pump asks again until Retry, and an
// animated picture that keeps playing while its page shows.
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
u32 requests, resolves, forced_resolves, settles, closed_tabs, navigations, opened_urls, opened_links, viewer_closes;
// The queue's thumbnail requests, and whether each named its track's i.ytimg.com picture under its key.
u32 thumb_requests;
bool thumb_url_ok = true;
// The media worker's playlist lane is busy (resolving a track): every title lookup answers busy.
bool titles_busy;
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
bool copy_ok, downloads_ok;
AccountMediaRequestState answer = AccountMediaRequestState::queued;
AccountMediaError answer_error = AccountMediaError::none;
const char kUrl[] = "https://cdn.discordapp.com/attachments/70/900/cat.png?ex=1&is=1&hm=00";
const char kSong[] = "https://cdn.discordapp.com/attachments/70/960/song.mp3?ex=1&is=1&hm=00";
const char kLink[] = "https://www.youtube.com/watch?v=abcdefghij1&list=PL0";

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
// An animated picture (a GIF here; a GIF embed's looping video and an animated WebP go the same way) plays on its page
// as on its card: the page asks for its own tier while the card's tier shows (and plays) meanwhile, every visibility
// scan observes its timeline so its frames come due while the chat under the page is hidden, a resident frame whose
// timeline went is asked again, and with GIFs set never to play the page holds its first frame without asking.
MediaAtlasEntry atlas_entries[8];
u32 atlas_buckets[16];
GlyphAtlasEntry atlas_allocations[8];
GlyphAtlasRow atlas_rows[8];
AnimationTimelineEntry timeline_entries[4];
u32 timeline_buckets[8];
// The page pump in a visibility scan (as the frame's scan runs it), committed at `now_us`; the requests it made.
u32 visibility_scan(u64 now_us) noexcept {
    const u32 before = requests;
    animation_timeline_begin_visibility(&g_app.animations.timeline);
    ApplicationMediaPump pump{g_app};
    pump.pass(1000, 2000, now_us, nullptr);
    pump.visibility_only = true;
    bool stop = false;
    check(application_media_page_pump(pump, &stop), "animation: a visibility scan never fails the pump");
    animation_timeline_end_visibility(&g_app.animations.timeline, now_us);
    return requests - before;
}
// One frame advance comes due by `now_us`.
bool frame_due(u64 now_us) noexcept {
    AnimationFrameAdvance advance{};
    const auto due = animation_timeline_collect_due(&g_app.animations.timeline, now_us, {&advance, 1});
    return due.ok() && due.advances == 1;
}
bool publish(const MediaAssetKey& key, bool visible) noexcept {
    animation_timeline_reset(&g_app.animations.timeline);
    return animation_timeline_publish(&g_app.animations.timeline, {key, 1000, 1, 3, 0, visible}, 100).ok();
}
void animation_tests(u8 slot) noexcept {
    MediaAtlas atlas{};
    MediaPageSource gif = image_source(970);
    gif.title = utf8_literal("wave.gif");
    gif.key = media_asset_attachment(970, MediaAttachmentFormat::gif, MediaResolutionTier::px2048);
    gif.fallback = media_asset_attachment(970, MediaAttachmentFormat::gif, MediaResolutionTier::px512);
    application_media_page_close(slot);
    check(media_atlas_initialize(&atlas, 256, 256, 1, {atlas_entries, 8}, {atlas_buckets, 16},
                                 {atlas_allocations, 8}, {atlas_rows, 8}) &&
              animation_timeline_initialize(&g_app.animations.timeline, {timeline_entries, 4}, {timeline_buckets, 8}) &&
              application_media_page_open(gif) == slot && application_media_page_show(slot) &&
              (gif.key.flags & media_asset_flag_animated),
          "animation: a GIF page shows, its key marked animated");
    g_app.renderer.preview_media_atlas = &atlas;
    // The card's tier is resident and plays while the page's own tier loads.
    check(media_atlas_allocate(&atlas, gif.fallback, 16, 16).ok() && application_media_page_synchronize() &&
              application_media_page_viewer()->media == UiImageViewerMedia::ready && pump_once() == 1,
          "animation: the card's tier shows meanwhile and the page still asks for its own tier");
    check(publish(gif.fallback, false) && visibility_scan(3000) == 0 && frame_due(5000),
          "animation: the card's tier the page shows meanwhile plays");
    // The page's own tier: resident, observed in every visibility scan, so its frames come due.
    check(media_atlas_allocate(&atlas, gif.key, 16, 16).ok() && publish(gif.key, false) && visibility_scan(3000) == 0 &&
              frame_due(5000),
          "animation: the page's own tier plays while the page shows (its timeline observed by the scan)");
    check(publish(gif.key, false) && pump_once() == 0, "animation: a resident playing picture asks nothing");
    application_media_page_hide();
    check(publish(gif.key, true) && visibility_scan(3000) == 0 && !frame_due(5000),
          "animation: a hidden page's picture pauses");
    // Its timeline went while hidden: the resident first frame is asked again, which publishes it anew.
    animation_timeline_reset(&g_app.animations.timeline);
    check(application_media_page_show(slot) && pump_once() == 1,
          "animation: a resident frame without its timeline is asked again to play");
    MessageRichDisplay display = message_rich_display();
    display.gifs = MessageMediaAnimation::never;
    message_rich_set_display(display);
    check(pump_once() == 0 && publish(gif.key, false) && visibility_scan(3000) == 0 && !frame_due(5000),
          "animation: with GIFs never playing the page holds its first frame and asks nothing");
    display.gifs = MessageMediaAnimation::always;
    message_rich_set_display(display);
    animation_timeline_reset(&g_app.animations.timeline);
    g_app.renderer.preview_media_atlas = nullptr;
    application_media_page_close(slot);
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
    downloads_ok = true;

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
    check(ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_zoom_in)) &&
              ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_copy)) &&
              ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_save)),
          "show: a page with its picture offers zoom, Copy and Save");
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
          "header: Open original opens an attachment page's own URL");
    check(press(ui_image_viewer_close) && closed_tabs == 1, "header: Close tab asks the tabs to close the page");
    check(press(ui_image_viewer_jump) && navigations == 1 && navigated_message == 900 &&
              !application_media_page_shown(),
          "header: Jump to message hides the page and selects its message");

    // The request through the page's own {url, key}.
    const u32 opened_resolves = resolves;
    check(application_media_page_show(first) && pump_once() == 1 && resolves == opened_resolves + 1 &&
              forced_resolves == 0 && settles == 1,
          "pump: the shown image asks once through its resolved URL");
    // Copy: its check follows the toast that confirms it, with no frames of its own meanwhile.
    copy_ok = true;
    check(application_media_page_synchronize() && press(ui_image_viewer_copy) && scene.copied,
          "copy: Copy checks its button");
    g_app.frame_pending = false;
    check(application_media_page_synchronize() && scene.copied && !g_app.frame_pending,
          "copy: the check stays while the toast shows without drawing a frame for it");
    g_app.toasts.count = 0;
    check(application_media_page_synchronize() && !scene.copied, "copy: the check ends with the toast");
    copy_ok = false;
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
    check(!ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_zoom_in)) &&
              !ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_zoom_out)) &&
              !ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_fit)) &&
              !ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_copy)) &&
              !ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_save)) &&
              ui_rect_has_area(ui_image_viewer_button(scene, ui_image_viewer_open_original)),
          "unavailable: zoom, Copy and Save are hidden, Open original stays");
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
    animation_tests(last);

    // A playlist page hosts the player: a play button until it starts, the queue (rows of thumbnail, title over
    // artist and duration as the playlist knows them), row jumps, the wheel, full screen; closing it closes its
    // session. Open original opens the embed's own link, not the media it plays.
    MediaPageSource playlist = image_source(950);
    playlist.kind = MediaPageKind::video;
    playlist.flags = media_page_embed | media_page_playlist;
    playlist.link = {kLink, sizeof(kLink) - 1};
    application_media_page_close(last);
    const u8 player = application_media_page_open(playlist);
    queue_state = {40, 6, 125'000};
    check(player && application_media_page_show(player) && video_shows == 1 && application_media_page_synchronize() &&
              application_media_page_synchronize(),
          "player: a video page shows and asks the player to take it");
    const u32 links = opened_links;
    check(press(ui_image_viewer_open_original) && opened_links == links + 1,
          "header: Open original opens an embed page's own link");
    const auto& page = *application_media_page_viewer();
    const auto layout = ui_image_viewer_page_layout(page);
    const u32 rows = ui_image_viewer_queue_rows(page);
    const float view = ui_rect_height(ui_image_viewer_queue_body(page));
    check(page.idle && page.queue && ui_rect_has_area(layout.queue) && rows >= 5,
          "player: a playlist page that has not started shows its play button and its queue");
    check(page.queue && page.queue->count == 40 && page.queue->current == 6 && page.queue->scroll == 4 * 56.0f &&
              page.queue->first == 3,
          "player: the queue shows the current track with one row of the past above it");
    // Rows from track 4: the playlist knows its odd tracks (title, artist, length).
    check(page.queue && utf8_equal(page.queue->rows[0].title, utf8_literal("Track 4")) &&
              !page.queue->rows[0].detail.bytes &&
              !page.queue->rows[0].duration.bytes && utf8_equal(page.queue->rows[1].title, utf8_literal("Fixture song")) &&
              utf8_equal(page.queue->rows[1].detail, utf8_literal("Fixture Artist")) &&
              utf8_equal(page.queue->rows[1].duration, utf8_literal("1:05")),
          "player: rows show title over artist and duration, numbered until the playlist knows them");
    check(page.queue && utf8_equal(page.queue->rows[2].duration, utf8_literal("2:05")),
          "player: the current track without a known length shows the player's duration");
    // A row the playlist does not know yet is "Track N" for its own N, never empty and never another row's number,
    // with no artist line.
    bool fallbacks = page.queue != nullptr;
    for (u32 n = 0; fallbacks && n < rows && page.queue->first + n < page.queue->count; ++n) {
        const auto& item = page.queue->rows[n];
        const u32 track = page.queue->first + n + 1;
        char expected[16] = "Track ";
        const u32 bytes = 6 + decimal_write(expected + 6, track);
        fallbacks = item.title.bytes != 0 &&
                    ((track & 1) ? utf8_equal(item.title, utf8_literal("Fixture song"))
                                 : utf8_equal(item.title, {expected, bytes}) && !item.detail.bytes);
    }
    check(fallbacks, "player: an unknown row is titled with its own track number and has no artist line");
    check(page.queue && media_asset_key_valid(page.queue->rows[1].thumb) &&
              !media_asset_key_equal(page.queue->rows[1].thumb, page.queue->rows[3].thumb) &&
              application_media_page_key_current(page.queue->rows[1].thumb),
          "player: each YouTube track has its own thumbnail, current while it shows");
    const u32 thumbs = thumb_requests;
    check(pump_once() == 0 && thumb_requests - thumbs == rows && thumb_url_ok,
          "player: a video page requests no picture of its own, only its rows' thumbnails from i.ytimg.com");
    // A track change moves the highlight only: no scroll, the same thumbnails, and every row but the old and the
    // new current one keeps its text.
    UiImageViewerQueueRow before[kUiImageViewerQueueRows];
    char text[kUiImageViewerQueueRows][256];
    for (u32 n = 0; n < rows; ++n) {
        before[n] = page.queue->rows[n];
        copy_bytes(text[n], before[n].title.data, before[n].title.bytes + before[n].detail.bytes + before[n].duration.bytes);
    }
    queue_state = {40, 7, 64'000};
    check(application_media_page_synchronize() && page.queue->current == 7 && page.queue->scroll == 4 * 56.0f &&
              page.queue->first == 3 && !ui_scroll_host_active(g_app.scroll_host),
          "track change: the list stays where it is while the next track is in view");
    u32 changed_rows = 0;
    bool same_thumbs = true;
    for (u32 n = 0; n < rows; ++n) {
        const auto& row = page.queue->rows[n];
        const u32 bytes = row.title.bytes + row.detail.bytes + row.duration.bytes;
        changed_rows += row.title.bytes != before[n].title.bytes || row.detail.bytes != before[n].detail.bytes ||
                        bytes != before[n].title.bytes + before[n].detail.bytes + before[n].duration.bytes ||
                        !bytes_equal(text[n], row.title.data, bytes)
                            ? (n == 2 || n == 3 ? 1u : 100u)
                            : 0u;
        same_thumbs = same_thumbs && media_asset_key_equal(row.thumb, before[n].thumb);
    }
    check(changed_rows <= 2 && same_thumbs,
          "track change: only the old and the new current rows change, and no thumbnail changes");
    // The real advance: the session for the next track starts and the media worker's lane that keeps the playlist
    // resolves it, so every title lookup answers busy for a while. The rows keep their text, thumbnails and keys (the
    // list does not fall back to "Track N"), also when the list moves by a row meanwhile; then the answers return.
    {
        const u32 kept = rows < kUiImageViewerQueueRows ? rows : kUiImageViewerQueueRows;
        UiImageViewerQueueRow held[kUiImageViewerQueueRows];
        char held_text[kUiImageViewerQueueRows][256];
        for (u32 n = 0; n < kept; ++n) {
            held[n] = page.queue->rows[n];
            copy_bytes(held_text[n], held[n].title.data, held[n].title.bytes + held[n].detail.bytes);
        }
        const auto same = [&](u32 n, u32 from) noexcept {
            const auto& row = page.queue->rows[n];
            return row.title.bytes == held[from].title.bytes && row.detail.bytes == held[from].detail.bytes &&
                   bytes_equal(row.title.data, held_text[from], row.title.bytes + row.detail.bytes) &&
                   media_asset_key_equal(row.thumb, held[from].thumb);
        };
        titles_busy = true;
        const u32 thumbs_before = thumb_requests;
        queue_state = {40, 5, 0};
        bool kept_all = application_media_page_synchronize() && page.queue->current == 5;
        for (u32 n = 0; kept_all && n < kept && page.queue->first + n < 40; ++n)
            kept_all = same(n, n);
        check(kept_all, "track change: while the playlist lane resolves the next track every row keeps its data");
        g_pages.queue.scroll += 56;
        bool moved_rows = application_media_page_synchronize() && page.queue->first == 4;
        for (u32 n = 0; moved_rows && n + 1 < kept && page.queue->first + n < 40; ++n)
            moved_rows = same(n, n + 1);
        check(moved_rows, "track change: rows kept while the lane is busy move with the list");
        pump_once();
        check(thumb_url_ok && thumb_requests - thumbs_before <= rows,
              "track change: kept rows ask only for the thumbnails they already had");
        titles_busy = false;
        g_pages.queue.scroll -= 56;
        queue_state = {40, 7, 64'000};
        bool back = application_media_page_synchronize();
        for (u32 n = 0; back && n < kept && page.queue->first + n < 40; ++n)
            back = media_asset_key_equal(page.queue->rows[n].thumb, held[n].thumb);
        check(back, "track change: once the lane answers again the thumbnail keys are the same");
    }
    // A current track out of view glides into view along the scroll curve; it does not jump.
    queue_state = {40, 30, 64'000};
    check(application_media_page_synchronize() && page.queue->scroll == 4 * 56.0f &&
              ui_scroll_host_active(g_app.scroll_host),
          "track change: a current track out of view starts a smooth scroll");
    bool moved = false;
    check(ui_scroll_host_tick(&g_app.scroll_host, application_now_us() + 2'000'000, &moved) && moved &&
              application_media_page_synchronize() && page.queue->scroll == 30 * 56.0f - view &&
              page.queue->first + 1u <= 30 && page.queue->first + rows >= 30,
          "track change: the glide ends with the current track on the list's last line");
    const UiPoint stage{(layout.player.left + layout.player.right) * .5f,
                        (layout.player.top + layout.player.bottom) * .5f};
    check(dispatch(window_test::pointer_down(stage.x, stage.y)) && player_toggles == 1 &&
              dispatch(window_test::pointer_up(stage.x, stage.y)),
          "player: a press on a stage that has not started starts it");
    const auto body = ui_image_viewer_queue_body(page);
    const UiPoint row{body.left + 60, body.top + 56 * 2.5f};
    const u16 under = static_cast<u16>((page.queue->scroll + 56 * 2.5f) / 56 + 1);
    check(ui_image_viewer_queue_hit(page, row) == under && dispatch(window_test::pointer_move(row.x, row.y, 0)) &&
              page.queue->hovered == under && dispatch(window_test::pointer_down(row.x, row.y)) && jumped_track == under,
          "player: a queue row lights under the pointer and jumps to its track");
    dispatch(window_test::pointer_up(row.x, row.y));
    const float scrolled = page.queue->scroll;
    check(dispatch(window_test::wheel(row.x, row.y, -120)) && ui_scroll_host_active(g_app.scroll_host) &&
              ui_scroll_host_tick(&g_app.scroll_host, application_now_us() + 4'000'000, &moved) &&
              application_media_page_synchronize() && page.queue->scroll > scrolled,
          "player: the wheel scrolls the queue smoothly");
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

    // An audio page requests its artwork through its own key and URL, is never unavailable for it, and goes without
    // the picture when it cannot load; its picture is the artwork square, not the whole stage.
    MediaPageSource song = image_source(960);
    song.kind = MediaPageKind::audio;
    song.url = {kSong, sizeof(kSong) - 1};
    song.title = utf8_literal("song.mp3");
    song.key = media_asset_attachment(960, MediaAttachmentFormat::mp3, MediaResolutionTier::original);
    song.art = {kUrl, sizeof(kUrl) - 1};
    const u8 audio = application_media_page_open(song);
    queue_state = {};
    check(audio && application_media_page_show(audio) && application_media_page_synchronize() &&
              media_asset_key_equal(g_app.renderer.media_page_key, song.fallback) &&
              application_media_page_viewer()->body.right - application_media_page_viewer()->body.left ==
                  application_media_page_viewer()->body.bottom - application_media_page_viewer()->body.top &&
              ui_rect_equal(application_media_page_viewer()->image, application_media_page_viewer()->body),
          "audio: the artwork's key goes to the renderer and its square is the page's picture");
    answer = AccountMediaRequestState::queued;
    check(pump_once() == 1 && settles > 0, "audio: the shown page asks for its artwork through the artwork's own URL");
    answer = AccountMediaRequestState::failed;
    answer_error = AccountMediaError::decode;
    check(pump_once() == 1 && !application_media_page_missing(audio) && pump_once() == 0 &&
              application_media_page_synchronize() && application_media_page_viewer()->media == UiImageViewerMedia::ready,
          "audio: artwork that cannot load is given up, not the page, and is not asked again");
    answer = AccountMediaRequestState::queued;
    answer_error = AccountMediaError::none;
    application_media_page_close(audio);
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
// The playlist knows its odd tracks (title, artist and length) and every track's YouTube id.
AccountMediaRequestResult account_module_collection_title(void*, const AccountMediaCollectionTitleRequest* request,
                                                           MediaTrackTitle* title) noexcept {
    AccountMediaRequestResult result{};
    result.state = titles_busy ? AccountMediaRequestState::busy : AccountMediaRequestState::pending;
    if (titles_busy)
        return result;
    title->id.youtube_track = true;
    copy_bytes(title->id.youtube, "abcdefghij", 10);
    title->id.youtube[10] = static_cast<char>('A' + request->track % 26);
    if (request->track & 1) {
        copy_bytes(title->text, "Fixture song\x1F" "Fixture Artist\x1F" "65", 30);
        title->bytes = 30;
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
    opened_links += utf8_equal(url, {kLink, sizeof(kLink) - 1});
    return true;
}
bool ApplicationMediaPump::request_profile_media(const ProfileMedia& media, bool*, bool,
                                                 AccountMediaRequestResult*) noexcept {
    ++thumb_requests;
    const Utf8View url = media.url.view();
    thumb_url_ok = thumb_url_ok && url.bytes == 48 && utf8_starts_with(url, utf8_literal("https://i.ytimg.com/vi/abcdefghij")) &&
                   media_asset_key_equal(media.key, message_component_media_key(hash64_bytes(kHash64Seed, url.data, url.bytes),
                                                                                MediaAttachmentFormat::jpeg));
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
    // A copy confirms with a toast, as the real one does.
    if (copy_ok)
        g_app.toasts.count = 1;
    return copy_ok;
}
bool application_search_close_panel() noexcept {
    return true;
}
void application_thread_browser_close() noexcept {}
void application_composer_text_input_suspend() noexcept {}
bool application_downloads_available(u64, u64, bool) noexcept {
    return downloads_ok;
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
