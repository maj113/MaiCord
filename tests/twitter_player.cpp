#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool ok,const char* label) noexcept {test_check(&failures,ok,label);}
void run() noexcept {
    const Utf8View forms[]{utf8_literal("https://x.com/example/status/123"),
        utf8_literal("https://twitter.com/example/status/123/?s=20"),
        utf8_literal("https://www.x.com/example/status/123/video/1"),
        utf8_literal("https://mobile.twitter.com/example/status/123"),
        utf8_literal("https://twitter.com/i/videos/tweet/123?embed_source=clientlib&autoplay=1"),
        utf8_literal("https://x.com/i/videos/tweet/123"),
        utf8_literal("https://platform.twitter.com/embed/Tweet.html?id=123&lang=en&dnt=true")};
    for(auto url:forms) {
        const auto locator=embed_video_locator(url);
        check(locator.provider==EmbedProvider::twitter && utf8_equal(locator.id,utf8_literal("123")),
            "public post and official iframe preserve identity");
    }
    check(twitter_video_locator(utf8_literal("https://x.com/example/status/123/video/2")).media_index==2,
        "explicit media ordinal survives page parsing");
    const Utf8View invalid[]{utf8_literal("https://x.com"),utf8_literal("https://x.com/"),
        utf8_literal("https://x.com.evil.test/example/status/123"),utf8_literal("http://x.com/example/status/123"),
        utf8_literal("https://x.com:443/example/status/123"),utf8_literal("https://x.com@example.test/example/status/123"),
        utf8_literal("https://x.com/example/status/0123"),utf8_literal("https://x.com/example/status/18446744073709551616"),
        utf8_literal("https://x.com/example/status/123/video/0"),utf8_literal("https://x.com/example/status/123/video/5"),
        utf8_literal("https://x.com/example/status/123/photo/1"),utf8_literal("https://x.com/example/status/123/extra"),
        utf8_literal("https://twitter.com/i/videos/tweet/"),utf8_literal("https://twitter.com/i/videos/tweet/123/extra"),
        utf8_literal("https://twitter.com/i/videos/tweet/0123"),utf8_literal("https://platform.twitter.com/i/videos/tweet/123"),
        utf8_literal("https://platform.twitter.com/embed/Tweet.html?id=123&id=456"),
        utf8_literal("https://platform.twitter.com/example/status/123")};
    for(auto url:invalid) check(!embed_video_locator(url).id.bytes,"ambiguous or unsafe post URL rejected");
    // Oracle: Node's standard Number/toString(36) expression from the public
    // embed protocol. Hash synthetic requests, never emit request proofs.
    u32 hash=2166136261u;
    const auto proof=[&](u64 id) noexcept {
        u8 text[24],target[128];ProtocolWriter w{{text,sizeof(text)},0};
        protocol_writer_append_u64(&w,id);
        const u32 n=twitter_video_target({reinterpret_cast<const char*>(text),w.used},{target,sizeof(target)});
        check(n>0 && n<sizeof(target),"bounded native request proof");
        for(u32 i=0;i<n;++i) hash=(hash^target[i])*16777619u;
    };
    const u64 fixed[]{1,123,9007199254740991ull,9007199254740992ull,9007199254740993ull,~u64{0}};
    for(auto id:fixed) proof(id);
    u64 random=1;
    for(u32 i=0;i<10000;++i) {random=random*6364136223846793005ull+1442695040888963407ull;proof(random);}
    check(hash==3460213407u,"native binary64 proof matches 10006 independent JavaScript results");
    u8 tiny[4]{1,2,3,4};
    check(!twitter_video_target(utf8_literal("123"),{tiny,sizeof(tiny)}) && tiny[0]==1 && tiny[3]==4 &&
        !twitter_video_target(utf8_literal("0"),{tiny,sizeof(tiny)}),"invalid and short proof outputs remain untouched");

    constexpr auto fixture=utf8_literal(R"({"__typename":"Tweet","id_str":"123","mediaDetails":[{"type":"photo"},{"type":"video","original_info":{"width":1920,"height":1080},"ext_media_availability":{"status":"Available"},"video_info":{"variants":[{"content_type":"application/x-mpegURL","url":"https://video.twimg.com/unused.m3u8"},{"content_type":"video/mp4","bitrate":2000000,"url":"https://video.twimg.com/ext_tw_video/456/pu/vid/avc1/1280x720/large.mp4?tag=12"},{"content_type":"video/mp4","bitrate":800000,"url":"https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/small.mp4?tag=12"},{"content_type":"video/mp4","bitrate":900000,"url":"https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/expensive.mp4?tag=12"}]}},{"type":"video","video_info":{"variants":[{"content_type":"video/mp4","url":"https://video.twimg.com/ext_tw_video/789/pu/vid/480x480/square.mp4"}]}}]})");
    u32 scratch[2048];EmbedStreams streams{};
    const auto select=[&](Utf8View body,u16 height=360,u8 index=0) noexcept {
        return twitter_video_streams(body,utf8_literal("123"),height,index,{scratch,2048},&streams);
    };
    check(select(fixture)==EmbedPlayerError::none && streams.combined && streams.expires_seconds==3600 &&
        streams.video.width==640 && streams.video.height==360 && streams.video.bitrate==800000 &&
        !streams.audio.url_bytes && !streams.video.content_bytes,"first own video selects economical rendition within cap");
    check(select(fixture,1080,2)==EmbedPlayerError::none && streams.video.height==720,
        "expanded resolution selects larger bounded rendition from exact media item");
    check(select(fixture,360,3)==EmbedPlayerError::none && streams.video.width==480 && streams.video.height==480,
        "square video uses actual rendition dimensions, including smallest over-cap fallback");
    constexpr auto portrait=utf8_literal(R"({"__typename":"Tweet","id_str":"123","mediaDetails":[{"type":"video","video_info":{"variants":[{"content_type":"video/mp4","bitrate":950000,"url":"https://video.twimg.com/amplify_video/456/vid/avc1/364x640/portrait.mp4?tag=16"},{"content_type":"video/mp4","bitrate":632000,"url":"https://video.twimg.com/amplify_video/456/vid/avc1/320x562/portrait.mp4?tag=16"}]}}]})");
    check(select(portrait)==EmbedPlayerError::none && streams.video.width==320 && streams.video.height==562,
        "portrait Amplify media selects smallest rendition above inline cap without stretching original dimensions");
    const auto prior=streams;
    check(select(fixture,360,1)==EmbedPlayerError::unsupported && select(fixture,360,4)==EmbedPlayerError::unsupported &&
        bytes_equal(&streams,&prior,sizeof(streams)),"photo and missing explicit attachment never substitute another video");
    check(twitter_video_streams(fixture,utf8_literal("456"),360,0,{scratch,2048},&streams)==EmbedPlayerError::malformed &&
        bytes_equal(&streams,&prior,sizeof(streams)),"metadata identity mismatch is transactional");
    const Utf8View unavailable[]{utf8_literal(R"({"__typename":"TweetTombstone"})"),
        utf8_literal(R"({"__typename":"Tweet","id_str":"123","mediaDetails":[{"type":"video","ext_media_availability":{"status":"Unavailable"}}]})")};
    for(auto body:unavailable) check(select(body)==EmbedPlayerError::unavailable,"deleted or unavailable media stays unavailable");
    check(select(utf8_literal(R"({"__typename":"Tweet","id_str":"123","quoted_tweet":{"mediaDetails":[{"type":"video"}]}})"))==EmbedPlayerError::unsupported,
        "quoted post cannot supply the requested post's media");
    check(select(utf8_literal(R"({"__typename":"Tweet","id_str":"123","mediaDetails":[{"type":"video","additional_media_info":{"embeddable":false}}]})"))==EmbedPlayerError::not_embeddable,
        "explicit embed prohibition is preserved");
    check(select(utf8_literal(R"({"__typename":"Tweet","id_str":"123","id_str":"456"})"))==EmbedPlayerError::malformed &&
        twitter_video_streams(fixture,utf8_literal("123"),360,0,{scratch,1},&streams)==EmbedPlayerError::capacity &&
        bytes_equal(&streams,&prior,sizeof(streams)),"duplicate fields and insufficient scratch never publish partial output");
    EmbedStreamLocation location{};
    const Utf8View media{prior.video.url,prior.video.url_bytes};
    check(embed_stream_location(CdnHost::twitter_video,media,&location) &&
        utf8_equal(location.host,utf8_literal("video.twimg.com")),"selected MP4 enters the existing native range lane");
    const Utf8View bad_media[]{utf8_literal("https://video.twimg.com.evil.test/ext_tw_video/456/pu/vid/640x360/a.mp4"),
        utf8_literal("https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/../a.mp4"),
        utf8_literal("https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/a.mp4#fragment"),
        utf8_literal("https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/a.mp4?tag=12&redirect=1"),
        utf8_literal("https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/a.m3u8"),
        utf8_literal("https://video.twimg.com/ext_tw_video/456/pu/vid/4096x2160/a.mp4"),
        utf8_literal("https://video.twimg.com/amplify_video/456/pu/vid/640x360/a.mp4"),
        utf8_literal("https://video.twimg.com/amplify_video/456/vid/640x360/a.mp4?tag=16&redirect=1"),
        utf8_literal("https://video.twimg.com/ext_tw_video/456/pu/vid/hvc1/640x360/a.mp4")};
    for(auto url:bad_media) check(!embed_stream_location(CdnHost::twitter_video,url),"media host path codec dimensions and query bounds");
    u8 request[1024];const u32 n=embed_stream_get(CdnHost::twitter_video,media,4096,1024,{request,sizeof(request)});
    const Utf8View head{reinterpret_cast<const char*>(request),n};
    check(n && ascii_contains_ignore_case(head,utf8_literal("Range: bytes=4096-5119")) &&
        !ascii_contains_ignore_case(head,utf8_literal("Authorization:")) && !ascii_contains_ignore_case(head,utf8_literal("Cookie:")),
        "media requests are bounded and credential free");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_twitter_player_entry() noexcept {
    maicord::run();ExitProcess(maicord::failures?1:0);
}
