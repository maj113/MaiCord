#include "maicord/app/embed_resolver.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/app/cdn_client.hpp"
#include "maicord/app/account_media_worker.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/d3d11.hpp"
#include "test_output.hpp"

namespace maicord {
bool youtube_collection_tests(EmbedResolver*) noexcept;
bool apple_music_album_tests(EmbedResolver*) noexcept;
bool apple_music_playlist_tests(EmbedResolver*) noexcept;
bool soundcloud_collection_tests(EmbedResolver*) noexcept;
namespace {
constexpr auto page = utf8_literal("https://soundcloud.com/forss/flickermood");
constexpr auto widget = utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/293&visual=true");
constexpr auto canonical = utf8_literal("<link rel=\"canonical\" href=\"https://soundcloud.com/forss/flickermood\">");
constexpr auto manifest = utf8_literal("https://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/playlist.m3u8?expires=9999999999&Signature=test");
constexpr auto html = utf8_literal(R"(<script>window.__sc_hydration = [{"hydratable":"apiClient","data":{"id":"0123456789abcdef0123456789abcdef"}},{"hydratable":"sound","data":{"id":293,"kind":"track","permalink_url":"https://soundcloud.com/forss/flickermood","public":true,"streamable":true,"policy":"ALLOW","embeddable_by":"all","track_authorization":"public-test","media":{"transcodings":[{"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:293/hash/stream/hls","preset":"aac_160k","snipped":false,"format":{"protocol":"hls","mime_type":"audio/mp4; codecs=\"mp4a.40.2\""}}]}}}];</script>)");
constexpr auto manifest_response = utf8_literal(R"({"url":"https://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/playlist.m3u8?expires=9999999999&Signature=test"})");
constexpr auto playlist = utf8_literal("#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:10\n#EXT-X-MAP:URI=\"https://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/init.mp4?expires=9999999999\"\n#EXTINF:10,\nhttps://playback.media-streaming.soundcloud.cloud/example/aac_160k/hash/data000.m4s?expires=9999999999\n#EXT-X-ENDLIST\n");
u32 failures;
void check(bool value, const char* name) noexcept { test_check(&failures, value, name); }
void protocol_tests() noexcept {
    u32 scratch[4096]; EmbedStream out{};
    char decoded[1024];
    check(soundcloud_track_locator(widget, {decoded, sizeof(decoded)}).id == 293 && soundcloud_track_url(widget) &&
        utf8_equal(soundcloud_track_locator(utf8_literal("https://w.soundcloud.com/player/?url=https%3A%2F%2Fsoundcloud.com%2Fforss%2Fflickermood"),
            {decoded, sizeof(decoded)}).path, utf8_literal("/forss/flickermood")), "SoundCloud widget decodes either a numeric track or one public permalink");
    const Utf8View bad_widgets[]{
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/293&url=https%3A//api.soundcloud.com/tracks/294"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/0293"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/18446744073709551616"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/293"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/293%3Fsecret_token%3Dx"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//evil.test/tracks/293"),
        utf8_literal("https://w.soundcloud.com/player/?url=https%3A//soundcloud.com/forss/%"),
        utf8_literal("https://w.soundcloud.com.evil.test/player/?url=https%3A//api.soundcloud.com/tracks/293")};
    for (auto url : bad_widgets) check(!soundcloud_track_url(url), "SoundCloud widget rejects ambiguous identity, wrong origins and invalid encoding");
    check(soundcloud_widget_page(canonical, &out) == EmbedPlayerError::none && utf8_equal({out.url, out.url_bytes}, page),
        "SoundCloud canonical widget page is restricted to a public track permalink");
    check(embed_provider(page) == EmbedProvider::soundcloud && utf8_equal(soundcloud_track_path(page), utf8_literal("/forss/flickermood")) &&
        utf8_equal(soundcloud_track_path(utf8_literal("https://www.soundcloud.com/forss/flickermood/?utm_source=clipboard")), utf8_literal("/forss/flickermood")),
        "SoundCloud public permalink normalizes share parameters");
    const Utf8View bad[]{utf8_literal("https://soundcloud.com/forss/sets"), utf8_literal("https://soundcloud.com/forss"),
        utf8_literal("https://soundcloud.com/forss/sets/album"), utf8_literal("https://soundcloud.com.evil.test/forss/flickermood"),
        utf8_literal("https://soundcloud.com/forss/flickermood?secret_token=private"), utf8_literal("https://soundcloud.com/forss/%2e%2e")};
    for (auto url : bad) check(!soundcloud_track_path(url).bytes, "SoundCloud refuses ambiguous, private and collection locators");
    check(soundcloud_track_request(html, soundcloud_track_path(page), {scratch, 4096}, &out) == EmbedPlayerError::none &&
        out.bitrate == 160000 && utf8_equal({out.url, out.url_bytes}, utf8_literal("https://api-v2.soundcloud.com/media/soundcloud:tracks:293/hash/stream/hls?client_id=0123456789abcdef0123456789abcdef&track_authorization=public-test")),
        "SoundCloud binds the public full AAC request to track identity and its own hydration grants");
    const auto saved = out;
    check(soundcloud_widget_page(utf8_literal("<link rel=\"canonical\" href=\"https://evil.test/forss/flickermood\">"), &out) == EmbedPlayerError::malformed &&
        bytes_equal(&out, &saved, sizeof(out)), "SoundCloud widget cannot redirect resolution to another origin");
    check(soundcloud_track_request(html, soundcloud_track_path(page), {scratch, 4096}, &out, 294) == EmbedPlayerError::malformed &&
        bytes_equal(&out, &saved, sizeof(out)), "SoundCloud widget numeric identity must match the canonical page's real track");
    const auto changed = [&](Utf8View before, Utf8View after, EmbedPlayerError expected) noexcept {
        char body[4096]; ProtocolWriter writer{{reinterpret_cast<u8*>(body), sizeof(body)}, 0}; bool replaced = false;
        for (u32 at = 0; at < html.bytes;) {
            if (!replaced && at + before.bytes <= html.bytes && bytes_equal(html.data + at, before.data, before.bytes)) {
                protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(after.data), after.bytes); at += before.bytes; replaced = true;
            } else protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(html.data + at++), 1);
        }
        check(replaced && soundcloud_track_request({body, writer.used}, soundcloud_track_path(page), {scratch, 4096}, &out) == expected &&
            bytes_equal(&out, &saved, sizeof(out)), "SoundCloud rejected metadata cannot replace accepted source identity");
    };
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":294"), EmbedPlayerError::malformed);
    changed(utf8_literal("/forss/flickermood"), utf8_literal("/forss/another"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"streamable\":true"), utf8_literal("\"streamable\":false"), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"policy\":\"ALLOW\""), utf8_literal("\"policy\":\"BLOCK\""), EmbedPlayerError::unavailable);
    changed(utf8_literal("\"embeddable_by\":\"all\""), utf8_literal("\"embeddable_by\":\"none\""), EmbedPlayerError::not_embeddable);
    changed(utf8_literal("\"snipped\":false"), utf8_literal("\"snipped\":true"), EmbedPlayerError::unsupported);
    changed(utf8_literal("api-v2.soundcloud.com/media"), utf8_literal("evil.test/media"), EmbedPlayerError::malformed);
    changed(utf8_literal("\"id\":293"), utf8_literal("\"id\":293,\"id\":293"), EmbedPlayerError::malformed);
    check(soundcloud_manifest(manifest_response, {scratch, 4096}, &out) == EmbedPlayerError::none &&
        utf8_equal({out.url, out.url_bytes}, manifest) && !soundcloud_location(manifest, false), "SoundCloud separates playlist and media admission");
    check(!soundcloud_location(utf8_literal("https://playback.media-streaming.soundcloud.cloud.evil.test/example/aac_160k/hash/playlist.m3u8?expires=9"), true) &&
        !soundcloud_location(utf8_literal("https://playback.media-streaming.soundcloud.cloud/example/aac_160k/../playlist.m3u8?expires=9"), true),
        "SoundCloud rejects lookalike CDN origins and path traversal");
}
struct Exchange { u32 calls{}; u32 fail{}; bool widget{}; };
struct AudioFeed {
    ProgressiveVideoFetch* source;
    bool ok{true};
    u32 responses{}, http{}, client_error{}, policy_error{};
};
bool service_audio(CdnClient& client, ProgressiveVideoFetch* audio, AudioFeed& feed, u64& serial, bool ahead) noexcept {
    if (cdn_client_status(&client).phase == CdnClientPhase::response_ready)
        cdn_client_consume_response(&client, +[](void* opaque, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
            auto& f = *static_cast<AudioFeed*>(opaque); ++f.responses;
            const auto policy = cdn_client_response_policy(response);
            f.http = policy.status_code; f.client_error = static_cast<u32>(response.error);
            f.policy_error = static_cast<u32>(response.http_error);
            f.ok = progressive_video_fetch_accept(f.source, response.request_id, response.transport_ok(), policy, body) != ProgressiveVideoAccept::failed;
            return true;
        }, &feed);
    if (!feed.ok) return false;
    const auto next = progressive_video_fetch_next(audio, ahead, clock_unix_milliseconds() / 1000);
    if (next.step == ProgressiveVideoStep::submit) {
        const auto submitted = cdn_client_submit(&client, {{audio->target, audio->target_bytes}, {}, ++serial,
            kCdnClientDefaultTimeoutMs, audio->host, audio->image_type, next.range});
        if (!submitted.ok()) return false;
        progressive_video_fetch_submitted(audio, serial, next.range);
    } else if (next.step == ProgressiveVideoStep::cancel) {
        if (!cdn_client_cancel(&client, audio->inflight_id)) return false;
        progressive_video_fetch_cancelling(audio);
    } else if (next.step == ProgressiveVideoStep::failed) return false;
    return true;
}
bool continuous_probe(EmbedResolver* resolver, ProgressiveVideoFetch* audio) noexcept {
    const auto resolved = soundcloud_resolve(resolver, page, audio);
    CdnClient client{};
    const bool started = resolved.ok() && cdn_client_start(&client, 1024 * 1024).ok();
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool valid = started && SUCCEEDED(apartment);
    AudioFeed feed{audio}; AacDecoder decoder{};
    u8 pcm[kAacDecoderMaximumOutputBytes];
    u64 serial = 0, frames = 0, visited = 0;
    u32 packets = 0, peak = 0, preceding_frames = 0;
    bool draining = false, ended = false;
    Mp4DemuxError packet_error{};
    AacDecodeError decoder_error{};
    const u64 deadline = GetTickCount64() + 10000;
    while (valid && !ended && GetTickCount64() < deadline) {
        valid = service_audio(client, audio, feed, serial, decoder.initialized);
        if (!valid) break;
        bool wait = false;
        if (audio->state == ProgressiveVideoFetchState::ready && !decoder.initialized) {
            const auto& track = audio->fragment.track;
            valid = audio->segmented->store.count < 64 &&
                aac_decoder_initialize(&decoder, track.configuration, track.sample_rate, track.channels).ok();
        }
        if (valid && decoder.initialized) {
            const auto output = aac_decoder_read(&decoder, {pcm, sizeof(pcm)});
            decoder_error = output.error;
            if (output.ok()) {
                Mp4AudioFrameTimeBounds bounds{};
                const bool mapped = mp4_audio_frame_time_bounds_100ns(audio->audio.timeline, frames, &bounds);
                bool matches = mapped && account_video_audio_timestamp_within_bounds(output.presentation_time_100ns, bounds);
                if (mapped && !matches && preceding_frames) {
                    i64 preceding{};
                    matches = mp4_audio_frame_time_100ns(audio->audio.timeline, frames - preceding_frames, &preceding) &&
                        account_video_audio_timestamp_matches(output.presentation_time_100ns, bounds.floor_100ns,
                            preceding, preceding_frames, audio->audio.timeline.sample_rate);
                }
                valid = output.timestamp_valid && output.frames && matches;
                frames += output.frames; preceding_frames = output.frames;
            } else if (output.error == AacDecodeError::end_of_stream) ended = draining;
            else if (output.error != AacDecodeError::need_input || draining) valid = false;
            else {
                Mp4FragmentAudioPacket packet{};
                packet_error = mp4_fragment_audio_peek(&audio->audio, &packet);
                if (packet_error == Mp4DemuxError::none) {
                    if (audio->fragment.segment.index >= 64) { valid = false; break; }
                    visited |= u64{1} << audio->fragment.segment.index;
                    valid = aac_decoder_submit(&decoder, packet.bytes, packet.presentation_time_100ns, packet.duration_100ns).ok() &&
                        mp4_fragment_audio_commit(&audio->audio);
                    if (valid) ++packets;
                } else if (packet_error == Mp4DemuxError::end_of_stream) {
                    valid = aac_decoder_drain(&decoder).ok(); draining = true;
                } else if (packet_error == Mp4DemuxError::data_pending) wait = true;
                else valid = false;
            }
        } else wait = true;
        if (audio->segmented) {
            const auto& store = audio->segmented->store;
            peak = maximum(peak, store.initialization.committed + store.slots[0].storage.committed + store.slots[1].storage.committed);
        }
        if (wait) cdn_client_wait_response(&client, 10);
    }
    const u32 count = audio->segmented ? audio->segmented->store.count : 0;
    test_write_metric("SoundCloud continuous resolve error: ", static_cast<u32>(resolved.error));
    test_write_metric("SoundCloud continuous demux result: ", static_cast<u32>(packet_error));
    test_write_metric("SoundCloud continuous decoder result: ", static_cast<u32>(decoder_error));
    test_write_metric("SoundCloud continuous HTTP: ", feed.http);
    test_write_metric("SoundCloud continuous media responses: ", feed.responses);
    test_write_metric("SoundCloud continuous segments: ", count);
    test_write_metric("SoundCloud continuous packets: ", packets);
    test_write_metric("SoundCloud continuous PCM frames: ", frames);
    test_write_metric("SoundCloud continuous encoded peak bytes: ", peak);
    test_write_metric("SoundCloud continuous decoder reached EOF: ", ended);
    valid = valid && ended && count && count < 64 && visited == (u64{1} << count) - 1 &&
        packets == audio->audio.packet_count && frames == audio->audio.timeline.source_frames && peak <= 3 * 1024 * 1024;
    aac_decoder_release(&decoder);
    if (started && cdn_client_stop(&client, 1000).phase != CdnClientPhase::stopped) ExitProcess(1);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return valid;
}
bool worker_probe(Utf8View source, bool collection = false, u32 expected_count = 11, bool titles = false, u16 initial_track = 1) noexcept {
    auto* worker = static_cast<AccountMediaWorker*>(VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool valid = worker && account_media_worker_initialize(worker).ok();
    if (!worker) return false;
    const bool youtube = youtube_collection_locator(source).list.bytes != 0;
    const bool apple = apple_music_collection_locator(source).id.bytes != 0;
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    if (youtube) {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
        valid = valid && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device, &level, &context));
        ID3D11Multithread* guard{};
        if (context && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&guard)))) { guard->SetMultithreadProtected(TRUE); guard->Release(); }
    }
    const auto key = media_asset_embed(1, 1, 0, MediaEmbedResource::video, youtube ? MediaAttachmentFormat::mp4 : MediaAttachmentFormat::m4a, youtube);
    AccountMediaAttachmentRequest request{source, key, 1, 1, 0, 0, youtube ? CdnImageType::mp4 : CdnImageType::m4a, {1, {0, 0}}, device};
    if (initial_track > 1) request.options.collection_track = initial_track;
    AccountMediaFrame frame{}; AccountMediaRequestResult requested{};
    const u64 deadline = GetTickCount64() + (collection ? 12000 : 6500);
    const auto acquire = [&]() noexcept {
        bool ready = false;
        while (valid && !ready && GetTickCount64() < deadline) {
            requested = account_media_worker_request_attachment(worker, request, clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000);
            ready = account_media_worker_take_frame(worker, &frame);
            valid = requested.state != AccountMediaRequestState::failed && requested.state != AccountMediaRequestState::unsupported &&
                requested.state != AccountMediaRequestState::invalid;
            if (!ready) WaitForSingleObject(worker->output_event, 50);
        }
        return valid && ready && (youtube ? frame.kind == AccountMediaFrameKind::video_nv12_gpu || frame.kind == AccountMediaFrameKind::audio_source : frame.kind == AccountMediaFrameKind::audio_source && !frame.gpu_video) && (youtube || frame.content_identity);
    };
    valid = acquire();
    AccountMediaCollection* owner{};
    AccountVideoIdentity prior{};
    u32 completed = 0, collection_count = 0, named_tracks = 0;
    const u32 cycles = initial_track > 1 ? 1u : collection ? 3u : 2u;
    for (u32 replay = 0; valid && replay < cycles; ++replay) {
        if (collection && replay) {
            request.options.collection_track = replay == 1 ? 2 : 1;
            const AccountVideoSelection wrong{key, {1, 1, 0, 1 + replay, 1, 0, 0}, request.options.collection_track};
            const u64 previous_content = frame.content_identity;
            valid = account_media_worker_video_select(worker, wrong).error == AccountVideoError::busy && acquire() && (youtube || frame.content_identity != previous_content);
            if (!valid) break;
        }
        const AccountVideoSelection selection{key, {1, 1, frame.content_identity, 1 + replay, 1, frame.cache_generation, 0}, request.options.collection_track};
        valid = account_media_worker_video_select(worker, selection).ok();
        auto snapshot = account_media_worker_video_status(worker);
        const auto identity = snapshot.playback.identity;
        if (collection) {
            if (!replay) collection_count = snapshot.collection_count;
            valid = valid && collection_count > 1 && (!expected_count || collection_count == expected_count) &&
                snapshot.collection_count == collection_count && snapshot.collection_track == (replay == 1 ? 2 : replay ? 1 : initial_track) && (initial_track == 1 || collection_count > initial_track);
            u32 owners = 0;
            for (auto& lane : worker->lanes) if (lane.media.collection) {
                ++owners;
                if (!owner) owner = lane.media.collection;
                valid = valid && owner == lane.media.collection;
            }
            auto blocked = request; blocked.options.collection_track = replay == 1 ? 1 : 2;
            const auto queued_before = ReadAcquire64(&worker->counters.queued);
            valid = valid && owners == 1 && account_media_worker_request_attachment(worker, blocked,
                clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000).state == AccountMediaRequestState::busy &&
                ReadAcquire64(&worker->counters.queued) == queued_before;
            if (replay) valid = valid && !account_media_worker_video_control(worker, {prior, 99, 0, 1, AccountVideoCommand::play, true}).ok();
            prior = identity;
        }
        const auto command = [&](u64 serial, AccountVideoCommand kind, i64 seek = 0) noexcept {
            return account_media_worker_video_control(worker, {identity, serial, seek, 1, kind, true}).ok();
        };
        valid = valid && command(1, AccountVideoCommand::mute) && command(2, AccountVideoCommand::play);
        bool sought = false;
        u32 named = titles ? 0 : 2;
        const auto metadata_before = ReadAcquire64(&worker->counters.queued);
        while (valid && GetTickCount64() < deadline) {
            snapshot = account_media_worker_video_status(worker);
            AccountVideoFrame picture{};
            valid = snapshot.selected && snapshot.playback.error == AccountVideoError::none;
            if (account_media_worker_video_take(worker, &picture)) valid = valid && youtube && account_media_worker_video_acknowledge(worker, picture);
            if (valid && named < 2) {
                const u16 index = named ? (replay == 1 ? 1 : 2) : (replay == 1 ? 2 : 1);
                MediaTrackTitle title{};
                const AccountMediaCollectionTitleRequest lookup{key, 1, index, true};
                const auto result = account_media_worker_collection_title(worker, lookup, &title);
                valid = result.state != AccountMediaRequestState::failed && result.state != AccountMediaRequestState::invalid &&
                    result.state != AccountMediaRequestState::retry_wait;
                if (result.state == AccountMediaRequestState::ready) {
                    const auto queued = ReadAcquire64(&worker->counters.queued);
                    valid = valid && title.bytes && account_media_worker_collection_title(worker, lookup, &title).state == AccountMediaRequestState::ready &&
                        ReadAcquire64(&worker->counters.queued) == queued;
                    ++named; ++named_tracks;
                }
            }
            if (valid && named == 2 && !sought && snapshot.playback.position_100ns >= 1000000) {
                valid = command(3, AccountVideoCommand::seek, snapshot.playback.duration_100ns - 5000000);
                sought = true;
            }
            if (sought && snapshot.playback.observed_command >= 3 && snapshot.playback.ended) break;
            WaitForSingleObject(worker->output_event, 10);
        }
        if (titles && named == 2) {
            const auto queued = ReadAcquire64(&worker->counters.queued);
            u32 available=0;
            for (u16 index=1;index<=collection_count;++index) {
                MediaTrackTitle name{};
                const AccountMediaCollectionTitleRequest query{key,1,index,false};
                if (account_media_worker_collection_title(worker,query,&name).state == AccountMediaRequestState::ready && name.bytes) ++available;
            }
            for (const auto& lane:worker->lanes) valid=valid && !lane.collection_titles && !lane.collection_title_bytes && !lane.collection_title_committed;
            test_write_metric("Collection retained titles: ",available);
            valid = valid && available == collection_count && ReadAcquire64(&worker->counters.queued) == queued;
        }
        if (youtube) { test_write_metric("YTM seek ended: ", snapshot.playback.ended); test_write_metric("YTM playback error: ", static_cast<u32>(snapshot.playback.error)); test_write_metric("YTM metadata operations: ", ReadAcquire64(&worker->counters.queued) - metadata_before); }
        valid = valid && sought && snapshot.playback.ended && command(4, AccountVideoCommand::stop) &&
            (!titles || ReadAcquire64(&worker->counters.queued) == metadata_before + (replay || youtube || apple ? 0 : 1));
        while (valid && GetTickCount64() < deadline && account_media_worker_video_status(worker).selected)
            WaitForSingleObject(worker->output_event, 10);
        valid = valid && !account_media_worker_video_status(worker).selected;
        if (valid) ++completed;
    }
    test_write_metric("Collection worker request state: ", static_cast<u32>(requested.state));
    test_write_metric("Collection worker request error: ", static_cast<u32>(requested.error));
    test_write_metric("Collection worker audio frame: ", frame.kind == AccountMediaFrameKind::audio_source);
    test_write_metric("Collection worker completed play/seek/stop cycles: ", completed);
    if (collection) test_write_metric("Collection worker retained queue count: ", collection_count);
    if (titles) test_write_metric("Collection worker cached title reads: ", named_tracks);
    if (!account_media_worker_stop(worker, 1000)) ExitProcess(1);
    VirtualFree(worker, 0, MEM_RELEASE);
    if (context) context->Release(); if (device) device->Release();
    return valid && completed == cycles;
}
bool live_probe(EmbedResolver* resolver, ProgressiveVideoFetch* audio) noexcept {
    char flag[2]{};
    if (GetEnvironmentVariableA("MAICORD_SOUNDCLOUD_STATION_PROBE",flag,2)==1 && flag[0]=='1') return worker_probe(utf8_literal("https://soundcloud.com/discover/sets/track-stations:293"),true,0,true);
    if (GetEnvironmentVariableA("MAICORD_APPLE_ALBUM_PROBE",flag,2)==1) {
        if (flag[0]=='1') return worker_probe(utf8_literal("https://music.apple.com/us/album/imagine/255144028"),true,34,true);
        if (flag[0]=='2') return worker_probe(utf8_literal("https://music.apple.com/us/playlist/todays-hits/pl.f4d106fed2bd41149aaacabb233eb5eb"),true,50,true);
    }
    if (GetEnvironmentVariableA("MAICORD_SOUNDCLOUD_LIVE_PROBE", flag, sizeof(flag)) != 1 || flag[0] < '1' || flag[0] > '9') return true;
    if (flag[0] == '9') return worker_probe(utf8_literal("https://music.youtube.com/watch?v=CZQdi1sxd30&list=RDAMVMCZQdi1sxd30"), true, 0, true, 50);
    if (flag[0] == '8') return worker_probe(utf8_literal("https://music.youtube.com/watch?v=CZQdi1sxd30&list=RDAMVMCZQdi1sxd30"), true, 0, true);
    if (flag[0] == '7') {
        MediaTrackTitle title{};
        const auto result = soundcloud_title_resolve(resolver, 285103732, &title);
        test_write_metric("SoundCloud title HTTP: ", result.status);
        test_write_metric("SoundCloud title result: ", static_cast<u32>(result.error));
        test_write_metric("SoundCloud title bytes: ", title.bytes);
        return result.ok() && title.bytes && !resolver->response.committed && !resolver->structurals.committed;
    }
    if (flag[0] == '3') return continuous_probe(resolver, audio);
    if (flag[0] == '5') return worker_probe(utf8_literal("https://soundcloud.com/forss/sets/soulhack"), true);
    if (flag[0] == '6') return worker_probe(utf8_literal("https://soundcloud.com/discover/sets/artist-stations:59880230"), true, 0, true);
    auto source = flag[0] == '2' ? widget : page;
    char selected_url[128];
    if (flag[0] == '4') {
        u64 first[32]{}, second[32]{}; SoundcloudCollection direct{}, nested{};
        const auto a = soundcloud_collection_resolve(resolver, utf8_literal("https://soundcloud.com/forss/sets/soulhack"), {first, 32}, &direct);
        const auto b = soundcloud_collection_resolve(resolver,
            utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/playlists/18&start_track=10"), {second, 32}, &nested);
        test_write_metric("SoundCloud direct collection HTTP: ", a.status);
        test_write_metric("SoundCloud widget collection HTTP: ", b.status);
        test_write_metric("SoundCloud collection parser result: ", static_cast<u32>(b.error));
        test_write_metric("SoundCloud collection tracks: ", nested.count);
        if (!a.ok() || !b.ok() || direct.id != 18 || nested.id != 18 || direct.count != 11 || nested.count != 11 ||
            nested.selected != 10 || !bytes_equal(first, second, 11 * sizeof(u64)) || first[0] != 290) return false;
        ProtocolWriter writer{{reinterpret_cast<u8*>(selected_url), sizeof(selected_url)}, 0};
        if (!protocol_writer_append_literal(&writer, "https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/") ||
            !protocol_writer_append_u64(&writer, second[nested.selected])) return false;
        source = {selected_url, writer.used};
    }
    const auto resolved = soundcloud_resolve(resolver, source, audio);
    bool valid = resolved.ok();
    CdnClient client{};
    const bool started = valid && cdn_client_start(&client, 1024 * 1024).ok();
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    valid = valid && started && SUCCEEDED(apartment) && event;
    AudioFeed feed{audio};
    AccountVideoSession session{}; const AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    u64 serial = 0, first_pcm = 0, seek_epoch = 0;
    const u64 deadline = GetTickCount64() + 7000;
    while (valid && GetTickCount64() < deadline && !session.status.ended) {
        valid = service_audio(client, audio, feed, serial, session.initialized);
        if (valid && !session.initialized && audio->state == ProgressiveVideoFetchState::ready) {
            valid = account_video_session_initialize_fragment_audio(&session, &audio->audio, identity, event).ok() &&
                !session.video && !session.workspace && session.status.duration_100ns > 10000000 &&
                account_video_session_control(&session, identity, 1, AccountVideoCommand::mute, 0, 1, true).ok() &&
                account_video_session_control(&session, identity, 2, AccountVideoCommand::play).ok();
        }
        AccountVideoService work{};
        if (valid && session.initialized) {
            work = account_video_session_service(&session);
            valid = session.status.error == AccountVideoError::none && !work.frame_ready;
            const auto progress = account_video_session_audio_progress(&session);
            if (valid && !seek_epoch && progress.consumed_frames >= 4096) {
                first_pcm = progress.consumed_frames;
                valid = account_video_session_control(&session, identity, 3, AccountVideoCommand::seek,
                    session.status.duration_100ns - 5000000).ok();
                seek_epoch = session.playback_epoch;
            }
        }
        if (!work.needs_service) {
            const HANDLE events[]{event, cdn_client_notification_handle(&client)};
            WaitForMultipleObjects(2, events, FALSE, minimum(10u, work.next_service_delay_ms));
        }
    }
    const auto progress = account_video_session_audio_progress(&session);
    test_write_metric("SoundCloud resolve error: ", static_cast<u32>(resolved.error));
    test_write_metric("SoundCloud metadata HTTP: ", resolved.status);
    test_write_metric("SoundCloud media HTTP: ", feed.http);
    test_write_metric("SoundCloud media client error: ", feed.client_error);
    test_write_metric("SoundCloud media policy error: ", feed.policy_error);
    test_write_metric("SoundCloud source state: ", static_cast<u32>(audio->state));
    test_write_metric("SoundCloud audio error: ", static_cast<u32>(session.status.error));
    test_write_metric("SoundCloud initial consumed PCM: ", first_pcm);
    test_write_metric("SoundCloud sought consumed PCM: ", progress.consumed_frames);
    test_write_metric("SoundCloud media responses: ", feed.responses);
    valid = valid && first_pcm >= 4096 && seek_epoch > 1 && session.playback_epoch == seek_epoch && session.status.ended &&
        progress.consumed_frames == audio->audio.timeline.sample_rate / 2 && !session.video;
    if (!account_video_session_release(&session, 1000).ok()) ExitProcess(1);
    if (started && cdn_client_stop(&client, 1000).phase != CdnClientPhase::stopped) ExitProcess(1);
    if (event) CloseHandle(event);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return valid && worker_probe(source);
}
UpdateTransportError exchange(void* opaque, const UpdateHttpRequest& request, const HttpsExchangeOptions&,
    UpdateHttpResponse* response_out) noexcept {
    auto& fixture = *static_cast<Exchange*>(opaque); ++fixture.calls;
    const u32 step = fixture.calls - (fixture.widget ? 1u : 0u);
    check(step <= 3, "SoundCloud resolver has bounded request count");
    const Utf8View hosts[]{utf8_literal("soundcloud.com"), utf8_literal("api-v2.soundcloud.com"),
        utf8_literal("playback.media-streaming.soundcloud.cloud")};
    const Utf8View bodies[]{html, manifest_response, playlist};
    if (step > 3) return UpdateTransportError::invalid_input;
    check(utf8_equal(request.host_name, step ? hosts[step - 1] : utf8_literal("w.soundcloud.com")), "SoundCloud requests only exact metadata and media origins");
    if (fixture.calls == fixture.fail) { response_out->status = 503; return UpdateTransportError::none; }
    const auto body = step ? bodies[step - 1] : canonical;
    if (!request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})) return UpdateTransportError::sink;
    response_out->status = 200; response_out->body_bytes = body.bytes;
    return UpdateTransportError::none;
}
}
bool soundcloud_player_tests() noexcept {
    protocol_tests();
    auto* resolver = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* audio = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(resolver && audio, "SoundCloud resolver test owners allocate");
    check((soundcloud_collection_tests(resolver) && youtube_collection_tests(resolver) && apple_music_album_tests(resolver) && apple_music_playlist_tests(resolver)), "SoundCloud collection protocol and bounded native resolution");
    if (resolver && audio) for (u32 kind = 0; kind < 2; ++kind) for (u32 fail = 0; fail <= 3 + kind; ++fail) {
        Exchange fixture{0, fail, kind != 0};
        const auto result = soundcloud_resolve(resolver, kind ? widget : page, audio, {&fixture, exchange});
        check(fail ? !result.ok() && fixture.calls == fail && audio->state == ProgressiveVideoFetchState::empty
                   : result.ok() && fixture.calls == 3 + kind && audio->kind == ProgressiveVideoKind::fragmented_audio &&
                     audio->host == CdnHost::soundcloud_audio && audio->segmented && audio->segmented->playlist.ended,
              "SoundCloud resolution owns one finite audio source and rolls back failed exchanges");
        check(!resolver->response.committed && !resolver->structurals.committed && !resolver->response_bytes,
              "SoundCloud resolver releases sensitive metadata after every result");
        progressive_video_fetch_release(audio);
    }
    if (resolver && audio) {
        Exchange fixture{0, 0, true};
        const auto result = soundcloud_resolve(resolver,
            utf8_literal("https://w.soundcloud.com/player/?url=https%3A//api.soundcloud.com/tracks/294"), audio, {&fixture, exchange});
        check(result.error == EmbedPlayerError::malformed && fixture.calls == 2 && audio->state == ProgressiveVideoFetchState::empty,
              "mismatched canonical track stops before requesting any stream grant or media");
    }
    if (resolver && audio) check(live_probe(resolver, audio), "SoundCloud native HTTPS, segmented AAC, seek and EOF without GPU");
    if (resolver) { embed_resolver_release(resolver); VirtualFree(resolver, 0, MEM_RELEASE); }
    if (audio) VirtualFree(audio, 0, MEM_RELEASE);
    return !failures;
}
} // namespace maicord
