#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool condition, const char* label) noexcept { test_check(&failures, condition, label); }
constexpr auto app = utf8_literal("570"), movie = utf8_literal("256692021");
constexpr auto manifest = utf8_literal("https://video.akamai.steamstatic.com/store_trailers/570/116737/0123456789abcdef0123456789abcdef01234567/1749859757/dash_h264.mpd?t=1749859757");
constexpr auto media = utf8_literal("https://video.akamai.steamstatic.com/store_trailers/570/116737/0123456789abcdef0123456789abcdef01234567/1749859757/dash_h264/chunk-stream2-00001.m4s");
void run() noexcept {
    using E = EmbedPlayerError;
    const auto url = steam_video_locator(utf8_literal("https://store.steampowered.com/video/570/256692021/"));
    check(utf8_equal(url.app, app) && utf8_equal(url.movie, movie) &&
          embed_video_locator(utf8_literal("https://store.steampowered.com/video/570/256692021/")).provider == EmbedProvider::steam,
          "explicit Steam trailer retains exact app and movie identity");
    check(utf8_equal(steam_video_locator(utf8_literal("https://store.steampowered.com/app/570/Dota_2/")).app, app) &&
          !steam_video_locator(utf8_literal("https://store.steampowered.com/video/570/")).movie.bytes,
          "app and video overview select the first listed trailer");
    const Utf8View bad[]{utf8_literal("http://store.steampowered.com/video/570/256692021/"),
        utf8_literal("https://store.steampowered.com.evil.test/video/570/256692021/"),
        utf8_literal("https://store.steampowered.com@evil.test/video/570/256692021/"),
        utf8_literal("https://store.steampowered.com/video/570/256692021/extra"),
        utf8_literal("https://store.steampowered.com/video/0570/256692021/"),
        utf8_literal("https://store.steampowered.com/video/4294967296/256692021/"),
        utf8_literal("https://store.steampowered.com/app/570/../"),
        utf8_literal("https://store.steampowered.com/widget/570/")};
    for (auto value : bad) check(!steam_video_locator(value).app.bytes, "untrusted or ambiguous Steam page cannot become a video source");
    check(steam_manifest_location(manifest, app) && !steam_manifest_location(manifest, utf8_literal("730")) &&
          steam_media_location(media) && !steam_media_location(manifest) && !steam_manifest_location(media),
          "Steam CDN admission separates manifest and media files and binds metadata to the requested app");
    char invalid[2048]; copy_bytes(invalid, media.data, media.bytes); invalid[8] = 'x';
    check(!steam_media_location({invalid, media.bytes}), "Steam media cannot change CDN origins");
    copy_bytes(invalid, media.data, media.bytes); invalid[media.bytes] = '?';
    check(!steam_media_location({invalid, media.bytes + 1}), "Steam segment targets reject query injection");
    u8 json[4096]; ProtocolWriter w{{json, sizeof(json)}, 0};
    protocol_writer_append_literal(&w, "{\"570\":{\"success\":true,\"data\":{\"movies\":[{\"id\":256692021,\"dash_h264\":");
    protocol_writer_append_json_string(&w, manifest);
    protocol_writer_append_literal(&w, "},{\"id\":3,\"name\":\"unrelated trailer\"}]}}}");
    static u32 scratch[4096]; EmbedStream selected{};
    const Utf8View body{reinterpret_cast<const char*>(json), w.used};
    check(steam_video_manifest(body, app, movie, {scratch, 4096}, &selected) == E::none &&
          utf8_equal({selected.url, selected.url_bytes}, manifest) &&
          steam_video_manifest(body, app, {}, {scratch, 4096}, &selected) == E::none,
          "matching store record resolves only its clear AVC manifest; overview resolves the first record");
    const auto prior = selected;
    check(steam_video_manifest(body, app, utf8_literal("4"), {scratch, 4096}, &selected) == E::unavailable &&
          steam_video_manifest(body, app, utf8_literal("3"), {scratch, 4096}, &selected) == E::unsupported &&
          steam_video_manifest(body, utf8_literal("730"), movie, {scratch, 4096}, &selected) == E::malformed &&
          bytes_equal(&prior, &selected, sizeof(selected)), "wrong app/movie or missing compatible manifest cannot replace the accepted source");
    check(steam_video_manifest(utf8_literal(R"({"570":{"success":false}})"), app, movie, {scratch, 4096}, &selected) == E::unavailable &&
          steam_video_manifest(utf8_literal(R"({"570":{"success":true,"success":false}})"), app, movie, {scratch, 4096}, &selected) == E::malformed,
          "failed or ambiguous store metadata has no playable source");
    u8 request[2048];
    const auto size = embed_stream_get(CdnHost::steam_video, media, 0, 1024 * 1024, {request, sizeof(request)});
    const Utf8View head{reinterpret_cast<const char*>(request), size};
    check(size && ascii_contains_ignore_case(head, utf8_literal("Range: bytes=0-1048575")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Cookie:")) && !ascii_contains_ignore_case(head, utf8_literal("Authorization:")),
          "Steam segment GET is bounded and credential free");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_steam_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
