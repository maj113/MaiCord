#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[8192];
u8 body[8192];
EmbedStreams result;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
constexpr auto id = utf8_literal("aB_01234-xy");
constexpr auto video360 = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=360","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"1000000","bitrate":100000,"width":640,"height":360})");
constexpr auto video720 = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=720","mimeType":"video/mp4; codecs=\"avc1.4d401f\"","contentLength":"2000000","bitrate":200000,"width":1280,"height":720})");
constexpr auto audio = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=audio\u0026range=0-99","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":128000,"audioChannels":2,"audioTrack":{"audioIsDefault":true}})");
constexpr auto alternate = utf8_literal(R"({"url":"https://rr1---sn-test.googlevideo.com/videoplayback?id=alternate","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":160000,"audioChannels":2,"audioTrack":{"audioIsDefault":false}})");
Utf8View response(Utf8View a, Utf8View b = {}, Utf8View c = {}, Utf8View d = {}) noexcept {
    ProtocolWriter w{{body, sizeof(body)}, 0};
    protocol_writer_append_literal(&w, "{\"playabilityStatus\":{\"status\":\"OK\",\"playableInEmbed\":true},\"videoDetails\":{\"videoId\":\"aB_01234-xy\"},\"streamingData\":{\"expiresInSeconds\":\"21600\",\"adaptiveFormats\":[");
    bool first = true;
    const Utf8View entries[]{a, b, c, d};
    for (auto entry : entries) {
        if (!entry.bytes) continue;
        if (!first) protocol_writer_append_byte(&w, ',');
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(entry.data), entry.bytes);
        first = false;
    }
    protocol_writer_append_literal(&w, "]}}");
    return {reinterpret_cast<const char*>(body), w.used};
}
EmbedPlayerError decode(Utf8View source, u16 height = 480, Utf8View expected = id) noexcept {
    return youtube_player_streams(source, expected, height, {scratch, 8192}, &result);
}
void rejected(Utf8View source, EmbedPlayerError expected) noexcept {
    EmbedStreams before = result;
    check(decode(source) == expected && bytes_equal(&before, &result, sizeof(result)),
          "player errors are typed and leave output untouched");
}
} // namespace
}
extern "C" __declspec(noreturn) void WINAPI maicord_youtube_player_entry() noexcept {
    using namespace maicord;
    using E = EmbedPlayerError;
    auto source = response(video720, audio, video360, alternate);
    check(decode(source) == E::none && result.video.height == 360 && result.video.width == 640 &&
          result.video.content_bytes == 1000000 && result.audio.bitrate == 128000 && !result.combined &&
          result.expires_seconds == 21600 &&
          utf8_equal({result.audio.url, result.audio.url_bytes},
             utf8_literal("https://rr1---sn-test.googlevideo.com/videoplayback?id=audio&range=0-99")),
          "bounded video choice, default audio and JSON-escaped URL decoding");
    check(decode(source, 720) == E::none && result.video.height == 720, "quality follows requested extent");
    check(decode(source, 144) == E::none && result.video.height == 360, "smallest available above extent");
    const auto muxed = utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=muxed","mimeType":"video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"","contentLength":"2000000","bitrate":250000,"width":640,"height":360,"audioQuality":"AUDIO_QUALITY_LOW"})");
    check(decode(response(video720, audio, muxed), 720) == E::none && !result.combined &&
          result.video.height == 720 && result.audio.url_bytes, "higher requested quality selects adaptive video despite a lower-resolution muxed alternative");
    check(decode(response(muxed, audio, video720), 720) == E::none && !result.combined &&
          result.video.height == 720, "adaptive quality choice is independent of format order");
    check(decode(response(video720, audio, muxed), 360) == E::none && result.combined && result.video.height == 360,
          "muxed source avoids a second stream when it already supplies the requested extent");
    check(decode(response(video720, muxed), 720) == E::none && result.combined,
          "missing compatible adaptive audio retains the working muxed alternative");
    check(decode(response(muxed, video720), 720) == E::none && result.combined,
          "muxed fallback survives an earlier appearance in the format list");
    const auto high_audio = utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=high","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"200000","bitrate":258000,"audioChannels":2})");
    check(decode(response(video720, audio, high_audio),720)==E::none && result.audio.bitrate==258000,
          "high-quality stereo AAC is not discarded by the old 192 kbps limit");
    check(decode(response(video720, high_audio, audio),720)==E::none && result.audio.bitrate==258000,
          "high-quality stereo AAC selection is independent of order");
    source = response(video720, audio, video360, alternate);
    check(decode(source, 480, utf8_literal("bB_01234-xy")) == E::malformed, "wrong video identity rejected");
    rejected(response(video360), E::unsupported);
    constexpr auto live_player=utf8_literal(R"({"playabilityStatus":{"status":"OK","playableInEmbed":true},"videoDetails":{"videoId":"aB_01234-xy","isLive":true},"streamingData":{"expiresInSeconds":"21600","hlsManifestUrl":"https://manifest.googlevideo.com/api/manifest/hls_variant/id/example/file/index.m3u8"}})");
    check(decode(live_player)==E::none && result.live && result.video.url_bytes && !result.audio.url_bytes,
        "live identity resolves to a bounded clear HLS master for the native playlist lane");
    constexpr auto playlist_url=utf8_literal("https://manifest.googlevideo.com/api/manifest/hls_playlist/id/example/file/index.m3u8");
    check(embed_stream_location(CdnHost::youtube_playlist,playlist_url) &&
        !embed_stream_location(CdnHost::youtube_live,playlist_url) &&
        !embed_stream_location(CdnHost::youtube_playlist,utf8_literal("https://manifest.googlevideo.com.evil.test/api/manifest/hls_playlist/id/x")) &&
        !embed_stream_location(CdnHost::youtube_playlist,utf8_literal("https://manifest.googlevideo.com/api/manifest/hls_playlist/id/x\r\nCookie: bad")) &&
        !embed_stream_location(CdnHost::youtube_playlist,utf8_literal("https://manifest.googlevideo.com/other")),
        "YouTube live playlists have a distinct exact origin and path boundary");
    check(embed_stream_get(CdnHost::youtube_playlist,playlist_url,0,0,{body,sizeof(body)}) &&
        !embed_stream_get(CdnHost::youtube_playlist,playlist_url,0,100,{body,sizeof(body)}) &&
        embed_stream_location(CdnHost::youtube_live,utf8_literal("https://rr1.googlevideo.com/videoplayback/id/example/file/seg.ts")) &&
        !embed_stream_location(CdnHost::google_video,utf8_literal("https://rr1.googlevideo.com/videoplayback/id/example/file/seg.ts")),
        "live playlist and segment requests cannot enter the ordinary ranged MP4 lane");
    constexpr char live_binary[]="HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 188\r\n\r\n";
    const ConstSlice<u8> live_head{reinterpret_cast<const u8*>(live_binary),sizeof(live_binary)-1};
    check(validate_cdn_range_response(CdnImageType::mpeg_ts,live_head,188,{0,1024},1000,CdnHost::youtube_live).ok() &&
        !validate_cdn_range_response(CdnImageType::mpeg_ts,live_head,188,{0,1024},1000,CdnHost::google_video).ok() &&
        !validate_cdn_range_response(CdnImageType::mp4,live_head,188,{0,1024},1000,CdnHost::youtube_live).ok(),
        "YouTube live binary responses require the typed transport parser instead of ordinary MP4 admission");
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"LOGIN_REQUIRED"}})"), E::login_required);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"UNPLAYABLE"}})"), E::unavailable);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"OK","playableInEmbed":false}})"), E::not_embeddable);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"OK","status":"LOGIN_REQUIRED"}})"), E::malformed);
    rejected(utf8_literal(R"({"playabilityStatus":{"status":"OK"}})"), E::malformed);
    rejected(utf8_literal("{} trailing"), E::malformed);
    const auto cipher = utf8_literal(R"({"signatureCipher":"opaque","mimeType":"video/mp4; codecs=\"avc1.4d401e\""})");
    rejected(response(cipher, audio), E::unsupported);
    const auto malformed = utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=v","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"bad","bitrate":100000,"width":640,"height":360})");
    rejected(response(malformed, audio), E::malformed);
    const auto unsafe = utf8_literal(R"({"url":"https://googlevideo.com.evil.test/videoplayback?id=v","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"1000000","bitrate":100000,"width":640,"height":360})");
    rejected(response(unsafe, audio), E::malformed);
    constexpr Utf8View bad[]{utf8_literal("http://rr1.googlevideo.com/videoplayback?id=x"),
        utf8_literal("https://rr1.googlevideo.com@evil.test/videoplayback?id=x"),
        utf8_literal("https://rr1.googlevideo.com:443/videoplayback?id=x"),
        utf8_literal("https://rr1.googlevideo.com/other?id=x"),
        utf8_literal("https://rr1.googlevideo.com/videoplayback?id=%zz"),
        utf8_literal("https://rr1.googlevideo.com/videoplayback?id=x#fragment"),
        utf8_literal("https://rr1.googlevideo.com/videoplayback?id=x\r\nCookie: bad")};
    for (auto url : bad) check(!youtube_stream_url(url), "media origin/path/header boundary");
    u8 range_head[kCdnMaximumRequestHeadBytes];
    const auto url = utf8_literal("https://rr1.googlevideo.com/videoplayback?id=v");
    const u32 range_bytes = embed_stream_get(CdnHost::google_video, url, 4096, 4096, {range_head, sizeof(range_head)});
    const Utf8View head_text{reinterpret_cast<const char*>(range_head), range_bytes};
    check(range_bytes && ascii_contains_ignore_case(head_text, utf8_literal("Range: bytes=4096-8191\r\n")) &&
          !ascii_contains_ignore_case(head_text, utf8_literal("Cookie:")) &&
          !ascii_contains_ignore_case(head_text, utf8_literal("Authorization:")) &&
          !ascii_contains_ignore_case(head_text, utf8_literal("Visitor")), "media requests are ranged and credential-free");
    range_head[0] = 42;
    check(!embed_stream_get(CdnHost::google_video, url, 0, 0, {range_head, sizeof(range_head)}) && range_head[0] == 42,
          "provider download cannot silently become whole-file GET");
    check(!embed_stream_get(CdnHost::google_video, url, ~u64{0}, 4096, {range_head, sizeof(range_head)}), "range overflow rejected");
    source = response(video360, audio);
    check(youtube_player_streams(source, id, 480, {scratch, 1}, &result) == E::capacity,
          "caller bounds structural memory");
    u8 request[2048];
    constexpr auto processed= utf8_literal(R"({"url":"https://rr1.googlevideo.com/videoplayback?id=drc","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":128011,"audioChannels":2,"isDrc":true})");
    check(decode(response(video720,audio,processed),720)==E::none && result.audio.bitrate==128000,
        "original audio wins over a slightly larger dynamic-range-compressed alternative");
    check(decode(response(video720,processed,audio),720)==E::none && result.audio.bitrate==128000,
        "original audio selection is independent of response ordering");
    check(decode(response(video720,processed),720)==E::none && result.audio.bitrate==128011,
        "processed audio remains usable when no original representation is offered");
    check(decode(response(video720,processed,high_audio),720)==E::none && result.audio.bitrate==258000,
        "high-quality original AAC wins over processed audio");
    const u32 bytes = youtube_player_body(id, utf8_literal("public-visitor"), {request, sizeof(request)});
    check(bytes && bytes < 512, "compact native player request");
    const Utf8View request_body{reinterpret_cast<const char*>(request),bytes};
    check(ascii_contains_ignore_case(request_body,utf8_literal("\"clientName\":\"VISIONOS\"")) &&
        ascii_contains_ignore_case(request_body,utf8_literal("\"clientVersion\":\"1.02\"")) &&
        ascii_contains_ignore_case(request_body,kYoutubeNativeUserAgent),"player body and CDN user agent use one matching delivery profile");
    request[0] = 42;
    check(!youtube_player_body(id, utf8_literal("public-visitor"), {request, 1}) && request[0] == 42,
          "request capacity failure is transactional");
    check(!youtube_player_body(id, utf8_literal("bad\r\nvisitor"), {request, sizeof(request)}),
          "bootstrap metadata cannot inject headers");
    ExitProcess(failures ? 1 : 0);
}
