#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/media/asset.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, scratch[2048];
constexpr auto id = utf8_literal("255145362"), country = utf8_literal("us");
constexpr auto url = utf8_literal("https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/test/sample.m4a");
constexpr auto metadata = utf8_literal(R"({"resultCount":1,"results":[{"wrapperType":"track","kind":"song","trackId":255145362,"trackTimeMillis":300000,"trackViewUrl":"https://music.apple.com/us/album/imagine/255144028?i=255145362","previewUrl":"https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/test/sample.m4a"}]})");
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
void run() noexcept {
    using E = EmbedPlayerError;
    const Utf8View urls[]{utf8_literal("https://music.apple.com/us/album/imagine/255144028?i=255145362&uo=4"),
        utf8_literal("https://music.apple.com/us/song/imagine/255145362"),
        utf8_literal("https://embed.music.apple.com/US/album/imagine/255144028?i=255145362")};
    for (auto value : urls) {
        const auto track = apple_music_track_locator(value);
        u8 request[64];
        const auto bytes = apple_music_track_target(track.id, track.country, {request, sizeof(request)});
        check(utf8_equal(track.id, id) && utf8_equal({reinterpret_cast<const char*>(request), bytes},
            utf8_literal("/lookup?id=255145362&country=us")), "single song identity and storefront normalize without searching");
    }
    const Utf8View bad[]{utf8_literal("https://music.apple.com/us/album/imagine/255144028"),
        utf8_literal("https://music.apple.com/us/playlist/name/pl.123?i=255145362"),
        utf8_literal("https://music.apple.com/us/music-video/name/255145362?i=7"),
        utf8_literal("https://music.apple.com/us/song/name/255145362?i=7"),
        utf8_literal("https://music.apple.com/us/album/name/255144028?i=7&i=8"),
        utf8_literal("https://music.apple.com.evil.test/us/song/name/255145362"),
        utf8_literal("https://music.apple.com/usa/song/name/255145362"),
        utf8_literal("https://music.apple.com/us/song/name/18446744073709551616")};
    for (auto value : bad) check(!apple_music_track_locator(value).id.bytes, "ambiguous and non-song URLs do not claim preview playback");
    EmbedStream stream{};
    check(apple_music_track_preview(metadata, id, country, {scratch, 2048}, &stream) == E::none &&
          utf8_equal({stream.url, stream.url_bytes}, url) && !stream.content_bytes && !stream.width && !stream.height,
          "matching song preview has no fabricated duration or image metadata");
    const auto prior = stream;
    const auto reject = [&](Utf8View json, E expected, Utf8View storefront = country) noexcept {
        check(apple_music_track_preview(json, id, storefront, {scratch, 2048}, &stream) == expected &&
              bytes_equal(&stream, &prior, sizeof(stream)), "rejected metadata preserves the selected source");
    };
    reject(metadata, E::malformed, utf8_literal("gb"));
    reject(utf8_literal(R"({"resultCount":0,"results":[]})"), E::unavailable);
    reject(utf8_literal(R"({"resultCount":1,"results":[]})"), E::malformed);
    reject(utf8_literal(R"({"resultCount":2,"results":[]})"), E::malformed);
    reject(utf8_literal(R"({"resultCount":1,"result\u0043ount":1,"results":[]})"), E::malformed);
    reject(utf8_literal(R"({"resultCount":1,"results":[{"wrapperType":"track","kind":"song","trackId":9}]})"), E::malformed);
    reject(utf8_literal(R"({"resultCount":1,"results":[{"wrapperType":"track","kind":"song","trackId":255145362}]})"), E::unsupported);
    check(apple_music_track_preview(metadata, id, country, {scratch, 1}, &stream) == E::capacity, "bounded metadata scratch");
    check(!embed_stream_location(CdnHost::apple_music_preview, utf8_literal("https://audio-ssl.itunes.apple.com.evil.test/itunes-assets/AudioPreview221/test/sample.m4a")) &&
          !embed_stream_location(CdnHost::apple_music_preview, utf8_literal("https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/../sample.m4a")) &&
          !embed_stream_location(CdnHost::apple_music_preview, utf8_literal("https://audio-ssl.itunes.apple.com/itunes-assets/Music221/sample.m4a")) &&
          !embed_stream_location(CdnHost::apple_music_preview, utf8_literal("https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/sample.m4p")),
          "strict public preview CDN path boundary");
    constexpr char head[] = "HTTP/1.1 200 OK\r\nContent-Type: audio/x-m4p\r\nContent-Length: 16\r\n\r\n";
    const ConstSlice<u8> bytes{reinterpret_cast<const u8*>(head), sizeof(head) - 1};
    check(validate_static_image_response(CdnImageType::m4a, bytes, 16, 1000, CdnHost::apple_music_preview).ok() &&
          !validate_static_image_response(CdnImageType::m4a, bytes, 16, 1000).ok() &&
          !validate_static_image_response(CdnImageType::mp3, bytes, 16, 1000, CdnHost::apple_music_preview).ok() &&
          !validate_static_image_response(CdnImageType::m4a, bytes, 16, 1000, CdnHost::spotify_preview).ok(),
          "legacy MIME alias is limited to Apple's validated AAC preview source");
    const auto key = media_asset_search_embed(1, 2, 9, MediaEmbedResource::video, MediaAttachmentFormat::m4a,
                                             false, MediaResolutionTier::px512, true);
    check(media_asset_key_valid(key) && media_asset_audio_format(key) == MediaAttachmentFormat::m4a &&
          media_asset_resolution_tier(key) == MediaResolutionTier::original && media_asset_search_snapshot(key),
          "AAC preview retains exact typed search and snapshot identity");
    constexpr auto video_id = utf8_literal("1445828315");
    constexpr auto video_source = utf8_literal("https://music.apple.com/us/music-video/name/1445828315");
    constexpr auto video_url = utf8_literal("https://video-ssl.itunes.apple.com/itunes-assets/Video128/test/sample.m4v");
    constexpr auto video_metadata = utf8_literal(R"({"resultCount":1,"results":[{"wrapperType":"track","kind":"music-video","trackId":1445828315,"trackTimeMillis":180805,"trackViewUrl":"https://music.apple.com/us/music-video/name/1445828315","previewUrl":"https://video-ssl.itunes.apple.com/itunes-assets/Video128/test/sample.m4v"}]})");
    const auto located = apple_music_track_locator(video_source);
    check(located.video && utf8_equal(located.id, video_id) && utf8_equal(located.country, country),
          "music-video identity remains distinct from a song with the same numeric id");
    check(apple_music_track_preview(video_metadata, video_id, country, {scratch, 2048}, &stream, true) == E::none &&
          utf8_equal({stream.url, stream.url_bytes}, video_url) && !stream.content_bytes && !stream.width && !stream.height,
          "public music-video preview selects its M4V source without inventing full-video duration or dimensions");
    const auto selected_video = stream;
    check(apple_music_track_preview(video_metadata, video_id, country, {scratch, 2048}, &stream) == E::malformed &&
          apple_music_track_preview(metadata, id, country, {scratch, 2048}, &stream, true) == E::malformed &&
          bytes_equal(&stream, &selected_video, sizeof(stream)),
          "audio and video preview metadata cannot be substituted across typed requests");
    check(embed_stream_location(CdnHost::apple_music_video_preview, video_url) &&
          !embed_stream_location(CdnHost::apple_music_preview, video_url) &&
          !embed_stream_location(CdnHost::apple_music_video_preview, url) &&
          !embed_stream_location(CdnHost::apple_music_video_preview, utf8_literal("https://video-ssl.itunes.apple.com.evil.test/itunes-assets/Video128/test/sample.m4v")) &&
          !embed_stream_location(CdnHost::apple_music_video_preview, utf8_literal("https://video-ssl.itunes.apple.com/itunes-assets/Video128/../sample.m4v")),
          "M4V preview has an exact provider host and path separate from audio");
    constexpr char video_head[] = "HTTP/1.1 206 Partial Content\r\nContent-Type: video/x-m4v\r\nContent-Length: 16\r\nContent-Range: bytes 16-31/64\r\n\r\n";
    const ConstSlice<u8> video_bytes{reinterpret_cast<const u8*>(video_head), sizeof(video_head) - 1};
    check(validate_cdn_range_response(CdnImageType::mp4, video_bytes, 16, {16, 16}, 1000, CdnHost::apple_music_video_preview).ok() &&
          !validate_cdn_range_response(CdnImageType::mp4, video_bytes, 16, {16, 16}, 1000, CdnHost::google_video).ok() &&
          !validate_cdn_range_response(CdnImageType::m4a, video_bytes, 16, {16, 16}, 1000, CdnHost::apple_music_video_preview).ok(),
          "M4V MIME alias remains scoped to validated Apple video ranges");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_apple_music_player_entry() noexcept {
    maicord::run(); ExitProcess(maicord::failures ? 1 : 0);
}
