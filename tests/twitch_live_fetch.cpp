#include "maicord/app/account_media.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/app/embed_resolver.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/d3d11.hpp"
#include "test_output.hpp"

namespace maicord {
// Public channel name opt-in; emits counts/errors only, never signed targets.
bool twitch_live_fetch_probe() noexcept {
    char channel[26]{};
    const u32 length = GetEnvironmentVariableA("MAICORD_TWITCH_CHANNEL_PROBE", channel, sizeof(channel));
    if (!length) return true;
    if (length >= sizeof(channel) || !twitch_channel_id({channel, length})) return false;
    char high[2]{};
    const u16 height = GetEnvironmentVariableA("MAICORD_TWITCH_HIGH_PROBE", high, sizeof(high)) == 1 && high[0] == '1' ? 1080 : 480;
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* media = static_cast<AccountMedia*>(VirtualAlloc(nullptr, sizeof(AccountMedia), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* resolver = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool valid = SUCCEEDED(apartment) && media && resolver && account_media_initialize(media).ok();
    if (valid) {
        media->progressive = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        media->progressive_audio = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        valid = media->progressive && media->progressive_audio;
    }
    u32 stage = valid ? 1u : 0u;
    EmbedResolveResult resolved{};
    if (valid) {
        valid = update_https_initialize(&resolver->https); resolver->native_state = 2;
        char skew[2]{};
        struct Metadata { EmbedResolver* resolver; u16 height; u32 calls{}; bool skew{}; } metadata{resolver, height};
        metadata.skew = GetEnvironmentVariableA("MAICORD_TWITCH_SKEW_PROBE", skew, sizeof(skew)) == 1 && skew[0] == '1';
        const auto exchange = +[](void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
            auto& m = *static_cast<Metadata*>(opaque); ++m.calls;
            // Optional network skew: the independent audio snapshot advances
            // while video metadata is already retained. Assert media counts,
            // timestamps and continuity below, never elapsed wall time.
            if (m.skew && m.calls == 4) Sleep(2100);
            const auto result = https_exchange(&m.resolver->https, request, options, response);
            test_write_metric("Twitch live metadata step: ", m.calls);
            if (result == UpdateTransportError::none && response->status == 200 && m.calls >= 2) {
                const Utf8View text{reinterpret_cast<char*>(m.resolver->response.base), m.resolver->response_bytes};
                if (m.calls == 2) {
                    HlsMasterPlaylist master{}; const auto parsed = hls_master_playlist(text, m.height, &master);
                    test_write_metric("Twitch live master parse: ", static_cast<u32>(parsed));
                    test_write_metric("Twitch live video playlist gate: ", twitch_live_location(master.video.uri, true));
                    test_write_metric("Twitch live audio playlist gate: ", twitch_live_location(master.audio.uri, true));
                } else {
                    HlsMediaPlaylist playlist{}; const auto parsed = hls_media_playlist(text, &playlist, HlsPlaylistMode::sliding);
                    test_write_metric("Twitch live media playlist parse: ", static_cast<u32>(parsed));
                    test_write_metric("Twitch live transport format: ",playlist.transport);
                    test_write_metric("Twitch live initialization gate: ", playlist.transport || twitch_live_location(playlist.initialization, false));
                    HlsMediaSegment segment{};
                    test_write_metric("Twitch live media gate: ", hls_media_segment(playlist, 0, &segment) && twitch_live_location(segment.uri, false,nullptr,playlist.transport));
                    u32 valid_segments = 0;
                    for (u32 i = 0; i < playlist.segments; ++i) {
                        if (!(i ? hls_media_follow(playlist, segment, &segment) : hls_media_segment(playlist, 0, &segment))) {
                            test_write_metric("Twitch live failed segment walk: ", i); break;
                        }
                        if (!twitch_live_location(segment.uri, false, nullptr, playlist.transport)) {
                            test_write_metric("Twitch live rejected segment index: ", i);
                            test_write_metric("Twitch live rejected URI bytes: ", segment.uri.bytes);
                            break;
                        }
                        ++valid_segments;
                    }
                    test_write_metric("Twitch live admitted segments: ", valid_segments);
                    test_write_metric("Twitch live playlist segments: ", playlist.segments);
                }
            }
            return result;
        };
        resolved = twitch_hls_resolve(resolver, {channel, length}, height, clock_unix_milliseconds() / 1000,
            media->progressive, media->progressive_audio, {&metadata, exchange}, true);
        valid = resolved.ok(); if (valid) stage = 2;
    }
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
    valid = valid && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
        D3D11_SDK_VERSION, &device, &level, &context));
    if (valid) {
        valid = cdn_client_start(&media->client, 8 * 1024 * 1024).ok();
        media->client_started = valid; if (valid) stage = 3;
    }
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    valid = valid && event;
    AccountVideoSession session{}; const AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    struct Feed { AccountMedia* media; bool valid{true}; u32 playlists[2]{}; } feed{media};
    u32 frames = 0; u64 epoch = 0, prior_written = 0;
    const u64 deadline = GetTickCount64() + 12000;
    while (valid && GetTickCount64() < deadline) {
        if (cdn_client_status(&media->client).phase == CdnClientPhase::response_ready)
            cdn_client_consume_response(&media->client, +[](void* opaque, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
                auto& f = *static_cast<Feed*>(opaque);
                auto* video = f.media->progressive;
                const u32 index = video->inflight_id == response.request_id ? 0 : 1;
                const auto* owner = index ? f.media->progressive_audio : video;
                const bool playlist = owner->host == CdnHost::twitch_playlist;
                const auto accepted = account_media_accept_progressive(f.media, response, body);
                if (accepted == ProgressiveVideoAccept::failed) {
                    test_write_metric("Twitch failed response track: ", index);
                    test_write_metric("Twitch failed response HTTP: ", cdn_client_response_policy(response).status_code);
                    test_write_metric("Twitch failed response bytes: ", body.count);
                    test_write_metric("Twitch failed response client: ", static_cast<u32>(response.error));
                    test_write_metric("Twitch failed response entity: ", response.entity_bytes);
                    test_write_metric("Twitch failed response disposition: ", static_cast<u32>(cdn_client_response_policy(response).disposition));

                }
                f.valid = accepted != ProgressiveVideoAccept::failed;
                if (playlist && accepted == ProgressiveVideoAccept::progress) ++f.playlists[index];
                return true;
            }, &feed);
        valid = feed.valid;
        ProgressiveVideoFetch* selected{};
        const auto next = account_media_progressive_next(media, true, clock_unix_milliseconds() / 1000, &selected);
        if (next.step == ProgressiveVideoStep::submit)
            valid = valid && account_media_submit_progressive(media, next.range, selected == media->progressive_audio);
        else if (next.step == ProgressiveVideoStep::cancel) {
            if (cdn_client_cancel(&media->client, selected->inflight_id)) progressive_video_fetch_cancelling(selected);
        } else if (next.step == ProgressiveVideoStep::failed) valid = false;
        if (!valid) {
            test_write_metric("Twitch failed next step: ", static_cast<u32>(next.step));
            test_write_metric("Twitch failed next audio: ", selected == media->progressive_audio);
            test_write_metric("Twitch failed client native: ", cdn_client_status(&media->client).native_error);
            break;
        }
        if (!session.initialized && media->progressive->state == ProgressiveVideoFetchState::ready &&
            media->progressive_audio->state == ProgressiveVideoFetchState::ready && media->progressive->segmented->synchronized) {
            stage = 4;
            media->mp4_decoder = &media->owned_mp4_decoder;
            valid = segmented_video_synchronize(media->progressive, media->progressive_audio) == Mp4DemuxError::none &&
                mp4_decoder_initialize_fragmented(media->mp4_decoder, &media->progressive->fragment, device) &&
                account_video_session_initialize(&session, media->mp4_decoder, identity, event, {}, &media->progressive_audio->audio).ok() &&
                session.status.live && session.status.has_audio &&
                account_video_session_control(&session, identity, 1, AccountVideoCommand::mute, 0, 1, true).ok() &&
                account_video_session_control(&session, identity, 2, AccountVideoCommand::play).ok();
            epoch = session.playback_epoch; if (valid) stage = 5;
            if (valid) {
                i64 first_video{}, first_audio{};
                const auto& v = media->progressive->fragment; const auto& a = media->progressive_audio->fragment;
                Mp4AudioSampleWindow window{};
                valid = mp4_fragment_source_time(v, static_cast<i64>(v.index.first_time), &first_video) &&
                    mp4_fragment_source_time(a, static_cast<i64>(a.index.first_time), &first_audio) && first_video <= 0 && first_audio <= 0 &&
                    mp4_audio_sample_window(media->progressive_audio->audio.timeline, 0, &window) && window.leading_silence_frames <= 1;
                test_write_metric("Twitch live initial video trim us: ", static_cast<u64>(-first_video));
                test_write_metric("Twitch live initial audio trim us: ", static_cast<u64>(-first_audio));
                test_write_metric("Twitch live leading silence frames: ", window.leading_silence_frames);
            }
        }
        AccountVideoService work{};
        if (valid && session.initialized) {
            work = account_video_session_service(&session);
            if (work.frame_ready) { ++frames; valid = account_video_session_acknowledge(&session, identity, work.playback_epoch, work.frame_serial); }
            const auto progress = account_video_session_audio_progress(&session);
            valid = valid && session.status.error == AccountVideoError::none && !session.status.ended &&
                session.playback_epoch == epoch && progress.written_frames >= prior_written;
            prior_written = progress.written_frames;
            if (valid && feed.playlists[0] && feed.playlists[1] && media->progressive->fragment.segment.index >= 3 &&
                media->progressive_audio->fragment.segment.index >= 3 && frames >= 60 && progress.consumed_frames) { stage = 6; break; }
        }
        if (!work.needs_service) {
            const HANDLE events[]{event, cdn_client_notification_handle(&media->client)};
            WaitForMultipleObjects(2, events, FALSE, minimum(50u, work.next_service_delay_ms));
        }
    }
    test_write_metric("Twitch live fetch stage: ", stage);
    test_write_metric("Twitch live resolve error: ", static_cast<u32>(resolved.error));
    test_write_metric("Twitch live HTTP status: ", resolved.status);
    test_write_metric("Twitch live session error: ", static_cast<u32>(session.status.error));
    test_write_metric("Twitch live native error: ", session.status.native_error);
    test_write_metric("Twitch live frames: ", frames);
    if (media && media->progressive && stage >= 5)
        test_write_metric("Twitch live decoded height: ", media->progressive->fragment.track.height);
    test_write_metric("Twitch live video refreshes: ", feed.playlists[0]);
    test_write_metric("Twitch live audio refreshes: ", feed.playlists[1]);
    test_write_metric("Twitch live consumed PCM: ", account_video_session_audio_progress(&session).consumed_frames);
    if(media&&media->progressive&&media->progressive_audio&&media->progressive->segmented&&media->progressive_audio->segmented) {
        u32 committed=0,peak=0;
        const Mp4SegmentStore* stores[]{&media->progressive->segmented->store,&media->progressive_audio->segmented->store};
        for(const auto* store:stores) {committed+=store->initialization.committed;peak+=store->initialization.peak;
            for(const auto& slot:store->slots) {committed+=slot.storage.committed;peak+=slot.storage.peak;}}
        test_write_metric("Twitch live encoded committed bytes: ",committed);
        test_write_metric("Twitch live encoded peak bytes: ",peak);
    }
    valid = valid && stage == 6;
    if (session.initialized && !account_video_session_release(&session, 1000).ok()) ExitProcess(1);
    if (media) { if (!account_media_stop(media, 1000)) ExitProcess(1); VirtualFree(media, 0, MEM_RELEASE); }
    if (resolver) { embed_resolver_release(resolver); VirtualFree(resolver, 0, MEM_RELEASE); }
    if (context) context->Release(); if (device) device->Release(); if (event) CloseHandle(event);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return valid;
}
} // namespace maicord
