#include "maicord/app/embed_resolver.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"
namespace maicord {
namespace {
u32 failures{};
void check(bool ok,const char* message) noexcept { test_check(&failures,ok,message); }
constexpr auto json=utf8_literal(R"JSON({"resultCount":4,"results":[{"wrapperType":"collection","collectionType":"Album","collectionId":10,"trackCount":3,"collectionViewUrl":"https://music.apple.com/us/album/test/10"},{"wrapperType":"track","kind":"song","collectionId":10,"trackId":103,"discNumber":2,"trackNumber":1,"trackName":"Caf\u00e9","trackViewUrl":"https://music.apple.com/us/album/test/10?i=103","previewUrl":"https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/test/sample.m4a"},{"wrapperType":"track","kind":"song","collectionId":10,"trackId":102,"discNumber":1,"trackNumber":2,"trackName":"Caf\u00e9","trackViewUrl":"https://music.apple.com/us/album/test/10?i=102"},{"wrapperType":"track","kind":"song","collectionId":10,"trackId":101,"discNumber":1,"trackNumber":1,"trackName":"Caf\u00e9","trackViewUrl":"https://music.apple.com/us/album/test/10?i=101","previewUrl":"https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/test/sample.m4a"}]})JSON");
constexpr auto page=utf8_literal("https://music.apple.com/us/album/test/10");
struct Tracks { u64 ids[200]{}; u32 count{},previews{},bytes{}; };
const auto receive=+[](void* opaque,u16 index,u64 id,const MediaTrackTitle& title,Utf8View preview) noexcept {
    auto& out=*static_cast<Tracks*>(opaque);
    if (index!=out.count+1 || out.count==200 || !title.bytes) return false;
    out.ids[out.count++]=id; out.previews+=preview.bytes!=0; out.bytes+=title.bytes; return true;
};
}
bool apple_music_album_tests(EmbedResolver* resolver) noexcept {
    using E=EmbedPlayerError;
    check(apple_music_album_locator(page).id.bytes==2 && apple_music_album_locator(utf8_literal("https://embed.music.apple.com/US/album/test/10?theme=dark&i=")).id.bytes==2 &&
        !apple_music_album_locator(utf8_literal("https://music.apple.com/us/album/test/10?i=101")).id.bytes && !apple_music_track_locator(page).id.bytes,
        "album links and song links remain distinct, including Discord's empty song selector");
    const Utf8View bad[]{utf8_literal("https://music.apple.com/us/playlist/test/pl.123"),utf8_literal("https://music.apple.com/us/album/test/10?i=&i="),
        utf8_literal("https://music.apple.com.evil.test/us/album/test/10"),utf8_literal("https://music.apple.com/us/song/test/10")};
    for (auto url:bad) check(!apple_music_album_locator(url).id.bytes,"non-albums cannot become preview queues");
    u32 scratch[4096]; u16 count=99; Tracks tracks{};
    const auto parse=[&](Utf8View body,Utf8View country=utf8_literal("us")) noexcept {
        tracks={}; count=99; return apple_music_album(body,utf8_literal("10"),country,{scratch,4096},&count,{&tracks,receive});
    };
    check(parse(json)==E::none && count==3 && tracks.count==3 && tracks.previews==2 && tracks.ids[0]==101 && tracks.ids[1]==102 && tracks.ids[2]==103 && tracks.bytes==15,
        "album sorts discs and track numbers, retains Unicode titles and unavailable-song positions");
    check(parse(json,utf8_literal("gb"))==E::malformed && count==99,"album storefront cannot change silently");
    const auto changed=[&](Utf8View before,Utf8View after,E expected) noexcept {
        char body[4096]; ProtocolWriter writer{{reinterpret_cast<u8*>(body),sizeof(body)},0}; bool found=false;
        for (u32 at=0;at<json.bytes;) {
            if (!found && at+before.bytes<=json.bytes && bytes_equal(json.data+at,before.data,before.bytes)) {
                protocol_writer_append_bytes(&writer,reinterpret_cast<const u8*>(after.data),after.bytes); at+=before.bytes; found=true;
            } else protocol_writer_append_bytes(&writer,reinterpret_cast<const u8*>(json.data+at++),1);
        }
        check(found && parse({body,writer.used})==expected && count==(expected==E::none?3:99),"invalid/partial album cannot publish a complete queue");
    };
    changed(utf8_literal("\"collectionType\":\"Album\""),utf8_literal("\"collectionType\":\"Compilation\""),E::none);
    changed(utf8_literal("\"trackCount\":3"),utf8_literal("\"trackCount\":4"),E::unsupported);
    changed(utf8_literal("\"resultCount\":4"),utf8_literal("\"resultCount\":3"),E::malformed);
    changed(utf8_literal("\"collectionId\":10"),utf8_literal("\"collectionId\":11"),E::malformed);
    changed(utf8_literal("\"trackId\":103"),utf8_literal("\"trackId\":104"),E::malformed);
    changed(utf8_literal("\"discNumber\":2"),utf8_literal("\"discNumber\":1"),E::malformed);
    changed(utf8_literal("audio-ssl.itunes.apple.com/"),utf8_literal("audio-ssl.itunes.apple.com.evil.test/"),E::malformed);
    count=99;
    check(apple_music_album(json,utf8_literal("10"),utf8_literal("us"),{scratch,1},&count,{&tracks,receive})==E::capacity && count==99,"bounded album scratch preserves count");
    if (resolver) for (u32 fail=0;fail<2;++fail) {
        struct Fixture {u32 calls{},fail; } f{0,fail};
        const auto exchange=+[](void* opaque,const UpdateHttpRequest& request,const HttpsExchangeOptions& options,UpdateHttpResponse* response) noexcept {
            auto& f=*static_cast<Fixture*>(opaque); ++f.calls;
            check(utf8_equal(request.host_name,utf8_literal("itunes.apple.com")) && utf8_equal({request.target.data,request.target.count},utf8_literal("/lookup?id=10&country=us&entity=song&limit=200")) && options.headers.count==1,
                "one public album metadata request contains no login or playback grants");
            response->status=f.fail?429:200; response->retry_after_seconds=f.fail?13:0;
            if (!f.fail && !request.sink(request.sink_context,{reinterpret_cast<const u8*>(json.data),json.bytes})) return UpdateTransportError::sink;
            return UpdateTransportError::none;
        };
        tracks={};count=99;
        const auto result=apple_music_collection_resolve(resolver,page,&count,{&tracks,receive},{&f,exchange});
        check(f.calls==1 && (fail ? !result.ok() && result.retry_after_seconds==13 && count==99 : result.ok() && count==3 && tracks.count==3) &&
            !resolver->response.committed && !resolver->structurals.committed,"album fetch preserves throttling and releases metadata scratch");
    }
    return !failures;
}
}
