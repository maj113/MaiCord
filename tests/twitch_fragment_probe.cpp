#include "maicord/media/mp4_fragment.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/app/account_video_session.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "test_output.hpp"

namespace maicord {
bool twitch_live_session(ConstSlice<u8>, bool tail, bool startup_only) noexcept;
namespace {
bool native_file(ConstSlice<u8> bytes, bool audio, bool live = false) noexcept {
    if (bytes.count < 16) return false;
    const bool pair = bytes_equal(bytes.data, "MCHLS002", 8);
    if (pair ? !live || bytes.count < 20 : !bytes_equal(bytes.data, "MCHLS001", 8)) return false;
    const u32 init = byte_load_le32(bytes.data + 8), media = byte_load_le32(bytes.data + 12);
    const u32 following_bytes = pair ? byte_load_le32(bytes.data + 16) : 0, header = pair ? 20 : 16;
    if (!init || init > 1048576 || u64{init} + media + following_bytes != bytes.count - header) return false;
    const ConstSlice<u8> following{bytes.data + header + init + media, following_bytes};
    Mp4SegmentStore store{}; Mp4FragmentSource source{};
    bool valid = mp4_segment_store_initialize(&store, {bytes.data + header, init}, audio, 1, pair ? 2 : 10, 1, pair) == Mp4DemuxError::none &&
        mp4_segment_store_request(&store, 0) == Mp4DemuxError::data_pending &&
        mp4_segment_store_accept(&store, 0, store.request_serial, {bytes.data + header + init, media}) == Mp4DemuxError::none &&
        mp4_fragment_source_initialize_segments(&store, &source) == Mp4DemuxError::none;
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    valid = valid && SUCCEEDED(apartment);
    const auto* slot = valid ? mp4_segment_store_find(store, 0) : nullptr;
    const u32 first_count = slot ? slot->media.samples : 0;
    Mp4MediaSegment appended{};
    if (pair) valid = valid && mp4_media_segment_initialize(following, store.track, &appended) == Mp4DemuxError::none;
    const u32 sample_count = first_count + appended.samples;
    valid = valid && sample_count && sample_count <= 4096;
    bool extended = false;
    const auto advance = [&]() noexcept {
        if (!pair) return false;
        if (extended) return !store.ended && mp4_segment_store_extend(&store, 0, 2, true) == Mp4DemuxError::none;
        extended = mp4_segment_store_extend(&store, 0, 2, false) == Mp4DemuxError::none &&
            mp4_segment_store_request(&store, 1) == Mp4DemuxError::data_pending &&
            mp4_segment_store_accept(&store, 1, store.request_serial, following) == Mp4DemuxError::none;
        return extended;
    };
    if (audio) {
        Mp4FragmentAudio packets{}; AacDecoder decoder{};
        valid = valid && mp4_fragment_audio_initialize(&source, &packets) == Mp4DemuxError::none && packets.packet_count == first_count &&
            (live || sample_count == 470) &&
            aac_decoder_initialize(&decoder, source.track.configuration, source.track.sample_rate, source.track.channels).ok();
        for (u32 pass = 0; valid && pass < 2; ++pass) {
            const u32 begin = pass && sample_count > 20 ? sample_count - 20 : 0;
            const u32 count = pass ? minimum(12u, sample_count - begin) : sample_count;
            valid = mp4_fragment_audio_seek(&packets, begin) == Mp4DemuxError::none && aac_decoder_flush(&decoder).ok();
            u32 submitted = 0, frames = 0, previous_frames = 0; bool ended = false, draining = false;
            u8 pcm[kAacDecoderMaximumOutputBytes];
            for (u32 calls = 0; valid && calls < 2048 && !ended; ++calls) {
                const auto block = aac_decoder_read(&decoder, {pcm, sizeof(pcm)});
                if (block.ok()) {
                    const u64 frame = u64{begin} * 1024 + frames;
                    Mp4AudioFrameTimeBounds bounds{};
                    valid = block.timestamp_valid && mp4_audio_frame_time_bounds_100ns(packets.timeline, frame, &bounds);
                    bool matches = valid && account_video_audio_timestamp_within_bounds(block.presentation_time_100ns, bounds);
                    if (valid && !matches && previous_frames) {
                        i64 preceding{};
                        matches = mp4_audio_frame_time_100ns(packets.timeline, frame - previous_frames, &preceding) &&
                            account_video_audio_timestamp_matches(block.presentation_time_100ns, bounds.floor_100ns,
                                preceding, previous_frames, packets.timeline.sample_rate);
                    }
                    valid = valid && matches; frames += block.frames; previous_frames = block.frames;
                } else if (block.error == AacDecodeError::end_of_stream) ended = draining;
                else if (block.error != AacDecodeError::need_input || draining) valid = false;
                else if (submitted == count) {
                    if (pair && !pass) {
                        Mp4FragmentAudioPacket end{};
                        valid = advance() && mp4_fragment_audio_peek(&packets, &end) == Mp4DemuxError::end_of_stream;
                    }
                    valid = valid && aac_decoder_drain(&decoder).ok(); draining = true;
                }
                else {
                    Mp4FragmentAudioPacket packet{};
                    const auto result = mp4_fragment_audio_peek(&packets, &packet);
                    if (result == Mp4DemuxError::data_pending && pair) { valid = advance(); continue; }
                    valid = result == Mp4DemuxError::none &&
                        aac_decoder_submit(&decoder, packet.bytes, packet.presentation_time_100ns, packet.duration_100ns).ok() &&
                        mp4_fragment_audio_commit(&packets);
                    if (valid) ++submitted;
                }
            }
            valid = valid && ended && submitted == count && frames == count * 1024;
            test_write_metric("Twitch source AAC decoded frames: ", frames);
        }
        aac_decoder_release(&decoder);
    } else {
        ID3D11Device* device{}; ID3D11DeviceContext* context{}; Mp4Decoder decoder{};
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
        valid = valid && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
            D3D11_SDK_VERSION, &device, &level, &context)) && mp4_decoder_initialize_fragmented(&decoder, &source, device);
        const u64 initial_duration = mp4_decoder_movie(decoder).duration_us;
        if (pair) valid = valid && mp4_decoder_movie(decoder).live && !mp4_decoder_movie(decoder).duration_complete;
        u32 frames = 0; bool ended = false; i64 previous = 0;
        for (u32 calls = 0; valid && calls < 4096 && !ended; ++calls) {
            const auto frame = h264_gpu_decoder_read(&decoder.native, false, 4);
            if (frame.ok()) {
                valid = frame.gpu && frame.width == source.track.width && frame.height == source.track.height &&
                    frame.height == 480 && frame.presentation_time_us > previous &&
                    h264_gpu_decoder_acknowledge_frame(&decoder.native);
                previous = frame.presentation_time_us; ++frames;
            } else if (frame.error == Mp4DecodeError::data_pending && pair) valid = advance();
            else if (frame.error == Mp4DecodeError::end_of_stream) ended = true;
            else if (frame.error != Mp4DecodeError::needs_service)
                valid = h264_gpu_decoder_busy(decoder.native) && h264_gpu_decoder_wait(&decoder.native, 10);
        }
        test_write_metric("Twitch source GPU frames: ", frames);
        test_write_metric("Twitch source GPU parse error: ", static_cast<u32>(decoder.native.parse_error));
        test_write_metric("Twitch source GPU demux error: ", static_cast<u32>(decoder.native.demux_error));
        test_write_metric("Twitch source presentation endpoint: ", source.presentation_end);
        valid = valid && ended && frames == sample_count && (live || (frames == 300 && source.presentation_end == 72033000));
        if (pair) {
            u64 expected{};
            valid = valid && mp4_fragment_source_duration(source, &expected) && expected > initial_duration &&
                mp4_decoder_movie(decoder).duration_us == expected && mp4_decoder_movie(decoder).duration_complete;
        }
        mp4_decoder_release(&decoder); if (context) context->Release(); if (device) device->Release();
    }
    if (pair) valid = valid && extended && store.count == 2 && store.ended;
    test_write_metric("Twitch source committed bytes: ", store.initialization.committed + store.slots[0].storage.committed + store.slots[1].storage.committed);
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);
    if (SUCCEEDED(apartment)) CoUninitialize(); return valid;
}
bool inspect(ConstSlice<u8> bytes) noexcept {
    if (bytes.count < 16 || !bytes_equal(bytes.data, "MCHLS001", 8)) return false;
    const u32 init_bytes = byte_load_le32(bytes.data + 8), media_bytes = byte_load_le32(bytes.data + 12);
    if (!init_bytes || init_bytes > 1048576 || u64{init_bytes} + media_bytes != bytes.count - 16) return false;
    Mp4FragmentTrack tracks[2]{};
    for (u32 kind = 0; kind < 2; ++kind) {
        const auto result = mp4_fragment_track_initialize({bytes.data + 16, init_bytes}, kind == 1, tracks + kind);
        test_write_metric("Twitch fragment initialization: ", static_cast<u32>(result));
        if (result != Mp4DemuxError::none) return false;
    }
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) return false;
    AacDecoder decoder{};
    bool valid = tracks[1].sample_rate == 48000 && tracks[1].timescale == 48000 &&
        tracks[1].frames_per_packet == 1024 && aac_decoder_initialize(&decoder,
            tracks[1].configuration, tracks[1].sample_rate, tracks[1].channels).ok();
    const auto* media = bytes.data + 16 + init_bytes;
    u32 at = 0, fragments = 0, counts[2]{}, frames = 0, previous_frames = 0;
    u64 first[2]{}, end[2]{}; bool ended = false;
    const auto read = [&]() noexcept {
        u8 pcm[kAacDecoderMaximumOutputBytes];
        for (u32 i = 0; i < 16; ++i) {
            const auto block = aac_decoder_read(&decoder, {pcm, sizeof(pcm)});
            if (block.error == AacDecodeError::need_input) return true;
            if (block.error == AacDecodeError::end_of_stream) { ended = true; return true; }
            if (!block.ok() || !block.timestamp_valid) return false;
            const u64 ticks = (first[1] + frames) * 10000000;
            const Mp4AudioFrameTimeBounds bounds{static_cast<i64>(ticks / 48000),
                static_cast<i64>(ticks / 48000 + (ticks % 48000 != 0))};
            const i64 preceding = static_cast<i64>((first[1] + frames - previous_frames) * 10000000 / 48000);
            if (!account_video_audio_timestamp_within_bounds(block.presentation_time_100ns, bounds) &&
                !account_video_audio_timestamp_matches(block.presentation_time_100ns, bounds.floor_100ns,
                    preceding, previous_frames, 48000)) return false;
            frames += block.frames; previous_frames = block.frames;
        }
        return false;
    };
    while (valid && at < media_bytes) {
        if (media_bytes - at < 8) { valid = false; break; }
        const u32 size = byte_load_be32(media + at), type = byte_load_be32(media + at + 4);
        if (size < 8 || size > media_bytes - at) { valid = false; break; }
        if (type == 0x656d7367) { at += size; continue; }
        if (type != 0x6d6f6f66 || media_bytes - at - size < 8) { valid = false; break; }
        const u32 mdat = byte_load_be32(media + at + size);
        if (mdat < 8 || mdat > media_bytes - at - size || byte_load_be32(media + at + size + 4) != 0x6d646174) {
            valid = false; break;
        }
        for (u32 kind = 0; valid && kind < 2; ++kind) {
            Mp4Fragment fragment{};
            const auto error = mp4_fragment_initialize({media + at, size + mdat}, at, size + mdat,
                tracks[kind].defaults, &fragment);
            if (error != Mp4DemuxError::none || (counts[kind] && fragment.first_time != end[kind])) { valid = false; break; }
            if (!counts[kind]) first[kind] = fragment.first_time;
            for (u32 i = 0; valid && i < fragment.count; ++i) {
                Mp4FragmentSample sample{};
                valid = mp4_fragment_next(&fragment, &sample) == Mp4DemuxError::none && sample.offset >= at + size + 8 &&
                    sample.offset < at + size + mdat && sample.bytes <= at + size + mdat - sample.offset;
                if (valid && kind) {
                    const u64 time = sample.decode_time * 10000000 / 48000;
                    valid = sample.duration == 1024 && aac_decoder_submit(&decoder,
                        {media + sample.offset, sample.bytes}, static_cast<i64>(time),
                        (sample.decode_time + sample.duration) * 10000000 / 48000 - time).ok() && read();
                }
                if (valid) { ++counts[kind]; end[kind] = sample.decode_time + sample.duration; }
            }
        }
        ++fragments; at += size + mdat;
    }
    valid = valid && aac_decoder_drain(&decoder).ok() && read() && ended;
    test_write_metric("Twitch multiplexed fragments: ", fragments);
    test_write_metric("Twitch video packets: ", counts[0]); test_write_metric("Twitch AAC packets: ", counts[1]);
    test_write_metric("Twitch AAC PCM frames: ", frames);
    valid = valid && at == media_bytes && fragments == 95 && counts[0] == 300 && counts[1] == 470 && frames == 481280;
    aac_decoder_release(&decoder); CoUninitialize(); return valid;
}
}
bool twitch_capture(const char* name, bool audio, bool live = false, bool session = false, bool tail = false, bool startup = false) noexcept {
    char path[1024]{}; wchar_t wide[1024]{};
    const u32 length = GetEnvironmentVariableA(name, path, sizeof(path));
    if (!length) return true;
    if (length >= sizeof(path)) return false;
    for (u32 i = 0; i < length; ++i) { if (static_cast<u8>(path[i]) >= 128) return false; wide[i] = path[i]; }
    const HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Win32LargeInteger size{};
    const bool sized = GetFileSizeEx(file, size.native()) && size.QuadPart >= 16 && size.QuadPart <= 9 * 1024 * 1024;
    auto* data = sized ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<usize>(size.QuadPart), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
    DWORD read{};
    const bool loaded = data && ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
    CloseHandle(file);
    const bool valid = loaded && (session ? twitch_live_session({data, read}, tail, startup)
        : (audio || live || inspect({data, read})) && native_file({data, read}, audio, live));
    if (data) VirtualFree(data, 0, MEM_RELEASE);
    return valid;
}
bool twitch_fragment_probe() noexcept {
    return twitch_capture("MAICORD_TWITCH_FRAGMENT_PROBE", false) && twitch_capture("MAICORD_TWITCH_AUDIO_FRAGMENT_PROBE", true) &&
        twitch_capture("MAICORD_TWITCH_LIVE_FRAGMENT_PROBE", false, true) && twitch_capture("MAICORD_TWITCH_LIVE_FRAGMENT_PROBE", true, true) &&
        twitch_capture("MAICORD_TWITCH_LIVE_WINDOW_PROBE", false, true) && twitch_capture("MAICORD_TWITCH_LIVE_WINDOW_PROBE", true, true) &&
        twitch_capture("MAICORD_TWITCH_LIVE_SESSION_PROBE", false, true, true) &&
        twitch_capture("MAICORD_TWITCH_LIVE_TAIL_PROBE", false, true, true, true) &&
        twitch_capture("MAICORD_TWITCH_LIVE_START_PROBE", false, true, true, false, true);
}
}
