#include "maicord/app/embed_resolver.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/protocol/dash_manifest.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u32 kResponseLimit = 2u * 1024u * 1024u;
constexpr auto kAgent = kYoutubeNativeUserAgent;
bool bootstrap_sink(void* context, ConstSlice<u8> bytes) noexcept {
    return youtube_bootstrap_feed(static_cast<YoutubeBootstrap*>(context), bytes);
}
bool response_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto& r = *static_cast<EmbedResolver*>(context);
    if (bytes.count > kResponseLimit - r.response_bytes ||
        !reserved_bytes_commit(&r.response, r.response_bytes + bytes.count)) return false;
    copy_bytes(r.response.base + r.response_bytes, bytes.data, bytes.count);
    r.response_bytes += bytes.count;
    return true;
}
UpdateTransportError native_exchange(void* context, const UpdateHttpRequest& request,
    const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    return https_exchange(static_cast<UpdateHttpsTransport*>(context), request, options, response);
}
UpdateTransportError transport_for(EmbedResolver& r, EmbedResolveTransport& transport) noexcept {
    if (transport.exchange) return UpdateTransportError::none;
    const LONG state = InterlockedCompareExchange(&r.native_state, 1, 0);
    if (state == 3) return UpdateTransportError::cancelled;
    if (!state) {
        const bool initialized = update_https_initialize(&r.https);
        if (InterlockedCompareExchange(&r.native_state, 2, 1) != 1) {
            if (initialized) update_https_cancel(&r.https);
            return UpdateTransportError::cancelled;
        }
        if (!initialized) return UpdateTransportError::connect;
    }
    if (!r.https.winsock) return UpdateTransportError::connect;
    transport = {&r.https, native_exchange};
    return UpdateTransportError::none;
}
struct ScratchLifetime {
    EmbedResolver* r;
    ~ScratchLifetime() noexcept {
        // Player responses can contain signed URLs: erase committed pages,
        // return them to the system and retain only the address reservation.
        if (r->response.committed) secure_clear_bytes(r->response.base, r->response.committed);
        reserved_bytes_decommit(&r->response);
        reserved_bytes_decommit(&r->structurals);
        r->response_bytes = 0;
    }
};
} // namespace
bool youtube_bootstrap_feed(YoutubeBootstrap* s, ConstSlice<u8> input) noexcept {
    if (!s || s->complete || s->failed || (!input.data && input.count)) return false;
    constexpr char upper[] = "\"VISITOR_DATA\"", camel[] = "\"visitorData\"";
    for (u32 i = 0; i < input.count; ++i) {
        const char c = static_cast<char>(input[i]);
        if (!s->phase) {
            s->match_upper = c == upper[s->match_upper] ? s->match_upper + 1 : c == '"' ? 1 : 0;
            s->match_camel = c == camel[s->match_camel] ? s->match_camel + 1 : c == '"' ? 1 : 0;
            if (s->match_upper == sizeof(upper) - 1 || s->match_camel == sizeof(camel) - 1)
                s->phase = 1;
        } else if (s->phase < 3) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            if (c == (s->phase == 1 ? ':' : '"')) ++s->phase;
            else { s->phase = s->match_upper = s->match_camel = 0; }
        } else {
            if (c == '"') {
                s->complete = s->bytes != 0;
                s->failed = !s->complete;
                return false;
            }
            if ((!ascii_is_alnum(c) && c != '-' && c != '_' && c != '=' && c != '%') ||
                s->bytes == sizeof(s->visitor)) { s->failed = true; return false; }
            s->visitor[s->bytes++] = c;
        }
    }
    return true;
}
static EmbedResolveResult resolve(EmbedResolver* r, Utf8View id, u16 height,
    EmbedStreams* out, EmbedResolveTransport transport, CdnHost provider, u64 now, Utf8View country = {}, u8 media_index = 0, bool episode = false) noexcept {
    const bool tiktok = provider == CdnHost::tiktok_video, twitch = provider == CdnHost::twitch_video;
    const bool youtube = provider == CdnHost::google_video;
    const bool spotify = provider == CdnHost::spotify_preview;
    const bool streamable = provider == CdnHost::streamable_video;
    const bool twitter = provider == CdnHost::twitter_video;
    const bool video_preview = provider == CdnHost::apple_music_video_preview;
    const bool apple = provider == CdnHost::apple_music_preview || video_preview;
    const bool get = tiktok || spotify || streamable || apple || twitter;
    using E = EmbedPlayerError;
    if (!r || !out || !height || height > 2048 || (!youtube && !now) || media_index > 4) return {E::invalid_input};
    u8 body[2048];
    // Validate the identity before any network or allocation.
    u32 request_bytes = tiktok ? tiktok_player_target(id, {body, sizeof(body)}) : twitch ? twitch_clip_body(id, {body, sizeof(body)})
        : spotify ? spotify_audio_target(id, {body, sizeof(body)},episode)
        : streamable ? streamable_video_target(id, {body, sizeof(body)})
        : twitter ? twitter_video_target(id, {body, sizeof(body)})
        : apple ? apple_music_track_target(id, country, {body, sizeof(body)})
        : youtube ? youtube_player_body(id, utf8_literal("validation"), {body, sizeof(body)}) : 0;
    if (!request_bytes) return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if (youtube && !r->bootstrap.complete) {
        r->bootstrap = {};
        const HttpsHeader headers[]{{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")},
            {utf8_literal("Cookie"), utf8_literal("SOCS=CAI")}};
        const UpdateHttpRequest request{UpdateHost::provider, utf8_literal("www.youtube.com"),
            {"/", 1}, kResponseLimit, bootstrap_sink, &r->bootstrap};
        UpdateHttpResponse response{};
        const auto fetched = transport.exchange(transport.context, request, {{headers, 2}}, &response);
        // The scanner's deliberate sink stop avoids downloading the rest of
        // the homepage after visitorData. No other network error is ignored.
        if (response.status != 200 || !r->bootstrap.complete ||
            (fetched != UpdateTransportError::none && fetched != UpdateTransportError::sink))
            return {E::unavailable, fetched, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
    }
    const Utf8View visitor{r->bootstrap.visitor, r->bootstrap.bytes};
    if (youtube) request_bytes = youtube_player_body(id, visitor, {body, sizeof(body)});
    if (!request_bytes) return {E::invalid_input};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512u * 1024u))) return {E::capacity};
    ScratchLifetime lifetime{r};
    r->response_bytes = 0;
    const HttpsHeader headers[]{
        {utf8_literal("User-Agent"), kAgent}, {utf8_literal("Cookie"), utf8_literal("SOCS=CAI")},
        {utf8_literal("Content-Type"), utf8_literal("application/json")},
        {utf8_literal("Origin"), utf8_literal("https://www.youtube.com")},
        {utf8_literal("X-YouTube-Client-Name"), utf8_literal("101")},
        {utf8_literal("X-YouTube-Client-Version"), utf8_literal("1.02")},
        {utf8_literal("X-Goog-Visitor-Id"), visitor}};
    const HttpsHeader clip_headers[]{{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")},
        {utf8_literal("Referer"), utf8_literal("https://www.tiktok.com/")}};
    // Public application identifier observed in the embed client; not a user
    // credential, OAuth token or application secret.
    const HttpsHeader twitch_headers[]{{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")},
        {utf8_literal("Content-Type"), utf8_literal("application/json")},
        {utf8_literal("Client-ID"), utf8_literal("b31o4btkqth5bzbvr9ub2ovr79umhh")}};
    constexpr char path[] = "/youtubei/v1/player?prettyPrint=false";
    const UpdateHttpRequest request{UpdateHost::provider, tiktok ? utf8_literal("www.tiktok.com")
            : twitch ? utf8_literal("gql.twitch.tv") : spotify ? utf8_literal("open.spotify.com")
            : streamable ? utf8_literal("api.streamable.com") : twitter ? utf8_literal("cdn.syndication.twimg.com")
            : apple ? utf8_literal("itunes.apple.com") : utf8_literal("www.youtube.com"),
        get ? ConstSlice<char>{reinterpret_cast<const char*>(body), request_bytes}
            : twitch ? ConstSlice<char>{"/gql", 4} : ConstSlice<char>{path, sizeof(path) - 1},
        spotify ? kSpotifyEmbedMaximumBytes : streamable ? kStreamableMetadataMaximumBytes
            : twitter ? kTwitterMetadataMaximumBytes
            : apple ? kAppleMusicMetadataMaximumBytes : kResponseLimit, response_sink, r};
    UpdateHttpResponse response{};
    const auto fetched = transport.exchange(transport.context, request,
        {get ? ConstSlice<HttpsHeader>{clip_headers, tiktok ? 2u : 1u} : twitch ? ConstSlice<HttpsHeader>{twitch_headers, 3}
            : ConstSlice<HttpsHeader>{headers, 7}, {body, get ? 0 : request_bytes}}, &response);
    if (fetched != UpdateTransportError::none || response.status != 200)
        return {E::unavailable, fetched, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
    // At most one structural per input byte; reserve a tighter ordinary
    // response prefix, with the fixed 128K-entry ceiling enforced by scanning.
    const u32 entries = minimum(r->response_bytes, 128u * 1024u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    const Utf8View json{reinterpret_cast<const char*>(r->response.base), r->response_bytes};
    const Slice<u32> scratch{reinterpret_cast<u32*>(r->structurals.base), entries};
    if (spotify || apple) {
        EmbedStreams staged{};
        const auto error = apple ? apple_music_track_preview(json, id, country, scratch,
                                                           video_preview ? &staged.video : &staged.audio, video_preview)
                                 : spotify_audio_preview(json, id, scratch, &staged.audio,episode);
        if (error == E::none) {
            staged.expires_seconds = 3600;
            staged.combined = video_preview;
            *out = staged;
        }
        secure_clear_bytes(&staged, sizeof(staged));
        return {error, UpdateTransportError::none, 0, response.status};
    }
    const auto error = tiktok ? tiktok_player_streams(json, id, height, now, scratch, out)
                              : twitch ? twitch_clip_streams(json, id, height, now, scratch, out)
                              : streamable ? streamable_video_streams(json, id, height, now, scratch, out)
                              : twitter ? twitter_video_streams(json, id, height, media_index, scratch, out)
                                       : youtube_player_streams(json, id, height, scratch, out);
    return {error, UpdateTransportError::none, 0, response.status};
}
EmbedResolveResult pornhub_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport) noexcept {
    using E = EmbedPlayerError;
    u8 target[64];
    const u32 bytes = pornhub_embed_target(id, {target, sizeof(target)});
    if (!r || !out || !bytes || !now || !height || height > 2048) return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512u * 1024u))) return {E::capacity};
    ScratchLifetime lifetime{r};
    struct Metadata { EmbedStream stream{}; ~Metadata() noexcept { secure_clear_bytes(this, sizeof(*this)); } } metadata;
    EmbedStreamLocation location{utf8_literal("www.pornhub.com"), {reinterpret_cast<char*>(target), bytes}};
    const HttpsHeader headers[]{{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")},
        {utf8_literal("Referer"), utf8_literal("https://www.pornhub.com/")}};
    for (u32 step = 0; step < 2; ++step) {
        r->response_bytes = 0;
        const UpdateHttpRequest request{UpdateHost::provider, location.host, {location.target.data, location.target.bytes},
            kPornhubMetadataMaximumBytes, response_sink, r};
        UpdateHttpResponse response{};
        const auto fetched = transport.exchange(transport.context, request, {{headers, 2}}, &response);
        if (fetched != UpdateTransportError::none || response.status != 200)
            return {E::unavailable, fetched, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
        const u32 index_bytes = minimum(r->response_bytes * 4u, 512u * 1024u);
        if (!reserved_bytes_commit(&r->structurals, index_bytes)) return {E::capacity};
        const Slice<u32> scratch{reinterpret_cast<u32*>(r->structurals.base), index_bytes / 4};
        const Utf8View body{reinterpret_cast<char*>(r->response.base), r->response_bytes};
        const auto error = step ? pornhub_video_streams(body, height, now, scratch, out)
                               : pornhub_embed_metadata(body, id, scratch, &metadata.stream);
        if (error != E::none || step) return {error, {}, 0, response.status};
        if (!pornhub_metadata_location({metadata.stream.url, metadata.stream.url_bytes}, &location)) return {E::malformed};
    }
    return {E::unavailable};
}
EmbedResolveResult youtube_resolve(EmbedResolver* r, Utf8View id, u16 height, EmbedStreams* out,
    EmbedResolveTransport transport) noexcept { return resolve(r, id, height, out, transport, CdnHost::google_video, 0); }
EmbedResolveResult tiktok_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport) noexcept { return resolve(r, id, height, out, transport, CdnHost::tiktok_video, now); }
EmbedResolveResult twitch_clip_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport) noexcept { return resolve(r, id, height, out, transport, CdnHost::twitch_video, now); }
EmbedResolveResult spotify_preview_resolve(EmbedResolver* r, Utf8View id, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport, bool episode) noexcept { return resolve(r, id, 1, out, transport, CdnHost::spotify_preview, now,{},0,episode); }
EmbedResolveResult streamable_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport) noexcept { return resolve(r, id, height, out, transport, CdnHost::streamable_video, now); }
EmbedResolveResult twitter_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport, u8 index) noexcept { return resolve(r, id, height, out, transport, CdnHost::twitter_video, now, {}, index); }
EmbedResolveResult apple_music_preview_resolve(EmbedResolver* r, Utf8View id, Utf8View country, u64 now, EmbedStreams* out,
    EmbedResolveTransport transport, bool video) noexcept {
    return resolve(r, id, 1, out, transport, video ? CdnHost::apple_music_video_preview : CdnHost::apple_music_preview, now, country);
}
EmbedResolveResult amazon_music_preview_resolve(EmbedResolver* r,Utf8View page,u64 now,EmbedStreams* out,
    EmbedResolveTransport transport) noexcept {
    using E=EmbedPlayerError;
    const auto track=amazon_music_track_locator(page);
    if(!r || !out || !now || !track.id.bytes) return {E::invalid_input};
    const auto ready=transport_for(*r,transport);
    if(ready!=UpdateTransportError::none) return {E::unavailable,ready};
    char target[64];constexpr auto path=utf8_literal("/getSampleTrack/");
    copy_bytes(target,path.data,path.bytes);copy_bytes(target+path.bytes,track.id.data,track.id.bytes);
    u32 bytes=path.bytes+track.id.bytes;auto host=track.host;
    const HttpsHeader headers[]{{utf8_literal("User-Agent"),utf8_literal("Mozilla/5.0")}};
    // Two known redirects, never a generic redirect follower. No metadata body,
    // JSON index, cookies or account tokens are needed for these public previews.
    for(u32 hop=0;hop<2;++hop) {
        const UpdateHttpRequest request{UpdateHost::provider,host,{target,bytes},1,
            +[](void*,ConstSlice<u8> b) noexcept {return !b.count;},nullptr};
        UpdateHttpResponse response{};
        const auto fetched=transport.exchange(transport.context,request,{{headers,1}},&response);
        EmbedResolveResult result{E::unavailable,fetched,
            static_cast<u32>(minimum<u64>(response.retry_after_seconds,86400)),response.status};
        bool next=false;
        if(fetched==UpdateTransportError::none && response.status==302 &&
            response.location_bytes<=sizeof(response.location)) {
            const Utf8View location{response.location,response.location_bytes};
            if(!hop) {
                EmbedStreamLocation redirect{};
                if(amazon_music_redirect(location,track.id,&redirect)) {
                    host=utf8_literal("d2usgkmxlnua9y.cloudfront.net");bytes=redirect.target.bytes;
                    copy_bytes(target,redirect.target.data,bytes);next=true;
                } else result.error=E::malformed;
            } else {
                result.error=amazon_music_preview_stream(location,now,out);
                if(result.error==E::none) result.status=200;
            }
        }
        secure_clear_bytes(&response,sizeof(response));
        if(!next) return result;
    }
    return {E::unavailable};
}
void embed_resolver_cancel(EmbedResolver* r) noexcept {
    if (!r) return;
    const LONG state = InterlockedExchange(&r->native_state, 3);
    InterlockedExchange(&r->https.cancelled, 1);
    if (state == 2) update_https_cancel(&r->https);
}
EmbedResolveResult steam_resolve(EmbedResolver* r, Utf8View app, Utf8View movie, u16 height,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport) noexcept {
    using E = EmbedPlayerError;
    u8 target[128]; const u32 bytes = steam_video_target(app, {target, sizeof(target)});
    if (!r || !video || !audio || video == audio || video->state != ProgressiveVideoFetchState::empty ||
        audio->state != ProgressiveVideoFetchState::empty || !bytes || (movie.bytes && !steam_id(movie)) || !height || height > 2048)
        return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512 * 1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    const HttpsHeader headers[]{{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")}};
    const auto get = [&](Utf8View host, Utf8View path, u32 limit) noexcept {
        r->response_bytes = 0;
        const UpdateHttpRequest request{UpdateHost::provider, host, {path.data, path.bytes}, limit, response_sink, r};
        UpdateHttpResponse response{};
        const auto error = transport.exchange(transport.context, request, {{headers, 1}}, &response);
        return EmbedResolveResult{error == UpdateTransportError::none && response.status == 200 ? E::none : E::unavailable,
            error, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
    };
    auto result = get(utf8_literal("store.steampowered.com"), {reinterpret_cast<const char*>(target), bytes}, 128 * 1024);
    if (!result.ok()) return result;
    const u32 entries = minimum(r->response_bytes, 128u * 1024u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    EmbedStream manifest{};
    result.error = steam_video_manifest({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, app, movie,
        {reinterpret_cast<u32*>(r->structurals.base), entries}, &manifest);
    if (!result.ok()) return result;
    EmbedStreamLocation location{};
    const Utf8View url{manifest.url, manifest.url_bytes};
    if (!steam_manifest_location(url, app, &location)) return {E::malformed};
    result = get(location.host, location.target, 64 * 1024);
    if (!result.ok()) return result;
    DashManifest selected{};
    const auto parsed = dash_manifest_parse({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, height, &selected);
    if (parsed != DashManifestError::none) { result.error = parsed == DashManifestError::unsupported ? E::unsupported : E::malformed; return result; }
    if (!progressive_video_fetch_begin_segments(video, url, selected.video, false) ||
        (selected.audio.id.bytes && !progressive_video_fetch_begin_segments(audio, url, selected.audio, true))) {
        progressive_video_fetch_release(video); progressive_video_fetch_release(audio); result.error = E::capacity;
    }
    return result;
}
void embed_resolver_release(EmbedResolver* r) noexcept {
    if (!r) return;
    if (r->https.winsock) update_https_release(&r->https);
    if (r->response.committed) secure_clear_bytes(r->response.base, r->response.committed);
    reserved_bytes_release(&r->response);
    reserved_bytes_release(&r->structurals);
    secure_clear_bytes(&r->bootstrap, sizeof(r->bootstrap));
    r->response_bytes = 0;
}
static EmbedResolveResult hls_media_resolve(EmbedResolver* r,u16 height,u64 now,
    ProgressiveVideoFetch* video,ProgressiveVideoFetch* audio,EmbedResolveTransport transport,bool live,bool youtube,Utf8View base = {}) noexcept {
    using E=EmbedPlayerError;
    const bool vimeo = base.bytes != 0;
    struct Urls { EmbedStream video{},audio{}; ~Urls() noexcept {secure_clear_bytes(this,sizeof(*this));} } urls;
    EmbedResolveResult result{E::none,UpdateTransportError::none,0,200};
    const auto locate=[&](Utf8View url,EmbedStreamLocation* out) noexcept {
        if (vimeo) return vimeo_location(url, true, out);
        return youtube ? embed_stream_location(CdnHost::youtube_playlist,url,out) :
            live ? twitch_live_location(url,true,out) : twitch_vod_location(url,true,out);
    };
    const auto exchange=[&](Utf8View host,Utf8View path,u32 limit) noexcept {
        r->response_bytes=0;
        const UpdateHttpRequest request{UpdateHost::provider,host,{path.data,path.bytes},limit,response_sink,r};
        const HttpsHeader header{utf8_literal("User-Agent"),youtube?kAgent:utf8_literal("Mozilla/5.0")};
        UpdateHttpResponse response{};
        const auto error=transport.exchange(transport.context,request,{{&header,1}},&response);
        return EmbedResolveResult{error==UpdateTransportError::none && response.status==200?E::none:E::unavailable,
            error,static_cast<u32>(minimum<u64>(response.retry_after_seconds,86400)),response.status};
    };
    // Reuse retired JSON scratch to retain this small master during bounded
    // quality fallback. It never needs another request or allocation.
    if (!reserved_bytes_commit(&r->structurals, r->response_bytes)) return {E::capacity};
    copy_bytes(r->structurals.base, r->response.base, r->response_bytes);
    const Utf8View master_text{reinterpret_cast<const char*>(r->structurals.base), r->response_bytes};
    HlsMasterPlaylist master{};
    EmbedStream* saved[]{&urls.video, &urls.audio};
    const auto select = [&](u32 budget) noexcept {
        const auto parsed = hls_master_playlist(master_text, height, &master, budget, vimeo);
        if (parsed != HlsPlaylistError::none) return parsed == HlsPlaylistError::unsupported ? E::unsupported : E::malformed;
        const Utf8View selected[]{master.video.uri, master.audio.uri}; u32 root[2]{};
        for (u32 i = 0; i < 2; ++i) {
            if (vimeo) {
                saved[i]->url_bytes = hls_resolve_uri(base, selected[i], {saved[i]->url, sizeof(saved[i]->url)});
                if (!saved[i]->url_bytes || !locate({saved[i]->url, saved[i]->url_bytes}, nullptr)) return E::unsupported;
                continue;
            }
            if (!locate(selected[i],nullptr)) return E::unsupported;
            copy_bytes(saved[i]->url, selected[i].data, selected[i].bytes); saved[i]->url_bytes = selected[i].bytes;
            if (live) continue;
            u32 at = selected[i].bytes;
            while (at && selected[i].data[at - 1] != '/') --at;
            if (at) --at;
            while (at && selected[i].data[at - 1] != '/') --at;
            root[i] = at;
        }
        return (!live && !vimeo && (!root[0] || root[0] != root[1] || !bytes_equal(urls.video.url, urls.audio.url, root[0]))) ||
            utf8_equal(selected[0], selected[1]) ? E::malformed : E::none;
    };
    result.error = select(~u32{0});
    if (!result.ok()) return result;
    ProgressiveVideoFetch* outputs[]{video, audio};
    for (u32 i = 0; i < 2; ++i) {
        bool started = false;
        for (u32 attempt = 0; attempt < 3; ++attempt) {
            const Utf8View url{saved[i]->url, saved[i]->url_bytes}; EmbedStreamLocation location{};
            if (!locate(url,&location)) { result.error = E::malformed; break; }
            result = exchange(location.host, location.target, (live ? 64u : 256u) * 1024);
            if (!result.ok()) break;
            const Utf8View text{reinterpret_cast<const char*>(r->response.base), r->response_bytes};
            if (!i) {
                HlsMediaPlaylist playlist{};
                if (hls_media_playlist(text, &playlist, live ? HlsPlaylistMode::sliding : vimeo ? HlsPlaylistMode::complete_uri : HlsPlaylistMode::complete) != HlsPlaylistError::none) { result.error = E::unsupported; break; }
                // Peak advertised bitrate and the longest actual segment must
                // fit the existing 8 MiB slot, leaving 64 KiB of headroom.
                const u32 budget = static_cast<u32>(minimum<u64>(~u32{0},
                    u64{8 * 1024 * 1024 - 64 * 1024} * 8 * 1000000 / playlist.maximum_duration_us));
                if (master.video.bandwidth > budget) {
                    result.error = select(budget);
                    if (!result.ok()) break;
                    continue;
                }
            }
            started = progressive_video_fetch_begin_hls(outputs[i], url, text, i != 0, live, now);
            if (!started) result.error = E::unsupported;
            break;
        }
        if (!started) { if (result.ok()) result.error = E::unsupported; break; }
    }
    if (result.ok() && live && video->segmented->playlist.transport != audio->segmented->playlist.transport)
        result.error = E::unsupported;
    if (!result.ok()) { progressive_video_fetch_release(video); progressive_video_fetch_release(audio); }
    return result;
}
namespace {
EmbedResolveResult public_metadata_exchange(EmbedResolver* r, EmbedResolveTransport transport,
    Utf8View host, Utf8View target, u32 limit, UpdateHttpResponse* headers = nullptr) noexcept {
    r->response_bytes = 0;
    const UpdateHttpRequest request{UpdateHost::provider, host, {target.data, target.bytes}, limit, response_sink, r};
    const HttpsHeader agent{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")};
    UpdateHttpResponse response{};
    const auto error = transport.exchange(transport.context, request, {{&agent, 1}}, &response);
    if (headers) *headers=response;
    return {error == UpdateTransportError::none && response.status == 200 ? EmbedPlayerError::none : EmbedPlayerError::unavailable,
        error, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
}
// Leaves one bounded metadata response for parsing. Numeric widgets must bind
// the discovered canonical page back to their numeric identity in the parser.
EmbedResolveResult soundcloud_page(EmbedResolver* r, Utf8View page, bool collection, Slice<char> decoded,
    SoundcloudCollectionLocator* out, EmbedResolveTransport& transport) noexcept {
    using E = EmbedPlayerError;
    SoundcloudCollectionLocator locator{};
    if (collection) locator = soundcloud_collection_locator(page, decoded);
    else {
        const auto track = soundcloud_track_locator(page, decoded);
        locator = {track.path, track.id};
    }
    if (!locator.path.bytes && !locator.id) return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512 * 1024))) return {E::capacity};
    if (locator.id) {
        char widget[128]; ProtocolWriter writer{{reinterpret_cast<u8*>(widget), sizeof(widget)}, 0};
        if (!protocol_writer_append_literal(&writer, "/player/?url=https%3A%2F%2Fapi.soundcloud.com%2F") ||
            !(collection ? protocol_writer_append_literal(&writer, "playlists%2F") : protocol_writer_append_literal(&writer, "tracks%2F")) ||
            !protocol_writer_append_u64(&writer, locator.id)) return {E::capacity};
        auto result = public_metadata_exchange(r, transport, utf8_literal("w.soundcloud.com"), {widget, writer.used}, 65536);
        if (!result.ok()) return result;
        EmbedStream canonical{};
        result.error = soundcloud_widget_page({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, &canonical, collection);
        if (!result.ok()) return result;
        const auto path = collection ? soundcloud_collection_path({canonical.url, canonical.url_bytes}) :
            soundcloud_track_path({canonical.url, canonical.url_bytes});
        if (!path.bytes || path.bytes > decoded.count) return {E::malformed};
        copy_bytes(decoded.data, path.data, path.bytes); locator.path = {decoded.data, path.bytes};
    }
    *out = locator;
    return public_metadata_exchange(r, transport, utf8_literal("soundcloud.com"), locator.path, 512 * 1024);
}
} // namespace
EmbedResolveResult track_title_resolve(EmbedResolver* r, u64 track, Utf8View video, MediaTrackTitle* out,
    EmbedResolveTransport transport) noexcept {
    using E = EmbedPlayerError;
    if (!r || !out || (!track && video.bytes != 11)) return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512 * 1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    char path[160]; ProtocolWriter writer{{reinterpret_cast<u8*>(path), sizeof(path)}, 0};
    if (video.bytes) {
        if (!protocol_writer_append_literal(&writer, "/oembed?format=json&url=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3D") || !protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(video.data), video.bytes)) return {E::capacity};
    } else if (!protocol_writer_append_literal(&writer, "/oembed?format=json&url=https%3A%2F%2Fapi.soundcloud.com%2Ftracks%2F") || !protocol_writer_append_u64(&writer, track)) return {E::capacity};
    auto result = public_metadata_exchange(r, transport, video.bytes ? utf8_literal("www.youtube.com") : utf8_literal("soundcloud.com"), {path, writer.used}, 32768);
    if (!result.ok()) return result;
    const u32 entries = minimum(r->response_bytes, 8192u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    const Utf8View json{reinterpret_cast<const char*>(r->response.base), r->response_bytes};
    const Slice<u32> scratch{reinterpret_cast<u32*>(r->structurals.base), entries};
    result.error = video.bytes ? youtube_track_title(json, video, scratch, out) : soundcloud_track_title(json, track, scratch, out);
    return result;
}
EmbedResolveResult soundcloud_title_resolve(EmbedResolver* r, u64 track, MediaTrackTitle* out, EmbedResolveTransport t) noexcept { return track_title_resolve(r, track, {}, out, t); }
EmbedResolveResult youtube_title_resolve(EmbedResolver* r, Utf8View video, MediaTrackTitle* out, EmbedResolveTransport t) noexcept { return track_title_resolve(r, 0, video, out, t); }

EmbedResolveResult youtube_collection_resolve(EmbedResolver* r, Utf8View page, Utf8View continuation,
    YoutubeCollectionPage* out, EmbedResolveTransport transport, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    const auto locator = youtube_collection_locator(page);
    if (!r || !out || !locator.list.bytes || continuation.bytes > 16384) return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512 * 1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    constexpr auto host = utf8_literal("music.youtube.com");
    const HttpsHeader headers[]{{utf8_literal("User-Agent"), kAgent}, {utf8_literal("Cookie"), utf8_literal("SOCS=CAI")},
        {utf8_literal("Content-Type"), utf8_literal("application/json")}, {utf8_literal("Origin"), utf8_literal("https://music.youtube.com")}};
    if (!r->music_version_bytes) {
        r->response_bytes = 0; UpdateHttpResponse response{};
        const UpdateHttpRequest request{UpdateHost::provider, host, {"/", 1}, kResponseLimit, response_sink, r};
        const auto fetched = transport.exchange(transport.context, request, {{headers, 2}}, &response);
        if (fetched != UpdateTransportError::none || response.status != 200) return {E::unavailable, fetched, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
        constexpr auto marker = utf8_literal("\"INNERTUBE_CLIENT_VERSION\":\"");
        const auto* data = reinterpret_cast<const char*>(r->response.base);
        for (u32 i = 0; i + marker.bytes < r->response_bytes; ++i) {
            if (!bytes_equal(data + i, marker.data, marker.bytes)) continue;
            const u32 first = i + marker.bytes; u32 end = first;
            while (end < r->response_bytes && (ascii_is_digit(data[end]) || data[end] == '.')) ++end;
            if (end > first && end < r->response_bytes && data[end] == '"' && end - first <= sizeof(r->music_version)) {
                copy_bytes(r->music_version, data + first, end - first); r->music_version_bytes = static_cast<u8>(end - first);
            }
            break;
        }
        if (!r->music_version_bytes) return {E::malformed};
    }
    u8 body[1024], target[50176]; ProtocolWriter writer{{body, sizeof(body)}, 0}, path{{target, sizeof(target)}, 0};
    bool ok = protocol_writer_append_literal(&writer, "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":") &&
        protocol_writer_append_json_string(&writer, {r->music_version, r->music_version_bytes}) &&
        protocol_writer_append_literal(&writer, ",\"hl\":\"en\",\"gl\":\"US\"}},\"enablePersistentPlaylistPanel\":true,\"isAudioOnly\":true,\"playlistId\":") &&
        protocol_writer_append_json_string(&writer, locator.list);
    if (locator.video.bytes) ok = ok && protocol_writer_append_literal(&writer, ",\"videoId\":") && protocol_writer_append_json_string(&writer, locator.video);
    // A full watch response includes menus and unrelated UI data and can
    // exceed the metadata budget. Request only the fields consumed below.
    constexpr auto queue_fields = utf8_literal(
        "contents.singleColumnMusicWatchNextResultsRenderer.tabbedRenderer.watchNextTabbedResultsRenderer."
        "tabs.tabRenderer.content.musicQueueRenderer.content.playlistPanelRenderer"
        "(playlistId,contents(playlistPanelVideoRenderer(videoId,selected,title),"
        "playlistPanelVideoWrapperRenderer(primaryRenderer(playlistPanelVideoRenderer(videoId,selected,title)))),continuations),"
        "continuationContents.playlistPanelContinuation"
        "(playlistId,contents(playlistPanelVideoRenderer(videoId,selected,title),"
        "playlistPanelVideoWrapperRenderer(primaryRenderer(playlistPanelVideoRenderer(videoId,selected,title)))),continuations)");
    ok = ok && protocol_writer_append_literal(&writer, ",\"params\":\"wAEB\"}") &&
        protocol_writer_append_literal(&path, "/youtubei/v1/next?prettyPrint=false&fields=") &&
        protocol_writer_append_percent_encoded(&path, queue_fields);
    if (continuation.bytes) ok = ok && protocol_writer_append_literal(&path, "&continuation=") && protocol_writer_append_percent_encoded(&path, continuation);
    if (!ok) return {E::capacity};
    r->response_bytes = 0; UpdateHttpResponse response{};
    const UpdateHttpRequest request{UpdateHost::provider, host, {reinterpret_cast<const char*>(target), path.used}, kResponseLimit, response_sink, r};
    HttpsExchangeOptions options{{headers, 4}}; options.body = {body, writer.used};
    const auto fetched = transport.exchange(transport.context, request, options, &response);
    EmbedResolveResult result{E::unavailable, fetched, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
    if (fetched != UpdateTransportError::none || response.status != 200) {
        if (response.status == 400) r->music_version_bytes = 0;
        return result;
    }
    const u32 entries = minimum(r->response_bytes, 128u * 1024u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    result.error = youtube_collection_page({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, locator.list,
        {reinterpret_cast<u32*>(r->structurals.base), entries}, out, sink);
    return result;
}

EmbedResolveResult soundcloud_batch_titles_resolve(EmbedResolver* r, const SoundcloudTitleBatch& batch, u16 preferred,
    MediaTrackTitleSink sink, EmbedResolveTransport transport) noexcept {
    using E = EmbedPlayerError;
    if (!r || !batch.count || batch.count > 50 || !sink.write) return {E::invalid_input};
    for (char c : batch.client) if (!ascii_is_alnum(c)) return {E::invalid_input};
    char target[1280]; ProtocolWriter path{{reinterpret_cast<u8*>(target),sizeof(target)},0};
    bool ok = protocol_writer_append_literal(&path,"/tracks?ids=");
    for (u32 i=0;i<batch.count;++i) {
        if (!batch.tracks[i].id || !batch.tracks[i].index) return {E::invalid_input};
        bool repeated=false;
        for (u32 j=0;j<i;++j) if (batch.tracks[j].id==batch.tracks[i].id) { repeated=true; break; }
        if (!repeated) ok = ok && (!i || protocol_writer_append_byte(&path,',')) && protocol_writer_append_u64(&path,batch.tracks[i].id);
    }
    ok = ok && protocol_writer_append_literal(&path,"&client_id=") && protocol_writer_append_bytes(&path,reinterpret_cast<const u8*>(batch.client),32);
    if (!ok) return {E::invalid_input};
    const auto prepared=transport_for(*r,transport);
    if (prepared!=UpdateTransportError::none) return {E::unavailable,prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response,kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals,512*1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    auto result=public_metadata_exchange(r,transport,utf8_literal("api-v2.soundcloud.com"),{target,path.used},512*1024);
    if (!result.ok()) return result;
    const u32 entries=minimum(r->response_bytes,128u*1024u);
    if (!reserved_bytes_commit(&r->structurals,entries*sizeof(u32))) return {E::capacity};
    result.error=soundcloud_batch_titles({reinterpret_cast<const char*>(r->response.base),r->response_bytes},batch,preferred,
        {reinterpret_cast<u32*>(r->structurals.base),entries},sink);
    return result;
}
EmbedResolveResult apple_music_collection_resolve(EmbedResolver* r, Utf8View page, u16* count, AppleMusicCollectionSink sink, EmbedResolveTransport transport) noexcept {
    using E=EmbedPlayerError;
    const auto collection=apple_music_collection_locator(page);
    if (!r || !count || !sink.write || !collection.id.bytes) return {E::invalid_input};
    const auto prepared=transport_for(*r,transport);
    if (prepared!=UpdateTransportError::none) return {E::unavailable,prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response,kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals,512*1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    const auto index=[&]() noexcept { return reserved_bytes_commit(&r->structurals,minimum(r->response_bytes,128u*1024u)*sizeof(u32)); };
    const auto scratch=[&]() noexcept { return Slice<u32>{reinterpret_cast<u32*>(r->structurals.base),minimum(r->response_bytes,128u*1024u)}; };
    const auto body=[&]() noexcept { return Utf8View{reinterpret_cast<char*>(r->response.base),r->response_bytes}; };
    u8 target[2048];
    if (!collection.playlist) {
        const u32 start=apple_music_track_target(collection.id,collection.country,{target,sizeof(target)});
        ProtocolWriter path{{target,sizeof(target)},start};
        if (!start || !protocol_writer_append_literal(&path,"&entity=song&limit=200")) return {E::invalid_input};
        auto result=public_metadata_exchange(r,transport,utf8_literal("itunes.apple.com"),{reinterpret_cast<char*>(target),path.used},512*1024);
        if (!result.ok()) return result;
        if (!index()) return {E::capacity};
        result.error=apple_music_album(body(),collection.id,collection.country,scratch(),count,sink); return result;
    }
    if (collection.path.bytes>sizeof(target)) return {E::invalid_input};
    copy_bytes(target,collection.path.data,collection.path.bytes); u32 target_bytes=collection.path.bytes;
    EmbedResolveResult result{};
    for (u32 hop=0;hop<2;++hop) {
        UpdateHttpResponse headers{};
        result=public_metadata_exchange(r,transport,utf8_literal("music.apple.com"),{reinterpret_cast<char*>(target),target_bytes},2*1024*1024,&headers);
        if (result.ok()) break;
        if (hop || result.transport_error!=UpdateTransportError::none ||
            (headers.status!=301 && headers.status!=302 && headers.status!=307 && headers.status!=308) || headers.location_bytes>sizeof(headers.location)) return result;
        const auto next=apple_music_collection_locator({headers.location,headers.location_bytes});
        if (!next.playlist || !utf8_equal(next.id,collection.id) || !ascii_equal_ignore_case(next.country,collection.country) || next.path.bytes>sizeof(target)) return {E::malformed};
        copy_bytes(target,next.path.data,next.path.bytes); target_bytes=next.path.bytes;
    }
    if (!result.ok() || !index()) return result.ok()?EmbedResolveResult{E::capacity}:result;
    AppleMusicPlaylist playlist{};
    result.error=apple_music_playlist(body(),collection.id,collection.country,scratch(),&playlist,sink);
    if (!result.ok()) return result;
    for (u32 first=0;first<playlist.count;first+=50) {
        const u32 size=minimum<u32>(50,playlist.count-first); const ConstSlice<u64> ids{playlist.tracks+first,size};
        ProtocolWriter path{{target,sizeof(target)},0}; bool ok=protocol_writer_append_literal(&path,"/lookup?id="), any=false;
        for (u32 i=0;i<size;++i) {
            if (!ids[i]) continue;
            bool repeat=false; for (u32 j=0;j<i;++j) if (ids[j]==ids[i]) { repeat=true; break; }
            if (repeat) continue;
            ok=ok && (!any || protocol_writer_append_byte(&path,',')) && protocol_writer_append_u64(&path,ids[i]); any=true;
        }
        if (!any) continue;
        ok=ok && protocol_writer_append_literal(&path,"&country=") && protocol_writer_append_byte(&path,ascii_lower(collection.country.data[0])) && protocol_writer_append_byte(&path,ascii_lower(collection.country.data[1]));
        if (!ok) return {E::capacity};
        result=public_metadata_exchange(r,transport,utf8_literal("itunes.apple.com"),{reinterpret_cast<char*>(target),path.used},512*1024);
        if (!result.ok()) return result;
        if (!index()) return {E::capacity};
        result.error=apple_music_preview_batch(body(),ids,static_cast<u16>(first),collection.country,scratch(),sink);
        if (!result.ok()) return result;
    }
    *count=playlist.count; return result;
}
EmbedResolveResult soundcloud_collection_resolve(EmbedResolver* r, Utf8View page, Slice<u64> tracks,
    SoundcloudCollection* out, EmbedResolveTransport transport, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    if (!r || !out || !tracks.data || !tracks.count) return {E::invalid_input};
    ScratchLifetime lifetime{r};
    char decoded[1024]; SoundcloudCollectionLocator locator{};
    auto result = soundcloud_page(r, page, true, {decoded, sizeof(decoded)}, &locator, transport);
    if (!result.ok()) return result;
    const u32 entries = minimum(r->response_bytes, 128u * 1024u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    result.error = soundcloud_collection({reinterpret_cast<const char*>(r->response.base), r->response_bytes},
        locator.path, locator.id, locator.start_track, {reinterpret_cast<u32*>(r->structurals.base), entries}, tracks, out, sink);
    return result;
}
EmbedResolveResult soundcloud_resolve(EmbedResolver* r, Utf8View page, ProgressiveVideoFetch* audio,
    EmbedResolveTransport transport) noexcept {
    using E = EmbedPlayerError;
    if (!r || !audio || audio->state != ProgressiveVideoFetchState::empty) return {E::invalid_input};
    ScratchLifetime lifetime{r};
    char decoded[1024]; SoundcloudCollectionLocator locator{};
    auto result = soundcloud_page(r, page, false, {decoded, sizeof(decoded)}, &locator, transport);
    if (!result.ok()) return result;
    struct Url { EmbedStream value{}; ~Url() noexcept { secure_clear_bytes(this, sizeof(*this)); } } url;
    const u32 entries = minimum(r->response_bytes, 128u * 1024u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    result.error = soundcloud_track_request({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, locator.path,
        {reinterpret_cast<u32*>(r->structurals.base), entries}, &url.value, locator.id);
    if (!result.ok()) return result;
    constexpr u32 origin = sizeof("https://api-v2.soundcloud.com") - 1;
    result = public_metadata_exchange(r, transport, utf8_literal("api-v2.soundcloud.com"), {url.value.url + origin, url.value.url_bytes - origin}, 8192);
    if (!result.ok()) return result;
    result.error = soundcloud_manifest({reinterpret_cast<const char*>(r->response.base), r->response_bytes},
        {reinterpret_cast<u32*>(r->structurals.base), entries}, &url.value);
    if (!result.ok()) return result;
    EmbedStreamLocation location{};
    if (!soundcloud_location({url.value.url, url.value.url_bytes}, true, &location)) return {E::malformed};
    result = public_metadata_exchange(r, transport, location.host, location.target, 65536);
    if (result.ok() && !progressive_video_fetch_begin_hls(audio, {url.value.url, url.value.url_bytes},
        {reinterpret_cast<const char*>(r->response.base), r->response_bytes}, true)) result.error = E::unsupported;
    return result;
}
EmbedResolveResult vimeo_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport, Utf8View privacy_hash) noexcept {
    using E = EmbedPlayerError;
    if (!r || !video || !audio || video == audio || !steam_id(id) || !now || !height || height > 2048 ||
        (privacy_hash.bytes && !vimeo_privacy_hash(privacy_hash)) ||
        video->state != ProgressiveVideoFetchState::empty || audio->state != ProgressiveVideoFetchState::empty)
        return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512 * 1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    const auto exchange = [&](Utf8View host, Utf8View path, u32 limit) noexcept {
        r->response_bytes = 0;
        const UpdateHttpRequest request{UpdateHost::provider, host, {path.data, path.bytes}, limit, response_sink, r};
        const HttpsHeader agent{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")};
        UpdateHttpResponse response{};
        const auto error = transport.exchange(transport.context, request, {{&agent, 1}}, &response);
        return EmbedResolveResult{error == UpdateTransportError::none && response.status == 200 ? E::none : E::unavailable,
            error, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
    };
    char path[32] = "/video/";
    copy_bytes(path + 7, id.data, id.bytes);
    u32 path_bytes = 7 + id.bytes;
    if (privacy_hash.bytes) {
        copy_bytes(path + path_bytes, "?h=", 3);
        copy_bytes(path + path_bytes + 3, privacy_hash.data, privacy_hash.bytes);
        path_bytes += 3 + privacy_hash.bytes;
    }
    auto result = exchange(utf8_literal("player.vimeo.com"), {path, path_bytes}, 128 * 1024);
    if (!result.ok()) return result;
    if (!reserved_bytes_commit(&r->structurals, r->response_bytes * sizeof(u32))) return {E::capacity};
    struct Manifest { EmbedStream value{}; ~Manifest() noexcept { secure_clear_bytes(this, sizeof(*this)); } } manifest;
    result.error = vimeo_video_manifest({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, id,
        {reinterpret_cast<u32*>(r->structurals.base), r->response_bytes}, &manifest.value, privacy_hash);
    if (!result.ok()) return result;
    const Utf8View url{manifest.value.url, manifest.value.url_bytes};
    EmbedStreamLocation location{};
    if (!vimeo_location(url, true, &location)) return {E::unsupported};
    result = exchange(location.host, location.target, 64 * 1024);
    if (!result.ok()) return result;
    return hls_media_resolve(r, height, now, video, audio, transport, false, false, url);
}
EmbedResolveResult twitch_hls_resolve(EmbedResolver* r, Utf8View id, u16 height, u64 now,
    ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio, EmbedResolveTransport transport, bool live) noexcept {
    using E = EmbedPlayerError;
    u8 body[512]; const u32 bytes = twitch_playback_body(id, {body, sizeof(body)}, live);
    if (!r || !video || !audio || video == audio || !bytes || !now || !height || height > 2048 ||
        video->state != ProgressiveVideoFetchState::empty || audio->state != ProgressiveVideoFetchState::empty) return {E::invalid_input};
    const auto prepared = transport_for(*r, transport);
    if (prepared != UpdateTransportError::none) return {E::unavailable, prepared};
    if ((!r->response.base && !reserved_bytes_initialize(&r->response, kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals, 512 * 1024))) return {E::capacity};
    ScratchLifetime lifetime{r};
    const HttpsHeader headers[]{{utf8_literal("User-Agent"), utf8_literal("Mozilla/5.0")},
        {utf8_literal("Content-Type"), utf8_literal("application/json")},
        {utf8_literal("Client-ID"), utf8_literal("b31o4btkqth5bzbvr9ub2ovr79umhh")}};
    const auto exchange = [&](Utf8View host, Utf8View path, u32 limit, bool token = false) noexcept {
        r->response_bytes = 0;
        const UpdateHttpRequest request{UpdateHost::provider, host, {path.data, path.bytes}, limit, response_sink, r};
        UpdateHttpResponse response{};
        const auto error = transport.exchange(transport.context, request,
            {{headers, token ? 3u : 1u}, {body, token ? bytes : 0u}}, &response);
        return EmbedResolveResult{error == UpdateTransportError::none && response.status == 200 ? E::none : E::unavailable,
            error, static_cast<u32>(minimum<u64>(response.retry_after_seconds, 86400)), response.status};
    };
    auto result = exchange(utf8_literal("gql.twitch.tv"), utf8_literal("/gql"), 64 * 1024, true);
    if (!result.ok()) return result;
    const u32 entries = minimum(r->response_bytes, 64u * 1024u);
    if (!reserved_bytes_commit(&r->structurals, entries * sizeof(u32))) return {E::capacity};
    struct Urls { EmbedStream manifest{}, video{}, audio{}; ~Urls() noexcept { secure_clear_bytes(this, sizeof(*this)); } } urls;
    result.error = twitch_playback_manifest({reinterpret_cast<const char*>(r->response.base), r->response_bytes}, id, now,
        {reinterpret_cast<u32*>(r->structurals.base), entries}, &urls.manifest, live);
    if (!result.ok()) return result;
    constexpr u32 usher_origin = sizeof("https://usher.ttvnw.net") - 1;
    result = exchange(utf8_literal("usher.ttvnw.net"), {urls.manifest.url + usher_origin, urls.manifest.url_bytes - usher_origin}, 64 * 1024);
    if (!result.ok()) return result;
    result = hls_media_resolve(r,height,now,video,audio,transport,live,false);
    // A live playlist is a changing snapshot. Unsupported contents must not
    // permanently negative-cache the channel URL after this snapshot expires.
    if (live && result.error == E::unsupported) result.error = E::unavailable;
    return result;
}
EmbedResolveResult youtube_hls_resolve(EmbedResolver* r,Utf8View manifest,u16 height,u64 now,
    ProgressiveVideoFetch* video,ProgressiveVideoFetch* audio,EmbedResolveTransport transport) noexcept {
    using E=EmbedPlayerError;EmbedStreamLocation location{};
    if(!r || !video || !audio || video==audio || !now || !height || height>2048 ||
        video->state!=ProgressiveVideoFetchState::empty || audio->state!=ProgressiveVideoFetchState::empty ||
        !embed_stream_location(CdnHost::youtube_playlist,manifest,&location) ||
        !utf8_starts_with(location.target,utf8_literal("/api/manifest/hls_variant/"))) return {E::invalid_input};
    const auto prepared=transport_for(*r,transport);
    if(prepared!=UpdateTransportError::none) return {E::unavailable,prepared};
    if((!r->response.base && !reserved_bytes_initialize(&r->response,kResponseLimit)) ||
        (!r->structurals.base && !reserved_bytes_initialize(&r->structurals,512*1024))) return {E::capacity};
    ScratchLifetime lifetime{r};r->response_bytes=0;
    const HttpsHeader header{utf8_literal("User-Agent"),kAgent};
    const UpdateHttpRequest request{UpdateHost::provider,location.host,{location.target.data,location.target.bytes},65536,response_sink,r};
    UpdateHttpResponse response{};
    const auto error=transport.exchange(transport.context,request,{{&header,1}},&response);
    if(error!=UpdateTransportError::none || response.status!=200) return {E::unavailable,error,0,response.status};
    return hls_media_resolve(r,height,now,video,audio,transport,true,true);
}
} // namespace maicord
