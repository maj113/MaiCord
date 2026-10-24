#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[2048];
u8 json[4096];
constexpr auto id = utf8_literal("hn8hq");
constexpr auto mobile = utf8_literal(R"({"status":2,"url":"//cdn-cf-east.streamable.com/video/mp4-mobile/hn8hq.mp4?Expires=2000&Signature=fixture","height":360,"width":640,"size":4746054,"bitrate":1510573})");
constexpr auto full = utf8_literal(R"({"status":2,"url":"https://cdn-cf-west.streamable.com/video/mp4/hn8hq.mp4?Expires=3000&Signature=fixture","height":720,"width":1280,"size":14230108,"bitrate":4607887})");
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
Utf8View response(Utf8View low = mobile, Utf8View high = full, Utf8View extra = {}) noexcept {
    ProtocolWriter w{{json, sizeof(json)}, 0};
    protocol_writer_append_literal(&w, "{\"status\":2,\"url\":\"streamable.com/hn8hq\",\"files\":{\"mp4-mobile\":");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(low.data), low.bytes);
    protocol_writer_append_literal(&w, ",\"mp4\":");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(high.data), high.bytes);
    protocol_writer_append_literal(&w, "}");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(extra.data), extra.bytes);
    protocol_writer_append_literal(&w, "}");
    return {reinterpret_cast<const char*>(json), w.used};
}
void run() noexcept {
    using E = EmbedPlayerError;
    const Utf8View forms[]{utf8_literal("https://streamable.com/hn8hq"), utf8_literal("https://streamable.com/e/hn8hq"),
        utf8_literal("https://www.streamable.com/o/hn8hq")};
    for (auto url : forms) {
        const auto locator = embed_video_locator(url);
        check(locator.provider == EmbedProvider::streamable && utf8_equal(locator.id, id), "public and iframe video locators");
    }
    const Utf8View bad[]{utf8_literal("https://streamable.com.evil.test/hn8hq"), utf8_literal("https://streamable.com@evil.test/hn8hq"),
        utf8_literal("http://streamable.com/hn8hq"), utf8_literal("https://streamable.com/e/hn8hq/extra"),
        utf8_literal("https://streamable.com/e/a%20b"), utf8_literal("https://streamable.com/hn8hq?start=1&start=2")};
    for (auto url : bad) check(!embed_video_locator(url).id.bytes, "ambiguous and untrusted locators rejected");
    check(embed_video_locator(utf8_literal("https://streamable.com/e/hn8hq?start=12")).start_seconds == 12,
          "explicit start position preserved");
    EmbedStreams streams{};
    check(streamable_video_streams(response(), id, 480, 1000, {scratch, 2048}, &streams) == E::none &&
          streams.combined && streams.video.height == 360 && streams.video.content_bytes == 4746054 &&
          streams.expires_seconds == 1000 && utf8_starts_with({streams.video.url, streams.video.url_bytes}, utf8_literal("https://")),
          "small native rendition selected with signed-source lifetime and HTTPS promotion");
    check(streamable_video_streams(response(), id, 1080, 1000, {scratch, 2048}, &streams) == E::none && streams.video.height == 720,
          "largest fitting rendition selected");
    check(streamable_video_streams(response(), id, 144, 1000, {scratch, 2048}, &streams) == E::none && streams.video.height == 360,
          "smallest available rendition when all exceed preview size");
    const auto prior = streams;
    const auto reject = [&](Utf8View body, E expected, u64 now = 1000) noexcept {
        check(streamable_video_streams(body, id, 480, now, {scratch, 2048}, &streams) == expected &&
              bytes_equal(&prior, &streams, sizeof(streams)), "invalid metadata preserves selected stream");
    };
    reject(response(), E::unsupported, 3000);
    reject(response(mobile, full, utf8_literal(",\"url\":\"streamable.com/other\"")), E::malformed);
    reject(response(mobile, full, utf8_literal(R"(,"statu\u0073":2)")), E::malformed);
    reject(utf8_literal(R"({"status":1})"), E::unavailable);
    reject(utf8_literal(R"({"status":2,"url":"streamable.com/other","files":{}})"), E::malformed);
    reject(response(utf8_literal(R"({"status":2,"url":"https://cdn-cf-east.streamable.com/video/mp4/other.mp4?Expires=2000","height":360,"width":640,"size":1,"bitrate":1})")), E::malformed);
    reject(response(utf8_literal(R"({"status":2,"url":"https://cdn-cf-east.streamable.com/video/mp4/hn8hq.mp4?Expires=&expires=2000","height":360,"width":640,"size":1,"bitrate":1})")), E::malformed);
    reject(response(utf8_literal(R"({"status":2,"url":"https://cdn-cf-east.streamable.com/video/mp4/hn8hq.mp4?Expires=2000&Expires=3000","height":360,"width":640,"size":1,"bitrate":1})")), E::malformed);
    check(streamable_video_streams(response(), id, 480, 1000, {scratch, 1}, &streams) == E::capacity, "bounded metadata scratch");
    u8 request[1024];
    const u32 bytes = embed_stream_get(CdnHost::streamable_video, {streams.video.url, streams.video.url_bytes}, 4096, 1024, {request, sizeof(request)});
    const Utf8View head{reinterpret_cast<const char*>(request), bytes};
    check(bytes && ascii_contains_ignore_case(head, utf8_literal("Range: bytes=4096-5119")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Cookie:")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Authorization:")), "native CDN range has no account or browser credentials");
    check(!embed_stream_location(CdnHost::streamable_video, utf8_literal("https://cdn-cf-east.streamable.com.evil.test/video/mp4/hn8hq.mp4")) &&
          !embed_stream_location(CdnHost::streamable_video, utf8_literal("https://cdn-cf-east.streamable.com/other/hn8hq.mp4")) &&
          !embed_stream_location(CdnHost::streamable_video, utf8_literal("https://cdn-cf-east.streamable.com/video/mp4/hn8hq.m3u8")) &&
          !embed_stream_location(CdnHost::streamable_video, utf8_literal("https://cdn-cf-east.streamable.com/video/mp4/hn8hq.mp4#fragment")),
          "exact provider CDN and MP4 path boundary");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_streamable_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
