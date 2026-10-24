#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[4096];
u8 json[8192];
constexpr auto id = utf8_literal("6718335390845095173");
constexpr auto profile = utf8_literal(R"({"codec_type":"h264","bitrate":800000,"play_addr":{"width":360,"height":640,"data_size":2000000,"url_list":["https://evil.test/a.mp4","https://v16-webapp-prime.tiktok.com/video/tos/test/id?expire=2000&signature=test"]}})");
constexpr auto high = utf8_literal(R"({"codec_type":"h264","bitrate":2200000,"play_addr":{"width":576,"height":1024,"data_size":3000000,"url_list":["https://v19-webapp-prime.tiktok.com/video/tos/test/id?expire=2100&signature=test"]}})");
void check(bool v, const char* name) noexcept { test_check(&failures, v, name); }
Utf8View response(Utf8View first, Utf8View second = {}, Utf8View identity = id) noexcept {
    ProtocolWriter w{{json, sizeof(json)}, 0};
    protocol_writer_append_literal(&w, "{\"status_code\":0,\"items\":[{\"id_str\":");
    protocol_writer_append_json_string(&w, identity);
    protocol_writer_append_literal(&w, ",\"video_info\":{\"profiles\":[");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(first.data), first.bytes);
    if (second.bytes) {
        protocol_writer_append_byte(&w, ',');
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(second.data), second.bytes);
    }
    protocol_writer_append_literal(&w, "]}}]}");
    return {reinterpret_cast<const char*>(json), w.used};
}
void run() noexcept {
    using E = EmbedPlayerError;
    const Utf8View urls[]{utf8_literal("https://www.tiktok.com/player/v1/6718335390845095173"),
        utf8_literal("https://www.tiktok.com/embed/v2/6718335390845095173"),
        utf8_literal("https://www.tiktok.com/@scout2015/video/6718335390845095173")};
    for (auto url : urls) {
        const auto locator = embed_video_locator(url);
        check(locator.provider == EmbedProvider::tiktok && utf8_equal(locator.id, id), "TikTok post identity");
    }
    const Utf8View bad[]{utf8_literal("https://www.tiktok.com/player/v1/0"),
        utf8_literal("https://www.tiktok.com/player/v1/12/extra"),
        utf8_literal("https://www.tiktok.com/player/v1/18446744073709551616"),
        utf8_literal("https://www.tiktok.com.evil.test/player/v1/12"),
        utf8_literal("https://www.tiktok.com/@name/photo/12")};
    for (auto url : bad) check(!embed_video_locator(url).id.bytes, "reject invalid/non-video post");
    EmbedStreams out{};
    auto value = tiktok_player_streams(response(high, profile), id, 720, 1000, {scratch, 4096}, &out);
    check(value == E::none && out.combined && out.video.height == 640 && out.video.content_bytes == 2000000 &&
          out.expires_seconds == 1000, "select bounded H.264 rendition and validated signed origin");
    value = tiktok_player_streams(response(profile, high), id, 1080, 1000, {scratch, 4096}, &out);
    check(value == E::none && out.video.height == 1024 && out.expires_seconds == 1100, "native quality selection");
    const auto before = out;
    const auto rejects = [&](Utf8View body, E expected, u64 now = 1000) noexcept {
        check(tiktok_player_streams(body, id, 720, now, {scratch, 4096}, &out) == expected &&
              bytes_equal(&before, &out, sizeof(out)), "typed transactional rejection");
    };
    rejects(response(profile, {}, utf8_literal("6718335390845095174")), E::malformed);
    rejects(response(profile), E::unsupported, 2000);
    rejects(utf8_literal(R"({"status_code":1})"), E::unavailable);
    rejects(utf8_literal(R"({"status_code":0,"items":[]})"), E::unsupported);
    rejects(response(utf8_literal(R"({"codec_type":"bytevc1"})")), E::unsupported);
    rejects(response(utf8_literal(R"({"codec_type":"h264","codec_type":"h264"})")), E::malformed);
    check(tiktok_player_streams(response(profile), id, 720, 1000, {scratch, 1}, &out) == E::capacity,
          "bounded structural scratch");
    u8 target[128];
    const auto n = tiktok_player_target(id, {target, sizeof(target)});
    check(utf8_equal({reinterpret_cast<const char*>(target), n}, utf8_literal(
        "/player/api/v1/items?item_ids=6718335390845095173&language=en&aid=1459&data_source=web_core")), "current public player GET");
    const auto url = utf8_literal("https://v16-webapp-prime.tiktok.com/video/tos/test/id?expire=2000&signature=test");
    u8 head[4096];
    const auto bytes = embed_stream_get(CdnHost::tiktok_video, url, 1048576, 4096, {head, sizeof(head)});
    const Utf8View text{reinterpret_cast<const char*>(head), bytes};
    check(bytes && ascii_contains_ignore_case(text, utf8_literal("Range: bytes=1048576-1052671\r\n")) &&
          ascii_contains_ignore_case(text, utf8_literal("Referer: https://www.tiktok.com/\r\n")) &&
          !ascii_contains_ignore_case(text, utf8_literal("Cookie:")) &&
          !ascii_contains_ignore_case(text, utf8_literal("Authorization:")), "credential-free ranged playback");
    check(!embed_stream_location(CdnHost::google_video, url) &&
          !embed_stream_location(CdnHost::tiktok_video, utf8_literal("https://v16-webapp-prime.tiktok.com.evil.test/video/tos/a")) &&
          !embed_stream_location(CdnHost::tiktok_video, utf8_literal("https://www.tiktok.com/aweme/v1/play/")),
          "provider-specific origin and redirect boundary");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_tiktok_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
