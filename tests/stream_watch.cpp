#include "maicord/app/gateway_stream_session.hpp"
#include "maicord/app/stream_preview_cache.hpp"
#include "maicord/app/stream_watch_view.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/stream_card_scene.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/media/asset.hpp"

// Go Live watching on the client: preview cache and backoff, the snapshot ->
// call-view state projection, stream tiles (layout, controls, media, states),
// the stream volume slider binding and the preview card.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

GatewayStreamKey key(u64 owner, u64 channel = 20) noexcept {
    return gateway_stream_key_for(10, channel, owner);
}
const StreamPreviewEntry* preview(const StreamPreviewCache& cache, const GatewayStreamKey& stream) noexcept {
    for (const auto& entry : cache.entries)
        if (entry.key.type != GatewayStreamType::none && entry.key == stream)
            return &entry;
    return nullptr;
}
Utf8View text(const char* value) noexcept {
    u32 bytes = 0;
    while (value[bytes])
        ++bytes;
    return {value, bytes};
}
bool close_to(float a, float b, float tolerance = .05f) noexcept {
    return a - b <= tolerance && b - a <= tolerance;
}
UiPoint middle(UiRect r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}

void preview_cache(Check check) noexcept {
    static StreamPreviewCache cache;
    stream_preview_clear(&cache);
    const auto a = key(1);
    check(!stream_preview_want(&cache, GatewayStreamKey{}, 1000) && !stream_preview_next_due(cache, 1000),
          "stream preview: invalid keys are never cached or fetched");
    auto* entry = stream_preview_want(&cache, a, 1000);
    check(entry && stream_preview_next_due(cache, 1000) == entry && !stream_preview_ready(*entry) &&
              !media_asset_key_valid(stream_preview_media_key(*entry)),
          "stream preview: a shown stream is due at once and has no image yet");
    check(stream_preview_submitted(&cache, a, 77, 1000) && !stream_preview_next_due(cache, 1001) &&
              !stream_preview_submitted(&cache, a, 78, 1001),
          "stream preview: one fetch per stream is in flight at a time");
    const auto url = utf8_literal("https://media.example/preview/1.jpg");
    const auto* done = stream_preview_complete(&cache, 77, 200, url, 0, 1200);
    const MediaAssetKey first = done ? stream_preview_media_key(*done) : MediaAssetKey{};
    check(done && stream_preview_ready(*done) && media_asset_key_valid(first) && first.kind == MediaAssetKind::embed &&
              first.id == 1 && done->next_ms == 1200 + kStreamPreviewCacheMs,
          "stream preview: a fetched URL becomes an embed image keyed by owner and fetch serial");
    stream_preview_want(&cache, a, 60'000);
    check(!stream_preview_next_due(cache, 60'000) && stream_preview_next_wake(cache, 60'000) == 1200 + kStreamPreviewCacheMs,
          "stream preview: the official 120 s cache holds the image and schedules the refresh");
    stream_preview_want(&cache, a, 121'300);
    check(stream_preview_next_due(cache, 121'300) == done, "stream preview: a still-shown preview refreshes after 120 s");
    stream_preview_submitted(&cache, a, 79, 121'300);
    done = stream_preview_complete(&cache, 79, 200, url, 0, 121'400);
    check(done && media_asset_key_equal(stream_preview_media_key(*done), first),
          "stream preview: an unchanged URL keeps its image identity");
    stream_preview_want(&cache, a, 241'500);
    stream_preview_submitted(&cache, a, 80, 241'500);
    done = stream_preview_complete(&cache, 80, 200, utf8_literal("https://media.example/preview/2.jpg"), 0, 241'600);
    check(done && !media_asset_key_equal(stream_preview_media_key(*done), first) && stream_preview_ready(*done),
          "stream preview: a new URL is a new image");

    // Failures back off 10 s x attempts; the last image stays.
    const auto b = key(2);
    stream_preview_want(&cache, b, 300'000);
    stream_preview_submitted(&cache, b, 90, 300'000);
    done = stream_preview_complete(&cache, 90, 404, {}, 0, 300'100);
    check(done && done->attempts == 1 && done->next_ms == 310'100 && !stream_preview_ready(*done),
          "stream preview: no thumbnail yet (404) retries after 10 s");
    stream_preview_want(&cache, b, 310'100);
    stream_preview_submitted(&cache, b, 91, 310'100);
    done = stream_preview_complete(&cache, 91, 0, {}, 0, 310'200);
    check(done && done->attempts == 2 && done->next_ms == 330'200, "stream preview: the second failure waits 20 s");
    stream_preview_want(&cache, b, 330'200);
    stream_preview_submitted(&cache, b, 92, 330'200);
    done = stream_preview_complete(&cache, 92, 429, {}, 400'000, 330'300);
    check(done && done->next_ms == 400'000, "stream preview: a 429 waits for retry_after when that is longer");
    for (u64 i = 0; i < 10; ++i) {
        stream_preview_want(&cache, b, 500'000 + i);
        stream_preview_submitted(&cache, b, 100 + i, 500'000 + i);
        done = stream_preview_complete(&cache, 100 + i, 500, {}, 0, 500'000 + i);
    }
    check(done && done->attempts == kStreamPreviewMaximumAttempts &&
              done->next_ms == 500'009 + kStreamPreviewRetryMs * kStreamPreviewMaximumAttempts,
          "stream preview: backoff stops growing at the attempt cap");
    stream_preview_deferred(&cache, b, 600'000);
    check(preview(cache, b)->next_ms == 600'000 && preview(cache, b)->attempts == done->attempts,
          "stream preview: a busy lane defers without counting a failure");
    // Wants expire: nothing is fetched for a surface that closed.
    check(!stream_preview_next_due(cache, 700'000 + kStreamPreviewWantMs),
          "stream preview: streams no longer shown are not fetched");
    check(!stream_preview_complete(&cache, 4242, 200, url, 0, 1), "stream preview: unknown responses are ignored");
    // Recycling keeps in-flight entries.
    for (u64 owner = 3; owner < 3 + kStreamPreviewEntries; ++owner)
        stream_preview_want(&cache, key(owner), 800'000 + owner);
    check(preview(cache, key(2 + kStreamPreviewEntries)) && !preview(cache, a),
          "stream preview: the least recently shown stream is recycled");
}

void watch_view(Check check) noexcept {
    const auto a = key(7), b = key(8);
    GatewayClientStreamSnapshot snapshot{};
    auto view = stream_watch_view(snapshot, {}, 0, 0);
    check(view.state == StreamWatchState::none && view.volume_percent == 100,
          "stream watch: nothing requested shows nothing (default volume 100 %)");
    view = stream_watch_view(snapshot, a, 1000, 1000);
    check(view.state == StreamWatchState::connecting && view.key == a,
          "stream watch: a request the worker has not taken yet is connecting");
    snapshot.key = a;
    snapshot.phase = GatewayStreamPhase::awaiting_server;
    check(stream_watch_view(snapshot, a, 1000, 1100).state == StreamWatchState::connecting,
          "stream watch: STREAM_CREATE/SERVER_UPDATE in flight is connecting");
    snapshot.phase = GatewayStreamPhase::active;
    snapshot.video_ssrc = 0x8000'0042u;
    snapshot.viewer_count = 3;
    snapshot.paused = true;
    snapshot.volume_percent = 150;
    view = stream_watch_view(snapshot, a, 1000, 1200);
    check(view.state == StreamWatchState::live && view.video_ssrc == 0x8000'0042u && view.viewers == 3 &&
              view.paused && view.volume_percent == 150,
          "stream watch: attached media is live with its tagged SSRC, viewers, pause and volume");
    snapshot.phase = GatewayStreamPhase::reconnecting;
    check(stream_watch_view(snapshot, a, 1000, 1300).state == StreamWatchState::reconnecting,
          "stream watch: a lost stream connection reconnects");
    snapshot.phase = GatewayStreamPhase::ended;
    snapshot.end = GatewayStreamEnd::deleted;
    snapshot.delete_reason = GatewayStreamDeleteReason::user_requested;
    view = stream_watch_view(snapshot, a, 1000, 9000);
    check(view.state == StreamWatchState::ended && view.notice == StreamWatchNotice::ended && !view.viewers,
          "stream watch: STREAM_DELETE shows that the stream has ended");
    check(stream_watch_view(snapshot, a, 8500, 9000).state == StreamWatchState::connecting,
          "stream watch: a re-watch ignores the previous watch's end for a moment");
    check(stream_watch_view(snapshot, b, 1000, 9000).state == StreamWatchState::connecting &&
              stream_watch_view(snapshot, b, 1000, 9000).key == b,
          "stream watch: switching streams shows the new one connecting");
    check(stream_watch_notice(GatewayStreamEnd::deleted, GatewayStreamDeleteReason::stream_full) ==
                  StreamWatchNotice::full &&
              stream_watch_notice(GatewayStreamEnd::deleted, GatewayStreamDeleteReason::unauthorized) ==
                  StreamWatchNotice::not_allowed &&
              stream_watch_notice(GatewayStreamEnd::deleted, GatewayStreamDeleteReason::safety_guild_rate_limited) ==
                  StreamWatchNotice::limited &&
              stream_watch_notice(GatewayStreamEnd::timeout, {}) == StreamWatchNotice::timed_out &&
              stream_watch_notice(GatewayStreamEnd::failed, {}) == StreamWatchNotice::failed &&
              stream_watch_notice(GatewayStreamEnd::session, {}) == StreamWatchNotice::interrupted,
          "stream watch: delete reasons and failures map to their notices");
    snapshot.end = GatewayStreamEnd::left_channel;
    check(stream_watch_view(snapshot, a, 1000, 9000).state == StreamWatchState::none &&
              stream_watch_notice(GatewayStreamEnd::stopped, {}) == StreamWatchNotice::none,
          "stream watch: stopping or leaving the channel ends silently");
    // Notices and their copy line up with the scene's.
    check(static_cast<u8>(StreamWatchNotice::interrupted) == static_cast<u8>(UiCallStreamNotice::interrupted) &&
              bytes_equal(ui_call_stream_notice_text(UiCallStreamNotice::ended).data, "The stream has ended", 20) &&
              !ui_call_stream_notice_text(UiCallStreamNotice::none).bytes,
          "stream watch: notices match the call view's copy");
}

UiCallParticipant stream_tile(u64 id, const char* name, UiCallStream state) noexcept {
    UiCallParticipant p{};
    p.user_id = gateway_stream_video_user(id);
    p.name = text(name);
    p.stream = state;
    return p;
}
UiCallParticipant person(u64 id, const char* name) noexcept {
    UiCallParticipant p{};
    p.user_id = id;
    p.name = text(name);
    return p;
}

void stream_tiles(Check check) noexcept {
    static UiCallScene scene;
    UiCallParticipant people[4]{stream_tile(1, "Ana", UiCallStream::live), stream_tile(2, "Bo", UiCallStream::available),
                                person(1, "Ana"), person(3, "Cy")};
    people[0].video_ssrc = 0x8000'0005u;
    people[0].video_width = 1920;
    people[0].video_height = 1200; // 16:10 screen.
    people[0].viewers = 12;
    people[1].preview = media_asset_embed(2, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::jpeg);
    UiCallContent content{};
    content.title = utf8_literal("Lounge");
    content.participants = {people, 4};
    content.connected = true;
    const UiRect viewport{300, 0, 1500, 900};
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && scene.tile_count == 4 &&
              scene.tiles[0].user_id == gateway_stream_video_user(1) && scene.tiles[2].user_id == 1,
          "stream tiles: a stream and its owner's camera are separate tiles");
    bool aspect = true;
    for (u32 i = 0; i < scene.tile_count; ++i)
        aspect = aspect && close_to(ui_rect_width(scene.tiles[i].target) / ui_rect_height(scene.tiles[i].target),
                                    kUiCallTileAspect, .01f);
    check(aspect, "stream tiles: stream tiles share the 16:9 grid");
    UiCallTileMedia media{};
    check(ui_call_scene_tile_media(scene, 0, &media) && media.video_ssrc == 0x8000'0005u &&
              !ui_rect_has_area(media.avatar) && close_to(ui_rect_height(media.video), ui_rect_height(scene.tiles[0].target)) &&
              ui_rect_width(media.video) < ui_rect_width(scene.tiles[0].target),
          "stream tiles: a shared screen is letterboxed, never cropped, and has no avatar");
    check(ui_call_scene_tile_media(scene, 1, &media) && !media.video_ssrc && media_asset_key_equal(media.preview, people[1].preview) &&
              !media.dim,
          "stream tiles: an unwatched stream shows its preview");
    // "Watch stream" is always offered on an unwatched stream.
    const auto watch = ui_call_scene_tile_control(scene, gateway_stream_video_user(2), UiCallControl::stream_watch);
    const auto at = middle(watch);
    const auto hit = ui_call_scene_hit_test(scene, at.x, at.y);
    check(ui_rect_has_area(watch) && hit.control == UiCallControl::stream_watch &&
              hit.user_id == gateway_stream_video_user(2),
          "stream tiles: Watch stream is hit on an unwatched stream's tile");
    // The watched stream's bar appears on hover: volume and stop watching.
    const auto& live = scene.tiles[0];
    check(!ui_rect_has_area(ui_call_scene_tile_control(scene, live.user_id, UiCallControl::stream_stop)),
          "stream tiles: the hover bar is hidden until hover");
    ui_call_scene_set_hover(&scene, middle(live.display).x, middle(live.display).y);
    const auto stop = ui_call_scene_tile_control(scene, live.user_id, UiCallControl::stream_stop);
    const auto volume = ui_call_scene_tile_control(scene, live.user_id, UiCallControl::stream_volume);
    check(ui_rect_has_area(stop) && ui_rect_has_area(volume) && volume.right <= stop.left &&
              ui_call_scene_hit_test(scene, middle(stop).x, middle(stop).y).control == UiCallControl::stream_stop &&
              ui_call_scene_hit_test(scene, middle(volume).x, middle(volume).y).control == UiCallControl::stream_volume,
          "stream tiles: hovering the watched stream shows volume and Stop watching");
    // The labelled button fits "Stop watching" (13 DIP bold) after its glyph.
    check(ui_rect_width(live.display) < 320 || ui_rect_width(stop) - 38 >= 96,
          "stream tiles: Stop watching is never truncated on a wide tile");
    UiRect anchor{};
    u32 identity = 0;
    const auto label = ui_call_scene_tooltip(scene, middle(volume).x, middle(volume).y, &anchor, &identity);
    check(label.bytes == 13 && bytes_equal(label.data, "Stream volume", 13) && ui_rect_equal(anchor, volume),
          "stream tiles: the volume button explains itself");
    check(!ui_call_scene_tooltip(scene, middle(watch).x, middle(watch).y, nullptr, nullptr).bytes,
          "stream tiles: labeled buttons need no tooltip");
    check(scene.viewer_text[0][0] == '1' && scene.viewer_text[0][1] == '2' && !scene.viewer_text[0][2],
          "stream tiles: the viewer count is formatted once per projection");
    check(!ui_call_scene_spinner_visible(scene), "stream tiles: a live picture needs no spinner");
    // States: connecting spins over the preview; ended offers close.
    people[0].stream = UiCallStream::connecting;
    people[0].preview = people[1].preview;
    const u32 revision = scene.revision;
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && scene.revision != revision &&
              ui_call_scene_spinner_visible(scene) && ui_call_scene_tile_media(scene, 0, &media) && !media.video_ssrc &&
              media_asset_key_valid(media.preview) && media.dim,
          "stream tiles: connecting spins over the dimmed preview");
    people[0].stream = UiCallStream::reconnecting;
    people[0].video_ssrc = 0;
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && ui_call_scene_spinner_visible(scene),
          "stream tiles: reconnecting spins too");
    people[0].stream = UiCallStream::ended;
    people[0].notice = UiCallStreamNotice::full;
    ui_call_scene_update(&scene, content, viewport, 0, true);
    const auto dismiss = ui_call_scene_tile_control(scene, live.user_id, UiCallControl::stream_dismiss);
    check(ui_rect_has_area(dismiss) && !ui_call_scene_spinner_visible(scene) &&
              ui_call_scene_tile_media(scene, 0, &media) && !media_asset_key_valid(media.preview) &&
              ui_call_scene_hit_test(scene, middle(dismiss).x, middle(dismiss).y).control ==
                  UiCallControl::stream_dismiss,
          "stream tiles: an ended stream explains why and can be closed");
    // Focus and fullscreen work on a stream like on a camera.
    people[0].stream = UiCallStream::live;
    people[0].video_ssrc = 0x8000'0005u;
    content.focus_user = gateway_stream_video_user(1);
    content.fullscreen = true;
    check(ui_call_scene_update(&scene, content, {0, 0, 1600, 900}, 0, true) && scene.tiles[0].focused &&
              scene.tiles[0].user_id == gateway_stream_video_user(1) && scene.fullscreen,
          "stream tiles: a stream can be focused in fullscreen");
    ui_call_scene_release(&scene);
}

void volume_binding(Check check) noexcept {
    // The tile menu's slider maps its track onto 0..200 % in single steps.
    constexpr u32 steps = kGatewayStreamVolumeMaximum + 1;
    const UiRect track{100, 10, 300, 30};
    const auto tokens = ui_settings_tokens(kDefaultTheme);
    const auto g = ui_slider_geometry(tokens, track, steps, 0, false);
    check(ui_slider_index_at(g, steps, track.left - 50) == 0 &&
              ui_slider_index_at(g, steps, track.right + 50) == kGatewayStreamVolumeMaximum,
          "stream volume: the slider ends at 0 % and 200 %");
    const u32 half = ui_slider_index_at(g, steps, (track.left + track.right) * .5f);
    check(half >= 95 && half <= 105, "stream volume: the slider middle is about 100 %");
    static GatewayStreamSession session;
    gateway_stream_session_set_volume(&session, 500);
    auto snapshot = gateway_stream_session_snapshot(session, 1);
    check(snapshot.volume_percent == kGatewayStreamVolumeMaximum, "stream volume: the session clamps to 200 %");
    gateway_stream_session_set_volume(&session, 40);
    snapshot = gateway_stream_session_snapshot(session, 1);
    check(snapshot.volume_percent == 40 && stream_watch_view(snapshot, {}, 0, 0).volume_percent == 40,
          "stream volume: the chosen volume comes back through the snapshot");
    gateway_stream_session_release(&session);
}

void card(Check check) noexcept {
    static UiStreamCardScene scene;
    UiStreamCardContent content{};
    content.name = utf8_literal("Ana");
    content.channel = utf8_literal("Lounge");
    content.preview_state = UiStreamCardPreview::loading;
    const UiRect viewport{0, 0, 1200, 800};
    const UiRect row{72, 300, 300, 324};
    check(ui_stream_card_show(&scene, 5, content, row, viewport, 1000, false) && scene.active &&
              scene.settled.left >= row.right && close_to(ui_rect_width(scene.settled), kUiStreamCardWidth) &&
              scene.progress < 1 && ui_stream_card_spinner_visible(scene),
          "stream card: hovering a live user opens the card beside the row with a spring entrance");
    u64 now = 1000;
    for (u32 i = 0; i < 64 && ui_stream_card_next_wake(scene, now, false); ++i) {
        now = ui_stream_card_next_wake(scene, now, false);
        ui_stream_card_tick(&scene, now, false);
    }
    check(scene.progress >= .999f && ui_rect_equal(scene.panel, scene.settled),
          "stream card: the entrance settles on the card's place");
    const auto button = middle(scene.button);
    check(ui_stream_card_hit(scene, button.x, button.y) == UiStreamCardControl::watch &&
              ui_stream_card_contains(scene, button.x, button.y) &&
              ui_stream_card_set_hover(&scene, button.x, button.y) && scene.hovered,
          "stream card: Watch stream is the card's action");
    UiStreamCardMedia media{};
    check(!ui_stream_card_media(scene, kDefaultTheme, &media), "stream card: no image while the preview loads");
    content.preview_state = UiStreamCardPreview::ready;
    content.preview = media_asset_embed(1, 3, 0, MediaEmbedResource::image, MediaAttachmentFormat::jpeg);
    const u32 revision = scene.revision;
    check(ui_stream_card_show(&scene, 5, content, row, viewport, now, false) && scene.revision != revision &&
              scene.progress >= .999f && ui_stream_card_media(scene, kDefaultTheme, &media) &&
              ui_rect_equal(media.rectangle, scene.thumbnail) && !ui_stream_card_spinner_visible(scene),
          "stream card: a ready preview fills the thumbnail without replaying the entrance");
    const UiRect edge{900, 780, 1180, 800};
    check(ui_stream_card_show(&scene, 6, content, edge, viewport, now, true) && scene.settled.right <= edge.left &&
              scene.settled.bottom <= viewport.bottom && scene.progress >= 1,
          "stream card: near the window edge it opens on the other side, inside the window");
    ui_stream_card_close(&scene);
    check(!scene.active && !ui_stream_card_contains(scene, button.x, button.y), "stream card: closing hides it");
}
} // namespace

void test_stream_watch(Check check) noexcept {
    preview_cache(check);
    watch_view(check);
    stream_tiles(check);
    volume_binding(check);
    card(check);
}
} // namespace maicord
