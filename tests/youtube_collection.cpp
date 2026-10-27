#include "maicord/app/embed_resolver.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
namespace maicord {
namespace {
u32 failures{};
void check(bool ok, const char* name) noexcept { if (!ok) { ++failures; test_write_text(name); test_write_text("\n"); } }
constexpr auto url = utf8_literal("https://music.youtube.com/watch?v=CZQdi1sxd30&list=RDAMVMCZQdi1sxd30");
constexpr auto list = utf8_literal("RDAMVMCZQdi1sxd30");
constexpr auto json = utf8_literal(R"({"continuationContents":{"playlistPanelContinuation":{"playlistId":"RDAMVMCZQdi1sxd30","contents":[{"playlistPanelVideoRenderer":{"videoId":"CZQdi1sxd30","selected":true,"title":{"runs":[{"text":"First song"}]}}},{"playlistPanelVideoRenderer":{"videoId":"zlQ0Lfpq078"}},{"automixPreviewVideoRenderer":{}}],"continuations":[{"nextRadioContinuationData":{"continuation":"next+/=%"}}]}}})");
}
bool youtube_collection_tests(EmbedResolver* resolver) noexcept {
    check(utf8_equal(youtube_collection_locator(url).list, list), "YTM watch preserves playlist identity");
    check(youtube_collection_locator(utf8_literal("https://music.youtube.com/playlist?list=PL123")).list.bytes == 5,
        "YTM playlist-only links are playable queues");
    check(!youtube_collection_locator(utf8_literal("https://music.youtube.com/playlist?list=RD123")).list.bytes &&
        !youtube_collection_locator(utf8_literal("https://music.youtube.com/watch?v=CZQdi1sxd30&list=PL1&list=PL2")).list.bytes &&
        !youtube_collection_locator(utf8_literal("https://example.com/watch?v=CZQdi1sxd30&list=PL123")).list.bytes,
        "queues require exact provider identity, one list and a radio seed");
    check(youtube_collection_locator(utf8_literal("https://www.youtube.com/embed/videoseries?list=PL123")).list.bytes == 5 && youtube_collection_locator(utf8_literal("https://youtu.be/CZQdi1sxd30?list=PL123")).video.bytes == 11, "playlist iframe and shared short links retain queue identity");
    constexpr auto radio = utf8_literal("https://music.youtube.com/watch?v=eyk25H75ld0&list=RDAMVMY5f3S26WwDs");
    constexpr auto canonical = utf8_literal("https://www.youtube.com/embed/eyk25H75ld0");
    check(utf8_equal(youtube_collection_locator(radio).video, utf8_literal("eyk25H75ld0")) &&
          utf8_equal(youtube_collection_locator(radio).list, utf8_literal("RDAMVMY5f3S26WwDs")),
          "current song and radio seed remain independent");
    check(utf8_equal(youtube_collection_source(canonical, radio), radio) &&
          utf8_equal(youtube_collection_source(canonical, utf8_literal("[Mix](https://music.youtube.com/watch?v=eyk25H75ld0&list=RDAMVMY5f3S26WwDs)")), radio) &&
          utf8_equal(youtube_collection_source(canonical, utf8_literal("Mix: <https://music.youtube.com/watch?v=eyk25H75ld0&list=RDAMVMY5f3S26WwDs>.")), radio),
          "canonical embed recovers the same video's radio from bare and formatted message links");
    check(utf8_equal(youtube_collection_source(canonical, url), canonical) &&
          utf8_equal(youtube_collection_source(radio, url), radio) &&
          utf8_equal(youtube_collection_source(canonical, utf8_literal("https://example.com/watch?v=eyk25H75ld0&list=RDAMVMY5f3S26WwDs")), canonical) &&
          utf8_equal(youtube_collection_source(canonical, utf8_literal("https://example.com/https://music.youtube.com/watch?v=eyk25H75ld0&list=RDAMVMY5f3S26WwDs")), canonical) &&
          utf8_equal(youtube_collection_source(canonical, utf8_literal("https://music.youtube.com/watch?v=eyk25H75ld0&list=PL123 https://music.youtube.com/watch?v=eyk25H75ld0&list=PL456")), canonical),
          "recovery refuses unrelated videos, foreign hosts, nested URLs and ambiguous lists, preserving explicit queues");
    u32 scratch[4096]; YoutubeCollectionPage page{};
    struct Names { u32 count{}, bytes{}; u16 last{}; } names{};
    const MediaTrackTitleSink sink{&names, +[](void* opaque,u16 index,const MediaTrackTitle& name) noexcept {
        auto& n=*static_cast<Names*>(opaque); ++n.count; n.bytes+=name.bytes; n.last=index; return true;
    }};
    check(youtube_collection_page(json, list, {scratch, 4096}, &page, sink) == EmbedPlayerError::none && page.count == 2 && page.selected == 1 &&
        page.tracks[0].youtube_track && names.last == 1 && names.bytes == 10 && names.count == 1 && utf8_equal({page.tracks[0].youtube, 11}, utf8_literal("CZQdi1sxd30")) &&
        utf8_equal({page.continuation, page.continuation_bytes}, utf8_literal("next+/=%")), "queue keeps exact order, selected identity and continuation");
    {
        u8 body[4096]; ProtocolWriter writer{{body,sizeof(body)},0};
        protocol_writer_append_literal(&writer,R"({"continuationContents":{"playlistPanelContinuation":{"playlistId":"PL123","contents":[)");
        for (u32 i=0;i<12;++i) {
            if (i) protocol_writer_append_literal(&writer,",");
            protocol_writer_append_literal(&writer,R"({"playlistPanelVideoRenderer":{"videoId":"CZQdi1sxd30","title":{"runs":[{"text":"Song"}]}}})");
        }
        protocol_writer_append_literal(&writer,"]}}}");
        names={}; YoutubeCollectionPage many{};
        check(youtube_collection_page({reinterpret_cast<char*>(body),writer.used},utf8_literal("PL123"),{scratch,4096},&many,sink)==EmbedPlayerError::none &&
            many.count==12 && names.count==12 && names.last==12 && names.bytes==48,"all queue titles survive beyond eight entries, including repeated songs");
    }
    const auto saved = page;
    check(youtube_collection_page(json, utf8_literal("other"), {scratch, 4096}, &page) == EmbedPlayerError::malformed &&
        bytes_equal(&saved, &page, sizeof(page)), "another playlist cannot replace the queue");
    check(youtube_collection_page(json, list, {scratch, 1}, &page) == EmbedPlayerError::capacity &&
        bytes_equal(&saved, &page, sizeof(page)), "queue output is transactional under scratch pressure");
    constexpr auto title_json = utf8_literal(R"({"type":"video","provider_name":"YouTube","provider_url":"https://www.youtube.com/","title":"Song \u00e9","html":"<iframe src=\"https://www.youtube.com/embed/CZQdi1sxd30?feature=oembed\"></iframe>"})");
    MediaTrackTitle title{};
    check(youtube_track_title(title_json, utf8_literal("CZQdi1sxd30"), {scratch, 4096}, &title) == EmbedPlayerError::none &&
        utf8_equal({title.text,title.bytes},utf8_literal("Song é")), "YTM title metadata is Unicode and bound to exact video");
    struct Fixture { u32 calls{}; bool continuation{}; } f;
    const auto exchange = +[](void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        auto& f = *static_cast<Fixture*>(opaque); ++f.calls;
        check(utf8_equal(request.host_name, utf8_literal("music.youtube.com")), "queue uses only the music provider host");
        const bool bootstrap = f.calls == 1;
        const auto target = Utf8View{request.target.data, request.target.count};
        check(bootstrap ? utf8_equal(target, utf8_literal("/")) && !options.body.count :
            utf8_starts_with(target, utf8_literal("/youtubei/v1/next?prettyPrint=false&fields=")) &&
            ascii_contains_ignore_case(target,utf8_literal("playlistPanelVideoWrapperRenderer")) &&
            ascii_contains_ignore_case(target,utf8_literal("videoId%2Cselected%2Ctitle")) &&
            ascii_contains_ignore_case(target,utf8_literal("continuationContents.playlistPanelContinuation")) &&
            (f.continuation == ascii_contains_ignore_case(target,utf8_literal("&continuation=next%2B%2F%3D%25"))) && options.body.count,
            "queue requests only track metadata and encoded continuation; bootstrap has no authorization body");
        const auto body = bootstrap ? utf8_literal("{\"INNERTUBE_CLIENT_VERSION\":\"1.20261020.10.00\"}") : json;
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes}) ? UpdateTransportError::none : UpdateTransportError::sink;
    };
    if (resolver) {
        resolver->music_version_bytes = 0;
        check(youtube_collection_resolve(resolver, url, {}, &page, {&f, exchange}).ok() && f.calls == 2 && !resolver->response.committed && !resolver->structurals.committed,
            "queue resolves one public bootstrap and one metadata page, then decommits scratch");
        f.continuation = true;
        check(youtube_collection_resolve(resolver, url, {page.continuation,page.continuation_bytes}, &page, {&f,exchange}).ok() && f.calls == 3,
            "next page reuses public client version without another bootstrap");
    }
    return !failures;
}
}
