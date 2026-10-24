#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[4096];
u8 json[8192];
constexpr auto id = utf8_literal("IncredulousAbstemiousFennelImGlitch");
constexpr auto claims = utf8_literal(R"({"clip_slug":"IncredulousAbstemiousFennelImGlitch","authorization":{"forbidden":false},"expires":2000})");
constexpr auto quality = utf8_literal(R"({"videoCodec":"AVC","height":720,"width":1280,"bitrate":0,"sourceURL":"https://d1ndex63qxojbr.cloudfront.net/nauth/test/landscape/avc/720/index.mp4"})");
void check(bool v, const char* name) noexcept { test_check(&failures, v, name); }
Utf8View response(Utf8View rendition = quality, Utf8View token = claims, Utf8View slug = id) noexcept {
    ProtocolWriter w{{json, sizeof(json)}, 0};
    protocol_writer_append_literal(&w, "{\"data\":{\"clip\":{\"slug\":");
    protocol_writer_append_json_string(&w, slug);
    protocol_writer_append_literal(&w, ",\"playbackAccessToken\":{\"signature\":\"0123456789abcdef\",\"value\":");
    protocol_writer_append_json_string(&w, token);
    protocol_writer_append_literal(&w, "},\"assets\":[{\"videoQualities\":[");
    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(rendition.data), rendition.bytes);
    protocol_writer_append_literal(&w, "]}]}}}");
    return {reinterpret_cast<const char*>(json), w.used};
}
void run() noexcept {
    using E = EmbedPlayerError;
    const Utf8View urls[]{utf8_literal("https://clips.twitch.tv/IncredulousAbstemiousFennelImGlitch"),
        utf8_literal("https://clips.twitch.tv/embed?clip=IncredulousAbstemiousFennelImGlitch&parent=discord.com"),
        utf8_literal("https://www.twitch.tv/twitchdev/clip/IncredulousAbstemiousFennelImGlitch")};
    for (auto url : urls) {
        const auto locator = embed_video_locator(url);
        check(locator.provider == EmbedProvider::twitch && utf8_equal(locator.id, id), "clip identity across page and iframe forms");
    }
    const Utf8View bad[]{utf8_literal("https://player.twitch.tv/?channel=twitchdev&video=v123"),
        utf8_literal("https://www.twitch.tv/videos/0123"), utf8_literal("https://clips.twitch.tv/a/b"),
        utf8_literal("https://clips.twitch.tv/embed?clip=a&clip=b"),
        utf8_literal("https://clips.twitch.tv.evil.test/test"), utf8_literal("https://clips.twitch.tv/%22")};
    for (auto url : bad) check(!embed_video_locator(url).id.bytes, "only unambiguous clips use progressive playback");
    EmbedStreams out{};
    check(twitch_clip_streams(response(), id, 720, 1000, {scratch, 4096}, &out) == E::none &&
          out.combined && out.video.height == 720 && out.expires_seconds == 1000 &&
          ascii_contains_ignore_case({out.video.url, out.video.url_bytes}, utf8_literal("&token=%7B%22clip_slug%22")),
          "bounded AVC source with encoded short-lived access grant");
    const auto before = out;
    const auto rejects = [&](Utf8View text, E expected, u64 now = 1000) noexcept {
        check(twitch_clip_streams(text, id, 720, now, {scratch, 4096}, &out) == expected &&
              bytes_equal(&before, &out, sizeof(out)), "transactional clip rejection");
    };
    rejects(response(), E::unavailable, 2000);
    rejects(response(quality, claims, utf8_literal("DifferentClip")), E::malformed);
    rejects(response(quality, utf8_literal(R"({"clip_slug":"DifferentClip","authorization":{"forbidden":false},"expires":2000})")), E::malformed);
    rejects(response(quality, utf8_literal(R"({"clip_slug":"IncredulousAbstemiousFennelImGlitch","authorization":{"forbidden":true},"expires":2000})")), E::unavailable);
    rejects(response(utf8_literal(R"({"videoCodec":"HEVC"})")), E::unsupported);
    rejects(response(utf8_literal(R"({"videoCodec":"AVC","videoCodec":"AVC"})")), E::malformed);
    rejects(utf8_literal(R"({"data":{"clip":null}})"), E::unavailable);
    rejects(utf8_literal(R"({"errors":[]})"), E::unavailable);
    check(twitch_clip_streams(response(), id, 720, 1000, {scratch, 1}, &out) == E::capacity, "bounded parser scratch");
    u8 request[4096];
    const u32 body = twitch_clip_body(id, {request, sizeof(request)});
    check(body && ascii_contains_ignore_case({reinterpret_cast<char*>(request), body}, utf8_literal("supportedCodecs:[AVC]")), "explicit AVC metadata query");
    request[0] = 42;
    check(!twitch_clip_body(utf8_literal("invalid\"slug"), {request, sizeof(request)}) && request[0] == 42, "no query injection");
    const auto bytes = embed_stream_get(CdnHost::twitch_video, {out.video.url, out.video.url_bytes},
                                       1048576, 4096, {request, sizeof(request)});
    const Utf8View head{reinterpret_cast<char*>(request), bytes};
    check(bytes && ascii_contains_ignore_case(head, utf8_literal("Range: bytes=1048576-1052671\r\n")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Authorization:")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Client-ID:")) &&
          !ascii_contains_ignore_case(head, utf8_literal("Cookie:")), "no metadata or account credentials on clip CDN");
    check(!embed_stream_location(CdnHost::twitch_video, utf8_literal("https://other.cloudfront.net/nauth/a.mp4")) &&
          !embed_stream_location(CdnHost::twitch_video, utf8_literal("https://d1ndex63qxojbr.cloudfront.net/other/a.mp4")) &&
          !embed_stream_location(CdnHost::twitch_video, utf8_literal("https://d1ndex63qxojbr.cloudfront.net/nauth/a.m3u8")),
          "specific distribution and MP4 path fence");
    constexpr char binary[] = "HTTP/1.1 206 Partial Content\r\nContent-Type: binary/octet-stream\r\nContent-Length: 16\r\nContent-Range: bytes 0-15/1000\r\n\r\n";
    const ConstSlice<u8> binary_head{reinterpret_cast<const u8*>(binary), sizeof(binary) - 1};
    check(validate_cdn_range_response(CdnImageType::mp4, binary_head, 16, {0, 16}, 1000, CdnHost::twitch_video).ok() &&
          !validate_cdn_range_response(CdnImageType::mp4, binary_head, 16, {0, 16}, 1000).ok() &&
          !validate_cdn_range_response(CdnImageType::mp4, binary_head, 16, {0, 16}, 1000, CdnHost::google_video).ok(),
          "binary MP4 admission is restricted to the validated Twitch range source");
}
void vod() noexcept {
    using E = EmbedPlayerError;
    constexpr auto vod_id = utf8_literal("2908640776");
    constexpr Utf8View urls[]{utf8_literal("https://www.twitch.tv/videos/2908640776?t=1h2m3s"),
        utf8_literal("https://player.twitch.tv/?video=v2908640776&time=1h2m3s&parent=discord.com")};
    for (auto url : urls) {
        const auto located = embed_video_locator(url);
        check(located.provider == EmbedProvider::twitch_vod && utf8_equal(located.id, vod_id) && located.start_seconds == 3723,
            "Twitch VOD page and iframe share an unambiguous identity and start time");
    }
    check(!embed_video_locator(utf8_literal("https://player.twitch.tv/?video=v123&channel=twitch")).id.bytes &&
        !embed_video_locator(utf8_literal("https://player.twitch.tv/?video=v123&video=v456")).id.bytes &&
        !embed_video_locator(utf8_literal("https://player.twitch.tv/?video=v123&t=3s&time=4s")).id.bytes &&
        !embed_video_locator(utf8_literal("https://player.twitch.tv/?video=v123&time=3s&time=3s")).id.bytes,
        "Twitch player rejects conflicting VOD identities");
    constexpr auto token = utf8_literal(R"({"vod_id":2908640776,"expires":2000,"authorization":{"forbidden":false}})");
    const auto make = [&](Utf8View value) noexcept {
        ProtocolWriter w{{json, sizeof(json)}, 0};
        protocol_writer_append_literal(&w, "{\"data\":{\"videoPlaybackAccessToken\":{\"signature\":\"0123456789abcdef0123456789abcdef01234567\",\"value\":");
        protocol_writer_append_json_string(&w, value);
        protocol_writer_append_literal(&w, "}}}");
        return Utf8View{reinterpret_cast<const char*>(json), w.used};
    };
    u8 body[1024]{};
    const u32 count = twitch_playback_body(vod_id, {body, sizeof(body)});
    check(count && ascii_contains_ignore_case({reinterpret_cast<char*>(body), count}, utf8_literal("videoPlaybackAccessToken")),
        "Twitch VOD metadata request uses the requested identity");
    body[0] = 42;
    check(!twitch_playback_body(utf8_literal("12\"3"), {body, sizeof(body)}) && body[0] == 42 &&
        !twitch_vod_id(utf8_literal("0")) && !twitch_vod_id(utf8_literal("0123")) &&
        !twitch_vod_id(utf8_literal("18446744073709551616")), "Twitch VOD identity is canonical and bounded");
    EmbedStream stream{};
    check(twitch_playback_manifest(make(token), vod_id, 1000, {scratch, 4096}, &stream) == E::none &&
        ascii_contains_ignore_case({stream.url, stream.url_bytes}, utf8_literal("https://usher.ttvnw.net/vod/2908640776.m3u8?")) &&
        ascii_contains_ignore_case({stream.url, stream.url_bytes}, utf8_literal("nauth=%7B")),
        "Twitch VOD token is percent-encoded into its exact manifest URL");
    const auto saved = stream;
    check(twitch_playback_manifest(make(token), vod_id, 2000, {scratch, 4096}, &stream) == E::unavailable &&
        bytes_equal(&stream, &saved, sizeof(saved)), "expired VOD tokens preserve caller output");
    check(twitch_playback_manifest(make(token), utf8_literal("123"), 1000, {scratch, 4096}, &stream) == E::malformed,
        "Twitch VOD token cannot authorize a different identity");
    check(twitch_playback_manifest(make(utf8_literal(R"({"vod_id":2908640776,"expires":2000,"authorization":{"forbidden":true}})")),
        vod_id, 1000, {scratch, 4096}, &stream) == E::unavailable, "Twitch VOD denied authorization is respected");
    check(twitch_vod_location(utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/480p30/index-dvr.m3u8"), true) &&
        twitch_vod_location(utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/audio_only/0-muted.mp4"), false),
        "Twitch VOD admits the observed playlist and clear media distribution");
    constexpr Utf8View bad[]{utf8_literal("https://other.cloudfront.net/recording/audio_only/0.mp4"),
        utf8_literal("https://d2nvs31859zcd8.cloudfront.net.evil.test/recording/audio_only/0.mp4"),
        utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/../0.mp4"),
        utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/audio_only/0.mp4?token=x")};
    for (auto url : bad) check(!twitch_vod_location(url, false), "Twitch VOD rejects distribution and path confusion");
    constexpr char video_head[] = "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Length: 16\r\nContent-Range: bytes 0-15/1000\r\n\r\n";
    const ConstSlice<u8> head{reinterpret_cast<const u8*>(video_head), sizeof(video_head) - 1};
    check(validate_cdn_range_response(CdnImageType::m4a, head, 16, {0, 16}, 1000, CdnHost::twitch_vod).ok() &&
        !validate_cdn_range_response(CdnImageType::m4a, head, 16, {0, 16}, 1000, CdnHost::twitch_video).ok(),
        "only the VOD audio rendition admits its observed video/mp4 content type");
}
void live() noexcept {
    using E = EmbedPlayerError;
    constexpr Utf8View links[]{utf8_literal("https://www.twitch.tv/Example_Channel"),
        utf8_literal("https://twitch.tv/Example_Channel/"), utf8_literal("https://player.twitch.tv/?channel=Example_Channel&parent=discord.com")};
    for (auto link : links) {
        const auto found = embed_video_locator(link);
        check(found.provider == EmbedProvider::twitch_live && utf8_equal(found.id, utf8_literal("Example_Channel")) && !found.start_seconds,
            "channel page and player iframe select one native live channel");
    }
    constexpr Utf8View invalid[]{utf8_literal("https://twitch.tv/directory"), utf8_literal("https://twitch.tv/Downloads"),
        utf8_literal("https://twitch.tv/example/videos"), utf8_literal("https://player.twitch.tv/?channel=a&channel=b"),
        utf8_literal("https://player.twitch.tv/?channel=a&video=v123"), utf8_literal("https://player.twitch.tv/?channel=a&clip=b"),
        utf8_literal("https://player.twitch.tv/?channel=a%2fb")};
    for (auto link : invalid) check(!embed_video_locator(link).id.bytes, "live channel admission rejects other surfaces and conflicting identities");
    constexpr auto channel = utf8_literal("Example_Channel");
    const auto make = [&](Utf8View value) noexcept {
        ProtocolWriter w{{json, sizeof(json)}, 0};
        protocol_writer_append_literal(&w, "{\"data\":{\"streamPlaybackAccessToken\":{\"signature\":\"0123456789abcdef0123456789abcdef01234567\",\"value\":");
        protocol_writer_append_json_string(&w, value); protocol_writer_append_literal(&w, "}}}");
        return Utf8View{reinterpret_cast<const char*>(json), w.used};
    };
    constexpr auto token = utf8_literal(R"({"channel":"example_channel","expires":2000,"authorization":{"forbidden":false}})");
    u8 body[4096]{};
    const u32 count = twitch_playback_body(channel, {body, sizeof(body)}, true);
    check(count && ascii_contains_ignore_case({reinterpret_cast<char*>(body), count}, utf8_literal("streamPlaybackAccessToken(channelName:")),
        "live channel metadata uses the bounded channel identity");
    body[0] = 42;
    check(!twitch_channel_id(utf8_literal("a/b")) && !twitch_channel_id(utf8_literal("a\"b")) &&
        !twitch_channel_id(utf8_literal("abcdefghijklmnopqrstuvwxyz")) &&
        !twitch_playback_body(channel, {body, 1}, true) && body[0] == 42, "live request failures preserve the output");
    EmbedStream stream{};
    check(twitch_playback_manifest(make(token), channel, 1000, {scratch, 4096}, &stream, true) == E::none &&
        ascii_contains_ignore_case({stream.url, stream.url_bytes}, utf8_literal("https://usher.ttvnw.net/api/channel/hls/Example_Channel.m3u8?")) &&
        ascii_contains_ignore_case({stream.url, stream.url_bytes}, utf8_literal("&token=%7B")), "live token is bound case-insensitively to its channel and percent-encoded");
    const auto saved = stream;
    check(twitch_playback_manifest(make(token), channel, 2000, {scratch, 4096}, &stream, true) == E::unavailable &&
        bytes_equal(&stream, &saved, sizeof(saved)) &&
        twitch_playback_manifest(make(token), utf8_literal("another_channel"), 1000, {scratch, 4096}, &stream, true) == E::malformed,
        "expired or different-channel grants cannot replace live output");
    check(twitch_playback_manifest(make(utf8_literal(R"({"channel":"example_channel","expires":2000,"authorization":{"forbidden":true}})")),
        channel, 1000, {scratch, 4096}, &stream, true) == E::unavailable, "live denied authorization remains denied");
    constexpr auto playlist = utf8_literal("https://euc12.playlist.ttvnw.net/v1/playlist/abc_DEF-123=.m3u8");
    constexpr auto media = utf8_literal("https://0123456789ab.j.cloudfront.hls.ttvnw.net/v1/segment/abc_DEF-123.mp4?dna=abc%3D");
    check(twitch_live_location(playlist, true) && twitch_live_location(media, false) &&
        !twitch_live_location(playlist, false) && !twitch_live_location(media, true), "live playlists and media have separate owned host and path boundaries");
    constexpr auto transport=utf8_literal("https://0123456789ab.j.cloudfront.hls.ttvnw.net/v1/segment/abc_DEF-123.ts?dna=abc%3D");
    constexpr auto base64 = utf8_literal("https://x.hls.ttvnw.net/v1/segment/abc/DEF+123==.ts?dna=abc%3D");
    check(twitch_live_location(base64, false, nullptr, true) && embed_stream_location(CdnHost::twitch_live, base64),
          "live segment keys admit standard and URL-safe base64 alphabets without changing the CDN origin");
    check(!twitch_live_location(utf8_literal("https://x.hls.ttvnw.net/v1/segment/a/../b.ts"), false, nullptr, true) &&
          !twitch_live_location(utf8_literal("https://x.hls.ttvnw.net/v1/segment/a=b/c.ts"), false, nullptr, true),
          "opaque live keys still reject traversal and nonterminal padding");
    check(twitch_live_location(transport,false,nullptr,true) && !twitch_live_location(transport,false) &&
        !twitch_live_location(media,false,nullptr,true) && embed_stream_location(CdnHost::twitch_live,transport),
        "transport media uses the same owned host boundary with an explicit TS extension");
    constexpr char ts_response[]="HTTP/1.1 200 OK\r\nContent-Type: video/MP2T; codecs=avc1\r\nContent-Length: 188\r\n\r\n";
    const ConstSlice<u8> ts_head{reinterpret_cast<const u8*>(ts_response),sizeof(ts_response)-1};
    check(validate_cdn_range_response(CdnImageType::mpeg_ts,ts_head,188,{0,1024},1000,CdnHost::twitch_live).ok() &&
        !validate_cdn_range_response(CdnImageType::mpeg_ts,ts_head,188,{0,1024},1000,CdnHost::cdn).ok() &&
        !validate_cdn_range_response(CdnImageType::mp4,ts_head,188,{0,1024},1000,CdnHost::twitch_live).ok() &&
        cdn_encoded_body_limit(CdnImageType::mpeg_ts)==8u*1024u*1024u,"transport MIME and response size are confined to the admitted native live source");
    constexpr Utf8View bad[]{utf8_literal("https://euc12.playlist.ttvnw.net.evil.test/v1/playlist/test.m3u8"),
        utf8_literal("https://-x.playlist.ttvnw.net/v1/playlist/test.m3u8"),
        utf8_literal("https://x..playlist.ttvnw.net/v1/playlist/test.m3u8"),
        utf8_literal("https://x.playlist.ttvnw.net:443/v1/playlist/test.m3u8"),
        utf8_literal("https://x.playlist.ttvnw.net/v1/playlist/../test.m3u8"),
        utf8_literal("https://x.playlist.ttvnw.net/v1/playlist/a=b.m3u8"),
        utf8_literal("https://x.playlist.ttvnw.net/v1/playlist/test.m3u8?redirect=x")};
    for (auto url : bad) check(!twitch_live_location(url, true), "live playlist gate rejects host, authority and path confusion");
    check(!twitch_live_location(utf8_literal("https://x.hls.ttvnw.net/v1/segment/a.mp4?dna=x&dna=y"), false) &&
        !twitch_live_location(utf8_literal("https://x.hls.ttvnw.net/v1/segment/a.mp4?dna=%0D%0A"), false) &&
        !twitch_live_location(utf8_literal("https://x.hls.ttvnw.net/v1/segment/a.mp4?other=x"), false), "live media query rejects duplicates, controls and unrelated parameters");
    const u32 request = embed_stream_get(CdnHost::twitch_playlist, playlist, 0, 0, {body, sizeof(body)});
    const Utf8View head{reinterpret_cast<char*>(body), request};
    check(request && !ascii_contains_ignore_case(head, utf8_literal("Range:")) && !ascii_contains_ignore_case(head, utf8_literal("Authorization:")) &&
        !ascii_contains_ignore_case(head, utf8_literal("Client-ID:")) && !embed_stream_get(CdnHost::twitch_playlist, playlist, 0, 64, {body, sizeof(body)}),
        "playlist GET is whole, bounded by its response type, and carries no metadata credentials");
    constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl; charset=utf-8\r\nContent-Length: 16\r\n\r\n";
    const ConstSlice<u8> response_head{reinterpret_cast<const u8*>(response), sizeof(response) - 1};
    check(validate_static_image_response(CdnImageType::hls_playlist, response_head, 16, 1000, CdnHost::twitch_playlist).ok() &&
        !validate_static_image_response(CdnImageType::hls_playlist, response_head, 16, 1000, CdnHost::cdn).ok() &&
        cdn_encoded_body_limit(CdnImageType::hls_playlist) == 65536, "playlist content type is confined to typed Twitch requests with a64KiB bound");
    constexpr char binary[] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 16\r\n\r\n";
    const ConstSlice<u8> binary_head{reinterpret_cast<const u8*>(binary), sizeof(binary) - 1};
    check(validate_cdn_range_response(CdnImageType::mp4, binary_head, 16, {0, 16}, 1000, CdnHost::twitch_live).ok() &&
        validate_cdn_range_response(CdnImageType::m4a, binary_head, 16, {0, 16}, 1000, CdnHost::twitch_live).ok() &&
        !validate_static_image_response(CdnImageType::hls_playlist, binary_head, 16, 1000, CdnHost::twitch_playlist).ok(),
        "live clear container MIME does not admit binary data as a playlist");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_twitch_player_entry() noexcept {
    maicord::run(); maicord::vod(); maicord::live(); ExitProcess(maicord::failures ? 1 : 0);
}
