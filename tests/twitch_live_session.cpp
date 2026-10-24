#include "maicord/app/account_video_session.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/d3d11.hpp"
#include "test_output.hpp"

namespace maicord {
// Opt-in, muted native playback. Two consecutive captured files expose a
// temporary live edge, an early ENDLIST, then independently delivered tracks.
bool twitch_live_session(ConstSlice<u8> bytes, bool tail, bool startup_only) noexcept {
    if (bytes.count < 20 || !bytes_equal(bytes.data, "MCHLS002", 8)) return false;
    const u32 init = byte_load_le32(bytes.data + 8), first = byte_load_le32(bytes.data + 12), second = byte_load_le32(bytes.data + 16);
    if (!init || !first || !second || u64{init} + first + second != bytes.count - 20) return false;
    const ConstSlice<u8> initialization{bytes.data + 20, init}, initial{bytes.data + 20 + init, first}, next{initial.data + first, second};
    Mp4SegmentStore stores[2]{}; Mp4FragmentSource sources[2]{}; Mp4FragmentAudio audio{};
    Mp4Decoder decoder{}; AccountVideoSession session{};
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool valid = SUCCEEDED(apartment) && event;
    for (u32 i = 0; valid && i < 2; ++i)
        valid = mp4_segment_store_initialize(stores + i, initialization, i != 0, 1, 2, 1, true) == Mp4DemuxError::none &&
            mp4_segment_store_request(stores + i, 0) == Mp4DemuxError::data_pending &&
            mp4_segment_store_accept(stores + i, 0, stores[i].request_serial, initial) == Mp4DemuxError::none &&
            mp4_fragment_source_initialize_segments(stores + i, sources + i) == Mp4DemuxError::none;
    u32 startup = valid ? 1u : 0u;
    if (startup_only) for (u32 i = 0; valid && i < 2; ++i)
        valid = mp4_segment_store_extend(stores + i, 0, 2, false) == Mp4DemuxError::none &&
            mp4_segment_store_request(stores + i, 1) == Mp4DemuxError::data_pending &&
            mp4_segment_store_accept(stores + i, 1, stores[i].request_serial, next) == Mp4DemuxError::none &&
            mp4_fragment_source_refresh(sources + i);
    // Apply the earlier track's rational origin to both clocks.
    if (valid) {
        const u64 vt = sources[0].index.first_time, at = sources[1].index.first_time;
        const u64 vs = sources[0].track.timescale, as = sources[1].track.timescale;
        const bool video_first = vt / vs != at / as ? vt / vs < at / as : (vt % vs) * as <= (at % as) * vs;
        u64 origin = video_first ? vt : at, scale = video_first ? vs : as;
        if (startup_only) {
            // Delayed audio metadata has moved the shared origin into the
            // following video file. Keep both files resident to isolate decode
            // preroll from network and scheduling variability.
            auto cursor = mp4_segment_store_find(stores[0], 1)->media.fragment;
            Mp4FragmentSample sample{};
            valid = mp4_fragment_next(&cursor, &sample) == Mp4DemuxError::none && sample.presentation_time > 0;
            origin = static_cast<u64>(sample.presentation_time); scale = vs;
        }
        for (auto& source : sources)
            source.track.media_start = static_cast<i64>((origin / scale) * source.track.timescale +
                ((origin % scale) * source.track.timescale + scale / 2) / scale);
        valid = valid && sources[0].track.media_start >= 0 &&
            (startup_only || static_cast<u64>(sources[0].track.media_start) <= sources[0].index.first_time) &&
            mp4_fragment_audio_initialize(sources + 1, &audio) == Mp4DemuxError::none;
    }
    if (valid) startup = 2;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
    valid = valid && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
        D3D11_SDK_VERSION, &device, &level, &context)) && mp4_decoder_initialize_fragmented(&decoder, sources, device);
    if (valid) startup = 3;
    const AccountVideoIdentity id{1, 2, 3, 4, 0, 1, 0};
    valid = valid && account_video_session_initialize(&session, &decoder, id, event, {}, &audio).ok() && session.status.live &&
        !mp4_decoder_movie(decoder).duration_complete &&
        account_video_session_control(&session, id, 1, AccountVideoCommand::seek, 0).error == AccountVideoError::invalid_input &&
        session.status.error == AccountVideoError::none && session.playback_epoch == 1 &&
        account_video_session_control(&session, id, 1, AccountVideoCommand::mute, 0, 1, true).ok() &&
        account_video_session_control(&session, id, 2, AccountVideoCommand::play).ok();
    if (valid) startup = 4;
    const u32 initial_segment = sources[0].segment.index;
    const u64 epoch = session.playback_epoch;
    const auto* workspace = session.workspace;
    const i64 initial_duration = session.status.duration_100ns;
    const auto frames_for = [&](i64 duration) noexcept -> u64 {
        return duration > 0 ? (static_cast<u64>(duration) / 10000000) * sources[1].track.sample_rate +
            ((static_cast<u64>(duration) % 10000000) * sources[1].track.sample_rate + 9999999) / 10000000 : 0;
    };
    const u32 owned = account_video_session_owned_bytes(&session);
    u32 stage = 0, held_turns = 0, frames = 0;
    u64 edge_written = 0;
    const u64 deadline = GetTickCount64() + 8000;
    while (valid && !session.status.ended && GetTickCount64() < deadline) {
        const auto work = account_video_session_service(&session);
        valid = session.status.error == AccountVideoError::none && session.playback_epoch == epoch &&
            session.workspace == workspace && account_video_session_owned_bytes(&session) == owned;
        if (work.frame_ready) {
            ++frames;
            if (startup_only) valid = valid && session.held_frame.presentation_time_us == 0;
            valid = valid && account_video_session_acknowledge(&session, id, work.playback_epoch, work.frame_serial);
        }
        if (!valid) break;
        if (startup_only) { if (frames) { stage = 3; break; } }
        else if (!stage && stores[1].refresh_pending) {
            valid = audio.packet_index == audio.packet_count && !session.audio_eof && !session.status.ended;
            edge_written = account_video_session_audio_progress(&session).written_frames;
            for (u32 i = 0; i < 2; ++i)
                valid = valid && mp4_segment_store_extend(stores + i, 0, tail && i ? 1u : 2u, true) == Mp4DemuxError::none;
            valid = valid && !mp4_decoder_movie(decoder).duration_complete;
            stage = 1;
        } else if (stage == 1) {
            valid = session.status.duration_100ns == initial_duration && !session.audio_eof && !session.status.ended &&
                (tail || account_video_session_audio_progress(&session).written_frames == edge_written) &&
                !mp4_decoder_movie(decoder).duration_complete;
            if (tail && sources[1].ended && session.audio_pending &&
                account_video_session_audio_progress(&session).written_frames == frames_for(initial_duration)) {
                // AAC has really ended, but the final video body is still
                // absent. Its arrival must extend silence on the same ring.
                valid = valid && mp4_segment_store_request(stores, 1) == Mp4DemuxError::data_pending &&
                    mp4_segment_store_accept(stores, 1, stores[0].request_serial, next) == Mp4DemuxError::none;
                stage = 3;
            } else if (!tail && ++held_turns == 3) {
                valid = valid && mp4_segment_store_request(stores + 1, 1) == Mp4DemuxError::data_pending &&
                    mp4_segment_store_accept(stores + 1, 1, stores[1].request_serial, next) == Mp4DemuxError::none;
                stage = 2;
            }
        } else if (stage == 2) {
            valid = session.status.duration_100ns > initial_duration && !session.status.ended;
            if (stores[0].pending) {
                valid = valid && stores[0].demand == 1 && mp4_segment_store_accept(stores, 1, stores[0].request_serial, next) == Mp4DemuxError::none &&
                    mp4_decoder_movie(decoder).duration_complete;
                stage = 3;
            }
        }
        if (!work.needs_service) WaitForSingleObject(event, minimum(10u, work.next_service_delay_ms));
    }
    const auto progress = account_video_session_audio_progress(&session);
    const auto duration = session.status.duration_100ns;
    const u64 expected = frames_for(duration);
    valid = valid && stage == 3 && frames && (startup_only ? !session.status.ended && !stores[0].pending :
        session.status.ended && session.audio_eof && session.video_eof && progress.written_frames == expected &&
        progress.consumed_frames == expected && session.status.duration_100ns > initial_duration);
    if (startup_only) {
        valid = valid && initial_segment == 1 && h264_gpu_decoder_diagnostic(decoder.native).next_sample_index <= 4;
        test_write_metric("Live startup selected file: ", initial_segment);
        test_write_metric("Live startup submitted pictures: ", h264_gpu_decoder_diagnostic(decoder.native).next_sample_index);
    }
    test_write_metric("Live session startup: ", startup);
    test_write_metric("Live session short audio: ", tail);
    test_write_metric("Live session stage: ", stage);
    test_write_metric("Live session error: ", static_cast<u32>(session.status.error));
    test_write_metric("Live session native error: ", session.status.native_error);
    test_write_metric("Live session frames: ", frames);
    test_write_metric("Live session PCM written: ", progress.written_frames);
    test_write_metric("Live session PCM expected: ", expected);
    test_write_metric("Live session PCM consumed: ", progress.consumed_frames);
    test_write_metric("Live session owned bytes: ", owned);
    if (session.initialized && !account_video_session_release(&session, 1000).ok()) ExitProcess(1);
    mp4_decoder_release(&decoder);
    if (context) context->Release(); if (device) device->Release();
    for (u32 i = 0; i < 2; ++i) { mp4_fragment_source_release(sources + i); mp4_segment_store_release(stores + i); }
    if (event) CloseHandle(event);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return valid;
}
} // namespace maicord
