#include "maicord/app/embed_resolver.hpp"
#include "maicord/app/account_media.hpp"
#include "maicord/app/account_media_worker.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/app/cdn_client.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/ascii.hpp"
#include "test_output.hpp"

namespace maicord {
bool twitch_live_fetch_probe() noexcept;
bool native_player_perf_probe() noexcept;
bool soundcloud_player_tests() noexcept;
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
void paired_ranges() noexcept {
    auto* media = static_cast<AccountMedia*>(VirtualAlloc(nullptr, sizeof(AccountMedia), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* video = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* audio = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
    auto* r=static_cast<EmbedResolver*>(VirtualAlloc(nullptr,sizeof(EmbedResolver),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* v=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* a=static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr,sizeof(ProgressiveVideoFetch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
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
        *response={};response->status=200;
        return request.sink(request.sink_context,{reinterpret_cast<const u8*>(body.data),body.bytes})?UpdateTransportError::none:UpdateTransportError::sink;
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
    if(v) {progressive_video_fetch_release(v);VirtualFree(v,0,MEM_RELEASE);}
    if(a) {progressive_video_fetch_release(a);VirtualFree(a,0,MEM_RELEASE);}
    if(r) {embed_resolver_release(r);VirtualFree(r,0,MEM_RELEASE);}
}
void vimeo_resolver_test() noexcept {
    struct Fixture { EmbedResolver resolver; ProgressiveVideoFetch video, audio; u32 calls; bool deny, shared; };
    auto* f = static_cast<Fixture*>(VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
            ? UpdateTransportError::none : UpdateTransportError::sink;
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
    auto* r = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* v = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* a = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes}) ? UpdateTransportError::none : UpdateTransportError::sink;
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
    if (v) { progressive_video_fetch_release(v); VirtualFree(v, 0, MEM_RELEASE); }
    if (a) { progressive_video_fetch_release(a); VirtualFree(a, 0, MEM_RELEASE); }
    if (r) { embed_resolver_release(r); VirtualFree(r, 0, MEM_RELEASE); }
}
void vod_resolver_test() noexcept {
    auto* r = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* v = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* a = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
            ? UpdateTransportError::none : UpdateTransportError::sink;
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
    if (v) { progressive_video_fetch_release(v); VirtualFree(v, 0, MEM_RELEASE); }
    if (a) { progressive_video_fetch_release(a); VirtualFree(a, 0, MEM_RELEASE); }
    if (r) { embed_resolver_release(r); VirtualFree(r, 0, MEM_RELEASE); }
}
void steam_resolver_test() noexcept {
    auto* r = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* v = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* a = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
            ? UpdateTransportError::none : UpdateTransportError::sink;
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
    if (v) { progressive_video_fetch_release(v); VirtualFree(v, 0, MEM_RELEASE); }
    if (a) { progressive_video_fetch_release(a); VirtualFree(a, 0, MEM_RELEASE); }
    if (r) { embed_resolver_release(r); VirtualFree(r, 0, MEM_RELEASE); }
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
    auto* r = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
                   ? UpdateTransportError::none : UpdateTransportError::sink;
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
                   ? UpdateTransportError::none : UpdateTransportError::sink;
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
                   ? UpdateTransportError::none : UpdateTransportError::sink;
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
                   ? UpdateTransportError::none : UpdateTransportError::sink;
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
            ? UpdateTransportError::none : UpdateTransportError::sink;
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
        response->status = 200;
        return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
                   ? UpdateTransportError::none : UpdateTransportError::sink;
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
        response->status=200;
        return request.sink(request.sink_context,{reinterpret_cast<const u8*>(body.data),body.bytes})
            ? UpdateTransportError::none : UpdateTransportError::sink;
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
bool audio_bytes(AccountMedia& media, Mp4AudioDemuxer& demux, ConstSlice<u8> bytes) noexcept {
    const auto deadline = GetTickCount64() + 3000;
    while (!mp4_demux_available(demux.samples, bytes) && GetTickCount64() < deadline) {
        auto* fetch = media.progressive;
        if (cdn_client_status(&media.client).phase == CdnClientPhase::response_ready)
            cdn_client_consume_response(&media.client,
                +[](void* p, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
                    progressive_video_fetch_accept(static_cast<ProgressiveVideoFetch*>(p), response.request_id,
                        response.transport_ok(), cdn_client_response_policy(response), body);
                    return true;
                }, fetch);
        const auto next = progressive_video_fetch_next(fetch, false, clock_unix_milliseconds() / 1000);
        if (next.step == ProgressiveVideoStep::submit) account_media_submit_progressive(&media, next.range);
        else if (next.step == ProgressiveVideoStep::failed) return false;
        cdn_client_wait_response(&media.client, 10);
    }
    return mp4_demux_available(demux.samples, bytes);
}
bool fragment_audio_input(AccountMedia& media, u32 seek, Mp4FragmentAudioPacket* packet = nullptr) noexcept {
    const auto deadline = GetTickCount64() + 3000;
    do {
        const auto result = packet ? mp4_fragment_audio_peek(&media.progressive_audio->audio, packet)
                                   : mp4_fragment_audio_seek(&media.progressive_audio->audio, seek);
        if (result != Mp4DemuxError::data_pending) return result == Mp4DemuxError::none;
        bool accepted = true;
        struct Pending { AccountMedia* media; bool* accepted; } pending{&media, &accepted};
        if (cdn_client_status(&media.client).phase == CdnClientPhase::response_ready)
            cdn_client_consume_response(&media.client,
                +[](void* opaque, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
                    auto& p = *static_cast<Pending*>(opaque);
                    *p.accepted = account_media_accept_progressive(p.media, response, body) != ProgressiveVideoAccept::failed;
                    return true;
                }, &pending);
        if (!accepted) return false;
        ProgressiveVideoFetch* selected{};
        const auto next = account_media_progressive_next(&media, false, clock_unix_milliseconds() / 1000, &selected);
        if (next.step == ProgressiveVideoStep::submit) {
            if (!account_media_submit_progressive(&media, next.range, selected == media.progressive_audio)) return false;
        } else if (next.step == ProgressiveVideoStep::cancel && selected) {
            if (!cdn_client_cancel(&media.client, selected->inflight_id)) return false;
            progressive_video_fetch_cancelling(selected);
        } else if (next.step == ProgressiveVideoStep::failed) return false;
        cdn_client_wait_response(&media.client, 10);
    } while (GetTickCount64() < deadline);
    return false;
}
bool decode_audio(AccountMedia& media, Mp4AudioDemuxer demux, AacDecoder& decoder,
                  u32 start, u32 end, float* pcm) noexcept {
    if (media.progressive_audio ? !fragment_audio_input(media, start)
                               : mp4_demux_seek_sample(&demux.samples, start) != Mp4DemuxError::none) return false;
    float block[kAacDecoderMaximumOutputBytes / sizeof(float)];
    u32 written = 0;
    const u32 wanted = (end - start) * demux.frames_per_packet * demux.channels;
    for (u32 calls = 0; calls < (end - start + 2) * 4 && written < wanted; ++calls) {
        const auto output = aac_decoder_read(&decoder, {reinterpret_cast<u8*>(block), sizeof(block)});
        if (output.ok()) {
            const auto count = minimum(output.frames * demux.channels, wanted - written);
            copy_bytes(pcm + written, block, count * sizeof(float));
            written += count;
        } else if (output.error == AacDecodeError::need_input) {
            if (media.progressive_audio) {
                Mp4FragmentAudioPacket packet{};
                if (!fragment_audio_input(media, 0, &packet) ||
                    !aac_decoder_submit(&decoder, packet.bytes, packet.presentation_time_100ns, packet.duration_100ns).ok() ||
                    !mp4_fragment_audio_commit(&media.progressive_audio->audio)) return false;
            } else {
                const auto unit = mp4_audio_demux_next(&demux);
                i64 first{}, last{};
                if (!unit.ok() || !audio_bytes(media, demux, unit.bytes) ||
                    !mp4_audio_demux_frame_time_100ns(demux, u64{unit.sample_index} * demux.frames_per_packet, &first) ||
                    !mp4_audio_demux_frame_time_100ns(demux, (u64{unit.sample_index} + 1) * demux.frames_per_packet, &last) ||
                    !aac_decoder_submit(&decoder, unit.bytes, first, last - first).ok()) return false;
            }
        } else return false;
    }
    return written == wanted;
}
void audio_seek_probe(AccountMedia& media) noexcept {
    char enabled[2];
    if (GetEnvironmentVariableA("MAICORD_AAC_SEEK_PROBE", enabled, 2) != 1 || enabled[0] != '1') return;
    Mp4AudioDemuxer demux{};
    ConstSlice<u8> configuration{};
    if (media.progressive_audio) {
        const auto& track = media.progressive_audio->fragment.track;
        demux.sample_rate = track.sample_rate; demux.channels = track.channels; demux.frames_per_packet = track.frames_per_packet;
        configuration = track.configuration;
    } else if (mp4_audio_demux_initialize(&demux, progressive_source_bytes(media.progressive->source),
        progressive_source_reader(&media.progressive->source, ProgressiveTrack::audio)) != Mp4DemuxError::none) {
        check(false, "seek comparison demux");
        return;
    } else configuration = mp4_audio_demux_asc(demux);
    const u32 samples = 256 * demux.frames_per_packet * demux.channels;
    auto* reference = static_cast<float*>(VirtualAlloc(nullptr, samples * 2 * sizeof(float),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!reference) { check(false, "seek comparison storage"); return; }
    auto* sought = reference + samples;
    AacDecoder decoder{};
    bool valid = aac_decoder_initialize(&decoder, configuration, demux.sample_rate, demux.channels).ok() &&
        decode_audio(media, demux, decoder, 0, 256, reference);
    const u32 preroll = aac_decoder_seek_preroll_packets(demux.frames_per_packet);
    {
        u64 worst = 0;
        u64 lowest_high = ~u64{0}, highest_high = 0;
        const u32 targets[]{1, 4, 31, 32, 40, 140, 147, 154, 161, 168, 175, 182, 189, 196, 203, 210, 217, 224, 231, 238};
        for (u32 target : targets) {
            if (!valid) break;
            const u32 start = target > preroll ? target - preroll : 0;
            valid = aac_decoder_flush(&decoder).ok() && decode_audio(media, demux, decoder, start, target + 8, sought);
            if (!valid) break;
            double energy = 0, error = 0, ref_high = 0, seek_high = 0;
            const u32 first = target * demux.frames_per_packet * demux.channels;
            const u32 offset = (target - start) * demux.frames_per_packet * demux.channels;
            for (u32 i = 0; i < 8u * demux.frames_per_packet * demux.channels; ++i) {
                const double sample = reference[first + i], difference = sought[offset + i] - sample;
                energy += sample * sample;
                error += difference * difference;
                if (i >= demux.channels) {
                    const double r = sample - reference[first + i - demux.channels];
                    const double v = sought[offset + i] - sought[offset + i - demux.channels];
                    ref_high += r * r;
                    seek_high += v * v;
                }
            }
            const u64 ppm = energy > 0 ? static_cast<u64>(error / energy * 1000000) : 0;
            worst = maximum(worst, ppm);
            // SBR noise phase can differ after reset, so compare spectral energy
            // rather than requiring byte-identical PCM. Missing SBR loses treble.
            if (ref_high > 0.000001) {
                const auto ratio = static_cast<u64>(seek_high / ref_high * 1000000);
                lowest_high = minimum(lowest_high, ratio);
                highest_high = maximum(highest_high, ratio);
                check(ratio > 900000 && ratio < 1100000,
                      "seek retains high frequency energy from the first audible window");
            }
        }
        test_write_metric("seek preroll packets: ", preroll);
        test_write_metric("seek worst PCM error ppm: ", worst);
        test_write_metric("seek minimum high frequency ratio ppm: ", lowest_high);
        test_write_metric("seek maximum high frequency ratio ppm: ", highest_high);
    }
    check(valid, "seek comparison decodes continuous reference and discontinuous windows");
    aac_decoder_release(&decoder);
    if (media.progressive_audio) check(fragment_audio_input(media, 0), "seek comparison restores the borrowed adaptive cursor before playback");
    VirtualFree(reference, 0, MEM_RELEASE);
}
bool twitch_metadata_probe(Utf8View channel, u16 height) noexcept {
    struct Fixture { EmbedResolver resolver; ProgressiveVideoFetch video, audio; u32 calls; };
    auto* f = static_cast<Fixture*>(VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!f) return false;
    bool resolved = false;
    if (update_https_initialize(&f->resolver.https)) {
        const auto exchange = +[](void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                                 UpdateHttpResponse* response) noexcept {
            auto& f = *static_cast<Fixture*>(context); const u32 call = f.calls++;
            const auto error = https_exchange(&f.resolver.https, request, options, response);
            test_write_metric("diagnostic metadata stage: ", call);
            test_write_metric("diagnostic metadata HTTP: ", response->status);
            test_write_metric("diagnostic transport: ", static_cast<u32>(error));
            if (call >= 2 && error == UpdateTransportError::none && response->status == 200) {
                const Utf8View text{reinterpret_cast<const char*>(f.resolver.response.base), f.resolver.response_bytes};
                HlsMediaPlaylist playlist{};
                test_write_metric("diagnostic playlist parse: ", static_cast<u32>(hls_media_playlist(text, &playlist, HlsPlaylistMode::sliding)));
                test_write_metric("diagnostic playlist segments: ", playlist.segments);
                HlsMediaSegment segment{}; u32 bad = 0;
                for (u32 i = 0; i < playlist.segments; ++i)
                    if (!hls_media_segment(playlist, i, &segment) || !twitch_live_location(segment.uri, false, nullptr, playlist.transport)) ++bad;
                test_write_metric("diagnostic rejected segment URLs: ", bad);
                test_write_metric("diagnostic initialization accepted: ", playlist.transport || twitch_live_location(playlist.initialization, false));
            }
            return error;
        };
        const auto result = twitch_hls_resolve(&f->resolver, channel, height, clock_unix_milliseconds() / 1000,
                                             &f->video, &f->audio, {f, exchange}, true);
        test_write_metric("diagnostic resolver result: ", static_cast<u32>(result.error));
        resolved = result.ok();
    }
    progressive_video_fetch_release(&f->video); progressive_video_fetch_release(&f->audio);
    embed_resolver_release(&f->resolver); VirtualFree(f, 0, MEM_RELEASE);
    return resolved;
}
void live_worker_probe(Utf8View source, ID3D11Device* device, u16 height) noexcept {
    auto* worker = static_cast<AccountMediaWorker*>(VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(worker && account_media_worker_initialize(worker).ok(), "live worker initialized");
    if (!worker) return;
    const auto key = media_asset_embed(1, 1, 0, MediaEmbedResource::video, MediaAttachmentFormat::mp4,
                                      true, MediaResolutionTier::px512, false);
    const AccountMediaAttachmentRequest request{source, key, 1, 1, 512, height, CdnImageType::mp4, {1, {0, 0}}, device};
    AccountMediaRequestResult result{};
    AccountMediaFrame frame{};
    const auto until = GetTickCount64() + 6000;
    bool ready = false;
    do {
        result = account_media_worker_request_attachment(worker, request, clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000);
        ready = account_media_worker_take_frame(worker, &frame);
        if (ready || result.state == AccountMediaRequestState::failed || result.state == AccountMediaRequestState::unsupported || result.state == AccountMediaRequestState::invalid) break;
        WaitForSingleObject(worker->output_event, 100);
    } while (GetTickCount64() < until);
    test_write_metric("live worker request state: ", static_cast<u32>(result.state));
    test_write_metric("live worker request error: ", static_cast<u32>(result.error));
    check(ready && frame.gpu_video, "live worker supplies a GPU frame through client admission");
    if (ready) {
        const AccountVideoSelection selection{key, {1, 1, 0, 1, 1, 0, 0}};
        const auto selected = account_media_worker_video_select(worker, selection);
        check(selected.ok() && selected.admitted, "live worker frame hands off to playback session");
    }
    if (!account_media_worker_stop(worker, 5000)) ExitProcess(1);
    VirtualFree(worker, 0, MEM_RELEASE);
}
void live_decode(Utf8View source = utf8_literal("https://www.youtube.com/embed/M7lc1UVf-VE"), u16 height = 360, bool audio_expected = true,
    u16 expected_height = 0) noexcept {
    const bool youtube_live=utf8_equal(source,utf8_literal("https://www.youtube.com/watch?v=ROboUHfPUrU"));
    const bool channel = embed_video_locator(source).provider == EmbedProvider::twitch_live || youtube_live;
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    check(SUCCEEDED(apartment), "live decoder apartment");
    if (FAILED(apartment)) return;
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    const auto created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    check(SUCCEEDED(created), "live GPU device");
    if (FAILED(created)) { CoUninitialize(); return; }
    ID3D11Multithread* guard{};
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&guard)))) {
        guard->SetMultithreadProtected(TRUE);
        guard->Release();
    }
    if (channel && !youtube_live) live_worker_probe(source, device, height);
    auto* media = static_cast<AccountMedia*>(VirtualAlloc(nullptr, sizeof(AccountMedia), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(media && account_media_initialize(media).ok(), "live media lane");
    if (media) {
        const auto key = media_asset_embed(1, 1, 0, MediaEmbedResource::video, MediaAttachmentFormat::mp4,
                                          true, MediaResolutionTier::px512, false);
        const AccountMediaAttachmentRequest request{source,
            key, 1, 1, static_cast<u16>(height > 360 ? 1280 : 640), height, CdnImageType::mp4, {1, {0, 0}}, device};
        const auto requested = account_media_request_attachment(media, request,
            clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000);
        check(requested.ok(), "live provider request");
        if(embed_video_locator(source).provider==EmbedProvider::imgur)
            check(!media->provider_resolver,"direct Imgur routing allocates no metadata resolver or HTTP bootstrap");
        const auto until = GetTickCount64() + 6000;
        while (media->active && !media->frame_ready && GetTickCount64() < until) {
            cdn_client_wait_response(&media->client, 100);
            if (!account_media_pump(media, clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000)) break;
        }
        test_write_metric("live media error: ", static_cast<u32>(media->last_failure.error));
        test_write_metric("live client error: ", static_cast<u32>(media->last_failure.client_error));
        test_write_metric("live HTTP error: ", static_cast<u32>(media->last_failure.http_error));
        test_write_metric("live native error: ", cdn_client_status(&media->client).native_error);
        test_write_metric("live video failure: ", static_cast<u32>(media->last_failure.video_failure));
        if(!media->frame_ready) {
            ProgressiveVideoFetch* owners[]{media->progressive,media->progressive_audio};
            for(auto* owner:owners) if(owner) {
                test_write_metric("failed source kind: ",static_cast<u32>(owner->kind));
                test_write_metric("failed source state: ",static_cast<u32>(owner->state));
                test_write_metric("failed source bytes: ",owner->source.total_bytes);
                if (owner->segmented) {
                    const auto& store = owner->segmented->store;
                    test_write_metric("failed segment init bytes: ", store.initialization_bytes);
                    test_write_metric("failed segment ordinal: ", owner->segmented->number);
                    test_write_metric("failed segment received count: ", store.received_count);
                    test_write_metric("failed segment first slot bytes: ", store.slots[0].bytes);
                    test_write_metric("failed segment second slot bytes: ", store.slots[1].bytes);
                }
                Mp4FragmentSource inspect{};
                const auto error=mp4_fragment_source_initialize(&owner->source,owner==media->progressive_audio,&inspect);
                test_write_metric("failed source demux: ",static_cast<u32>(error));
                if(error==Mp4DemuxError::none && owner==media->progressive_audio) {
                    Mp4FragmentAudio inspect_audio{};
                    test_write_metric("audio setup error: ",static_cast<u32>(mp4_fragment_audio_initialize(&inspect,&inspect_audio)));
                    test_write_metric("audio sample rate: ",inspect.track.sample_rate);
                    test_write_metric("audio timescale: ",inspect.track.timescale);
                    test_write_metric("audio packet frames: ",inspect.track.frames_per_packet);
                    test_write_metric("audio index first: ",inspect.index.first_time);
                    test_write_metric("audio index duration: ",inspect.index.duration);
                    test_write_metric("audio media start: ",inspect.track.media_start);
                    test_write_metric("audio edit duration: ",inspect.track.edit_duration);
                    Mp4FragmentPacket first_packet{};
                    test_write_metric("audio first packet status: ",static_cast<u32>(mp4_fragment_source_peek(&inspect,&first_packet)));
                    test_write_metric("audio first decode tick: ",first_packet.sample.decode_time);
                    test_write_metric("audio first fragment duration: ",inspect.fragment.duration);
                    test_write_metric("audio first fragment samples: ",inspect.fragment.count);
                    Mp4FragmentRange range{};
                    if(mp4_fragment_index_range(inspect.index,0,&range)) for(u32 i=0;i<inspect.index.count;++i) {
                        test_write_metric("audio index span: ",range.duration);
                        if(!mp4_fragment_index_follow(inspect.index,range,&range)) break;
                    }
                }
                mp4_fragment_source_release(&inspect);
            }
        }
        check(media->frame_ready && media->mp4_decoder && media->mp4_decoder->ready,
              "live provider reaches a decoded GPU frame from its native source");
        const auto provider = embed_video_locator(source).provider;
        const bool combined = provider == EmbedProvider::twitter || provider == EmbedProvider::pornhub;
        if (height > 360 && !combined) check(media->progressive_audio && media->mp4_decoder && media->mp4_decoder->fragmented,
                               "higher requested resolution selects independent adaptive sources");
        if (media->frame_ready) {
            if(combined) {
                const auto* gpu=media->mp4_decoder->gpu;
                test_write_metric("native combined video height: ",gpu?gpu->height:0);
                check(gpu && gpu->height==(expected_height?expected_height:height) && !media->progressive_audio && !media->mp4_decoder->fragmented,
                    "provider rendition decodes at requested height with one combined MP4 source");
            }
            if(height>360 && media->progressive->kind==ProgressiveVideoKind::fragmented_video) {
                test_write_metric("native adaptive height: ",media->progressive->fragment.track.height);
                if (channel) {
                    const auto& timeline = media->progressive->fragment;
                    test_write_metric("live source milliframes per second: ", timeline.fragment.duration
                        ? u64{timeline.track.timescale} * 1000 * timeline.fragment.count / timeline.fragment.duration : 0);
                    test_write_metric("live video target seconds: ", media->progressive->segmented->playlist.target_seconds);
                    test_write_metric("live audio target seconds: ", media->progressive_audio->segmented->playlist.target_seconds);
                }
                if (provider == EmbedProvider::vimeo || (channel && height == 1080))
                    check(media->progressive->fragment.track.height == height,
                          "native decode reaches the requested source resolution");
                if(expected_height) check(media->progressive->fragment.track.height==expected_height,
                    "native adaptive decode reaches the required source resolution");
                if(expected_height==1080) {
                    const auto& track_source=media->progressive->fragment;
                    const u64 fps=track_source.fragment.duration ? u64{track_source.track.timescale}*1000*track_source.fragment.count/track_source.fragment.duration : 0;
                    test_write_metric("native adaptive milliframes per second: ",fps);
                    check(fps==60000,"high-quality sample has a native 60 fps timeline");
                }
                if(utf8_equal(source,utf8_literal("https://www.youtube.com/embed/M7lc1UVf-VE")))
                    check(media->progressive->fragment.track.height==720,"YouTube sample selects real720p adaptive video");
            }
            if(embed_video_locator(source).provider==EmbedProvider::imgur) {
                MEMORY_BASIC_INFORMATION region{};
                const bool measured=VirtualQuery(media->progressive->source.base,&region,sizeof(region))==sizeof(region);
                test_write_metric("Imgur encoded committed bytes: ",measured?region.RegionSize:0);
                test_write_metric("Imgur media bytes: ",media->progressive->source.total_bytes);
                check(measured && region.RegionSize<=65536 && media->request_id==1 && !media->progressive_audio &&
                    !media->provider_resolver,"small Imgur clip uses one media range and at most64KiB encoded storage without metadata owners");
            }
            if (media->progressive->segmented)
                test_write_metric("live segmented video height: ", media->progressive->fragment.track.height);
            if (media->progressive->segmented && media->progressive->segmented->playlist.input.bytes) {
                u64 committed = 0; u32 allocations = 0;
                ProgressiveVideoFetch* owners[]{media->progressive, media->progressive_audio};
                for (auto* owner : owners) if (owner && owner->segmented) {
                    MEMORY_BASIC_INFORMATION state{}, playlist{};
                    if (VirtualQuery(owner->segmented, &state, sizeof(state)) == sizeof(state) &&
                        VirtualQuery(owner->segmented->playlist.input.data, &playlist, sizeof(playlist)) == sizeof(playlist)) {
                        committed += state.RegionSize; ++allocations;
                        if (state.AllocationBase != playlist.AllocationBase) { committed += playlist.RegionSize; ++allocations; }
                    }
                }
                test_write_metric("live HLS metadata committed bytes: ", committed);
                test_write_metric("live HLS metadata allocations: ", allocations);
            }
            AccountMediaFrame frame{};
            account_media_take_frame(media, &frame);
            mp4_decoder_acknowledge_frame(media->mp4_decoder);
            if (!channel && audio_expected) audio_seek_probe(*media);
            const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            AccountVideoSession session{};
            const AccountVideoIdentity identity{1, 1, 1, 1, 1, 1, 0};
            const auto initialized = account_video_session_initialize(&session, media->mp4_decoder, identity, event,
                media->progressive_audio ? Mp4SourceAvailability{} : progressive_source_reader(&media->progressive->source, ProgressiveTrack::audio),
                media->progressive_audio ? &media->progressive_audio->audio : nullptr);
            test_write_metric("live audio present: ", session.status.has_audio);
            test_write_metric("live initialization error: ", static_cast<u32>(initialized.error));
            check(initialized.ok() && session.status.has_audio == audio_expected && session.status.duration_100ns > 0,
                  "native session reports the verified media audio presence and duration");
            if (utf8_starts_with(source, utf8_literal("https://music.apple.com/"))) {
                test_write_metric("live video preview duration ms: ", session.status.duration_100ns / 10000);
                check(session.status.duration_100ns > 0 && session.status.duration_100ns <= 910000000,
                      "music-video controls use the preview container duration rather than the full track metadata");
            }
            if (initialized.ok()) {
                account_video_session_control(&session, identity, 1, AccountVideoCommand::mute, 0, 1, true);
                account_video_session_control(&session, identity, 2, AccountVideoCommand::play);
                u32 frames = 0;
                bool sought = channel;
                const bool vod = utf8_starts_with(source, utf8_literal("https://www.twitch.tv/videos/"));
                u32 seek_count = 0;
                u32 refreshes[2]{},peak_encoded=0; bool buffered = false;
                const u32 wanted_frames = channel ? 420 : expected_height==1080 ? 120 : vod ? 20 : 8;
                const auto deadline = GetTickCount64() + (channel ? 10000 : expected_height==1080 ? 6000 : vod ? 5000 : 3000);
                while (frames < wanted_frames && GetTickCount64() < deadline && session.status.error == AccountVideoError::none) {
                    if (cdn_client_status(&media->client).phase == CdnClientPhase::response_ready)
                        cdn_client_consume_response(&media->client,
                            +[](void* opaque, const CdnClientResponse& response, ConstSlice<u8> bytes) noexcept {
                                test_write_metric("play range HTTP: ", response.status_code);
                                test_write_metric("play range error: ", static_cast<u32>(response.error));
                                test_write_metric("play HTTP error: ", static_cast<u32>(response.http_error));
                                test_write_metric("play range bytes: ", bytes.count);
                                account_media_accept_progressive(static_cast<AccountMedia*>(opaque), response, bytes);
                                return true;
                            }, media);
                    const auto service = account_video_session_service(&session);
                    if (channel && buffered != session.status.buffering) {
                        buffered = session.status.buffering;
                        test_write_metric("live buffering: ", buffered);
                        test_write_metric("live buffering at frame: ", frames);
                        test_write_metric("live buffering position ticks: ", session.status.position_100ns);
                        test_write_metric("live buffering video ordinal: ", media->progressive->fragment.segment.index);
                        test_write_metric("live buffering audio ordinal: ", media->progressive_audio->fragment.segment.index);
                    }
                    if (service.frame_ready) {
                        ++frames;
                        check(account_video_session_acknowledge(&session, identity, service.playback_epoch,
                                                               service.frame_serial), "live frame acknowledgement");
                    }
                    if (!channel && frames == (seek_count + 1) * 4 && seek_count < (vod ? 4u : 1u)) {
                        const i64 target = !seek_count ? session.status.duration_100ns / 2 : seek_count == 1 ? 99500000
                            : seek_count == 2 ? session.status.duration_100ns - 10000000 : 0;
                        const auto seek = account_video_session_control(&session, identity, 3 + seek_count, AccountVideoCommand::seek, target);
                        sought = seek.ok() && seek.admitted;
                        ++seek_count;
                    }
                    ProgressiveVideoFetch* selected{};
                    const auto next = account_media_progressive_next(media, channel, clock_unix_milliseconds() / 1000, &selected);
                    if (next.step == ProgressiveVideoStep::submit) {
                        if(selected->segmented && selected->segmented->refreshing) ++refreshes[selected==media->progressive_audio?1:0];
                        test_write_metric("play request audio: ", selected == media->progressive_audio);
                        test_write_metric("play request offset: ", next.range.first);
                        test_write_metric("play request bytes: ", next.range.bytes);
                        account_media_submit_progressive(media, next.range, selected == media->progressive_audio);
                    }
                    else if (next.step == ProgressiveVideoStep::cancel && selected && cdn_client_cancel(&media->client, selected->inflight_id))
                        progressive_video_fetch_cancelling(selected);
                    if(channel) {
                        u32 committed=0;ProgressiveVideoFetch* tracks[]{media->progressive,media->progressive_audio};
                        for(auto* track:tracks) if(track && track->segmented) {
                            const auto& store=track->segmented->store;committed+=store.initialization.committed;
                            for(const auto& slot:store.slots) committed+=slot.storage.committed;
                        }
                        peak_encoded=maximum(peak_encoded,committed);
                    }
                    // Match the shipping lane: continue bounded decoder work
                    // immediately during preroll; wait only when it yields.
                    if (!service.needs_service) WaitForSingleObject(event, minimum(service.next_service_delay_ms, 10u));
                }
                test_write_metric("live decoded frames: ", frames);
                test_write_metric("live playback error: ", static_cast<u32>(session.status.error));
                test_write_metric("live playback native error: ", session.status.native_error);
                test_write_metric("live seek admitted: ", sought);
                test_write_metric("live seek count: ", seek_count);
                test_write_metric("live seeking: ", session.seeking);
                test_write_metric("live seek pending: ", session.seek_pending);
                test_write_metric("live video pending: ", session.video_pending);
                test_write_metric("live audio pending: ", session.audio_pending);
                test_write_metric("live fetch state: ", static_cast<u32>(media->progressive->state));
                test_write_metric("live range requests: ", media->request_id);
                test_write_metric("live audio written: ", account_video_session_audio_progress(&session).written_frames);
                test_write_metric("live audio consumed: ", account_video_session_audio_progress(&session).consumed_frames);
                if(channel) {
                    test_write_metric("live video playlist refreshes: ",refreshes[0]);
                    test_write_metric("live audio playlist refreshes: ",refreshes[1]);
                    test_write_metric("live peak encoded bytes: ",peak_encoded);
                    test_write_metric("live final video ordinal: ", media->progressive->fragment.segment.index);
                    test_write_metric("live final audio ordinal: ", media->progressive_audio->fragment.segment.index);
                    check(media->progressive->fragment.segment.index >= 3 && media->progressive_audio->fragment.segment.index >= 3,
                          "live playback crosses at least three segment boundaries on both tracks");
                    check(refreshes[0] && refreshes[1] && peak_encoded<=18*1024*1024+2*65536,
                        "live playback refreshes both tracks within the existing two-slot encoded budgets");
                }
                const auto audio = account_video_session_audio_progress(&session);
                check(frames >= wanted_frames && seek_count == (channel ? 0u : vod ? 4u : 1u) && sought &&
                      session.status.live == channel &&
                      (audio_expected ? session.status.muted && session.status.volume_valid && audio.written_frames && audio.consumed_frames
                                      : !audio.written_frames && !audio.consumed_frames) && session.status.error == AccountVideoError::none,
                      "native video and its available audio play and seek with bounded ranges");
            }
            if (!account_video_session_release(&session, 1000).ok()) ExitProcess(1);
            if(media->progressive_audio && media->progressive_audio->audio.index_edit_ticks) {
                auto& audio=media->progressive_audio->audio;
                Mp4FragmentAudioPacket packet{};Mp4DemuxError error=Mp4DemuxError::data_pending;
                const auto until_tail=GetTickCount64()+3000;
                while(error==Mp4DemuxError::data_pending && GetTickCount64()<until_tail) {
                    error=mp4_fragment_audio_seek(&audio,audio.packet_count-1);
                    if(error!=Mp4DemuxError::data_pending) break;
                    if(cdn_client_status(&media->client).phase==CdnClientPhase::response_ready)
                        cdn_client_consume_response(&media->client,
                            +[](void* p,const CdnClientResponse& r,ConstSlice<u8> b) noexcept {
                                account_media_accept_progressive(static_cast<AccountMedia*>(p),r,b);return true;
                            },media);
                    ProgressiveVideoFetch* selected{};
                    const auto next=account_media_progressive_next(media,false,clock_unix_milliseconds()/1000,&selected);
                    if(next.step==ProgressiveVideoStep::submit) account_media_submit_progressive(media,next.range,selected==media->progressive_audio);
                    else if(next.step==ProgressiveVideoStep::failed) break;
                    cdn_client_wait_response(&media->client,20);
                }
                const bool tail=error==Mp4DemuxError::none && mp4_fragment_audio_peek(&audio,&packet)==Mp4DemuxError::none &&
                    packet.first_source_frame==u64{audio.packet_count-1}*audio.source->track.frames_per_packet &&
                    mp4_fragment_audio_commit(&audio) && mp4_fragment_audio_peek(&audio,&packet)==Mp4DemuxError::end_of_stream;
                test_write_metric("edited AAC tail valid: ",tail);
                check(tail,"real edited AAC tail retains its exact final packet and EOF after a distant seek");
            }
            if (event) CloseHandle(event);
        }
        if (!account_media_stop(media, 5000)) ExitProcess(1);
        VirtualFree(media, 0, MEM_RELEASE);
    }
    context->Release();
    device->Release();
    CoUninitialize();
}
void live_music_preview(bool apple = false, bool amazon = false, Utf8View source = {}) noexcept {
    bool episode=false;
    spotify_audio_locator(source,&episode);
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    check(SUCCEEDED(apartment), "preview audio apartment");
    if (FAILED(apartment)) return;
    auto* media = static_cast<AccountMedia*>(VirtualAlloc(nullptr, sizeof(AccountMedia), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(media && account_media_initialize(media).ok(), "preview media lane");
    if (media) {
        const auto key = media_asset_embed(1, 1, 0, MediaEmbedResource::video,
                                           apple ? MediaAttachmentFormat::m4a : MediaAttachmentFormat::mp3);
        const AccountMediaAttachmentRequest request{source.bytes ? source : apple
            ? utf8_literal("https://music.apple.com/us/album/imagine/255144028?i=255145362")
            : amazon ? utf8_literal("https://music.amazon.com/albums/B000VBJ504?trackAsin=B000VBH2R2")
            : utf8_literal("https://open.spotify.com/track/11dFghVXANMlKmJXsNCbNl"),
            key, 1, 1, 0, 0, apple ? CdnImageType::m4a : CdnImageType::mp3, {}, nullptr};
        const auto requested = account_media_request_attachment(media, request,
            clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000);
        test_write_metric("preview request state: ", static_cast<u32>(requested.state));
        const auto until = GetTickCount64() + 5000;
        while (media->active && !media->frame_ready && GetTickCount64() < until) {
            cdn_client_wait_response(&media->client, 100);
            if (!account_media_pump(media, clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000)) break;
        }
        test_write_metric("preview media error: ", static_cast<u32>(media->last_failure.error));
        test_write_metric("preview HTTP error: ", static_cast<u32>(media->last_failure.http_error));
        const auto client = cdn_client_status(&media->client);
        test_write_metric("preview transport phase: ", static_cast<u32>(client.phase));
        test_write_metric("preview transport error: ", static_cast<u32>(client.error));
        test_write_metric("preview native error: ", client.native_error);
        test_write_metric("preview active: ", media->active);
        test_write_metric("preview frame kind: ", static_cast<u32>(media->frame_kind));
        check(account_media_audio_matches(media, key) && !media->mp4_decoder && !media->progressive,
              "public preview reaches resident native audio with no GPU decoder or sparse movie storage");
        if (account_media_audio_matches(media, key)) {
            test_write_metric("preview encoded bytes: ", media->audio_source.bytes.count);
            const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            AccountVideoSession session{};
            const AccountVideoIdentity identity{1, 1, 1, 1, 1, 1, 0};
            const auto initialized = account_video_session_initialize_audio(&session, &media->audio_source.metadata, identity, event);
            test_write_metric("preview decoded duration ms: ",static_cast<u64>(session.status.duration_100ns/10000));
            check(initialized.ok() && session.status.kind == (apple ? AccountPlaybackKind::mp4 : AccountPlaybackKind::mp3) &&
                  session.status.has_audio && session.status.duration_100ns > 0 &&
                  (episode ? session.status.duration_100ns==600000000 :
                   session.status.duration_100ns <= (apple || amazon ? 910000000 : 310000000)),
                  "preview duration comes from decoded samples, not the full song or episode metadata");
            if (initialized.ok()) {
                account_video_session_control(&session, identity, 1, AccountVideoCommand::mute, 0, 1, true);
                account_video_session_control(&session, identity, 2, AccountVideoCommand::play);
                bool sought = false, consumed = false;
                u32 seek_decode_calls = 0;
                const auto deadline = GetTickCount64() + 2000;
                while (!consumed && GetTickCount64() < deadline && session.status.error == AccountVideoError::none) {
                    const auto serviced = account_video_session_service(&session);
                    const auto progress = account_video_session_audio_progress(&session);
                    if (sought && !progress.written_frames) ++seek_decode_calls;
                    if (progress.consumed_frames > 4096 && session.status.volume_valid) {
                        if (!sought) {
                            check(account_video_session_control(&session, identity, 3, AccountVideoCommand::seek,
                                session.status.duration_100ns / 2).ok(), "preview seek admitted");
                            sought = true;
                        } else if (!session.status.seeking) consumed = true;
                    }
                    if (!consumed) WaitForSingleObject(event, minimum(serviced.next_service_delay_ms, 10u));
                }
                test_write_metric("preview decoder bytes: ", account_video_session_owned_bytes(&session));
                test_write_metric("preview session error: ", static_cast<u32>(session.status.error));
                test_write_metric("preview session native error: ", session.status.native_error);
                test_write_metric("preview sought: ", sought);
                test_write_metric("preview seeking: ", session.status.seeking);
                test_write_metric("preview written frames: ", account_video_session_audio_progress(&session).written_frames);
                test_write_metric("preview consumed frames: ", account_video_session_audio_progress(&session).consumed_frames);
                check(sought && consumed && session.status.muted && session.status.error == AccountVideoError::none,
                      "native preview playback and seeking produce audio through the existing muted WASAPI session");
                check(seek_decode_calls <= 16, "preview seek decodes bounded preroll instead of replaying the song prefix");
                test_write_metric("preview seek decode service calls: ", seek_decode_calls);
                if (apple && consumed) {
                    account_video_session_control(&session, identity, 4, AccountVideoCommand::pause);
                    account_video_session_control(&session, identity, 5, AccountVideoCommand::seek, session.status.duration_100ns);
                    for (u32 i = 0; i < 16 && session.status.seeking; ++i) {
                        account_video_session_service(&session);
                        WaitForSingleObject(event, 10);
                    }
                    check(!session.status.seeking && session.status.paused && !session.status.frame_pending &&
                          !account_video_session_audio_progress(&session).written_frames && session.status.error == AccountVideoError::none,
                          "audio-only AAC end seek completes without PCM or a nonexistent picture acknowledgement");
                }
            }
            if (!account_video_session_release(&session, 1000).ok()) ExitProcess(1);
            if (event) CloseHandle(event);
        }
        if (!account_media_stop(media, 5000)) ExitProcess(1);
        VirtualFree(media, 0, MEM_RELEASE);
    }
    CoUninitialize();
}
void live_probe() noexcept {
    char value[2];
    if (GetEnvironmentVariableA("MAICORD_YOUTUBE_LIVE_PROBE", value, sizeof(value)) != 1 || value[0] != '1') return;
    auto* r = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!r) { check(false, "live probe allocation"); return; }
    EmbedStreams streams{};
    const auto result = youtube_resolve(r, utf8_literal("M7lc1UVf-VE"), 720, &streams);
    test_write_metric("live HTTP status: ", result.status);
    test_write_metric("live transport error: ", static_cast<u32>(result.transport_error));
    test_write_metric("live player error: ", static_cast<u32>(result.error));
    test_write_metric("live video height: ", streams.video.height);
    test_write_metric("live response peak bytes: ", r->response.peak);
    test_write_metric("live scratch peak bytes: ", r->structurals.peak);
    check(result.ok(), "live native resolver");
    if (result.ok()) {
        CdnClient client{};
        const auto started = cdn_client_start(&client, kProgressiveBlockBytes);
        check(started.ok(), "live range client start");
        if (started.ok()) {
            for (u32 i = 0; i < (streams.combined ? 2u : 4u); ++i) {
                const bool audio = i >= 2;
                const auto& stream = audio ? streams.audio : streams.video;
                const u32 length = kProgressiveBlockBytes;
                const CdnClientRequest request{{reinterpret_cast<const u8*>(stream.url), stream.url_bytes}, {},
                    i + 1ull, 5000, CdnHost::google_video, audio ? CdnImageType::m4a : CdnImageType::mp4,
                    {i == 0 || i == 2 ? 0u : 1048576u, length}};
                if (!cdn_client_submit(&client, request).ok()) { check(false, "live range submit"); break; }
                cdn_client_wait_response(&client, 5000);
                CdnClientResponse response{};
                static u8 bytes[kProgressiveBlockBytes];
                const auto taken = cdn_client_take_response(&client, &response, {bytes, sizeof(bytes)});
                test_write_metric(audio ? "live audio HTTP: " : "live video HTTP: ", response.status_code);
                test_write_metric("live declared entity bytes: ", stream.content_bytes);
                test_write_metric("live returned entity bytes: ", response.entity_bytes);
                test_write_metric("live range length: ", length);
                test_write_metric("live CDN error: ", static_cast<u32>(response.error));
                check(taken.ok() && response.transport_ok() && response.status_code == 206 && taken.body_bytes == length &&
                      (request.range.first || bytes_equal(bytes + 4, "ftyp", 4)), "native bounded H.264/AAC range fetch");
            }
            if (!cdn_client_stop(&client, 5000).ok()) ExitProcess(1);
        }
    }
    embed_resolver_release(r);
    VirtualFree(r, 0, MEM_RELEASE);
    live_decode();
}
} // namespace
}
extern "C" __declspec(noreturn) void WINAPI maicord_youtube_resolver_entry() noexcept {
    maicord::vimeo_resolver_test();
    maicord::youtube_live_resolver_test();
    maicord::check(maicord::native_player_perf_probe(), "native live worker performance and paused activity probe");
    maicord::check(maicord::twitch_live_fetch_probe(), "native live channel refresh continues GPU and AAC playback on one epoch");
    maicord::check(maicord::soundcloud_player_tests(), "SoundCloud identity-bound native resolution");
    maicord::live_resolver_test();
    char channel[26]{};
    const auto channel_bytes = GetEnvironmentVariableA("MAICORD_TWITCH_URL_PROBE", channel, sizeof(channel));
    if (channel_bytes && channel_bytes < sizeof(channel) && maicord::twitch_channel_id({channel, channel_bytes})) {
        char metadata[2]{};
        if (GetEnvironmentVariableA("MAICORD_TWITCH_METADATA_PROBE", metadata, sizeof(metadata)) == 1 && metadata[0] == '1') {
            for (maicord::u32 i = 0; i < 3; ++i) maicord::check(maicord::twitch_metadata_probe({channel, channel_bytes}, 1080),
                "native Twitch metadata resolves both validated track owners");
            ExitProcess(maicord::failures ? 1 : 0);
        }
        constexpr char prefix[] = "https://www.twitch.tv/"; char url[sizeof(prefix) + sizeof(channel)]{};
        maicord::copy_bytes(url, prefix, sizeof(prefix) - 1); maicord::copy_bytes(url + sizeof(prefix) - 1, channel, channel_bytes);
        char small[2]{};
        const bool compact = GetEnvironmentVariableA("MAICORD_TWITCH_COMPACT_PROBE", small, sizeof(small)) == 1 && small[0] == '1';
        const bool high = GetEnvironmentVariableA("MAICORD_TWITCH_HIGH_PROBE", small, sizeof(small)) == 1 && small[0] == '1';
        maicord::live_decode({url, static_cast<maicord::u32>(sizeof(prefix) - 1 + channel_bytes)}, high ? 1080 : compact ? 300 : 480);
    }
    maicord::scanner(); maicord::headers(); maicord::resolver(); maicord::steam_resolver_test(); maicord::vod_resolver_test(); maicord::resident_selection(); maicord::paired_ranges(); maicord::live_probe();
    char live[2];
    if (GetEnvironmentVariableA("MAICORD_YTM_CARD_PLAYBACK_PROBE", live, sizeof(live)) == 1 && live[0] == '1') {
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/playlist?list=PLQ73uMywXkLE"),720);
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=j7bqDpjgM5M&list=RDAMVMj7bqDpjgM5M"),720);
    }
    if (GetEnvironmentVariableA("MAICORD_YTM_CARD_PLAYBACK_PROBE", live, sizeof(live)) == 1 && live[0] == '2')
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=eyk25H75ld0&list=RDAMVMY5f3S26WwDs"),720);
    if (GetEnvironmentVariableA("MAICORD_VIMEO_LIVE_PROBE", live, sizeof(live)) == 1 && (live[0] == '1' || live[0] == '2')) {
        maicord::live_decode(live[0] == '1' ? maicord::utf8_literal("https://player.vimeo.com/video/22439234")
            : maicord::utf8_literal("https://player.vimeo.com/video/22439234?h=0123456789"), 1080);
    }
    if (GetEnvironmentVariableA("MAICORD_PH_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://www.pornhub.com/view_video.php?viewkey=683dbe0849d99"), 720, true, 480);
    if (GetEnvironmentVariableA("MAICORD_YOUTUBE_HD_PROBE", live, sizeof(live)) == 1 && live[0] == '4')
        maicord::live_decode(maicord::utf8_literal("https://www.youtube.com/watch?v=ROboUHfPUrU"),720);
    if (GetEnvironmentVariableA("MAICORD_YOUTUBE_HD_PROBE", live, sizeof(live)) == 1 && live[0] == '3')
        maicord::live_decode(maicord::utf8_literal("https://www.youtube.com/watch?v=aqz-KE-bpKQ"),1080,true,1080);
    if (GetEnvironmentVariableA("MAICORD_YOUTUBE_HD_PROBE", live, sizeof(live)) == 1 && live[0] == '5')
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=zlQ0Lfpq078"),1080,true,1080);
    if (GetEnvironmentVariableA("MAICORD_AMAZON_MUSIC_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_music_preview(false,true);
    if (GetEnvironmentVariableA("MAICORD_AMAZON_MUSIC_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '2')
        maicord::live_music_preview(false,true,maicord::utf8_literal("https://music.amazon.de/tracks/B0BCHTKG7D"));
    if (GetEnvironmentVariableA("MAICORD_TWITTER_LIVE_PROBE", live, sizeof(live)) == 1 && (live[0] == '1' || live[0] == '2'))
        maicord::live_decode(maicord::utf8_literal("https://x.com/SpaceX/status/1732824684683784516/video/1"),live[0]=='1'?360:720);
    if (GetEnvironmentVariableA("MAICORD_TWITTER_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '3')
        maicord::live_decode(maicord::utf8_literal("https://twitter.com/i/videos/tweet/1819295962591862827?embed_source=clientlib"),360,true,562);
    if (GetEnvironmentVariableA("MAICORD_YOUTUBE_HD_PROBE", live, sizeof(live)) == 1 && (live[0] == '1' || live[0] == '2')) {
        if(live[0]=='1') {
            maicord::live_decode(maicord::utf8_literal("https://www.youtube.com/embed/M7lc1UVf-VE"),720);
            maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=zlQ0Lfpq078"),720);
        }
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=CZQdi1sxd30"),720);
    }
    if (GetEnvironmentVariableA("MAICORD_IMGUR_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://imgur.com/tfgwzwf/embed"),360,false);
    if (GetEnvironmentVariableA("MAICORD_STEAM_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://store.steampowered.com/video/570/256692021/"), 480);
    if (GetEnvironmentVariableA("MAICORD_APPLE_VIDEO_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://music.apple.com/us/music-video/adrift-live-from-the-solar-powered-plastic-plant/1445828315"));
    if (GetEnvironmentVariableA("MAICORD_APPLE_MUSIC_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_music_preview(true);
    if (GetEnvironmentVariableA("MAICORD_STREAMABLE_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://streamable.com/e/hn8hq"));
    if (GetEnvironmentVariableA("MAICORD_SPOTIFY_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_music_preview();
    if (GetEnvironmentVariableA("MAICORD_SPOTIFY_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '2')
        maicord::live_music_preview(false,false,maicord::utf8_literal("https://open.spotify.com/episode/7makk4oTQel546B0PZlDM5"));
    if (GetEnvironmentVariableA("MAICORD_TIKTOK_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://www.tiktok.com/player/v1/6718335390845095173"));
    if (GetEnvironmentVariableA("MAICORD_TWITCH_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://clips.twitch.tv/IncredulousAbstemiousFennelImGlitch"));
    if (GetEnvironmentVariableA("MAICORD_TWITCH_VOD_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://www.twitch.tv/videos/2908640776"), 480);
    if (GetEnvironmentVariableA("MAICORD_TWITCH_VOD_HD_PROBE", live, sizeof(live)) == 1 && live[0] == '1')
        maicord::live_decode(maicord::utf8_literal("https://www.twitch.tv/videos/2908640776"), 1080);
    if (GetEnvironmentVariableA("MAICORD_YTM_LIVE_PROBE", live, sizeof(live)) == 1 && live[0] == '1') {
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=zlQ0Lfpq078"));
        maicord::live_decode(maicord::utf8_literal("https://music.youtube.com/watch?v=CZQdi1sxd30"));
    }
    ExitProcess(maicord::failures ? 1 : 0);
}
