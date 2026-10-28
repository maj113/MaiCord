#include "maicord/app/embed_resolver.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/app/cdn_client.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord {
bool youtube_collection_tests(EmbedResolver*) noexcept;
bool apple_music_album_tests(EmbedResolver*) noexcept;
bool apple_music_playlist_tests(EmbedResolver*) noexcept;
bool soundcloud_collection_tests(EmbedResolver*) noexcept;
namespace {
constexpr auto page = utf8_literal("https://soundcloud.com/forss/flickermood");
constexpr auto widget = utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/293&visual=true");
constexpr auto canonical = utf8_literal("<link rel=\"canonical\" href=\"https://soundcloud.com/forss/flickermood\">");
constexpr auto manifest = utf8_literal("https://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/playlist.m3u8?expires=9999999999&Signature=test");
constexpr auto html = utf8_literal(R"(<script>window.__sc_hydration = [{"hydratable":"apiClient","data":{"id":"0123456789abcdef0123456789abcdef"}},{"hydratable":"sound","data":{"id":293,"kind":"track","permalink_url":"https://soundcloud.com/forss/flickermood","public":true,"streamable":true,"policy":"ALLOW","embeddable_by":"all","track_authorization":"public-test","media":{"transcodings":[{"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:293/hash/stream/hls","preset":"aac_160k","snipped":false,"format":{"protocol":"hls","mime_type":"audio/mp4; codecs=\"mp4a.40.2\""}}]}}}];</script>)");
constexpr auto manifest_response = utf8_literal(R"({"url":"https://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/playlist.m3u8?expires=9999999999&Signature=test"})");
constexpr auto playlist = utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:10\n#EXT-X-MAP:URI=\"https://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/init.mp4?expires=9999999999\"\n#EXTINF:10,\nhttps://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/data000.m4s?expires=9999999999\n#EXT-X-ENDLIST\n");
u32 failures;
void check(bool value, const char* name) noexcept { test_check(&failures, value, name); }
void protocol_tests() noexcept {
    u32 scratch[4096]; EmbedStream out{};
    char decoded[1024];
    check(soundcloud_track_locator(widget, {decoded, sizeof(decoded)}).id == 293 && soundcloud_track_url(widget) &&
        utf8_equal(soundcloud_track_locator(utf8_literal("https://w.soundcloud.com/player/?url=https%3A%2F%2Fsoundcloud.com%2Fforss%2Fflickermood"),
            {decoded, sizeof(decoded)}).path, utf8_literal("/forss/flickermood")), "SoundCloud widget decodes either a numeric track or one public permalink");
    const Utf8View bad_widgets[]{
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/293&url=https%3A//api.soundcloud.com/tracks/294"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/0293"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/18446744073709551616"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/293"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/293%3Fsecret_token%3Dx"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//evil.test/tracks/293"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//soundcloud.com/forss/%"),
        utf8_literal("https://w.soundcloud.com.evil.test/player/?url=https%3A//api.soundcloud.com/tracks/293")};
    for (auto url : bad_widgets) check(!soundcloud_track_url(url), "SoundCloud widget rejects ambiguous identity, wrong origins and invalid encoding");
    check(soundcloud_widget_page(canonical, &out) == EmbedPlayerError::none && utf8_equal({out.url, out.url_bytes}, page),
        "SoundCloud canonical widget page is restricted to a public track permalink");
    check(embed_provider(page) == EmbedProvider::soundcloud && utf8_equal(soundcloud_track_path(page), utf8_literal("/forss/flickermood")) &&
        utf8_equal(soundcloud_track_path(utf8_literal("https://www.soundcloud.com/forss/flickermood/?utm_source=clipboard")), utf8_literal("/forss/flickermood")),
        "SoundCloud public permalink normalizes share parameters");
    const Utf8View bad[]{utf8_literal("https://soundcloud.com/forss/sets"), utf8_literal("https://soundcloud.com/forss"),
        utf8_literal("https://soundcloud.com/forss/sets/album"), utf8_literal("https://soundcloud.com.evil.test/forss/flickermood"),
        utf8_literal("https://soundcloud.com/forss/flickermood?secret_token=private"), utf8_literal("https://soundcloud.com/forss/%2e%2e")};
    for (auto url : bad) check(!soundcloud_track_path(url).bytes, "SoundCloud refuses ambiguous, private and collection locators");
    check(soundcloud_track_request(html, soundcloud_track_path(page), {scratch, 4096}, &out) == EmbedPlayerError::none &&
        out.bitrate == 160000 && utf8_equal({out.url, out.url_bytes}, utf8_literal("https://api-v2.soundcloud.com/media/soundcloud:tracks:293/hash/stream/hls?client_id=0123456789abcdef0123456789abcdef&track_authorization=public-test")),
        "SoundCloud binds the public full AAC request to track identity and its own hydration grants");
    const auto saved = out;
    check(soundcloud_widget_page(utf8_literal("<link rel=\"canonical\" href=\"https://evil.test/forss/flickermood\">"), &out) == EmbedPlayerError::malformed &&
        bytes_equal(&out, &saved, sizeof(out)), "SoundCloud widget cannot redirect resolution to another origin");
    check(soundcloud_track_request(html, soundcloud_track_path(page), {scratch, 4096}, &out, 294) == EmbedPlayerError::malformed &&
        bytes_equal(&out, &saved, sizeof(out)), "SoundCloud widget numeric identity must match the canonical page's real track");
    const auto changed = [&](Utf8View before, Utf8View after, EmbedPlayerError expected) noexcept {
        char body[4096]; ProtocolWriter writer{{reinterpret_cast<u8*>(body), sizeof(body)}, 0}; bool replaced = false;
        for (u32 at = 0; at < html.bytes;) {
            if (!replaced && at + before.bytes <= html.bytes && bytes_equal(html.data + at, before.data, before.bytes)) {
                protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(after.data), after.bytes); at += before.bytes; replaced = true;
            } else protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(html.data + at++), 1);
        }
        check(replaced && soundcloud_track_request({body, writer.used}, soundcloud_track_path(page), {scratch, 4096}, &out) == expected &&
            bytes_equal(&out, &saved, sizeof(out)), "SoundCloud rejected metadata cannot replace accepted source identity");
    };
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":294"), EmbedPlayerError::malformed);
    changed(utf8_literal("/forss/flickermood"), utf8_literal("/forss/another"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"streamable\":true"), utf8_literal("\"streamable\":false"), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"policy\":\"ALLOW\""), utf8_literal("\"policy\":\"BLOCK\""), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"embeddable_by\":\"all\""), utf8_literal("\"embeddable_by\":\"none\""), EmbedPlayerError::not_embeddable);
    changed(utf8_literal("\"snipped\":false"), utf8_literal("\"snipped\":true"), EmbedPlayerError::unsupported);
    changed(utf8_literal("api-v2.soundcloud.com/media"), utf8_literal("evil.test/media"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":293,\"id\":293"), EmbedPlayerError::malformed);
    check(soundcloud_manifest(manifest_response, {scratch, 4096}, &out) == EmbedPlayerError::none &&
        utf8_equal({out.url, out.url_bytes}, manifest) && !soundcloud_location(manifest, false), "SoundCloud separates playlist and media admission");
    check(!soundcloud_location(utf8_literal("https://playback.media-streaming.soundcloud.cloud.evil.test/example/aac_160k/hash/playlist.m3u8?expires=9"), true) &&
        !soundcloud_location(utf8_literal("https://playback.media-streaming.soundcloud.cloud/example/aac_160k/../playlist.m3u8?expires=9"), true),
        "SoundCloud rejects lookalike CDN origins and path traversal");
}
struct Exchange { u32 calls{}; u32 fail{}; bool widget{}; };
UpdateTransportError exchange(void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions&,
    UpdateHttpResponse* response_out) noexcept {
    auto& fixture = *static_cast<Exchange*>(opaque); ++fixture.calls;
    const u32 step = fixture.calls - (fixture.widget ? 1u : 0u);
    check(step <= 3, "SoundCloud resolver has bounded request count");
    const Utf8View hosts[]{utf8_literal("soundcloud.com"), utf8_literal("api-v2.soundcloud.com"),
        utf8_literal("playback.media-streaming.soundcloud.cloud")};
    const Utf8View bodies[]{html, manifest_response, playlist};
    if (step > 3) return UpdateTransportError::invalid_input;
    check(utf8_equal(request.host_name, step ? hosts[step - 1] : utf8_literal("w.soundcloud.com")), "SoundCloud requests only exact metadata and media origins");
    if (fixture.calls == fixture.fail) { response_out->status = 503; return UpdateTransportError::none; }
    const auto body = step ? bodies[step - 1] : canonical;
    if (!request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})) return UpdateTransportError::sink;
    response_out->status = 200; response_out->body_bytes = body.bytes;
    return UpdateTransportError::none;
}
}
bool soundcloud_player_tests() noexcept {
    protocol_tests();
    auto* resolver = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* audio = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(resolver && audio, "SoundCloud resolver test owners allocate");
    check((soundcloud_collection_tests(resolver) && youtube_collection_tests(resolver) && apple_music_album_tests(resolver) && apple_music_playlist_tests(resolver)), "SoundCloud collection protocol and bounded native resolution");
    if (resolver && audio) for (u32 kind = 0; kind < 2; ++kind) for (u32 fail = 0; fail <= 3 + kind; ++fail) {
        Exchange fixture{0, fail, kind != 0};
        const auto result = soundcloud_resolve(resolver, kind ? widget : page, audio, {&fixture, exchange});
        check(fail ? !result.ok() && fixture.calls == fail && audio->state == ProgressiveVideoFetchState::empty
                   : result.ok() && fixture.calls == 3 + kind && audio->kind == ProgressiveVideoKind::fragmented_audio &&
                     audio->host == CdnHost::soundcloud_audio && audio->segmented && audio->segmented->playlist.ended,
              "SoundCloud resolution owns one finite audio source and rolls back failed exchanges");
        check(!resolver->response.committed && !resolver->structurals.committed && !resolver->response_bytes,
              "SoundCloud resolver releases sensitive metadata after every result");
        progressive_video_fetch_release(audio);
    }
    if (resolver && audio) {
        Exchange fixture{0, 0, true};
        const auto result = soundcloud_resolve(resolver,
            utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/294"), audio, {&fixture, exchange});
        check(result.error == EmbedPlayerError::malformed && fixture.calls == 2 && audio->state == ProgressiveVideoFetchState::empty,
              "mismatched canonical track stops before requesting any stream grant or media");
    }
    if (resolver) { embed_resolver_release(resolver); VirtualFree(resolver, 0, MEM_RELEASE); }
    if (audio) VirtualFree(audio, 0, MEM_RELEASE);
    return !failures;
}
} // namespace maicord
