#include "maicord/app/embed_resolver.hpp"
#include "maicord/app/account_media.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/app/cdn_client.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
bool soundcloud_player_tests() noexcept;
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
// Answers a scripted exchange with a 200 and the body.
UpdateTransportError reply(const UpdateHttpRequest& request, UpdateHttpResponse* response, Utf8View body) noexcept {
    response->status = 200;
    return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
        ? UpdateTransportError::none : UpdateTransportError::sink;
}
template <class T> T* allocate() noexcept {
    return static_cast<T*>(VirtualAlloc(nullptr, sizeof(T), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
// The resolver and the two source owners a provider test resolves into; released on scope exit.
struct Owners {
    EmbedResolver* const resolver = allocate<EmbedResolver>();
    ProgressiveVideoFetch* const video = allocate<ProgressiveVideoFetch>();
    ProgressiveVideoFetch* const audio = allocate<ProgressiveVideoFetch>();
    ~Owners() noexcept {
        if (video) { progressive_video_fetch_release(video); VirtualFree(video, 0, MEM_RELEASE); }
        if (audio) { progressive_video_fetch_release(audio); VirtualFree(audio, 0, MEM_RELEASE); }
        if (resolver) { embed_resolver_release(resolver); VirtualFree(resolver, 0, MEM_RELEASE); }
    }
};
void paired_ranges() noexcept {
    auto* media = allocate<AccountMedia>();
    auto* video = allocate<ProgressiveVideoFetch>();
    auto* audio = allocate<ProgressiveVideoFetch>();
    check(media && video && audio, "paired range fixture allocations");
    if (media && video && audio) {
        media->progressive = video; media->progressive_audio = audio;
        constexpr u32 block = kProgressiveBlockBytes, total = 32 * block;
        static u8 bytes[block];
        check(progressive_source_initialize(&video->source, total) && progressive_source_initialize(&audio->source, total, true),
              "independent video and smaller audio source budgets");
        video->state = audio->state = ProgressiveVideoFetchState::ready;
        video->expires_at_seconds = audio->expires_at_seconds = ~u64{0};
        const auto reader = progressive_source_reader(&audio->source, ProgressiveTrack::audio);
        reader.available(reader.context, 8 * block, 64);
        progressive_video_fetch_submitted(video, 100, {0, block});
        ProgressiveVideoFetch* selected{};
        check(account_media_progressive_next(media, true, 1, &selected).step == ProgressiveVideoStep::cancel && selected == video,
              "missing audio preempts the single video request after a seek");
        progressive_video_fetch_cancelling(video);
        check(account_media_progressive_next(media, true, 1, &selected).step == ProgressiveVideoStep::idle && selected == video,
              "replacement waits for cancelled request retirement");
        CdnClientResponse response{}; response.request_id = 101; response.error = CdnClientError::cancelled;
        check(account_media_accept_progressive(media, response, {}) == ProgressiveVideoAccept::ignored && video->inflight_id == 100,
              "stale response cannot retire another source request");
        response.request_id = 100;
        check(account_media_accept_progressive(media, response, {}) == ProgressiveVideoAccept::ignored && !video->inflight_id,
              "cancelled video response retires without failing either source");
        const auto next = account_media_progressive_next(media, true, 1, &selected);
        check(next.step == ProgressiveVideoStep::submit && selected == audio && next.range.first == 8 * block && next.range.bytes == block,
              "audio demand uses one small request on the shared client");
        progressive_video_fetch_submitted(audio, 102, next.range);
        response = {}; response.request_id = 102; response.status_code = 206;
        response.requested_range = next.range; response.range_first = next.range.first;
        response.body_bytes = block; response.entity_bytes = total;
        check(account_media_accept_progressive(media, response, {bytes, block}) == ProgressiveVideoAccept::progress &&
              audio->source.resident_blocks == 1 && !video->source.resident_blocks &&
              account_media_accept_progressive(media, response, {bytes, block}) == ProgressiveVideoAccept::ignored,
              "audio bytes reach only their owner and duplicate replies are ignored");
        audio->state = ProgressiveVideoFetchState::failed;
        check(account_media_progressive_next(media, true, 1, &selected).step == ProgressiveVideoStep::failed &&
              video->state == ProgressiveVideoFetchState::failed,
              "audio transport failure ends the pair instead of leaving a silent buffering player");
    }
    if (audio) { progressive_video_fetch_release(audio); VirtualFree(audio, 0, MEM_RELEASE); }
    if (video) { progressive_video_fetch_release(video); VirtualFree(video, 0, MEM_RELEASE); }
    if (media) VirtualFree(media, 0, MEM_RELEASE);
}
constexpr auto kId = utf8_literal("aB_01234-xy");
void youtube_live_resolver_test() noexcept {
    Owners owners;
    EmbedResolver* r = owners.resolver; ProgressiveVideoFetch *v = owners.video, *a = owners.audio;
    struct Calls {u32 count{};bool deny{};} calls;
    const auto exchange=+[](void* opaque,const UpdateHttpRequest& request,const HttpsExchangeOptions& options,UpdateHttpResponse* response) noexcept {
        auto& c=*static_cast<Calls*>(opaque);const u32 call=c.count++;
        check(call<3 && request.maximum_body_bytes==65536 && options.headers.count==1 && !options.body.count &&
            utf8_equal(request.host_name,utf8_literal("manifest.googlevideo.com")) &&
            utf8_equal(options.headers[0].name,utf8_literal("User-Agent")),"YouTube live uses only bounded credential-free playlist GETs");
        const auto body=!call?utf8_literal("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Original\",DEFAULT=YES,URI=\"https://manifest.googlevideo.com/api/manifest/hls_playlist/id/audio/file/index.m3u8\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS=\"avc1.4D4020,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"aac\"\nhttps://manifest.googlevideo.com/api/manifest/hls_playlist/id/video/file/index.m3u8\n")
            :call==2 && c.deny?utf8_literal("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n")
            :utf8_literal("#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:5,\nhttps://rr1.googlevideo.com/videoplayback/id/example/sq/100/file/seg.ts\n");
        *response={};
        return reply(request,response,body);
    };
    check(r && v && a,"YouTube live mock owners");
    if(r && v && a) {
        constexpr auto master=utf8_literal("https://manifest.googlevideo.com/api/manifest/hls_variant/id/example/file/index.m3u8");
        const auto resolved=youtube_hls_resolve(r,master,720,1000,v,a,{&calls,exchange});
        check(resolved.ok() && calls.count==3 && v->segmented && a->segmented && v->segmented->youtube && a->segmented->youtube &&
            !r->response.committed && !r->structurals.committed,"YouTube live owns two playlists after resolver scratch is retired");
        const auto next=progressive_video_fetch_next(v,false,1000);
        check(next.step==ProgressiveVideoStep::submit && next.range.bytes==8*1024*1024 && v->host==CdnHost::youtube_live &&
            v->image_type==CdnImageType::mpeg_ts,"YouTube live starts on the bounded native transport lane");
        progressive_video_fetch_release(v);progressive_video_fetch_release(a);calls={};calls.deny=true;
        check(youtube_hls_resolve(r,master,720,1000,v,a,{&calls,exchange}).error==EmbedPlayerError::unsupported &&
            calls.count==3 && !v->segmented && !a->segmented,"protected live audio rolls back both YouTube tracks without a key request");
    }
}
void vimeo_resolver_test() noexcept {
    struct Fixture { EmbedResolver resolver; ProgressiveVideoFetch video, audio; u32 calls; bool deny, shared; };
    auto* f = allocate<Fixture>();
    check(f != nullptr, "Vimeo resolver fixture"); if (!f) return;
    const auto exchange = +[](void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                             UpdateHttpResponse* response) noexcept {
        auto& fixture = *static_cast<Fixture*>(context); const u32 call = fixture.calls++;
        check(call < 4 && options.headers.count == 1 && !options.body.count &&
              utf8_equal(options.headers[0].name, utf8_literal("User-Agent")) &&
              request.maximum_body_bytes == (!call ? 128u * 1024 : call == 1 ? 64u * 1024 : 256u * 1024),
              "Vimeo uses bounded credential-free metadata and playlist GETs");
        check(utf8_equal(request.host_name, !call ? utf8_literal("player.vimeo.com") :
              utf8_literal("vod-adaptive-ak.vimeocdn.com")), "Vimeo requests stay on validated origins");
        if (!call) check(utf8_equal({request.target.data, request.target.count}, fixture.shared
            ? utf8_literal("/video/22439234?h=0123456789") : utf8_literal("/video/22439234")),
            "Vimeo forwards the explicit hash only to the matching player metadata request");
        const auto body = !call && fixture.shared ? utf8_literal(R"(window.playerConfig = {"video":{"id":22439234,"privacy":"unlisted","embed_permission":"public","unlisted_hash":"0123456789"},"request":{"files":{"hls":{"default_cdn":"ak","cdns":{"ak":{"url":"https://vod-adaptive-ak.vimeocdn.com/public/v2/playlist/av/primary/playlist.m3u8"}}}}}};)")
            : !call ? utf8_literal(R"(window.playerConfig = {"video":{"id":22439234,"privacy":"anybody","embed_permission":"public"},"request":{"files":{"hls":{"default_cdn":"ak","cdns":{"ak":{"url":"https://vod-adaptive-ak.vimeocdn.com/public/v2/playlist/av/primary/playlist.m3u8"}}}}}};)")
            : call == 1 ? utf8_literal("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Original\",DEFAULT=YES,URI=\"../audio/media.m3u8\"\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS=\"avc1.4D4020,mp4a.40.2\",RESOLUTION=1280x720,AUDIO=\"aac\"\n../video/media.m3u8\n")
            : call == 3 && fixture.deny ? utf8_literal("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n")
            : utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n#EXT-X-MAP:URI=\"../../../range/prot/init?range=0-100\"\n"
                "#EXTINF:2,\n../../../range/prot/one?range=0-100\n#EXT-X-ENDLIST\n");
        return reply(request, response, body);
    };
    auto result = vimeo_resolve(&f->resolver, utf8_literal("22439234"), 720, 1000, &f->video, &f->audio, {f, exchange});
    check(result.ok() && f->calls == 4 && f->video.segmented && f->audio.segmented &&
          !f->resolver.response.committed && !f->resolver.structurals.committed,
          "Vimeo playlists outlive retired resolver scratch");
    if (result.ok()) {
        const auto next = progressive_video_fetch_next(&f->video, false, 1000);
        check(next.step == ProgressiveVideoStep::submit && f->video.host == CdnHost::vimeo_video &&
              utf8_equal({reinterpret_cast<const char*>(f->video.target), f->video.target_bytes},
              utf8_literal("https://vod-adaptive-ak.vimeocdn.com/public/v2/range/prot/init?range=0-100")),
              "Vimeo resolves relative initialization paths into its native media lane");
    }
    progressive_video_fetch_release(&f->video); progressive_video_fetch_release(&f->audio);
    f->calls = 0; f->shared = true;
    result = vimeo_resolve(&f->resolver, utf8_literal("22439234"), 720, 1000, &f->video, &f->audio,
                           {f, exchange}, utf8_literal("0123456789"));
    check(result.ok() && f->calls == 4 && f->video.segmented && f->audio.segmented,
          "explicit unlisted link resolves both native media tracks");
    progressive_video_fetch_release(&f->video); progressive_video_fetch_release(&f->audio);
    f->calls = 0; f->shared = false; f->deny = true;
    result = vimeo_resolve(&f->resolver, utf8_literal("22439234"), 720, 1000, &f->video, &f->audio, {f, exchange});
    check(result.error == EmbedPlayerError::unsupported && f->calls == 4 && !f->video.segmented && !f->audio.segmented &&
          !f->resolver.response.committed && !f->resolver.structurals.committed,
          "protected Vimeo audio rolls back both owners without requesting a key");
    f->calls = 0;
    check(vimeo_resolve(&f->resolver, utf8_literal("0"), 720, 1000, &f->video, &f->audio, {f, exchange}).error ==
          EmbedPlayerError::invalid_input && !f->calls, "invalid Vimeo identity cannot issue network requests");
    check(vimeo_resolve(&f->resolver, utf8_literal("22439234"), 720, 1000, &f->video, &f->audio,
          {f, exchange}, utf8_literal("bad?value")).error == EmbedPlayerError::invalid_input && !f->calls,
          "invalid privacy hash is rejected before a network request");
    progressive_video_fetch_release(&f->video); progressive_video_fetch_release(&f->audio);
    embed_resolver_release(&f->resolver); VirtualFree(f, 0, MEM_RELEASE);
}
void live_resolver_test() noexcept {
    Owners owners;
    EmbedResolver* r = owners.resolver; ProgressiveVideoFetch *v = owners.video, *a = owners.audio;
    struct Calls { u32 count{}; bool deny{}; } calls{};
    const auto exchange = +[](void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        auto& c = *static_cast<Calls*>(opaque); const u32 call = c.count++;
        check(call < 4 && request.maximum_body_bytes == 65536 && options.headers.count == (call ? 1u : 3u) &&
            (options.body.count != 0) == !call, "live resolver confines token metadata and bounds every playlist response");
        check(utf8_equal(request.host_name, !call ? utf8_literal("gql.twitch.tv") : call == 1
            ? utf8_literal("usher.ttvnw.net") : utf8_literal("euc12.playlist.ttvnw.net")), "live resolver admits only the observed metadata and playlist origins");
        const auto body = !call ? utf8_literal(R"({"data":{"streamPlaybackAccessToken":{"signature":"0123456789abcdef0123456789abcdef01234567","value":"{\"channel\":\"example\",\"expires\":2000,\"authorization\":{\"forbidden\":false}}"}}})")
            : call == 1 ? utf8_literal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1500000,CODECS=\"avc1.4D401F,mp4a.40.2\",RESOLUTION=852x480\n"
                "https://euc12.playlist.ttvnw.net/v1/playlist/video.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=202277,CODECS=\"mp4a.40.2\"\n"
                "https://euc12.playlist.ttvnw.net/v1/playlist/audio.m3u8\n")
            : call == 3 && c.deny ? utf8_literal("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n")
            : utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:123\n"
                "#EXT-X-MAP:URI=\"https://example.hls.ttvnw.net/v1/segment/init.mp4\"\n"
                "#EXTINF:2,\nhttps://example.hls.ttvnw.net/s1/v1/segment/one.mp4\n");
        return reply(request, response, body);
    };
    check(r && v && a, "live resolver fixture owners");
    if (r && v && a) {
        auto result = twitch_hls_resolve(r, utf8_literal("example"), 480, 1000, v, a, {&calls, exchange}, true);
        check(result.ok() && calls.count == 4 && v->segmented && a->segmented &&
            v->segmented->manifest.bytes && a->segmented->manifest.bytes && v->segmented->sequence_base == 123 &&
            v->segmented->refresh_at == 1006 && !v->segmented->playlist.ended &&
            !r->response.committed && !r->structurals.committed, "live resolver retains its own sliding playlists after scratch retirement");
        progressive_video_fetch_release(v); progressive_video_fetch_release(a); calls = {}; calls.deny = true;
        result = twitch_hls_resolve(r, utf8_literal("example"), 480, 1000, v, a, {&calls, exchange}, true);
        check(result.error == EmbedPlayerError::unavailable && calls.count == 4 && !v->segmented && !a->segmented &&
            !r->response.committed && !r->structurals.committed, "unsupported live snapshot rolls back without permanently rejecting a changing channel or fetching a key");
        calls = {};
        result = twitch_hls_resolve(r, utf8_literal("example"), 480, 1001, v, a, {&calls, exchange}, true);
        check(result.ok() && calls.count == 4 && v->segmented && a->segmented,
              "a later supported live snapshot opens normally on the same resolver");
    }
}
void vod_resolver_test() noexcept {
    Owners owners;
    EmbedResolver* r = owners.resolver; ProgressiveVideoFetch *v = owners.video, *a = owners.audio;
    check(r && v && a, "VOD resolver fixture allocations");
    struct Calls { u32 count{}; bool deny_audio{}, high{}, oversized{}; } calls{};
    const auto exchange = +[](void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
        UpdateHttpResponse* response) noexcept {
        auto& c = *static_cast<Calls*>(opaque); const u32 request_index = c.count++;
        const u32 call = c.high && request_index > 2 ? request_index - 1 : request_index;
        check(call < 4 && options.headers.count == (call ? 1u : 3u) && (options.body.count != 0) == !call &&
            request.maximum_body_bytes == (call < 2 ? 64u * 1024u : 256u * 1024u),
            "VOD metadata token is isolated from bounded playlist GETs");
        check(utf8_equal(request.host_name, !call ? utf8_literal("gql.twitch.tv") : call == 1
            ? utf8_literal("usher.ttvnw.net") : utf8_literal("d2nvs31859zcd8.cloudfront.net")),
            "VOD resolver uses only its exact metadata, usher and media hosts");
        if (call >= 2) check(utf8_equal({request.target.data, request.target.count}, call == 2
            ? (c.high && request_index == 2 ? utf8_literal("/recording/chunked/index-dvr.m3u8")
                : utf8_literal("/recording/480p30/index-dvr.m3u8")) : utf8_literal("/recording/audio_only/index-dvr.m3u8")),
            "VOD media requests have no playback-token query");
        const auto body = !call ? utf8_literal(R"({"data":{"videoPlaybackAccessToken":{"signature":"0123456789abcdef0123456789abcdef01234567","value":"{\"vod_id\":123,\"expires\":2000,\"authorization\":{\"forbidden\":false}}"}}})")
            : call == 1 ? utf8_literal("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1352874,CODECS=\"avc1.4D401F,mp4a.40.2\",RESOLUTION=852x480\n"
                "https://d2nvs31859zcd8.cloudfront.net/recording/480p30/index-dvr.m3u8\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=8000000,CODECS=\"avc1.4D402A,mp4a.40.2\",RESOLUTION=1920x1080\n"
                "https://d2nvs31859zcd8.cloudfront.net/recording/chunked/index-dvr.m3u8\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=202277,CODECS=\"mp4a.40.2\"\nhttps://d2nvs31859zcd8.cloudfront.net/recording/audio_only/index-dvr.m3u8\n")
            : call == 3 && c.deny_audio ? utf8_literal("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n")
            : c.oversized ? utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:600\n#EXT-X-MAP:URI=\"init.mp4\"\n"
                "#EXTINF:600,\n0.mp4\n#EXT-X-ENDLIST\n")
            : utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:10\n#EXT-X-MAP:URI=\"init.mp4\"\n"
                "#EXTINF:2,\n0.mp4\n#EXTINF:10,\n1.mp4\n#EXT-X-ENDLIST\n");
        return reply(request, response, body);
    };
    if (r && v && a) {
        auto result = twitch_hls_resolve(r, utf8_literal("123"), 480, 1000, v, a, {&calls, exchange});
        check(result.ok() && calls.count == 4 && v->segmented && a->segmented && !r->response.committed && !r->structurals.committed &&
            v->segmented->playlist.duration_us == 12000000 && a->segmented->playlist.segments == 2 &&
            progressive_video_fetch_next(v, false, 1).step == ProgressiveVideoStep::submit &&
            progressive_video_fetch_next(a, false, 1).step == ProgressiveVideoStep::submit &&
            utf8_equal({reinterpret_cast<char*>(a->target), a->target_bytes},
                utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/audio_only/init.mp4")),
            "VOD sources own playlists after scratch retirement and defer media downloads");
        if (result.ok()) {
            MEMORY_BASIC_INFORMATION owner{}, text{};
            check(VirtualQuery(v->segmented, &owner, sizeof(owner)) == sizeof(owner) &&
                VirtualQuery(v->segmented->playlist.input.data, &text, sizeof(text)) == sizeof(text) &&
                owner.AllocationBase == text.AllocationBase && owner.RegionSize == 4096,
                "small HLS metadata and playlist share one committed page and one allocation");
            v->state = a->state = ProgressiveVideoFetchState::ready;
            v->fragment.segments = &v->segmented->store; a->fragment.segments = &a->segmented->store;
            v->fragment.index.first_time = 62033000; v->fragment.track.timescale = 1000000;
            auto& source = a->fragment;
            source.index.first_time = 2976784; source.index.duration = 470 * 1024; source.index.count = 1;
            source.track.audio = true; source.track.sample_rate = source.track.timescale = 48000;
            source.track.movie_timescale = v->fragment.track.movie_timescale = 100000;
            source.track.frames_per_packet = 1024;
            source.segment.duration = source.index.duration;
            i64 first_video{};
            check(segmented_video_synchronize(v, a) == Mp4DemuxError::none && a->fragment.track.media_start == 2976784 &&
                v->fragment.track.media_start == 62016333 && mp4_fragment_source_time(v->fragment, 62033000, &first_video) &&
                first_video == 16667 && a->audio.timeline.media_start_ticks == 2976784,
                "one rational HLS origin preserves the real audio/video offset instead of independently zeroing tracks");
            check(segmented_video_synchronize(v, a) == Mp4DemuxError::none && v->fragment.track.media_start == 62016333,
                "HLS pair synchronization is idempotent");
            v->segmented->synchronized = a->segmented->synchronized = false;
            v->fragment.index.first_time += 172800ull * v->fragment.track.timescale;
            a->fragment.index.first_time += 172800ull * a->fragment.track.timescale;
            check(segmented_video_synchronize(v, a) == Mp4DemuxError::none && a->fragment.track.media_start == 2976784 + 172800ll * 48000 &&
                mp4_fragment_source_time(v->fragment, static_cast<i64>(v->fragment.index.first_time), &first_video) && first_video == 16667,
                "long-running source clocks normalize through checked rational conversion without a one-day cutoff");
            v->segmented->synchronized = a->segmented->synchronized = false;
            v->fragment.index.first_time = a->fragment.index.first_time = ~u64{0};
            const i64 old_video = v->fragment.track.media_start, old_audio = a->fragment.track.media_start;
            check(segmented_video_synchronize(v, a) != Mp4DemuxError::none && v->fragment.track.media_start == old_video && a->fragment.track.media_start == old_audio,
                "overflowing shared origins leave both accepted clocks unchanged");
        }
        progressive_video_fetch_release(v); progressive_video_fetch_release(a);
        calls = {}; calls.high = true;
        result = twitch_hls_resolve(r, utf8_literal("123"), 1080, 1000, v, a, {&calls, exchange});
        check(result.ok() && calls.count == 5 && v->segmented && a->segmented &&
            progressive_video_fetch_next(v, false, 1).step == ProgressiveVideoStep::submit &&
            utf8_equal({reinterpret_cast<char*>(v->target), v->target_bytes},
                utf8_literal("https://d2nvs31859zcd8.cloudfront.net/recording/480p30/init.mp4")) &&
            !r->response.committed && !r->structurals.committed,
            "an oversized high-resolution rendition falls back using the retained master before any media download");
        progressive_video_fetch_release(v); progressive_video_fetch_release(a);
        calls = {}; calls.oversized = true;
        result = twitch_hls_resolve(r, utf8_literal("123"), 480, 1000, v, a, {&calls, exchange});
        check(result.error == EmbedPlayerError::unsupported && calls.count == 3 && !v->segmented && !a->segmented,
            "a master without a fitting rendition fails without downloading oversized media or retrying metadata");
        calls = {}; calls.deny_audio = true;
        result = twitch_hls_resolve(r, utf8_literal("123"), 480, 1000, v, a, {&calls, exchange});
        check(result.error == EmbedPlayerError::unsupported && calls.count == 4 && !v->segmented && !a->segmented &&
            !r->response.committed && !r->structurals.committed, "unsupported audio rolls back both HLS owners and scratch pages");
    }
}
void steam_resolver_test() noexcept {
    Owners owners;
    EmbedResolver* r = owners.resolver; ProgressiveVideoFetch *v = owners.video, *a = owners.audio;
    check(r && v && a, "Steam resolver fixture allocations");
    struct Calls { u32 count{}; bool protected_manifest{}; } calls{};
    const auto exchange = +[](void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
        UpdateHttpResponse* response) noexcept {
        auto& c = *static_cast<Calls*>(opaque);
        const bool metadata = (c.count++ % 2) == 0;
        check(!options.body.count && options.headers.count == 1 && utf8_equal(options.headers[0].name, utf8_literal("User-Agent")) &&
              request.maximum_body_bytes == (metadata ? 128u * 1024u : 64u * 1024u),
              "Steam public metadata and manifest GETs have exact byte bounds and no account or browser headers");
        check(utf8_equal(request.host_name, metadata ? utf8_literal("store.steampowered.com") : utf8_literal("video.akamai.steamstatic.com")),
              "Steam resolver makes only the identity-bound store and approved manifest requests");
        if (metadata) check(utf8_equal({request.target.data, request.target.count},
            utf8_literal("/api/appdetails?appids=570&filters=movies")), "Steam metadata request retains the requested app ID");
        auto body = metadata ? utf8_literal(R"({"570":{"success":true,"data":{"movies":[{"id":256692021,"dash_h264":"https://video.akamai.steamstatic.com/store_trailers/570/116737/0123456789abcdef0123456789abcdef01234567/1749859757/dash_h264.mpd"}]}}})")
            : c.protected_manifest ? utf8_literal(R"(<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT6S"><Period><ContentProtection/></Period></MPD>)")
            : utf8_literal(R"(<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT6S"><Period><AdaptationSet><Representation id="2" mimeType="video/mp4" codecs="avc1.640029" bandwidth="1400000" width="854" height="480"><SegmentTemplate timescale="1" duration="3" initialization="dash_h264/init-stream$RepresentationID$.m4s" media="dash_h264/chunk-stream$RepresentationID$-$Number%05d$.m4s"/></Representation></AdaptationSet><AdaptationSet><Representation id="4" mimeType="audio/mp4" codecs="mp4a.40.2" bandwidth="192000"><SegmentTemplate timescale="1" duration="3" initialization="dash_h264/init-stream$RepresentationID$.m4s" media="dash_h264/chunk-stream$RepresentationID$-$Number%05d$.m4s"/></Representation></AdaptationSet></Period></MPD>)");
        return reply(request, response, body);
    };
    if (r && v && a) {
        auto result = steam_resolve(r, utf8_literal("570"), utf8_literal("256692021"), 480, v, a, {&calls, exchange});
        check(result.ok() && calls.count == 2 && v->segmented && a->segmented &&
              !r->response.committed && !r->structurals.committed &&
              progressive_video_fetch_next(v, false, 1).step == ProgressiveVideoStep::submit &&
              progressive_video_fetch_next(a, false, 1).step == ProgressiveVideoStep::submit &&
              !v->segmented->store.initialization.base && !a->segmented->store.initialization.base,
              "Steam selection owns its small templates after scratch retirement and defers media downloads to the shared scheduler");
        progressive_video_fetch_release(v); progressive_video_fetch_release(a);
        calls.protected_manifest = true;
        result = steam_resolve(r, utf8_literal("570"), utf8_literal("256692021"), 480, v, a, {&calls, exchange});
        check(result.error == EmbedPlayerError::unsupported && calls.count == 4 && !v->segmented && !a->segmented &&
              !r->response.committed && !r->structurals.committed, "unsupported protected manifest leaves no source or retained metadata pages");
    }
}
constexpr auto kPage = utf8_literal("<html><script>ytcfg.set({\"VISITOR_DATA\" : \"public_visitor-123%3D\",\"other\":1});</script></html>");
constexpr auto kPlayer = utf8_literal(R"({"playabilityStatus":{"status":"OK","playableInEmbed":true},"videoDetails":{"videoId":"aB_01234-xy"},"streamingData":{"expiresInSeconds":"21600","adaptiveFormats":[{"url":"https://rr1.googlevideo.com/videoplayback?id=v","mimeType":"video/mp4; codecs=\"avc1.4d401e\"","contentLength":"1000000","bitrate":100000,"width":640,"height":360},{"url":"https://rr1.googlevideo.com/videoplayback?id=a","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","contentLength":"100000","bitrate":128000,"audioChannels":2}]}})");
struct Fake { u32 calls, bootstrap, player; bool login, rate_limited; };
UpdateTransportError exchange(void* context, const UpdateHttpRequest& request,
    const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    auto& f = *static_cast<Fake*>(context);
    ++f.calls;
    check(utf8_equal(request.host_name, utf8_literal("www.youtube.com")), "metadata stays on YouTube origin");
    *response = {};
    response->status = 200;
    Utf8View bytes{};
    if (!options.body.count) {
        ++f.bootstrap;
        check(request.target.count == 1 && request.target[0] == '/', "bootstrap only on first resolution");
        bytes = kPage;
    } else {
        ++f.player;
        check(utf8_equal({request.target.data, request.target.count},
            utf8_literal("/youtubei/v1/player?prettyPrint=false")), "player endpoint");
        check(options.headers.count == 7 && options.body.count < 512, "bounded native request metadata");
        check(utf8_equal(options.headers[0].value, kYoutubeNativeUserAgent) &&
              utf8_equal(options.headers[4].value, utf8_literal("101")) &&
              utf8_equal(options.headers[5].value, utf8_literal("1.02")),
              "player headers match the native client context");
        for (u32 i = 0; i < options.headers.count; ++i)
            check(!utf8_equal(options.headers[i].name, utf8_literal("Authorization")),
                  "anonymous resolver never borrows account credentials");
        if (f.rate_limited) {
            response->status = 429;
            response->retry_after_seconds = 120;
            return UpdateTransportError::none;
        }
        bytes = f.login ? utf8_literal(R"({"playabilityStatus":{"status":"LOGIN_REQUIRED"}})") : kPlayer;
    }
    for (u32 i = 0; i < bytes.bytes; ++i) {
        ++response->body_bytes;
        if (!request.sink(request.sink_context, {reinterpret_cast<const u8*>(bytes.data + i), 1}))
            return UpdateTransportError::sink;
    }
    return UpdateTransportError::none;
}
void scanner() noexcept {
    for (u32 split = 0; split <= kPage.bytes; ++split) {
        YoutubeBootstrap s{};
        youtube_bootstrap_feed(&s, {reinterpret_cast<const u8*>(kPage.data), split});
        youtube_bootstrap_feed(&s, {reinterpret_cast<const u8*>(kPage.data + split), kPage.bytes - split});
        check(s.complete && !s.failed && utf8_equal({s.visitor, s.bytes}, utf8_literal("public_visitor-123%3D")),
              "visitor scanner handles every boundary without keeping the page");
    }
    YoutubeBootstrap s{};
    constexpr char invalid[] = "\"visitorData\":\"bad\\nvalue\"";
    check(!youtube_bootstrap_feed(&s, {reinterpret_cast<const u8*>(invalid), sizeof(invalid) - 1}) && s.failed,
          "escaped visitor cannot become an HTTP header");
}
bool sink(void* context, ConstSlice<u8> bytes) noexcept {
    *static_cast<u32*>(context) += bytes.count;
    return true;
}
void headers() noexcept {
    u8 out[1024];
    u32 delivered = 0;
    constexpr char target[] = "/youtubei/v1/player";
    const UpdateHttpRequest request{UpdateHost::provider, utf8_literal("www.youtube.com"),
        {target, sizeof(target) - 1}, 1024, sink, &delivered};
    constexpr u8 body[]{'{', '}'};
    HttpsHeader header{utf8_literal("Content-Type"), utf8_literal("application/json")};
    HttpsExchangeOptions options{{&header, 1}, {body, sizeof(body)}};
    const auto bytes = https_exchange_head(request, options, {out, sizeof(out)});
    const auto expected = utf8_literal("POST /youtubei/v1/player HTTP/1.1\r\nHost: www.youtube.com\r\nAccept-Encoding: identity\r\nConnection: close\r\nContent-Type: application/json\r\nUser-Agent: MaiCord-Updater\r\nAccept: */*\r\nContent-Length: 2\r\n\r\n");
    check(utf8_equal({reinterpret_cast<const char*>(out), bytes}, expected), "native HTTPS request framing");
    out[0] = 42;
    header = {utf8_literal("hOsT"), utf8_literal("evil.test")};
    check(!https_exchange_head(request, options, {out, sizeof(out)}) && out[0] == 42, "no Host override");
    header = {utf8_literal("Cookie"), utf8_literal("test\r\nInjected: value")};
    check(!https_exchange_head(request, options, {out, sizeof(out)}) && out[0] == 42, "no header injection");
    header = {utf8_literal("Transfer-Encoding"), utf8_literal("chunked")};
    check(!https_exchange_head(request, options, {out, sizeof(out)}), "no request smuggling framing override");
    constexpr char error[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 2\r\n\r\n{}";
    for (u32 pass = 0; pass < 2; ++pass) {
        const bool read_error = pass != 0;
        copy_bytes(out, error, sizeof(error) - 1);
        u32 size = sizeof(error) - 1;
        delivered = 0;
        UpdateHttpReader reader{};
        UpdateHttpResponse response{};
        check(update_http_read(&reader, request, &response, out, &size, sizeof(out), true, read_error) ==
            UpdateTransportError::none && reader.complete && response.status == 400 &&
            delivered == (read_error ? 2u : 0u), "OAuth error bodies are opt-in; updater behavior preserved");
    }
}
void resident_selection() noexcept {
    static AccountMedia media{};
    ProgressiveVideoFetch fetch{};
    Mp4Decoder decoder{};
    decoder.ready = true;
    media.mp4_decoder = &decoder;
    media.mp4_key = media_asset_embed(1, 1, 0, MediaEmbedResource::video, MediaAttachmentFormat::mp4,
                                     true, MediaResolutionTier::px512, false);
    media.progressive = &fetch;
    fetch.state = ProgressiveVideoFetchState::ready;
    check(account_media_video_matches(&media, media.mp4_key), "ready provider decoder can be selected");
    fetch.state = ProgressiveVideoFetchState::failed;
    check(!account_media_video_matches(&media, media.mp4_key),
          "failed or expired provider ranges require fresh resolution on retry");
    media = {};
}
void resolver() noexcept {
    auto* r = allocate<EmbedResolver>();
    check(r != nullptr, "resolver allocation");
    if (!r) return;
    Fake f{};
    EmbedStreams streams{};
    auto result = youtube_resolve(r, kId, 480, &streams, {&f, exchange});
    check(result.ok() && f.calls == 2 && f.bootstrap == 1 && f.player == 1 &&
          streams.video.height == 360 && streams.audio.bitrate == 128000 && !r->response.committed &&
          !r->structurals.committed && !r->response_bytes && !r->https.winsock,
          "native resolution uses two requests and releases all response/scratch pages");
    result = youtube_resolve(r, kId, 480, &streams, {&f, exchange});
    check(result.ok() && f.calls == 3 && f.bootstrap == 1, "bootstrap reused without background traffic");
    f.login = true;
    auto prior = streams;
    result = youtube_resolve(r, kId, 480, &streams, {&f, exchange});
    check(result.error == EmbedPlayerError::login_required && f.calls == 4 &&
          bytes_equal(&prior, &streams, sizeof(streams)) && !r->response.committed,
          "login challenge neither loops nor replaces prior source");
    f.rate_limited = true;
    result = youtube_resolve(r, kId, 480, &streams, {&f, exchange});
    check(!result.ok() && result.status == 429 && result.retry_after_seconds == 120 && f.calls == 5,
          "rate limit reported without retries");
    u32 clip_calls = 0;
    const auto clip_exchange = +[](void* opaque, const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        ++*static_cast<u32*>(opaque);
        check(utf8_equal(request.host_name, utf8_literal("www.tiktok.com")) && !options.body.count &&
              options.headers.count == 2 && utf8_equal(options.headers[1].name, utf8_literal("Referer")),
              "TikTok is one public GET with no YouTube visitor, cookie or account credential");
        const auto body = utf8_literal(R"({"status_code":0,"items":[]})");
        return reply(request, response, body);
    };
    result = tiktok_resolve(r, utf8_literal("6718335390845095173"), 720, 1000, &streams, {&clip_calls, clip_exchange});
    check(result.error == EmbedPlayerError::unsupported && clip_calls == 1 &&
          !r->response.committed && !r->structurals.committed && bytes_equal(&prior, &streams, sizeof(streams)),
          "provider switch isolates metadata and keeps output transactional");
    const auto twitch_exchange = +[](void* opaque, const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        ++*static_cast<u32*>(opaque);
        check(utf8_equal(request.host_name, utf8_literal("gql.twitch.tv")) && options.body.count &&
              options.headers.count == 3 && utf8_equal(options.headers[2].name, utf8_literal("Client-ID")),
              "clip metadata uses one bounded public query, without account credentials or cookies");
        const auto body = utf8_literal(R"({"data":{"clip":null}})");
        return reply(request, response, body);
    };
    result = twitch_clip_resolve(r, utf8_literal("IncredulousAbstemiousFennelImGlitch"), 720, 1000, &streams,
                                {&clip_calls, twitch_exchange});
    check(result.error == EmbedPlayerError::unavailable && clip_calls == 2 &&
          !r->response.committed && !r->structurals.committed && bytes_equal(&prior, &streams, sizeof(streams)),
          "removed clip neither retries nor changes the selected source");
    const auto spotify_exchange = +[](void* opaque, const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        ++*static_cast<u32*>(opaque);
        check(utf8_equal(request.host_name, utf8_literal("open.spotify.com")) && !options.body.count &&
              options.headers.count == 1 && utf8_equal(options.headers[0].name, utf8_literal("User-Agent")) &&
              request.maximum_body_bytes == kSpotifyEmbedMaximumBytes,
              "Spotify metadata has one bounded public GET without other providers' state");
        const auto body = utf8_literal("<html>Unavailable</html>");
        return reply(request, response, body);
    };
    result = spotify_preview_resolve(r, utf8_literal("11dFghVXANMlKmJXsNCbNl"), 1000, &streams,
                                     {&clip_calls, spotify_exchange});
    check(result.error == EmbedPlayerError::unsupported && clip_calls == 3 &&
          !r->response.committed && !r->structurals.committed && bytes_equal(&prior, &streams, sizeof(streams)),
          "missing preview neither retries nor changes the selected source");
    const auto streamable_exchange = +[](void* opaque, const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        ++*static_cast<u32*>(opaque);
        check(utf8_equal(request.host_name, utf8_literal("api.streamable.com")) &&
              utf8_equal({request.target.data, request.target.count}, utf8_literal("/videos/hn8hq")) &&
              !options.body.count && options.headers.count == 1 &&
              utf8_equal(options.headers[0].name, utf8_literal("User-Agent")) &&
              request.maximum_body_bytes == kStreamableMetadataMaximumBytes,
              "Streamable uses one bounded metadata GET without account or other provider headers");
        const auto body = utf8_literal(R"({"status":1})");
        return reply(request, response, body);
    };
    result = streamable_resolve(r, utf8_literal("hn8hq"), 480, 1000, &streams, {&clip_calls, streamable_exchange});
    check(result.error == EmbedPlayerError::unavailable && clip_calls == 4 &&
          !r->response.committed && !r->structurals.committed && bytes_equal(&prior, &streams, sizeof(streams)),
          "processing video returns without polling, retaining output and releasing all scratch pages");
    u32 ph_calls = 0;
    const auto ph_exchange = +[](void* context, const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        const auto step = ++*static_cast<u32*>(context);
        check(utf8_equal(request.host_name, utf8_literal("www.pornhub.com")) && !options.body.count &&
              options.headers.count == 2 && request.maximum_body_bytes == kPornhubMetadataMaximumBytes &&
              utf8_equal(options.headers[1].name, utf8_literal("Referer")) &&
              utf8_equal(options.headers[1].value, utf8_literal("https://www.pornhub.com/")) &&
              utf8_equal({request.target.data, request.target.count}, step == 1
                  ? utf8_literal("/embed/683dbe0849d99") : utf8_literal("/video/get_media?s=fixture&v=1&e=2000&t=fixture")),
              "PH resolver makes two identity-bound public GETs without account credentials");
        const auto body = step == 1
            ? utf8_literal(R"(<script>var flashvars = {"link_url":"https://www.pornhub.com/view_video.php?viewkey=683dbe0849d99","video_unavailable":"false","mediaDefinitions":[{"format":"mp4","remote":true,"videoUrl":"https://www.pornhub.com/video/get_media?s=fixture&v=1&e=2000&t=fixture"}]};</script>)")
            : utf8_literal(R"([{"format":"mp4","width":854,"height":480,"videoUrl":"https://ee.phncdn.com/videos/fixture/video.mp4?validto=2000&hash=fixture"}])");
        return reply(request, response, body);
    };
    result = pornhub_resolve(r, utf8_literal("683dbe0849d99"), 720, 1000, &streams, {&ph_calls, ph_exchange});
    check(result.ok() && ph_calls == 2 && streams.combined && streams.video.height == 480 &&
          !r->response.committed && !r->structurals.committed,
          "PH resolver releases all temporary metadata after publishing a combined native source");
    streams = prior;
    const auto apple_exchange = +[](void* opaque, const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
        ++*static_cast<u32*>(opaque);
        check(utf8_equal(request.host_name, utf8_literal("itunes.apple.com")) &&
              utf8_equal({request.target.data, request.target.count}, utf8_literal("/lookup?id=255145362&country=us")) &&
              !options.body.count && options.headers.count == 1 &&
              utf8_equal(options.headers[0].name, utf8_literal("User-Agent")) &&
              request.maximum_body_bytes == kAppleMusicMetadataMaximumBytes,
              "Apple lookup uses one bounded public GET with no account or other provider credentials");
        const auto body = utf8_literal(R"({"resultCount":0,"results":[]})");
        return reply(request, response, body);
    };
    result = apple_music_preview_resolve(r, utf8_literal("255145362"), utf8_literal("us"), 1000, &streams,
                                         {&clip_calls, apple_exchange});
    check(result.error == EmbedPlayerError::unavailable && clip_calls == 5 &&
          !r->response.committed && !r->structurals.committed && bytes_equal(&prior, &streams, sizeof(streams)),
          "absent storefront track releases scratch and leaves the selected stream unchanged");
    u32 twitter_calls=0;
    const auto twitter_exchange=+[](void* opaque,const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options,UpdateHttpResponse* response) noexcept {
        ++*static_cast<u32*>(opaque);
        u8 target[128];const auto n=twitter_video_target(utf8_literal("123"),{target,sizeof(target)});
        check(utf8_equal(request.host_name,utf8_literal("cdn.syndication.twimg.com")) &&
            utf8_equal({request.target.data,request.target.count},{reinterpret_cast<const char*>(target),n}) &&
            !options.body.count && options.headers.count==1 &&
            utf8_equal(options.headers[0].name,utf8_literal("User-Agent")) &&
            request.maximum_body_bytes==kTwitterMetadataMaximumBytes,
            "public post resolution uses one bounded GET with no account credentials");
        const auto body=utf8_literal(R"({"__typename":"Tweet","id_str":"123","mediaDetails":[{"type":"video","video_info":{"variants":[{"content_type":"video/mp4","url":"https://video.twimg.com/ext_tw_video/456/pu/vid/640x360/a.mp4"}]}}]})");
        return reply(request,response,body);
    };
    result=twitter_resolve(r,utf8_literal("123"),360,1000,&streams,{&twitter_calls,twitter_exchange});
    check(result.ok() && twitter_calls==1 && streams.video.height==360 && streams.combined &&
        !r->response.committed && !r->structurals.committed,"post resolver publishes media and releases metadata scratch");
    const auto twitter_prior=streams;
    result=twitter_resolve(r,utf8_literal("123"),360,1000,&streams,{&twitter_calls,twitter_exchange},2);
    check(result.error==EmbedPlayerError::unsupported && twitter_calls==2 &&
        bytes_equal(&streams,&twitter_prior,sizeof(streams)),"resolver retains explicit media ordinal without fallback");
    result=twitter_resolve(r,utf8_literal("../123"),360,1000,&streams,{&twitter_calls,twitter_exchange});
    check(result.error==EmbedPlayerError::invalid_input && twitter_calls==2,"invalid post identity does no network work");
    struct AmazonProbe {u32 calls, mode;};AmazonProbe amazon{};
    const auto amazon_exchange=+[](void* opaque,const UpdateHttpRequest& request,
        const HttpsExchangeOptions& options,UpdateHttpResponse* response) noexcept {
        auto& p=*static_cast<AmazonProbe*>(opaque);const u32 hop=p.calls++;
        check(utf8_equal(request.host_name,hop?utf8_literal("d2usgkmxlnua9y.cloudfront.net"):utf8_literal("music.amazon.com")) &&
            utf8_equal({request.target.data,request.target.count},hop?utf8_literal("/US/B000VBH2R2"):utf8_literal("/getSampleTrack/B000VBH2R2")) &&
            request.maximum_body_bytes==1 && !options.body.count && options.headers.count==1 &&
            utf8_equal(options.headers[0].name,utf8_literal("User-Agent")),"Amazon preview follows only the two bounded credential-free redirect requests");
        u8 body=0;
        check(!request.sink(request.sink_context,{&body,1}),"preview redirect exchange refuses a metadata body");
        auto redirect=hop?utf8_literal("https://d28julafmv4ekl.cloudfront.net/64%2F30%2F123_S64.mp3?Expires=2000")
            :utf8_literal("https://d2usgkmxlnua9y.cloudfront.net/US/B000VBH2R2");
        if(p.mode==1) redirect=utf8_literal("https://d2usgkmxlnua9y.cloudfront.net/US/B076YT2CBT");
        if(p.mode==2 && hop) redirect=utf8_literal("https://d2usgkmxlnua9y.cloudfront.net/US/B000VBH2R2");
        copy_bytes(response->location,redirect.data,redirect.bytes);response->location_bytes=redirect.bytes;response->status=302;
        if(p.mode==3) {response->status=429;response->retry_after_seconds=60;}
        return UpdateTransportError::none;
    };
    constexpr auto amazon_page=utf8_literal("https://music.amazon.com/albums/B000VBJ504?trackAsin=B000VBH2R2");
    result=amazon_music_preview_resolve(r,amazon_page,1000,&streams,{&amazon,amazon_exchange});
    check(result.ok() && amazon.calls==2 && streams.audio.url_bytes && !streams.video.url_bytes &&
        !r->response.committed && !r->structurals.committed,"Amazon preview uses no response or JSON scratch pages");
    const auto amazon_prior=streams;
    for(u32 mode=1;mode<=3;++mode) {
        amazon={0,mode};result=amazon_music_preview_resolve(r,amazon_page,1000,&streams,{&amazon,amazon_exchange});
        check(!result.ok() && amazon.calls==(mode==2?2u:1u) && bytes_equal(&streams,&amazon_prior,sizeof(streams)) &&
            (mode!=3 || result.retry_after_seconds==60),"changed track redirect loops and throttling stop without publishing or retrying");
    }
    embed_resolver_release(r);
    check(!r->response.base && !r->structurals.base && !r->bootstrap.bytes, "joined resolver release");
    embed_resolver_cancel(r);
    result = youtube_resolve(r, kId, 480, &streams);
    check(result.transport_error == UpdateTransportError::cancelled && !r->https.winsock &&
          !r->response.base && !r->structurals.base,
          "stop before publication cannot start a new connection or allocate scratch");
    VirtualFree(r, 0, MEM_RELEASE);
}
} // namespace
}
extern "C" __declspec(noreturn) void WINAPI maicord_youtube_resolver_entry() noexcept {
    maicord::vimeo_resolver_test();
    maicord::youtube_live_resolver_test();
    maicord::check(maicord::soundcloud_player_tests(), "SoundCloud identity-bound native resolution");
    maicord::live_resolver_test();
    maicord::scanner(); maicord::headers(); maicord::resolver(); maicord::steam_resolver_test(); maicord::vod_resolver_test(); maicord::resident_selection(); maicord::paired_ranges();
    ExitProcess(maicord::failures ? 1 : 0);
}
