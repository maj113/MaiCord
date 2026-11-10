#include "maicord/core/virtual_memory.hpp"
#include "account_video_session_internal.hpp"

namespace maicord {

AccountVideoResult account_video_session_control(AccountVideoSession* s, const AccountVideoIdentity& id,
                                                 u64 serial, AccountVideoCommand command, i64 target,
                                                 float volume, bool muted) noexcept {
    if (!thread(*s) || !serial ||
        static_cast<u32>(command) > static_cast<u32>(AccountVideoCommand::stop))
        return refused(s, id, serial, AccountVideoError::invalid_input);
    if (!account_video_identity_equal(s->status.identity, id) || serial <= s->status.desired_command)
        return refused(s, id, serial, AccountVideoError::stale);
    if ((command == AccountVideoCommand::volume && !media_audio_gain_valid(volume)) ||
        (command == AccountVideoCommand::seek && (s->status.live || target < 0 || target > s->status.duration_100ns)))
        return refused(s, id, serial, AccountVideoError::invalid_input);
    if ((command == AccountVideoCommand::seek || command == AccountVideoCommand::stop) && s->published)
        return refused(s, id, serial, AccountVideoError::busy);
    refresh(*s);
    if (s->status.error != AccountVideoError::none)
        return refused(s, id, serial, s->status.error, s->status.native_error);
    if (command == AccountVideoCommand::seek || command == AccountVideoCommand::stop) {
        if (!reset(*s, command == AccountVideoCommand::stop ? 0 : target))
            return refused(s, id, serial,
                           s->status.error == AccountVideoError::none ? AccountVideoError::demux
                                                                      : s->status.error,
                           s->status.native_error);
        if (command == AccountVideoCommand::stop)
            s->desired_play = false;
    } else if (command == AccountVideoCommand::play) {
        if ((s->status.ended || !s->video_started) && !reset(*s, 0))
            return refused(s, id, serial,
                           s->published ? AccountVideoError::busy : AccountVideoError::demux);
        s->desired_play = true;
        s->status.ended = false;
        s->qpc_origin = s->status.position_100ns;
        if (!qpc(&s->qpc_anchor))
            return result(AccountVideoError::clock);
    } else if (command == AccountVideoCommand::pause) {
        s->desired_play = false;
        s->qpc_origin = s->status.position_100ns;
    } else if (command == AccountVideoCommand::volume)
        s->status.desired_volume = volume;
    else
        s->status.muted = muted;
    s->status.desired_command = serial;
    s->command_kind = command;
    s->status.rejected_command = 0;
    s->status.rejected_error = AccountVideoError::none;
    s->status.rejected_native_error = 0;
    if (s->status.has_audio) {
        if ((s->desired_play || s->seeking) && !workspace(*s))
            return result(s->status.error, s->status.native_error, true);
        if (auto* owner = audio_owner(*s)) {
            const auto gain = media_audio_volume(owner, s->status.muted ? 0 : s->status.desired_volume);
            if (!gain.ok()) {
                terminal(*s, AccountVideoError::audio, gain.native_error);
                return result(s->status.error, s->status.native_error, true);
            }
            if (!request_audio_state(*s))
                return result(s->status.error, s->status.native_error, true);
        }
    }
    refresh(*s);
    return result(AccountVideoError::none, 0, true, s->status.observed_command == serial);
}
void account_video_session_source_failed(AccountVideoSession* s) noexcept {
    if (thread(*s) && s->status.error == AccountVideoError::none)
        terminal(*s, AccountVideoError::demux);
}

AccountVideoService account_video_session_service(AccountVideoSession* s, bool (*cancelled)(void*) noexcept,
                                                  void* context) noexcept {
    AccountVideoService out{};
    if (!s || !thread(*s))
        return out;
    if (s->status.error == AccountVideoError::none && !s->status.release_pending && !live_window(*s))
        terminal(*s, AccountVideoError::timestamp);
    refresh(*s);
    out.playback_epoch = s->playback_epoch;
    if (s->status.error != AccountVideoError::none || s->status.release_pending)
        return out;
    if (cancelled && cancelled(context)) {
        out.interrupted = true;
        return out;
    }
    if (s->workspace)
        if (!request_audio_state(*s))
            return out;
    if (s->status.has_audio && (s->desired_play || s->seeking))
        out.needs_service = pcm_backed(*s)
                                ? pcm_service(*s, cancelled, context, &out.interrupted)
                                : s->status.kind == AccountPlaybackKind::ogg_opus
                                ? opus_service(*s, cancelled, context, &out.interrupted)
                                : audio_service(*s, cancelled, context, &out.interrupted);
    refresh(*s);
    if (out.interrupted || s->status.error != AccountVideoError::none)
        return out;
    if (standalone(*s)) {
        if (s->workspace && !request_audio_state(*s))
            return out;
        refresh(*s);
        return out;
    }
    if (s->published)
        return out;
    if (!s->desired_play && !s->seeking) {
        out.data_pending = data_pending(*s);
        return out;
    }
    if (s->seek_pending) {
        Mp4SeekPoint point{};
        const auto sought = s->seek_start_pending
            ? (mp4_decoder_seek_start(s->video) ? Mp4DemuxError::none
                : s->video->native.demux_error != Mp4DemuxError::none ? s->video->native.demux_error : Mp4DemuxError::invalid_input)
            : mp4_decoder_seek(s->video, s->pending_seek_us, &point);
        if (sought != Mp4DemuxError::none && sought != Mp4DemuxError::data_pending) {
            terminal(*s, AccountVideoError::demux, static_cast<u32>(sought));
            return out;
        }
        if (sought == Mp4DemuxError::none)
            set_pending(*s, &s->seek_pending, false);
    }
    for (u32 pictures = 0; pictures < 4 && !s->video_eof && !s->seek_pending; ++pictures) {
        if (cancelled && cancelled(context)) {
            out.interrupted = true;
            return out;
        }
        if (!s->held) {
            s->held_frame = mp4_decoder_read_selected(s->video);
            if (s->held_frame.error == Mp4DecodeError::data_pending) {
                set_pending(*s, &s->video_pending, true);
                break;
            }
            if (s->video_pending)
                set_pending(*s, &s->video_pending, false);
            if (s->held_frame.error == Mp4DecodeError::end_of_stream) {
                s->video_eof = s->video_seek_ready = true;
                break;
            }
            if (s->held_frame.error == Mp4DecodeError::needs_service) {
                out.needs_service = true;
                return out;
            }
            if (!s->held_frame.ok()) {
                if (mp4_decoder_busy(*s->video)) {
                    if (cancelled && cancelled(context)) {
                        out.interrupted = true;
                        return out;
                    }
                    mp4_decoder_wait(s->video, 16);
                    out.needs_service = true;
                    return out;
                }
                terminal(*s, AccountVideoError::decoder, static_cast<u32>(s->video->native.status));
                return out;
            }
            s->held = true;
        }
        auto& frame = s->held_frame;
        const u64 duration = frame.source_duration_us ? frame.source_duration_us : frame.duration_us;
        if (frame.presentation_time_us > kLimit / 10 || frame.presentation_time_us < -kLimit / 10 ||
            duration > static_cast<u64>(kLimit) / 10) {
            terminal(*s, AccountVideoError::timestamp);
            return out;
        }
        const auto movie = mp4_decoder_movie(*s->video);
        const auto interval =
            mp4_frame_interval(frame.presentation_time_us, duration, movie.duration_us, movie.edited);
        if (interval.visibility == Mp4FrameVisibility::invalid) {
            terminal(*s, AccountVideoError::timestamp);
            return out;
        }
        if (interval.visibility == Mp4FrameVisibility::before) {
            if (!drop_frame(*s))
                return out;
            out.needs_service = true;
            continue;
        }
        if (interval.visibility == Mp4FrameVisibility::after) {
            if (!drop_frame(*s))
                return out;
            s->video_eof = s->video_seek_ready = true;
            break;
        }
        frame.source_duration_us = duration;
        frame.duration_us = interval.visible_duration_us;
        const u64 visible_from_source =
            interval.visible_duration_us +
            (frame.presentation_time_us < 0 ? static_cast<u64>(-frame.presentation_time_us) : 0);
        const i64 pts = frame.presentation_time_us * 10;
        const auto due = account_video_frame_due(pts, visible_from_source * 10, s->status.position_100ns);
        if (due == AccountVideoFrameDue::invalid) {
            terminal(*s, AccountVideoError::timestamp);
            return out;
        }
        if (due == AccountVideoFrameDue::expired) {
            if (!drop_frame(*s))
                return out;
            out.needs_service = true;
            continue;
        }
        if (due == AccountVideoFrameDue::future) {
            if (s->seeking)
                s->video_seek_ready = true;
            if (s->desired_play && (!s->status.has_audio || s->status.clock_valid))
                out.next_service_delay_ms = delay(pts - s->status.position_100ns);
            break;
        }
        if (s->status.has_audio &&
            (!s->audio_epoch_ready || (s->desired_play && !s->status.clock_valid && !s->seeking)))
            break;
        if (s->frame_counter == ~u64{0}) {
            terminal(*s, AccountVideoError::clock);
            return out;
        }
        s->published = true;
        s->status.frame_pending = true;
        s->status.frame_serial = ++s->frame_counter;
        out.frame = frame;
        out.frame_ready = true;
        out.frame_serial = s->frame_counter;
        break;
    }
    // Future-frame/EOF readiness has no UI copy acknowledgement to wake us.
    // Start here even if the paused PCM ring filled during earlier preroll.
    // Target pictures already handed to the UI remain unready until the UI acknowledges the copy.
    if (s->workspace)
        if (!request_audio_state(*s))
            return out;
    refresh(*s);
    if (s->video_eof && s->desired_play && !s->status.has_audio && !s->status.ended)
        out.next_service_delay_ms = delay(s->status.duration_100ns - s->status.position_100ns);
    out.data_pending = data_pending(*s);
    return out;
}
bool account_video_session_acknowledge(AccountVideoSession* s, const AccountVideoIdentity& id, u64 epoch,
                                       u64 serial) noexcept {
    if (!thread(*s) || !account_video_identity_equal(s->status.identity, id) ||
        epoch != s->playback_epoch || !s->published || serial != s->status.frame_serial)
        return false;
    if (!drop_frame(*s))
        return false;
    if (s->seeking)
        s->video_seek_ready = true;
    if (s->workspace)
        if (!request_audio_state(*s))
            return false;
    refresh(*s);
    return true;
}
AccountVideoStatus account_video_session_status(const AccountVideoSession* s) noexcept {
    return thread(*s) ? s->status : AccountVideoStatus{};
}
AccountVideoAudioProgress account_video_session_audio_progress(const AccountVideoSession* s) noexcept {
    if (!thread(*s) || !s->workspace)
        return {};
    const auto* owner = audio_owner(*s);
    const auto a = media_audio_status(owner);
    if (a.observed_epoch != s->playback_epoch)
        return {};
    return {a.observed_epoch, a.written_frames, a.submitted_frames, a.consumed_frames};
}
AccountVideoResult account_video_session_release(AccountVideoSession* s, u32 timeout) noexcept {
    if (!s->initialized && !s->workspace) {
        reset_value(s);
        return result(AccountVideoError::none, 0, true, true);
    }
    if (!thread(*s))
        return result(AccountVideoError::invalid_input);
    if (s->published)
        return result(AccountVideoError::busy);
    s->status.release_pending = true;
    s->desired_play = false;
    if (!drop_frame(*s))
        return result(s->status.error, s->status.native_error);
    if (auto* owner = audio_owner(*s)) {
        const auto audio = media_audio_release(owner, timeout);
        if (!audio.ok())
            return result(AccountVideoError::release, audio.native_error, true);
        if (s->status.kind == AccountPlaybackKind::ogg_opus) {
            opus_decoder_release(&static_cast<OpusWorkspace*>(s->workspace)->decoder);
        } else if (s->status.kind == AccountPlaybackKind::mp3) {
            mp3_decoder_release(&static_cast<Mp3Workspace*>(s->workspace)->decoder);
        } else if (!standalone(*s)) {
            const auto decoder = aac_decoder_release(&static_cast<Workspace*>(s->workspace)->decoder);
            if (!decoder.ok())
                return result(AccountVideoError::release, static_cast<u32>(decoder.native_status), true);
        }
        if (!pages_release(s->workspace))
            return result(AccountVideoError::release, GetLastError(), true);
    }
    reset_value(s);
    return result(AccountVideoError::none, 0, true, true);
}
u32 account_video_session_owned_bytes(const AccountVideoSession* s) noexcept {
    if (!s->workspace)
        return 0;
    if (s->status.kind == AccountPlaybackKind::wave_pcm)
        return sizeof(PcmWorkspace);
    if (s->status.kind == AccountPlaybackKind::mp3)
        return static_cast<const Mp3Workspace*>(s->workspace)->allocation_bytes;
    return s->status.kind == AccountPlaybackKind::ogg_opus
               ? static_cast<const OpusWorkspace*>(s->workspace)->allocation_bytes
               : sizeof(Workspace);
}

} // namespace maicord
