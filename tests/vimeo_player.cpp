#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
void run() noexcept {
    using E = EmbedPlayerError;
    constexpr auto id = utf8_literal("22439234");
    const Utf8View pages[]{utf8_literal("https://vimeo.com/22439234"),
                          utf8_literal("https://vimeo.com/channels/staffpicks/22439234"),
                          utf8_literal("https://vimeo.com/groups/animation/videos/22439234"),
                          utf8_literal("https://vimeo.com/album/12345/video/22439234"),
                          utf8_literal("https://vimeo.com/ondemand/sample-film/22439234"),
                          utf8_literal("https://player.vimeo.com/video/22439234?autoplay=1")};
    for (const auto page : pages) {
        const auto route = embed_video_locator(page);
        check(route.provider == EmbedProvider::vimeo && utf8_equal(route.id, id), "public Vimeo activates the native embed route");
        const auto locator = vimeo_video_id(page);
        check(utf8_equal(locator, id), "Vimeo page and iframe identity");
    }
    const Utf8View timed[]{utf8_literal("https://vimeo.com/22439234#t=1m2s"),
                          utf8_literal("https://player.vimeo.com/video/22439234?h=0123456789#t=62s&share=copy")};
    for (auto url : timed) {
        const auto route = embed_video_locator(url);
        check(utf8_equal(route.id, id) && route.start_seconds == 62, "Vimeo shared and player fragments retain the requested start time");
    }
    const Utf8View shared[]{utf8_literal("https://vimeo.com/22439234/0123456789"),
        utf8_literal("https://player.vimeo.com/video/22439234?h=0123456789&autoplay=1")};
    for (auto page : shared) {
        Utf8View privacy{};
        check(utf8_equal(vimeo_video_id(page, &privacy), id) && utf8_equal(privacy, utf8_literal("0123456789")) &&
              utf8_equal(embed_video_locator(page).id, id), "Vimeo shared page and iframe preserve the explicit hash");
    }
    const Utf8View bad[]{utf8_literal("https://player.vimeo.com.evil.test/video/22439234"),
        utf8_literal("https://vimeo.com/channels/staffpicks/22439234/extra"),
        utf8_literal("https://vimeo.com/channels//22439234"),
        utf8_literal("https://vimeo.com/groups/../videos/22439234"),
        utf8_literal("https://vimeo.com/groups/animation/video/22439234"),
        utf8_literal("https://vimeo.com/album/name/video/22439234"),
        utf8_literal("https://player.vimeo.com/channels/staffpicks/22439234"),
        utf8_literal("https://player.vimeo.com:443/video/22439234"), utf8_literal("https://vimeo.com/22439234/other"),
        utf8_literal("https://player.vimeo.com/video/22439234?h=private"),
        utf8_literal("https://player.vimeo.com/video/22439234?h="),
        utf8_literal("https://player.vimeo.com/video/22439234?h=0123456789&h=0123456789"),
        utf8_literal("https://player.vimeo.com/video/22439234?h=%30123456789"),
        utf8_literal("https://player.vimeo.com/video/22439234/0123456789"),
        utf8_literal("https://vimeo.com/22439234/0123456789?h=abcdef1234"), utf8_literal("https://vimeo.com/022439234")};
    for (auto page : bad) {
        Utf8View unchanged = id;
        check(!vimeo_video_id(page, &unchanged).bytes && utf8_equal(unchanged, id),
              "invalid or ambiguous Vimeo locators leave the hash output unchanged");
    }
    const Utf8View bad_times[]{utf8_literal("https://vimeo.com/22439234#t=4294967296"),
        utf8_literal("https://vimeo.com/22439234#t=1m2h"), utf8_literal("https://vimeo.com/22439234#t="),
        utf8_literal("https://vimeo.com/22439234#t=1&t=2")};
    for (auto url : bad_times) check(!embed_video_locator(url).id.bytes, "Vimeo start time rejects overflow, malformed and conflicting fragments");
    constexpr auto manifest = utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/playlist/av/primary/playlist.m3u8?pathsig=test");
    constexpr auto media = utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/range/prot/0-100/avf/video?range=0-100");
    check(vimeo_location(manifest, true) && !vimeo_location(manifest, false) &&
          vimeo_location(media, false) && !vimeo_location(media, true), "Vimeo playlist and media capabilities remain separate");
    check(vimeo_location(utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=2000000000~acl=%2Fpublic%2Fv2%2F%2A~hmac=test/public/v2/playlist/av/primary/playlist.m3u8?pathsig=test"), true),
          "Vimeo signed prefix retains encoded ACL separators without decoding the request URL");
    const Utf8View wrong[]{utf8_literal("https://vod-adaptive-ak.vimeocdn.com.evil.test/public/v2/range/a"),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/../v2/range/a"),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/%2e%2e/v2/range/a"),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/range/a#fragment"),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=1~acl=%2F..%2F~hmac=x/public/v2/range/a"),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/exp=1~acl=%252F~hmac=x/public/v2/range/a"),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/range/.."),
        utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/range/..?x=1")};
    for (auto url : wrong) check(!vimeo_location(url, false), "Vimeo CDN rejects lookalikes, path traversal and fragments");
    constexpr auto html = utf8_literal(R"(<script>window.playerConfig = {"video":{"id":22439234,"privacy":"anybody","embed_permission":"public"},"request":{"files":{"hls":{"default_cdn":"ak","cdns":{"ak":{"url":"https://vod-adaptive-ak.vimeocdn.com/public/v2/playlist/av/primary/playlist.m3u8?pathsig=test"}}}}}};</script>)");
    u32 scratch[4096]; EmbedStream selected{};
    check(vimeo_video_manifest(html, id, {scratch, 4096}, &selected) == E::none &&
          utf8_equal({selected.url, selected.url_bytes}, manifest), "public Vimeo config binds its selected CDN manifest to the requested video");
    const auto prior = selected;
    check(vimeo_video_manifest(html, utf8_literal("22439235"), {scratch, 4096}, &selected) == E::malformed &&
          bytes_equal(&selected, &prior, sizeof(prior)), "mismatched Vimeo identity leaves output unchanged");
    check(vimeo_video_manifest(html, id, {scratch, 1}, &selected) == E::capacity,
          "Vimeo parsing obeys the caller scratch bound");
    constexpr auto private_html = utf8_literal(R"(window.playerConfig = {"video":{"id":22439234,"privacy":"password","embed_permission":"public"},"request":{}};)");
    check(vimeo_video_manifest(private_html, id, {scratch, 4096}, &selected) == E::not_embeddable,
          "private Vimeo video does not use a public playback route");
    constexpr auto unlisted = utf8_literal(R"(window.playerConfig = {"video":{"id":22439234,"privacy":"unlisted","embed_permission":"public","unlisted_hash":"0123456789"},"request":{"files":{"hls":{"default_cdn":"ak","cdns":{"ak":{"url":"https://vod-adaptive-ak.vimeocdn.com/public/v2/playlist/av/primary/playlist.m3u8?pathsig=test"}}}}}};)");
    check(vimeo_video_manifest(unlisted, id, {scratch, 4096}, &selected, utf8_literal("0123456789")) == E::none,
          "explicit shared hash matches the unlisted video configuration");
    check(vimeo_video_manifest(unlisted, id, {scratch, 4096}, &selected) == E::not_embeddable &&
          vimeo_video_manifest(unlisted, id, {scratch, 4096}, &selected, utf8_literal("abcdef1234")) == E::not_embeddable &&
          bytes_equal(&selected, &prior, sizeof(prior)), "missing or mismatched unlisted hash cannot publish a stream");
    check(vimeo_video_manifest(private_html, id, {scratch, 4096}, &selected, utf8_literal("0123456789")) == E::not_embeddable,
          "shared hashes do not remove password protection");
    constexpr auto restricted = utf8_literal(R"(window.playerConfig = {"video":{"id":22439234,"privacy":"unlisted","embed_permission":"whitelist","unlisted_hash":"0123456789"},"request":{}};)");
    check(vimeo_video_manifest(restricted, id, {scratch, 4096}, &selected, utf8_literal("0123456789")) == E::not_embeddable,
          "shared hashes do not remove domain restrictions");
    u8 request[4096];
    check(embed_stream_get(CdnHost::vimeo_video, media, 0, 1024, {request, sizeof(request)}) != 0 &&
          !embed_stream_get(CdnHost::vimeo_video, manifest, 0, 1024, {request, sizeof(request)}),
          "native media GET admits Vimeo fragments but not manifests through the media lane");
    constexpr char head[] = "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: 16\r\n\r\n";
    check(validate_cdn_range_response(CdnImageType::m4a,
              {reinterpret_cast<const u8*>(head), sizeof(head) - 1}, 16, {0, 1024}, 1000, CdnHost::vimeo_video).ok(),
          "Vimeo audio fragments accept the provider container MIME");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_vimeo_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
