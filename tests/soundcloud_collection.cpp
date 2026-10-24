#include "maicord/app/embed_resolver.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures{};
void check(bool value, const char* message) noexcept { test_check(&failures, value, message); }
constexpr auto page = utf8_literal("https://soundcloud.com/forss/sets/soulhack");
constexpr auto path = utf8_literal("/forss/sets/soulhack");
constexpr auto widget = utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/18&start_track=2");
constexpr auto html = utf8_literal(R"(<script>window.__sc_hydration=[{"hydratable":"apiClient","data":{"id":"01234567890123456789012345678901"}},{"hydratable":"playlist","data":{"id":18,"kind":"playlist","permalink_url":"https://soundcloud.com/forss/sets/soulhack","public":true,"sharing":"public","embeddable_by":"all","track_count":3,"tracks":[{"id":290,"kind":"track","title":"First song"},{"id":291},{"id":293}]}}];</script>)");
constexpr auto canonical = utf8_literal("<link rel=\"canonical\" href=\"https://soundcloud.com/forss/sets/soulhack\">");
constexpr auto station_page = utf8_literal("https://soundcloud.com/discover/sets/artist-stations:59880230");
constexpr auto station_path = utf8_literal("/discover/sets/artist-stations:59880230");
constexpr auto station_html = utf8_literal(R"(<script>window.__sc_hydration=[{"hydratable":"systemPlaylist","data":{"id":"soundcloud:system-playlists:artist-stations:59880230","urn":"soundcloud:system-playlists:artist-stations:59880230","kind":"system-playlist","permalink_url":"https://soundcloud.com/discover/sets/artist-stations:59880230","is_public":true,"playlist_type":"ARTIST_STATION","seed":{"urn":"soundcloud:users:59880230"},"tracks":[{"id":285103732,"kind":"track","policy":"ALLOW"},{"id":770110162}]}}];</script>)");
constexpr auto track_station_html = utf8_literal(R"(<script>window.__sc_hydration=[{"hydratable":"systemPlaylist","data":{"id":"soundcloud:system-playlists:track-stations:59880230","urn":"soundcloud:system-playlists:track-stations:59880230","kind":"system-playlist","permalink_url":"https://soundcloud.com/discover/sets/track-stations:59880230","is_public":true,"playlist_type":"TRACK_STATION","seed":{"urn":"soundcloud:tracks:59880230"},"tracks":[{"id":285103732,"kind":"track","policy":"ALLOW"},{"id":770110162}]}}];</script>)");
constexpr auto track_station_page=utf8_literal("https://soundcloud.com/discover/sets/track-stations:59880230");
constexpr auto track_station_path=utf8_literal("/discover/sets/track-stations:59880230");
struct Fixture { u32 calls{}, fail{}; bool widget{}, station{}, track_station{}; };
UpdateTransportError exchange(void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions&,
    UpdateHttpResponse* response) noexcept {
    auto& f = *static_cast<Fixture*>(opaque); ++f.calls;
    const bool lookup = f.widget && f.calls == 1;
    check(f.calls <= (f.widget ? 2u : 1u), "collection resolution fetches only bounded metadata, never all track streams");
    check(utf8_equal(request.host_name, lookup ? utf8_literal("w.soundcloud.com") : utf8_literal("soundcloud.com")) &&
        utf8_equal({request.target.data, request.target.count}, lookup ? utf8_literal("/player/?url=https%3A%2F%2Fapi.soundcloud.com%2Fplaylists%2F18") : f.track_station ? track_station_path : f.station ? station_path : path),
        "collection resolver preserves the canonical identity and exact request origin");
    if (f.calls == f.fail) { response->status = 429; response->retry_after_seconds = 23; return UpdateTransportError::none; }
    const auto body = lookup ? canonical : f.track_station ? track_station_html : f.station ? station_html : html;
    if (!request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})) return UpdateTransportError::sink;
    response->status = 200; response->body_bytes = body.bytes; return UpdateTransportError::none;
}
}
bool soundcloud_collection_tests(EmbedResolver* resolver) noexcept {
    constexpr auto title_json = utf8_literal(R"({"type":"rich","provider_name":"SoundCloud","provider_url":"https://soundcloud.com","title":"Caf\u00e9 by Artist","html":"<iframe src=\"https://w.soundcloud.com/player/?url=https%3A%2F%2Fapi.soundcloud.com%2Ftracks%2F293&show_artwork=true\"></iframe>"})");
    u32 title_scratch[4096]; MediaTrackTitle title{};
    check(soundcloud_track_title(title_json, 293, {title_scratch, 4096}, &title) == EmbedPlayerError::none &&
        utf8_equal({title.text, title.bytes}, utf8_literal("Café by Artist")), "track hover metadata decodes Unicode into owned title storage");
    const auto saved_title = title;
    check(soundcloud_track_title(title_json, 294, {title_scratch, 4096}, &title) == EmbedPlayerError::malformed &&
        bytes_equal(&title, &saved_title, sizeof(title)), "another track's hover title cannot replace the requested track metadata");
    check(soundcloud_track_title(title_json, 293, {title_scratch, 1}, &title) == EmbedPlayerError::capacity &&
        bytes_equal(&title, &saved_title, sizeof(title)), "title parsing respects caller scratch and preserves its old output");
    if (resolver) for (u32 fail = 0; fail < 2; ++fail) {
        struct TitleFixture { Utf8View body; u32 calls{}, status{}; } f{title_json, 0, fail ? 429u : 200u};
        const auto exchange_title = +[](void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
            UpdateHttpResponse* response) noexcept {
            auto& f = *static_cast<TitleFixture*>(opaque); ++f.calls;
            check(utf8_equal(request.host_name, utf8_literal("soundcloud.com")) &&
                utf8_equal({request.target.data, request.target.count}, utf8_literal("/oembed?format=json&url=https%3A%2F%2Fapi.soundcloud.com%2Ftracks%2F293")) &&
                options.headers.count == 1, "title request uses one exact public endpoint with no account or playback credentials");
            response->status = static_cast<u16>(f.status); response->retry_after_seconds = f.status == 429 ? 9 : 0;
            if (f.status == 200 && !request.sink(request.sink_context, {reinterpret_cast<const u8*>(f.body.data), f.body.bytes}))
                return UpdateTransportError::sink;
            return UpdateTransportError::none;
        };
        const auto result = soundcloud_title_resolve(resolver, 293, &title, {&f, exchange_title});
        check(f.calls == 1 && (fail ? !result.ok() && result.retry_after_seconds == 9 : result.ok()) &&
            bytes_equal(&title, &saved_title, sizeof(title)) && !resolver->response.committed && !resolver->structurals.committed,
            "hover title resolution uses one small request, preserves throttle metadata and releases scratch");
    }
    SoundcloudTitleBatch batch{};
    copy_bytes(batch.client,"01234567890123456789012345678901",32);
    batch.count=3; batch.tracks[0]={290,1}; batch.tracks[1]={291,2}; batch.tracks[2]={290,3};
    constexpr auto batch_json=utf8_literal(R"([{"id":291,"kind":"track","title":"Second"},{"id":290,"kind":"track","title":"Caf\u00e9"}])");
    struct Names { u16 indices[4]{}; u32 count{},bytes{}; } names{};
    const MediaTrackTitleSink sink{&names,+[](void* opaque,u16 index,const MediaTrackTitle& name) noexcept {
        auto& n=*static_cast<Names*>(opaque); if (n.count==4) return false;
        n.indices[n.count++]=index; n.bytes+=name.bytes; return true;
    }};
    check(soundcloud_batch_titles(batch_json,batch,3,{title_scratch,4096},sink)==EmbedPlayerError::none &&
        names.count==3 && names.indices[0]==3 && names.indices[1]==2 && names.indices[2]==1 && names.bytes==16,
        "batch names follow IDs rather than response order and preserve repeated songs with demanded title first");
    const Utf8View invalid_batches[]{
        utf8_literal(R"([{"id":999,"kind":"track","title":"Other"}])"),
        utf8_literal(R"([{"id":290,"kind":"track","title":"A"},{"id":290,"kind":"track","title":"B"}])"),
        utf8_literal(R"([{"id":290,"kind":"playlist","title":"Other"}])")};
    for (auto body:invalid_batches) { names={}; check(soundcloud_batch_titles(body,batch,3,{title_scratch,4096},sink)==EmbedPlayerError::malformed,
        "batch rejects foreign IDs, duplicate responses and wrong kinds"); }
    names={};
    check(soundcloud_batch_titles(utf8_literal(R"([{"id":290,"kind":"track","title":"Hidden","public":false}])"),batch,3,{title_scratch,4096},sink)==EmbedPlayerError::none && !names.count,
        "missing and private tracks do not invent titles");
    if (resolver) for (u32 fail=0;fail<2;++fail) {
        struct BatchFixture { Utf8View body; u32 calls{},fail{}; } f{batch_json,0,fail};
        const auto exchange_batch=+[](void* opaque,const UpdateHttpRequest& request,const HttpsExchangeOptions& options,UpdateHttpResponse* response) noexcept {
            auto& f=*static_cast<BatchFixture*>(opaque); ++f.calls;
            check(utf8_equal(request.host_name,utf8_literal("api-v2.soundcloud.com")) &&
                utf8_equal({request.target.data,request.target.count},utf8_literal("/tracks?ids=290,291&client_id=01234567890123456789012345678901")) && options.headers.count==1,
                "missing names share one exact public batch request without playback grants");
            response->status=f.fail?429:200; response->retry_after_seconds=f.fail?17:0;
            if (!f.fail && !request.sink(request.sink_context,{reinterpret_cast<const u8*>(f.body.data),f.body.bytes})) return UpdateTransportError::sink;
            return UpdateTransportError::none;
        };
        names={}; const auto result=soundcloud_batch_titles_resolve(resolver,batch,3,sink,{&f,exchange_batch});
        check(f.calls==1 && (fail ? !result.ok() && result.retry_after_seconds==17 && !names.count : result.ok() && names.count==3) &&
            !resolver->response.committed && !resolver->structurals.committed,"batch resolution preserves throttling and releases all response scratch");
    }
    char decoded[1024];
    const auto located = soundcloud_collection_locator(widget, {decoded, sizeof(decoded)});
    check(located.id == 18 && located.start_track == 2 && !located.path.bytes && !soundcloud_track_url(widget),
        "collection widget keeps its requested track index and never masquerades as a single track");
    check(utf8_equal(soundcloud_collection_path(page), path) &&
        utf8_equal(soundcloud_collection_path(utf8_literal("https://www.soundcloud.com/forss/sets/soulhack/?utm_source=share")), path),
        "public album and playlist permalinks share the sets route");
    check(utf8_equal(soundcloud_collection_locator(utf8_literal("https://w.soundcloud.com/player/?url=https%3A%2F%2Fsoundcloud.com%2Fforss%2Fsets%2Fsoulhack&start_track=1"),
        {decoded, sizeof(decoded)}).path, path), "permalink widgets resolve a collection without numeric canonical lookup");
    const Utf8View bad[]{utf8_literal("https://soundcloud.com/forss/sets"), utf8_literal("https://soundcloud.com/forss//sets/soulhack"),
        utf8_literal("https://soundcloud.com/forss/sets/soulhack/extra"), utf8_literal("https://soundcloud.com/forss/sets/soulhack?secret_token=private"),
        utf8_literal("https://soundcloud.com.evil.test/forss/sets/soulhack"), utf8_literal("https://soundcloud.com/forss/sets/%2e%2e"),
        utf8_literal("https://soundcloud.com/discover/sets/artist-stations:0"),
        utf8_literal("https://soundcloud.com/discover/sets/artist-stations:01"),
        utf8_literal("https://soundcloud.com/discover/sets/artist-stations:18446744073709551616"),
        utf8_literal("https://soundcloud.com/discover/sets/artist-stations:59880230/extra"),
        utf8_literal("https://soundcloud.com/discover/sets/artist-stations:59880230?secret_token=private"),
        utf8_literal("https://soundcloud.com/discover/sets/artist-stations%3A59880230"),
        utf8_literal("https://soundcloud.com/another/sets/artist-stations:59880230"),
        utf8_literal("https://soundcloud.com/discover/sets/genre-stations:59880230"),
        utf8_literal("https://soundcloud.com/discover/sets/track-stations:0"),
        utf8_literal("https://soundcloud.com/discover/sets/track-stations:01"),
        utf8_literal("https://soundcloud.com/discover/sets/track-stations:18446744073709551616"),
        utf8_literal("https://soundcloud.com/discover/sets/track-stations:293/extra"),
        utf8_literal("https://soundcloud.com/discover/sets/track-stations:293?secret_token=x"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/18&start_track=-1"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/18&start_track=4294967296"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/18&start_track="),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/18&start_track=1&start_track=2"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/18")};
    for (auto url : bad) {
        const auto invalid = soundcloud_collection_locator(url, {decoded, sizeof(decoded)});
        check(!invalid.path.bytes && !invalid.id, "collection locators reject ambiguous, private, malformed and wrong-kind inputs");
    }
    u32 scratch[4096]; u64 tracks[4]{901, 902, 903, 904}; SoundcloudCollection collection{999, 99, 9};
    const auto invalid = [&](Utf8View body, EmbedPlayerError expected, u64 id = 18, u32 selected = 1, u32 capacity = 4) noexcept {
        const auto saved = collection; u64 ids[4]; copy_bytes(ids, tracks, sizeof(ids));
        check(soundcloud_collection(body, path, id, selected, {scratch, 4096}, {tracks, capacity}, &collection) == expected &&
            bytes_equal(&collection, &saved, sizeof(saved)) && bytes_equal(tracks, ids, sizeof(ids)),
            "collection errors preserve both the active queue and its selection");
    };
    const auto changed = [&](Utf8View before, Utf8View after, EmbedPlayerError error) noexcept {
        char body[4096]; ProtocolWriter writer{{reinterpret_cast<u8*>(body), sizeof(body)}, 0}; bool replaced = false;
        for (u32 at = 0; at < html.bytes;) {
            if (!replaced && at + before.bytes <= html.bytes && bytes_equal(html.data + at, before.data, before.bytes)) {
                protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(after.data), after.bytes); at += before.bytes; replaced = true;
            } else protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(html.data + at++), 1);
        }
        check(replaced, "collection mutation fixture matches"); invalid({body, writer.used}, error);
    };
    invalid(html, EmbedPlayerError::malformed, 19);
    invalid(html, EmbedPlayerError::invalid_input, 18, 3);
    invalid(html, EmbedPlayerError::capacity, 18, 1, 2);
    changed(utf8_literal("/forss/sets/soulhack"), utf8_literal("/forss/sets/another"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"public\":true"), utf8_literal("\"public\":false"), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"sharing\":\"public\""), utf8_literal("\"sharing\":\"private\""), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"embeddable_by\":\"all\""), utf8_literal("\"embeddable_by\":\"none\""), EmbedPlayerError::not_embeddable);
    changed(utf8_literal("\"track_count\":3"), utf8_literal("\"track_count\":4"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"track_count\":3"), utf8_literal("\"track_count\":2"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"track_count\":3"), utf8_literal("\"track_count\":0"), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"track_count\":3"), utf8_literal("\"track_count\":3,\"track_count\":3"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":0"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":293,\"id\":293"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":18446744073709551616"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":293,\"kind\":\"playlist\""), EmbedPlayerError::malformed);
    names={};
    check(soundcloud_collection(html, path, 18, 2, {scratch, 4096}, {tracks, 4}, &collection, sink) == EmbedPlayerError::none &&
        names.count==1 && names.indices[0]==1 && names.bytes==10 && bytes_equal(collection.client,batch.client,32) && collection.id == 18 && collection.count == 3 && collection.selected == 2 && tracks[0] == 290 && tracks[1] == 291 && tracks[2] == 293 && tracks[3] == 904,
        "ordered full and ID-only entries become a compact exact queue with the requested selection");
    constexpr auto station_widget = utf8_literal("https://w.soundcloud.com/player/?url=https%3A%2F%2Fsoundcloud.com%2Fdiscover%2Fsets%2Fartist-stations%3A59880230&start_track=1");
    const auto station = soundcloud_collection_locator(station_widget, {decoded, sizeof(decoded)});
    check(utf8_equal(soundcloud_collection_path(station_page), station_path) &&
        utf8_equal(station.path, station_path) && !station.id && station.start_track == 1 && !soundcloud_track_url(station_page),
        "artist stations use direct canonical metadata and preserve a nested widget selection");
    check(soundcloud_collection(station_html, station_path, 0, 1, {scratch, 4096}, {tracks, 4}, &collection) == EmbedPlayerError::none &&
        collection.id == 59880230 && collection.count == 2 && collection.selected == 1 && tracks[0] == 285103732 && tracks[1] == 770110162,
        "public artist station validates its exact identity and derives the complete queue count");
    constexpr auto track_widget=utf8_literal("https://w.soundcloud.com/player/?url=https%3A%2F%2Fsoundcloud.com%2Fdiscover%2Fsets%2Ftrack-stations%3A59880230&start_track=1");
    check(!soundcloud_station_locator({nullptr,40}).seed,"station locator rejects missing path storage before inspecting a prefix");
    const auto track_station=soundcloud_collection_locator(track_widget,{decoded,sizeof(decoded)});
    check(utf8_equal(track_station.path,track_station_path) && track_station.start_track==1 && !track_station.id &&
        soundcloud_station_locator(track_station_path).track && !soundcloud_station_locator(station_path).track,
        "song and artist radio preserve distinct seed kinds even when numeric IDs match");
    check(soundcloud_collection(track_station_html,track_station_path,0,1,{scratch,4096},{tracks,4},&collection)==EmbedPlayerError::none &&
        collection.id==59880230 && collection.count==2 && collection.selected==1 && tracks[0]==285103732 && tracks[1]==770110162,
        "track station keeps the complete ordered queue and starting ordinal");
    const auto saved_station=collection;
    check(soundcloud_collection(station_html,track_station_path,0,1,{scratch,4096},{tracks,4},&collection)==EmbedPlayerError::malformed &&
        soundcloud_collection(track_station_html,station_path,0,1,{scratch,4096},{tracks,4},&collection)==EmbedPlayerError::malformed && bytes_equal(&collection,&saved_station,sizeof(collection)),
        "an artist station cannot replace a song station or vice versa");
    if (resolver) for (u32 nested=0;nested<2;++nested) {
        Fixture fixture{0,0,false,false,true};
        const auto result=soundcloud_collection_resolve(resolver,nested?track_widget:track_station_page,{tracks,4},&collection,{&fixture,exchange});
        check(result.ok() && fixture.calls==1 && collection.count==2 && collection.selected==nested && !resolver->response.committed && !resolver->structurals.committed,
            "direct and nested song stations use one bounded queue fetch and release metadata scratch");
    }
    const auto bad_station = [&](Utf8View body, EmbedPlayerError expected, u64 id = 0, u32 selected = 0, u32 capacity = 4) noexcept {
        const auto saved = collection; u64 ids[4]; copy_bytes(ids, tracks, sizeof(ids));
        check(soundcloud_collection(body, station_path, id, selected, {scratch, 4096}, {tracks, capacity}, &collection) == expected &&
            bytes_equal(&collection, &saved, sizeof(saved)) && bytes_equal(tracks, ids, sizeof(ids)),
            "invalid station data cannot change the resident queue or selected track");
    };
    const auto change_station = [&](Utf8View before, Utf8View after, EmbedPlayerError expected) noexcept {
        char body[4096]; ProtocolWriter writer{{reinterpret_cast<u8*>(body), sizeof(body)}, 0}; bool replaced = false;
        for (u32 at = 0; at < station_html.bytes;) {
            if (!replaced && at + before.bytes <= station_html.bytes && bytes_equal(station_html.data + at, before.data, before.bytes)) {
                protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(after.data), after.bytes); at += before.bytes; replaced = true;
            } else protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(station_html.data + at++), 1);
        }
        check(replaced, "station mutation fixture matches"); bad_station({body, writer.used}, expected);
    };
    bad_station(station_html, EmbedPlayerError::malformed, 59880230);
    bad_station(station_html, EmbedPlayerError::capacity, 0, 0, 1);
    bad_station(station_html, EmbedPlayerError::invalid_input, 0, 2);
    change_station(utf8_literal("\"is_public\":true"), utf8_literal("\"is_public\":false"), EmbedPlayerError::unavailable);
    change_station(utf8_literal("\"is_public\":true"), utf8_literal("\"is_public\":true,\"is_public\":true"), EmbedPlayerError::malformed);
    change_station(utf8_literal("\"urn\":\"soundcloud:system-playlists:artist-stations:59880230\""), utf8_literal("\"urn\":\"soundcloud:system-playlists:artist-stations:59880231\""), EmbedPlayerError::malformed);
    change_station(utf8_literal("soundcloud:users:59880230"), utf8_literal("soundcloud:users:59880231"), EmbedPlayerError::malformed);
    change_station(utf8_literal("ARTIST_STATION"), utf8_literal("TRACK_STATION"), EmbedPlayerError::malformed);
    change_station(utf8_literal("\"id\":770110162"), utf8_literal("\"id\":0"), EmbedPlayerError::malformed);
    change_station(utf8_literal("https://soundcloud.com/discover/sets/artist-stations:59880230"), utf8_literal("https://soundcloud.com/discover/sets/artist-stations:59880231"), EmbedPlayerError::malformed);
    if (resolver) for (u32 nested = 0; nested < 2; ++nested) {
        Fixture fixture{0, 0, false, true};
        const auto result = soundcloud_collection_resolve(resolver, nested ? station_widget : station_page, {tracks, 4}, &collection, {&fixture, exchange});
        check(result.ok() && fixture.calls == 1 && collection.id == 59880230 && collection.count == 2 && collection.selected == nested &&
            !resolver->response.committed && !resolver->structurals.committed,
            "station direct and nested links resolve one bounded public queue and release metadata");
    }
    if (resolver) for (u32 mode = 0; mode < 2; ++mode) for (u32 fail = 0; fail <= 1 + mode; ++fail) {
        Fixture fixture{0, fail, mode != 0}; const auto saved = collection; u64 ids[4]; copy_bytes(ids, tracks, sizeof(ids));
        const auto result = soundcloud_collection_resolve(resolver, mode ? widget : page, {tracks, 4}, &collection, {&fixture, exchange});
        check(fail ? !result.ok() && result.status == 429 && result.retry_after_seconds == 23 && fixture.calls == fail &&
                bytes_equal(&collection, &saved, sizeof(saved)) && bytes_equal(tracks, ids, sizeof(ids))
            : result.ok() && fixture.calls == 1 + mode && collection.count == 3 && collection.selected == (mode ? 2u : 0u),
            "collection requests preserve retry metadata and publish only a complete validated queue");
        check(!resolver->response.committed && !resolver->structurals.committed && !resolver->response_bytes,
            "collection requests erase and decommit metadata including unused track grants");
    }
    return !failures;
}
} // namespace maicord
