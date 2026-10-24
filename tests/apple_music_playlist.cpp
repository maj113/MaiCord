#include "maicord/app/embed_resolver.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"
namespace maicord {
namespace {
u32 failures{};
void check(bool ok,const char* message) noexcept { test_check(&failures,ok,message); }
constexpr auto page=utf8_literal("https://music.apple.com/us/playlist/test/pl.test"), playlist_id=utf8_literal("pl.test"), country=utf8_literal("us");
constexpr auto html=utf8_literal(R"HTML(<script defer id="other">ignored();</script><script data-foo="x" id='serialized-server-data' type="application/json">{"data":[{"data":{"canonicalURL":"https://music.apple.com/us/playlist/test/pl.test","sections":[{"itemKind":"containerDetailHeaderLockup","items":[{"trackCount":3,"contentDescriptor":{"kind":"playlist","identifiers":{"storeAdamID":"pl.test"},"url":"https://music.apple.com/us/playlist/test/pl.test"}}]},{"itemKind":"trackLockup","containerContentDescriptor":{"kind":"playlist","identifiers":{"storeAdamID":"pl.test"},"url":"https://music.apple.com/us/playlist/test/pl.test"},"items":[{"title":"Caf\u00e9","contentDescriptor":{"kind":"song","identifiers":{"storeAdamID":"11"},"url":"https://music.apple.com/us/song/test/11"},"isDisabled":false},{"title":"Caf\u00e9","contentDescriptor":{"kind":"song","identifiers":{"storeAdamID":"22"},"url":"https://music.apple.com/us/song/test/22"},"isDisabled":true},{"title":"Caf\u00e9","contentDescriptor":{"kind":"song","identifiers":{"storeAdamID":"11"},"url":"https://music.apple.com/us/song/test/11"},"isDisabled":false}]}]}}]}</script>)HTML");
constexpr auto batch_json=utf8_literal(R"JSON({"resultCount":1,"results":[{"wrapperType":"track","kind":"song","trackId":11,"trackName":"Caf\u00e9","trackViewUrl":"https://music.apple.com/us/song/test/11","previewUrl":"https://audio-ssl.itunes.apple.com/itunes-assets/AudioPreview221/test/sample.m4a"}]})JSON");
struct Titles { u64 ids[200]{}; u32 names{},previews{}; };
const auto receive=+[](void* opaque,u16 index,u64 id,const MediaTrackTitle& title,Utf8View preview) noexcept {
    auto& out=*static_cast<Titles*>(opaque); if (!index || index>200 || !title.bytes) return false;
    if (!out.ids[index-1]) { out.ids[index-1]=id; ++out.names; }
    else if (out.ids[index-1]!=id) return false;
    out.previews+=preview.bytes!=0; return true;
};
}
bool apple_music_playlist_tests(EmbedResolver* resolver) noexcept {
    using E=EmbedPlayerError;
    check(apple_music_collection_locator(page).playlist && apple_music_collection_locator(utf8_literal("https://embed.music.apple.com/us/playlist/pl.test?i=")).playlist &&
        !apple_music_collection_locator(utf8_literal("https://music.apple.com/us/playlist/test/pl.test?i=11")).id.bytes && !apple_music_album_locator(page).id.bytes,
        "playlist page and widget identities remain separate from albums and single songs");
    const Utf8View invalid[]{utf8_literal("https://music.apple.com/us/playlist/test/pl."),utf8_literal("https://music.apple.com/us/playlist/test/pl.%2f"),
        utf8_literal("https://music.apple.com.evil.test/us/playlist/test/pl.test"),utf8_literal("https://music.apple.com/us/playlist/test/pl.test?i=&i=")};
    for (auto url:invalid) check(!apple_music_collection_locator(url).id.bytes,"ambiguous playlist identities are rejected");
    u32 scratch[4096]; AppleMusicPlaylist queue{}; Titles titles{};
    check(apple_music_playlist(html,playlist_id,country,{scratch,4096},&queue,{&titles,receive})==E::none && queue.count==3 && queue.tracks[0]==11 && !queue.tracks[1] && queue.tracks[2]==11 && titles.names==3,
        "public page supplies all ordered names, repeated IDs and disabled positions");
    check(apple_music_preview_batch(batch_json,{queue.tracks,queue.count},0,country,{scratch,4096},{&titles,receive})==E::none && titles.names==3 && titles.previews==2,
        "one preview result maps to every matching position without enabling disabled tracks");
    const auto saved=queue;
    const auto change=[&](Utf8View before,Utf8View after,E expected) noexcept {
        char body[4096]; ProtocolWriter writer{{reinterpret_cast<u8*>(body),sizeof(body)},0}; bool found=false;
        for (u32 at=0;at<html.bytes;) {
            if (!found && at+before.bytes<=html.bytes && bytes_equal(html.data+at,before.data,before.bytes)) {
                protocol_writer_append_bytes(&writer,reinterpret_cast<const u8*>(after.data),after.bytes); at+=before.bytes; found=true;
            } else protocol_writer_append_bytes(&writer,reinterpret_cast<const u8*>(html.data+at++),1);
        }
        titles={};
        check(found && apple_music_playlist({body,writer.used},playlist_id,country,{scratch,4096},&queue,{&titles,receive})==expected && bytes_equal(&queue,&saved,sizeof(queue)),
            "partial, mismatched and oversized playlist metadata cannot replace the queue");
    };
    change(utf8_literal("\"trackCount\":3"),utf8_literal("\"trackCount\":4"),E::unsupported);
    change(utf8_literal("\"trackCount\":3"),utf8_literal("\"trackCount\":201"),E::capacity);
    change(utf8_literal("test/pl.test"),utf8_literal("test/pl.other"),E::malformed);
    change(utf8_literal("\"storeAdamID\":\"11\""),utf8_literal("\"storeAdamID\":\"12\""),E::malformed);
    check(apple_music_playlist(html,playlist_id,country,{scratch,1},&queue,{&titles,receive})==E::capacity && bytes_equal(&queue,&saved,sizeof(queue)),"playlist respects bounded structural scratch");
    const u64 reordered[]{22,11}; titles={};
    check(apple_music_preview_batch(batch_json,{reordered,2},50,country,{scratch,4096},{&titles,receive})==E::none && titles.ids[51]==11 && titles.names==1 && titles.previews==1,
        "batch response order cannot change the playlist ordinal across windows");
    const u64 foreign[]{33};
    check(apple_music_preview_batch(batch_json,{foreign,1},0,country,{scratch,4096},{&titles,receive})==E::malformed,"batch cannot substitute another song");
    check(apple_music_preview_batch(batch_json,{queue.tracks,3},0,utf8_literal("gb"),{scratch,4096},{&titles,receive})==E::malformed,"batch storefront remains exact");
    titles={};
    check(apple_music_preview_batch(utf8_literal(R"({"resultCount":0,"results":[]})"),{queue.tracks,3},0,country,{scratch,4096},{&titles,receive})==E::none && !titles.previews,"missing catalog entries remain unavailable, never replaced by search results");
    if (resolver) for (u32 mode=0;mode<4;++mode) {
        struct Fixture {u32 calls{},mode; } f{0,mode};
        const auto exchange=+[](void* opaque,const UpdateHttpRequest& request,const HttpsExchangeOptions& options,UpdateHttpResponse* response) noexcept {
            auto& f=*static_cast<Fixture*>(opaque); ++f.calls;
            const bool redirect=f.mode==1 || f.mode==3;
            if (redirect && f.calls==1) {
                constexpr auto good=utf8_literal("https://music.apple.com/us/playlist/test/pl.test"),bad=utf8_literal("https://example.com/us/playlist/test/pl.test");
                const auto location=f.mode==3?bad:good; copy_bytes(response->location,location.data,location.bytes); response->location_bytes=location.bytes; response->status=301; return UpdateTransportError::none;
            }
            const bool list=f.calls==(redirect?2u:1u);
            check(options.headers.count==1 && utf8_equal(request.host_name,list?utf8_literal("music.apple.com"):utf8_literal("itunes.apple.com")) &&
                utf8_equal({request.target.data,request.target.count},list?utf8_literal("/us/playlist/test/pl.test"):utf8_literal("/lookup?id=11&country=us")),
                "playlist plus one deduplicated batch has exact public origins and no credentials");
            if (f.mode==2 && !list) { response->status=429;response->retry_after_seconds=19;return UpdateTransportError::none; }
            const auto body=list?html:batch_json; response->status=200;
            return request.sink(request.sink_context,{reinterpret_cast<const u8*>(body.data),body.bytes})?UpdateTransportError::none:UpdateTransportError::sink;
        };
        titles={};u16 count=99;
        const auto result=apple_music_collection_resolve(resolver,page,&count,{&titles,receive},{&f,exchange});
        check(mode==2? !result.ok() && count==99 && result.retry_after_seconds==19 : mode==3? result.error==E::malformed && f.calls==1 && count==99 :
            result.ok() && count==3 && titles.names==3 && titles.previews==2 && f.calls==(mode==1?3u:2u),"playlist transaction preserves exact identity, batch count and retry metadata");
        check(!resolver->response.committed && !resolver->structurals.committed,"playlist metadata is erased and decommitted after resolution");
    }
    char flag[2]{};
    if (resolver && GetEnvironmentVariableA("MAICORD_APPLE_ALBUM_PROBE",flag,2)==1 && flag[0]=='2') {
        titles={};u16 count=0;
        const auto result=apple_music_collection_resolve(resolver,utf8_literal("https://music.apple.com/us/playlist/todays-hits/pl.f4d106fed2bd41149aaacabb233eb5eb"),&count,{&titles,receive});
        test_write_metric("Apple playlist HTTP: ",result.status);test_write_metric("Apple playlist result: ",static_cast<u32>(result.error));
        test_write_metric("Apple playlist names: ",titles.names);test_write_metric("Apple playlist previews: ",titles.previews);
        check(result.ok() && count==50 && titles.names==count && titles.previews==count,"live public playlist resolves all names and preview assets in batches");
    }
    return !failures;
}
}
