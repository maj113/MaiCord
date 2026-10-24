#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
struct CaptureTrack {
    ConstSlice<u8> init{}, segments[64]{};
    u32 count{}, requests{}, peak_bytes{}, visits[64]{};
};
// Feed the production request driver, including read-ahead, from a bounded
// public capture. The capture contains bytes only, never URLs or credentials.
bool service(ProgressiveVideoFetch* f, CaptureTrack& input, bool ahead) noexcept {
    const auto next = progressive_video_fetch_next(f, ahead, 1);
    if (next.step == ProgressiveVideoStep::idle || next.step == ProgressiveVideoStep::ready) return true;
    if (next.step != ProgressiveVideoStep::submit || next.range.first) return false;
    const auto& s = *f->segmented;
    if (!s.initialization && s.number >= input.count) return false;
    const auto bytes = s.initialization ? input.init : input.segments[s.number];
    if (next.range.bytes < bytes.count) return false;
    if (!s.initialization) ++input.visits[s.number];
    progressive_video_fetch_submitted(f, ++input.requests, next.range);
    CdnResponseResult reply{}; reply.disposition = CdnResponseDisposition::accept_partial;
    reply.status_code = 206; reply.body_bytes = bytes.count; reply.entity_bytes = bytes.count;
    if (progressive_video_fetch_accept(f, f->inflight_id, true, reply, bytes) != ProgressiveVideoAccept::progress) return false;
    const u32 committed = s.store.initialization.committed + s.store.slots[0].storage.committed + s.store.slots[1].storage.committed;
    input.peak_bytes = maximum(input.peak_bytes, committed);
    return true;
}
bool decode_video(ProgressiveVideoFetch* f, CaptureTrack& input, Mp4Decoder& decoder, u32 wanted, bool eof) noexcept {
    u32 frames = 0; bool ended = false, valid = true; i64 previous = -1000000, end = 0;
    for (u32 calls = 0; valid && calls < 16384 && !ended && (eof || frames < wanted); ++calls) {
        valid = service(f, input, true);
        if (!valid) break;
        const auto frame = h264_gpu_decoder_read(&decoder.native, false, 4);
        if (frame.ok()) {
            valid = frame.gpu && frame.width == 854 && frame.height == 480 && frame.presentation_time_us > previous &&
                h264_gpu_decoder_acknowledge_frame(&decoder.native);
            previous = frame.presentation_time_us; end = previous + static_cast<i64>(frame.duration_us); ++frames;
        } else if (frame.error == Mp4DecodeError::end_of_stream) ended = true;
        else if (frame.error == Mp4DecodeError::data_pending || frame.error == Mp4DecodeError::needs_service) continue;
        else valid = h264_gpu_decoder_busy(decoder.native) && h264_gpu_decoder_wait(&decoder.native, 10);
    }
    test_write_metric("Steam full GPU frames: ", frames);
    test_write_metric("Steam full GPU demux error: ", static_cast<u32>(decoder.native.demux_error));
    test_write_metric("Steam full GPU parse error: ", static_cast<u32>(decoder.native.parse_error));
    test_write_metric("Steam full GPU endpoint us: ", end > 0 ? static_cast<u64>(end) : 0);
    return valid && frames == wanted && (!eof || (ended && end >= 42917874 && end <= 42917875));
}
bool decode_audio(ProgressiveVideoFetch* f, CaptureTrack& input, AacDecoder& decoder, u32 wanted, u32 first = 0) noexcept {
    u8 pcm[kAacDecoderMaximumOutputBytes];
    u32 submitted = 0, frames = 0, preceding_frames = 0; bool valid = true, draining = false, ended = false;
    for (u32 calls = 0; valid && calls < 16384 && !ended; ++calls) {
        valid = service(f, input, true);
        if (!valid) break;
        const auto output = aac_decoder_read(&decoder, {pcm, sizeof(pcm)});
        if (output.ok()) {
            Mp4AudioFrameTimeBounds bounds{};
            const u64 source_frame = u64{first} * 1024 + frames;
            const bool mapped = mp4_audio_frame_time_bounds_100ns(f->audio.timeline, source_frame, &bounds);
            bool matches = mapped && account_video_audio_timestamp_within_bounds(output.presentation_time_100ns, bounds);
            if (mapped && !matches && preceding_frames) {
                i64 preceding{};
                matches = mp4_audio_frame_time_100ns(f->audio.timeline, source_frame - preceding_frames, &preceding) &&
                    account_video_audio_timestamp_matches(output.presentation_time_100ns, bounds.floor_100ns,
                        preceding, preceding_frames, f->audio.timeline.sample_rate);
            }
            valid = output.timestamp_valid && mapped && matches;
            if (!valid) {
                test_write_metric("Steam full AAC timestamp: ", static_cast<u64>(output.presentation_time_100ns));
                test_write_metric("Steam full AAC timestamp floor: ", static_cast<u64>(bounds.floor_100ns));
                test_write_metric("Steam full AAC timestamp ceil: ", static_cast<u64>(bounds.ceil_100ns));
            }
            frames += output.frames; preceding_frames = output.frames;
        } else if (output.error == AacDecodeError::end_of_stream) ended = draining;
        else if (output.error != AacDecodeError::need_input || draining) valid = false;
        else if (submitted == wanted) { valid = aac_decoder_drain(&decoder).ok(); draining = true; }
        else {
            Mp4FragmentAudioPacket packet{};
            const auto result = mp4_fragment_audio_peek(&f->audio, &packet);
            if (result == Mp4DemuxError::data_pending) continue;
            valid = result == Mp4DemuxError::none &&
                aac_decoder_submit(&decoder, packet.bytes, packet.presentation_time_100ns, packet.duration_100ns).ok() &&
                mp4_fragment_audio_commit(&f->audio);
            if (valid) ++submitted;
        }
    }
    test_write_metric("Steam full AAC packets: ", submitted);
    test_write_metric("Steam full AAC PCM frames: ", frames);
    if (!valid || !ended) test_write_metric("Steam full AAC drained: ", ended ? 1 : 0);
    return valid && ended && submitted == wanted && frames == wanted * 1024;
}
bool run_capture(ConstSlice<u8> bytes) noexcept {
    if (bytes.count < 16 || !bytes_equal(bytes.data, "MCDASH02", 8) || byte_load_le32(bytes.data + 8) != 2) return false;
    const u32 manifest_bytes = byte_load_le32(bytes.data + 12);
    if (manifest_bytes > 65536 || manifest_bytes > bytes.count - 16) return false;
    DashManifest manifest{};
    if (dash_manifest_parse({reinterpret_cast<const char*>(bytes.data + 16), manifest_bytes}, 480, &manifest) != DashManifestError::none) return false;
    CaptureTrack tracks[2]{}; u32 at = 16 + manifest_bytes;
    for (u32 kind = 0; kind < 2; ++kind) {
        if (bytes.count - at < 24) return false;
        const auto* p = bytes.data + at; const auto& track = kind ? manifest.audio : manifest.video;
        const u32 init = byte_load_le32(p + 4), count = byte_load_le32(p + 8); at += 24;
        if (byte_load_le32(p) != kind || !init || init > 1048576 || init > bytes.count - at || !count || count > 64 ||
            count != track.segment_count || byte_load_le32(p + 12) != track.timescale || byte_load_le64(p + 16) != track.segment_duration) return false;
        tracks[kind].init = {bytes.data + at, init}; tracks[kind].count = count; at += init;
        for (u32 i = 0; i < count; ++i) {
            if (bytes.count - at < 4) return false;
            const u32 size = byte_load_le32(bytes.data + at); at += 4;
            if (!size || size > (kind ? 1048576u : 8388608u) || size > bytes.count - at) return false;
            tracks[kind].segments[i] = {bytes.data + at, size}; at += size;
        }
    }
    if (at != bytes.count) return false;
    auto* fetch = static_cast<ProgressiveVideoFetch*>(VirtualAlloc(nullptr, 2 * sizeof(ProgressiveVideoFetch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fetch) return false;
    constexpr auto url = utf8_literal("https://video.akamai.steamstatic.com/store_trailers/570/116737/0123456789abcdef0123456789abcdef/1749859757/dash_h264.mpd");
    bool valid = true;
    for (u32 kind = 0; valid && kind < 2; ++kind) {
        valid = progressive_video_fetch_begin_segments(fetch + kind, url, kind ? manifest.audio : manifest.video, kind != 0);
        for (u32 i = 0; valid && i < 5 && fetch[kind].state != ProgressiveVideoFetchState::ready; ++i)
            valid = service(fetch + kind, tracks[kind], false);
        valid = valid && fetch[kind].state == ProgressiveVideoFetchState::ready && tracks[kind].requests == 3;
    }
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
    valid = valid && SUCCEEDED(apartment) && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device, &level, &context));
    Mp4Decoder video{}; AacDecoder audio{};
    valid = valid && mp4_decoder_initialize_fragmented(&video, &fetch[0].fragment, device);
    if (valid) valid = decode_video(fetch, tracks[0], video, 1029, true);
    const auto& t = fetch[1].fragment.track;
    valid = valid && aac_decoder_initialize(&audio, t.configuration, t.sample_rate, t.channels).ok() &&
        fetch[1].audio.packet_count == 2013 && decode_audio(fetch + 1, tracks[1], audio, 2013);
    for (u32 kind = 0; valid && kind < 2; ++kind) {
        const auto& track = tracks[kind];
        // First and last establish the clock; last is evicted by the next
        // segment, then fetched once again at EOF. Every other file is once.
        valid = track.requests == track.count + 2 && track.peak_bytes == (kind ? 5u : 20u) * 65536;
        for (u32 i = 0; valid && i < track.count; ++i) valid = track.visits[i] == (i + 1 == track.count ? 2u : 1u);
        test_write_metric("Steam full source requests: ", track.requests);
        test_write_metric("Steam full peak source bytes: ", track.peak_bytes);
    }
    if (valid) {
        const auto* video_owner = video.native.state;
        const auto* audio_owner = audio.runtime;
        for (u32 kind = 0; valid && kind < 2; ++kind) {
            progressive_video_fetch_retire(fetch + kind);
            const auto& store = fetch[kind].segmented->store;
            valid = !store.slots[0].storage.committed && !store.slots[1].storage.committed &&
                progressive_video_fetch_restore(fetch + kind) && service(fetch + kind, tracks[kind], false);
        }
        Mp4SeekPoint point{};
        auto sought = h264_gpu_decoder_seek(&video.native, 0, &point);
        if (sought == Mp4DemuxError::data_pending && service(fetch, tracks[0], false)) sought = h264_gpu_decoder_seek(&video.native, 0, &point);
        valid = valid && sought == Mp4DemuxError::none && decode_video(fetch, tracks[0], video, 8, false) &&
            aac_decoder_flush(&audio).ok() && decode_audio(fetch + 1, tracks[1], audio, 12) &&
            video.native.state == video_owner && audio.runtime == audio_owner &&
            tracks[0].requests == tracks[0].count + 4 && tracks[1].requests == tracks[1].count + 4;
        constexpr u32 seconds[]{40, 3, 21, 1, 39, 0};
        u32 seeks = 0;
        for (u32 second : seconds) {
            if (!valid) break;
            const i64 target = i64{second} * 1000000;
            for (u32 attempt = 0; attempt < 32; ++attempt) {
                sought = h264_gpu_decoder_seek(&video.native, target, &point);
                if (sought != Mp4DemuxError::data_pending || !service(fetch, tracks[0], false)) break;
            }
            valid = sought == Mp4DemuxError::none && point.presentation_time_us <= target &&
                point.presentation_time_us >= target - 3100000 && decode_video(fetch, tracks[0], video, 8, false);
            const u32 at_packet = second * 48000 / 1024;
            const u32 preroll = aac_decoder_seek_preroll_packets(1024);
            const u32 begin = at_packet > preroll ? at_packet - preroll : 0;
            for (u32 attempt = 0; valid && attempt < 32; ++attempt) {
                sought = mp4_fragment_audio_seek(&fetch[1].audio, begin);
                if (sought != Mp4DemuxError::data_pending || !service(fetch + 1, tracks[1], false)) break;
            }
            valid = valid && sought == Mp4DemuxError::none && aac_decoder_flush(&audio).ok() &&
                decode_audio(fetch + 1, tracks[1], audio, 12, begin) &&
                video.native.state == video_owner && audio.runtime == audio_owner;
            if (valid) ++seeks;
        }
        test_write_metric("Steam full reused decoder seeks: ", seeks);
        valid = valid && seeks == 6;
    }
    aac_decoder_release(&audio); mp4_decoder_release(&video);
    for (u32 kind = 0; kind < 2; ++kind) progressive_video_fetch_release(fetch + kind);
    VirtualFree(fetch, 0, MEM_RELEASE);
    if (context) context->Release(); if (device) device->Release(); if (SUCCEEDED(apartment)) CoUninitialize();
    return valid;
}
}
bool steam_full_playback_probe() noexcept {
    char path[1024]{}; wchar_t wide[1024]{};
    const u32 length = GetEnvironmentVariableA("MAICORD_DASH_FULL_PROBE", path, sizeof(path));
    if (!length) return true;
    if (length >= sizeof(path)) return false;
    for (u32 i = 0; i < length; ++i) { if (static_cast<u8>(path[i]) >= 128) return false; wide[i] = path[i]; }
    const HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Win32LargeInteger size{};
    const bool sized = GetFileSizeEx(file, size.native()) && size.QuadPart >= 16 && size.QuadPart <= 32 * 1024 * 1024;
    auto* data = sized ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<usize>(size.QuadPart), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
    DWORD read{};
    const bool loaded = data && ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
    CloseHandle(file);
    const bool valid = loaded && run_capture({data, read});
    if (data) VirtualFree(data, 0, MEM_RELEASE);
    return valid;
}
} // namespace maicord
