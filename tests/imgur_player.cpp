#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool ok,const char* label) noexcept {test_check(&failures,ok,label);}
void run() noexcept {
    const Utf8View forms[]{utf8_literal("https://imgur.com/tfgwzwf"),utf8_literal("https://www.imgur.com/tfgwzwf/embed?pub=true"),
        utf8_literal("https://i.imgur.com/tfgwzwf.gifv"),utf8_literal("https://i.imgur.com/tfgwzwf.mp4")};
    for(auto url:forms) {
        const auto selected=embed_video_locator(url);
        check(selected.provider==EmbedProvider::imgur && utf8_equal(selected.id,utf8_literal("tfgwzwf")),
            "single Imgur page iframe GIFV and MP4 preserve the same file identity");
    }
    const Utf8View invalid[]{utf8_literal("http://imgur.com/tfgwzwf"),utf8_literal("https://imgur.com.evil.test/tfgwzwf"),
        utf8_literal("https://imgur.com@evil.test/tfgwzwf"),utf8_literal("https://imgur.com:443/tfgwzwf"),
        utf8_literal("https://imgur.com/a/tfgwzwf"),utf8_literal("https://imgur.com/gallery/tfgwzwf"),
        utf8_literal("https://imgur.com/tfgwzwf/embed/extra"),utf8_literal("https://imgur.com/tfgwzwf.jpg"),
        utf8_literal("https://imgur.com/tfg%77zwf"),utf8_literal("https://imgur.com/tfg_wzf"),
        utf8_literal("https://imgur.com/"),utf8_literal("https://imgur.com/tfgwzwff")};
    for(auto url:invalid) check(!embed_video_locator(url).id.bytes,"albums unsafe hosts and ambiguous Imgur file paths rejected");
    EmbedStreams streams{};
    check(imgur_video_streams(utf8_literal("tfgwzwf"),&streams)==EmbedPlayerError::none && streams.combined &&
        streams.expires_seconds==3600 && !streams.audio.url_bytes && !streams.video.content_bytes &&
        utf8_equal({streams.video.url,streams.video.url_bytes},utf8_literal("https://i.imgur.com/tfgwzwf.mp4")),
        "Imgur mapping uses one native MP4 with length and codecs learned from the bounded media probe");
    const auto prior=streams;
    check(imgur_video_streams(utf8_literal("../bad"),&streams)==EmbedPlayerError::invalid_input &&
        bytes_equal(&prior,&streams,sizeof(streams)),"failed Imgur selection preserves output");
    check(imgur_video_id(utf8_literal("Ab123")) && !imgur_video_id({}),"legacy short ID and empty input bounds");
    EmbedStreamLocation location{};
    check(embed_stream_location(CdnHost::imgur_video,{streams.video.url,streams.video.url_bytes},&location) &&
        utf8_equal(location.host,utf8_literal("i.imgur.com")) && utf8_equal(location.target,utf8_literal("/tfgwzwf.mp4")),
        "only the canonical Imgur MP4 CDN path is admitted");
    const Utf8View bad_media[]{utf8_literal("https://imgur.com/tfgwzwf.mp4"),utf8_literal("https://i.imgur.com.evil.test/tfgwzwf.mp4"),
        utf8_literal("https://i.imgur.com/removed.png"),utf8_literal("https://i.imgur.com/tfgwzwf.gifv"),
        utf8_literal("https://i.imgur.com/tfgwzwf.mp4?redirect=1"),utf8_literal("https://i.imgur.com/tfgwzwf.mp4#fragment"),
        utf8_literal("https://i.imgur.com/a/tfgwzwf.mp4"),utf8_literal("https://i.imgur.com/../tfgwzwf.mp4")};
    for(auto url:bad_media) check(!embed_stream_location(CdnHost::imgur_video,url),"Imgur media cannot escape its exact host file type or path");
    u8 bytes[1024]{};
    const u32 length=embed_stream_get(CdnHost::imgur_video,{streams.video.url,streams.video.url_bytes},4096,1024,{bytes,sizeof(bytes)});
    const Utf8View request{reinterpret_cast<const char*>(bytes),length};
    check(length && ascii_contains_ignore_case(request,utf8_literal("Range: bytes=4096-5119")) &&
        !ascii_contains_ignore_case(request,utf8_literal("Cookie:")) && !ascii_contains_ignore_case(request,utf8_literal("Authorization:")),
        "Imgur ranges use the shared credential-free HTTP writer");
    check(!embed_stream_get(CdnHost::imgur_video,{streams.video.url,streams.video.url_bytes},0,0,{bytes,sizeof(bytes)}),
        "Imgur cannot request an unbounded media body");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_imgur_player_entry() noexcept {
    maicord::run();ExitProcess(maicord::failures?1:0);
}
