#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"
namespace maicord {
namespace {
u32 failures;
void check(bool ok,const char* label) noexcept {test_check(&failures,ok,label);}
void run() noexcept {
    const Utf8View forms[]{utf8_literal("https://music.amazon.com/tracks/B000VBH2R2"),
        utf8_literal("https://music.amazon.de/tracks/B000VBH2R2"),
        utf8_literal("https://music.amazon.co.uk/embed/B000VBH2R2/?id=widget"),
        utf8_literal("https://music.amazon.com/albums/B000VBJ504?trackAsin=B000VBH2R2"),
        utf8_literal("https://music.amazon.com/albums/B000VBJ504?trackAsin=B000VBH2R2&iframe_url=https%3A%2F%2Fmusic.amazon.com%2Fembed%2FB000VBH2R2%2F%3Fid%3Dwidget")};
    for(auto url:forms) check(utf8_equal(amazon_music_track_locator(url).id,utf8_literal("B000VBH2R2")),
        "track album-selection and nested widget preserve one Amazon track identity");
    const Utf8View bad[]{utf8_literal("https://music.amazon.com/albums/B000VBJ504"),
        utf8_literal("https://music.amazon.com.evil.test/tracks/B000VBH2R2"),
        utf8_literal("https://music.amazon.com:443/tracks/B000VBH2R2"),
        utf8_literal("https://music.amazon.com/playlists/B000VBH2R2"),
        utf8_literal("https://music.amazon.com/tracks/b000vbh2r2"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2/extra"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2?trackAsin=B076YT2CBT"),
        utf8_literal("https://music.amazon.com/albums/B000VBJ504?trackAsin=B000VBH2R2&trackAsin=B000VBH2R2"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2?iframe_url=https%3A%2F%2Fevil.test%2Fembed%2FB000VBH2R2"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2?iframe_url=https%3A%2F%2Fmusic.amazon.de%2Fembed%2FB000VBH2R2"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2?iframe_url=https%3A%2F%2Fmusic.amazon.com%2Fembed%2FB076YT2CBT"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2?iframe_url=%00"),
        utf8_literal("https://music.amazon.com/tracks/B000VBH2R2?iframe_url=%GG")};
    for(auto url:bad) check(!amazon_music_track_locator(url).id.bytes,"ambiguous identity territory nested frame or host rejected");
    EmbedStreamLocation location{};
    check(amazon_music_redirect(utf8_literal("https://d2usgkmxlnua9y.cloudfront.net/US/B000VBH2R2"),utf8_literal("B000VBH2R2"),&location) &&
        utf8_equal(location.target,utf8_literal("/US/B000VBH2R2")),"first redirect binds exact distribution path and track");
    check(!amazon_music_redirect(utf8_literal("https://d2usgkmxlnua9y.cloudfront.net/US/B076YT2CBT"),utf8_literal("B000VBH2R2"),&location) &&
        !amazon_music_redirect(utf8_literal("https://other.cloudfront.net/US/B000VBH2R2"),utf8_literal("B000VBH2R2"),&location),
        "redirect cannot change track or CloudFront tenant");
    constexpr auto media=utf8_literal("https://d28julafmv4ekl.cloudfront.net/64%2F30%2F123456789_S64.mp3?response-content-type=audio%2Fmpeg&Expires=2000&Signature=synthetic&Key-Pair-Id=test");
    EmbedStreams streams{};
    check(amazon_music_preview_stream(media,1000,&streams)==EmbedPlayerError::none && streams.expires_seconds==1000 &&
        !streams.video.url_bytes && !streams.audio.content_bytes && utf8_equal({streams.audio.url,streams.audio.url_bytes},media),
        "preview selects one expiring MP3 with length validated by native bounded fetch");
    const auto before=streams;
    constexpr auto regional=utf8_literal("https://d2q1srilgjznst.cloudfront.net/64%2F30%2F123456789012345_S64.mp3?Expires=2000");
    check(amazon_music_preview_stream(regional,1000,&streams)==EmbedPlayerError::none &&
        utf8_equal({streams.audio.url,streams.audio.url_bytes},regional),"regional preview distribution uses the same bounded native MP3 path");
    streams=before;
    check(amazon_music_preview_stream(media,2000,&streams)==EmbedPlayerError::unavailable && bytes_equal(&before,&streams,sizeof(streams)),
        "expired preview leaves selection untouched");
    const Utf8View bad_media[]{utf8_literal("https://evil.cloudfront.net/64%2F30%2F123_S64.mp3?Expires=2000"),
        utf8_literal("https://d2q1srilgjznst.cloudfront.net.evil.test/64%2F30%2F123_S64.mp3?Expires=2000"),
        utf8_literal("https://d28julafmv4ekl.cloudfront.net/64%2F30%2F..%2F123_S64.mp3?Expires=2000"),
        utf8_literal("https://d28julafmv4ekl.cloudfront.net/64%2F30%2F123_S64.mp3?Expires=2000&Expires=3000"),
        utf8_literal("https://d28julafmv4ekl.cloudfront.net/64%2F30%2F123_S64.mp3?Expires=2000#fragment"),
        utf8_literal("https://d28julafmv4ekl.cloudfront.net/64%2F30%2F123_S64.mp4?Expires=2000")};
    for(auto url:bad_media) check(!embed_stream_location(CdnHost::amazon_music_preview,url),"preview path type host and expiry boundary");
    u8 bytes[1024];const auto n=embed_stream_get(CdnHost::amazon_music_preview,media,0,0,{bytes,sizeof(bytes)});
    const Utf8View head{reinterpret_cast<const char*>(bytes),n};
    check(n && embed_audio_preview_limit(CdnHost::amazon_music_preview)==2*1024*1024 &&
        !ascii_contains_ignore_case(head,utf8_literal("Authorization:")) && !ascii_contains_ignore_case(head,utf8_literal("Cookie:")),
        "bounded whole-preview MP3 GET carries no account credentials");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_amazon_music_player_entry() noexcept {
    maicord::run();ExitProcess(maicord::failures?1:0);
}
