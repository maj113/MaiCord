#include "maicord/app/account_video_session.hpp"
#include "maicord/platform/aac_decoder.hpp"
#include "maicord/platform/media_audio.hpp"
#include "maicord/media/opus_decoder.hpp"
#include "maicord/media/encoded_audio.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/media/mp4_segment_store.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr i64 kLimit = 0x7fffffffffffffffll;
struct Workspace {
    float ring[8192 * 2], pcm[kAacDecoderMaximumOutputBytes / 4];
    Mp4AudioDemuxer demux{};
    Mp4AudioSampleWindow window{};
    Mp4AudioTimeline timeline{};
    u32 packet_index{}, packet_count{}, channels{}, frames_per_packet{};
    AacDecoder decoder{};
    MediaAudio audio{};
    u64 source_frames{}, queued_frames{}, wanted_frames{}, silence_frames{};
    u32 pending_offset{}, pending_frames{}, input_retries{}, preceding_frames{};
    bool reading{}, draining{}, decoded_end{}, sealed{}, requested_play{}, seek_pending{};
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
    AccountVideoTimestampDiagnostic diagnostic{};
#endif
};
static_assert(sizeof(Workspace) <= 84 * 1024);
struct OpusWorkspace {
    float ring[8192 * 2], pcm[kOpusDecoderMaximumFrames * 2];
    i16 pcm16[kOpusDecoderMaximumFrames * 2];
    u8 packet[kOggOpusMaximumPacketBytes];
    OggOpusDemuxer demux{};
    OpusDecoder decoder{};
    MediaAudio audio{};
    u64 first_frame{}, end_frame{}, source_frames{}, queued_frames{}, wanted_frames{};
    u32 pending_offset{}, pending_frames{}, allocation_bytes{};
    bool decoded_end{}, sealed{}, requested_play{};
};
static_assert(sizeof(OpusWorkspace) <= 208 * 1024);
struct PcmWorkspace {
    float ring[8192 * 2], pcm[kMediaAudioMaximumWriteFrames * 2];
    MediaAudio audio{};
    u64 first_frame{}, source_frames{}, queued_frames{}, wanted_frames{};
    u32 pending_offset{}, pending_frames{};
    bool decoded_end{}, sealed{}, requested_play{};
};
static_assert(sizeof(PcmWorkspace) <= 84 * 1024);
struct Mp3Workspace {
    PcmWorkspace pcm{};
    Mp3Stream demux{};
    Mp3SeekPoint points[kMp3DefaultSeekPoints]{};
    Mp3SeekIndex index{};
    Mp3Decoder decoder{};
    u32 allocation_bytes{};
};
static_assert(sizeof(Mp3Workspace) <= 96 * 1024);
PcmWorkspace& pcm_owner(AccountVideoSession& s) noexcept {
    return s.status.kind == AccountPlaybackKind::mp3 ? static_cast<Mp3Workspace*>(s.workspace)->pcm
                                                    : *static_cast<PcmWorkspace*>(s.workspace);
}
const PcmWorkspace& pcm_owner(const AccountVideoSession& s) noexcept {
    return s.status.kind == AccountPlaybackKind::mp3 ? static_cast<const Mp3Workspace*>(s.workspace)->pcm
                                                    : *static_cast<const PcmWorkspace*>(s.workspace);
}
bool pcm_backed(const AccountVideoSession& s) noexcept {
    return s.status.kind == AccountPlaybackKind::wave_pcm || s.status.kind == AccountPlaybackKind::mp3;
}
bool standalone(const AccountVideoSession& s) noexcept {
    return s.status.kind != AccountPlaybackKind::mp4;
}
const OggOpusDemuxer* opus_source(const AccountVideoSession& s) noexcept {
    return s.encoded_metadata ? &s.encoded_metadata->opus : nullptr;
}
u32 source_rate(const AccountVideoSession& s) noexcept {
    return encoded_audio_rate(*s.encoded_metadata);
}
u64 source_total_frames(const AccountVideoSession& s) noexcept {
    return encoded_audio_total_frames(*s.encoded_metadata);
}
MediaAudio* audio_owner(AccountVideoSession& s) noexcept {
    if (!s.workspace)
        return nullptr;
    if (s.status.kind == AccountPlaybackKind::ogg_opus)
        return &static_cast<OpusWorkspace*>(s.workspace)->audio;
    if (pcm_backed(s))
        return &pcm_owner(s).audio;
    return &static_cast<Workspace*>(s.workspace)->audio;
}
const MediaAudio* audio_owner(const AccountVideoSession& s) noexcept {
    if (!s.workspace)
        return nullptr;
    if (s.status.kind == AccountPlaybackKind::ogg_opus)
        return &static_cast<const OpusWorkspace*>(s.workspace)->audio;
    if (pcm_backed(s))
        return &pcm_owner(s).audio;
    return &static_cast<const Workspace*>(s.workspace)->audio;
}
u64 wanted_frames(const AccountVideoSession& s) noexcept {
    if (pcm_backed(s))
        return pcm_owner(s).wanted_frames;
    return s.status.kind == AccountPlaybackKind::ogg_opus
               ? static_cast<const OpusWorkspace*>(s.workspace)->wanted_frames
               : static_cast<const Workspace*>(s.workspace)->wanted_frames;
}
bool& requested_play(AccountVideoSession& s) noexcept {
    if (pcm_backed(s))
        return pcm_owner(s).requested_play;
    return s.status.kind == AccountPlaybackKind::ogg_opus
               ? static_cast<OpusWorkspace*>(s.workspace)->requested_play
               : static_cast<Workspace*>(s.workspace)->requested_play;
}
bool thread(const AccountVideoSession& s) noexcept {
    return s.initialized && s.thread_id == GetCurrentThreadId();
}
bool identity_valid(const AccountVideoIdentity& id) noexcept {
    return id.account_serial && id.message_id && id.content_identity && id.session_epoch &&
           id.cache_generation && !id.reserved;
}
AccountVideoResult result(AccountVideoError error, u32 native = 0, bool admitted = false,
                          bool completed = false) noexcept {
    return {error, native, admitted, completed};
}
AccountVideoResult refused(AccountVideoSession* s, const AccountVideoIdentity& id, u64 serial,
                           AccountVideoError error, u32 native = 0) noexcept {
    // A rejected request for the current session has a persistent result, separate
    // from terminal transport error. Requests for another session cannot mutate this state.
    if (s && thread(*s) && account_video_identity_equal(s->status.identity, id)) {
        s->status.rejected_command = serial;
        s->status.rejected_error = error;
        s->status.rejected_native_error = native;
    }
    return result(error, native);
}
void terminal(AccountVideoSession& s, AccountVideoError error, u32 native = 0) noexcept {
    s.status.error = error;
    s.status.native_error = native;
    s.desired_play = false;
    s.seeking = s.seek_pending = s.seek_start_pending = false;
    s.video_pending = s.audio_pending = false;
    s.status.seeking = s.status.buffering = false;
    if (auto* audio = audio_owner(s))
        media_audio_pause(audio);
}
bool frame_count(i64 duration, u32 rate, u64* frames) noexcept {
    if (duration < 0 || !rate || !frames)
        return false;
    const u64 time = static_cast<u64>(duration), seconds = time / 10000000, remainder = time % 10000000;
    if (seconds > ~u64{0} / rate)
        return false;
    const u64 whole = seconds * rate, fraction = (remainder * rate + 9999999) / 10000000;
    if (whole > ~u64{0} - fraction)
        return false;
    *frames = whole + fraction;
    return true;
}
u32 delay(i64 duration) noexcept {
    if (duration <= 0)
        return 0;
    const u64 ms = (static_cast<u64>(duration) + 9999) / 10000;
    return static_cast<u32>(minimum(ms, u64{INFINITE - 1}));
}
bool qpc(u64* now) noexcept {
    Win32LargeInteger value{};
    if (!QueryPerformanceCounter(value.native()) || value.QuadPart < 0)
        return false;
    *now = static_cast<u64>(value.QuadPart);
    return true;
}
bool data_pending(const AccountVideoSession& s) noexcept {
    return s.video_pending || s.audio_pending || s.seek_pending;
}
// Buffering holds the presentation clock: audio pauses through request_audio_state,
// and the audio-less QPC clock re-anchors at the held position.
void set_pending(AccountVideoSession& s, bool* flag, bool value) noexcept {
    const bool before = data_pending(s);
    *flag = value;
    if (before != data_pending(s) && !s.status.has_audio) {
        s.qpc_origin = s.status.position_100ns;
        qpc(&s.qpc_anchor);
    }
}
bool audio_window(AccountVideoSession& s, Workspace& w) noexcept {
    w.demux = s.audio_metadata;
    if (s.fragmented_audio) {
        if (mp4_fragment_audio_refresh(s.fragmented_audio) != Mp4DemuxError::none) return false;
        const auto& track = s.fragmented_audio->source->track;
        w.timeline = s.fragmented_audio->timeline;
        w.packet_count = s.fragmented_audio->packet_count;
        w.channels = track.channels; w.frames_per_packet = track.frames_per_packet;
    } else {
        mp4_audio_demux_reset(&w.demux);
        if (!mp4_audio_demux_timeline(w.demux, &w.timeline)) return false;
        w.packet_count = w.demux.samples.sample_count;
        w.channels = w.demux.channels; w.frames_per_packet = w.demux.frames_per_packet;
    }
    // Keep the audio clock through an absent track tail using the existing
    // silence path. Trim all decoder preroll on the original PCM sample grid.
    Mp4AudioSampleWindow entire{};
    if (!mp4_audio_sample_window(w.timeline, 0, &entire)) return false;
    const i64 audio_target = minimum(s.seek_target, entire.duration_100ns);
    if (!mp4_audio_sample_window(w.timeline, audio_target, &w.window) ||
        !frame_count(s.status.duration_100ns - s.seek_target, w.timeline.sample_rate, &w.wanted_frames)) return false;
    const u64 packet = w.window.first_source_frame / w.frames_per_packet;
    const u32 preroll = aac_decoder_seek_preroll_packets(w.frames_per_packet);
    w.packet_index = packet > preroll && packet - preroll < w.packet_count ? static_cast<u32>(packet - preroll) : 0;
    w.seek_pending = false;
    if (s.fragmented_audio) {
        if (w.window.first_source_frame == w.window.end_source_frame) w.packet_index = w.packet_count;
        const auto sought = mp4_fragment_audio_seek(s.fragmented_audio, w.packet_index);
        if (sought != Mp4DemuxError::none && sought != Mp4DemuxError::data_pending) return false;
        w.seek_pending = sought == Mp4DemuxError::data_pending;
        set_pending(s, &s.audio_pending, w.seek_pending);
    } else if (w.packet_index && mp4_demux_seek_sample(&w.demux.samples, w.packet_index) != Mp4DemuxError::none) return false;
    w.source_frames = u64{w.packet_index} * w.frames_per_packet;
    w.queued_frames = 0;
    w.pending_frames = w.pending_offset = w.input_retries = w.preceding_frames = 0;
    w.silence_frames = minimum(w.window.leading_silence_frames, w.wanted_frames);
    w.reading = w.draining = w.sealed = false;
    w.decoded_end = !w.wanted_frames || w.window.first_source_frame == w.window.end_source_frame;
    s.audio_eof = false;
    return true;
}
// Received live media extends the existing sample window. Never reset the
// decoder, PCM cursor, leading silence or playback epoch at a playlist edge.
bool live_window(AccountVideoSession& s) noexcept {
    if (!s.status.live) return true;
    const auto movie = mp4_decoder_movie(*s.video);
    if (!movie.live || !movie.duration_us || movie.duration_us > static_cast<u64>(kLimit) / 10) return false;
    i64 duration = static_cast<i64>(movie.duration_us * 10);
    Mp4AudioSampleWindow entire{};
    if (s.status.has_audio) {
        if (s.fragmented_audio ? mp4_fragment_audio_refresh(s.fragmented_audio) != Mp4DemuxError::none ||
                !mp4_audio_sample_window(s.fragmented_audio->timeline, 0, &entire)
            : !mp4_audio_demux_sample_window(s.audio_metadata, 0, &entire)) return false;
        duration = maximum(duration, entire.duration_100ns);
    }
    if (duration < s.status.duration_100ns) return false;
    if (s.workspace) {
        auto& w = *static_cast<Workspace*>(s.workspace);
        const auto& timeline = s.fragmented_audio ? s.fragmented_audio->timeline : w.timeline;
        Mp4AudioSampleWindow window{};
        u64 wanted{};
        if (!mp4_audio_sample_window(timeline, minimum(s.seek_target, entire.duration_100ns), &window) ||
            !frame_count(duration - s.seek_target, timeline.sample_rate, &wanted) || wanted < w.wanted_frames ||
            window.first_source_frame != w.window.first_source_frame || window.end_source_frame < w.window.end_source_frame ||
            (w.sealed && wanted != w.wanted_frames)) return false;
        const u32 packets = s.fragmented_audio ? s.fragmented_audio->packet_count : w.packet_count;
        if (packets < w.packet_count || ((w.draining || w.decoded_end) && packets != w.packet_count)) return false;
        w.timeline = timeline; w.window = window; w.packet_count = packets; w.wanted_frames = wanted;
        if (w.decoded_end && (w.queued_frames < wanted || movie.duration_complete)) set_pending(s, &s.audio_pending, false);
    }
    s.status.duration_100ns = duration;
    return true;
}
bool opus_window(AccountVideoSession& s, OpusWorkspace& w) noexcept {
    const auto* source = opus_source(s);
    if (!source || !source->ready)
        return false;
    w.demux = *source;
    u64 frame{};
    if (!frame_count(s.seek_target, kOggOpusSampleRate, &frame))
        return false;
    frame = minimum(frame, w.demux.total_frames);
    OggOpusSeekPoint point{};
    const auto sought = ogg_opus_demux_seek(&w.demux, frame, &point, {w.packet, sizeof(w.packet)},
                                            s.source_cancelled, s.source_cancel_context);
    if (sought != OggOpusError::none) {
        terminal(s, AccountVideoError::demux, static_cast<u32>(sought));
        return false;
    }
    if (w.demux.total_frames > ~u64{0} - w.demux.pre_skip)
        return false;
    w.first_frame = w.demux.pre_skip + frame;
    w.end_frame = w.demux.pre_skip + w.demux.total_frames;
    w.source_frames = point.decoded_first_frame;
    w.wanted_frames = w.demux.total_frames - frame;
    w.queued_frames = w.pending_frames = w.pending_offset = 0;
    w.decoded_end = !w.wanted_frames;
    w.sealed = false;
    s.audio_eof = false;
    s.video_seek_ready = false; // Even EOF must start a new audio epoch.
    return true;
}
// Opens the audio transport at the session's current epoch and start position.
bool start_audio(AccountVideoSession& s, MediaAudio* audio, float* ring, u32 channels,
                 u32 sample_rate) noexcept {
    MediaAudioConfig config{};
    config.pcm = {ring, 8192u * channels};
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.initial_epoch = s.playback_epoch;
    config.source_origin_100ns = s.seek_target;
    config.volume = s.status.muted ? 0 : s.status.desired_volume;
    config.refill_event = config.status_event = s.lane_event;
    const auto started = media_audio_initialize(audio, config);
    if (!started.ok())
        terminal(s, AccountVideoError::audio, started.native_error);
    return started.ok();
}
bool opus_workspace(AccountVideoSession& s) noexcept {
    const auto* source = opus_source(s);
    const u32 state_bytes = opus_decoder_storage_bytes(source->channels);
    if (!state_bytes || state_bytes > 128 * 1024) {
        terminal(s, AccountVideoError::decoder);
        return false;
    }
    constexpr u32 offset =
        (sizeof(OpusWorkspace) + kOpusDecoderStateAlignment - 1) & ~(kOpusDecoderStateAlignment - 1);
    auto* w = static_cast<OpusWorkspace*>(
        VirtualAlloc(nullptr, offset + state_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!w) {
        terminal(s, AccountVideoError::memory, GetLastError());
        return false;
    }
    s.workspace = w;
    w->allocation_bytes = offset + state_bytes;
    auto decoded = opus_decoder_initialize(&w->decoder, {reinterpret_cast<u8*>(w) + offset, state_bytes},
                                           source->channels);
    if (decoded.ok())
        decoded = opus_decoder_gain(&w->decoder, source->output_gain_q8);
    if (!decoded.ok()) {
        terminal(s, AccountVideoError::decoder, static_cast<u32>(decoded.native_status));
        return false;
    }
    if (!start_audio(s, &w->audio, w->ring, source->channels, kOggOpusSampleRate))
        return false;
    if (!opus_window(s, *w)) {
        if (s.status.error == AccountVideoError::none)
            terminal(s, AccountVideoError::timestamp);
        return false;
    }
    return true;
}
bool pcm_window(AccountVideoSession& s, PcmWorkspace& w) noexcept {
    u64 frame{};
    if (!frame_count(s.seek_target, source_rate(s), &frame))
        return false;
    w.first_frame = minimum(frame, source_total_frames(s));
    w.source_frames = w.first_frame;
    w.wanted_frames = source_total_frames(s) - w.first_frame;
    w.queued_frames = w.pending_offset = w.pending_frames = 0;
    w.decoded_end = !w.wanted_frames;
    w.sealed = false;
    s.audio_eof = s.video_seek_ready = false;
    if (s.status.kind == AccountPlaybackKind::mp3) {
        auto& mp3 = *static_cast<Mp3Workspace*>(s.workspace);
        mp3.demux = s.encoded_metadata->mp3;
        const auto sought = mp3_stream_seek(&mp3.demux, w.first_frame, &mp3.index,
                                             s.source_cancelled, s.source_cancel_context);
        if (sought != Mp3Error::none) {
            terminal(s, AccountVideoError::demux, static_cast<u32>(sought));
            return false;
        }
        w.source_frames = mp3.demux.cursor_frame;
    }
    return true;
}
bool pcm_workspace(AccountVideoSession& s) noexcept {
    constexpr u32 offset = (sizeof(Mp3Workspace) + kMp3DecoderStateAlignment - 1) &
                          ~(kMp3DecoderStateAlignment - 1);
    const bool mp3 = s.status.kind == AccountPlaybackKind::mp3;
    const u32 state_bytes = mp3 ? mp3_decoder_storage_bytes() : 0;
    if (mp3 && (!state_bytes || state_bytes > 32 * 1024)) {
        terminal(s, AccountVideoError::decoder);
        return false;
    }
    const u32 allocation = mp3 ? offset + state_bytes : sizeof(PcmWorkspace);
    s.workspace = VirtualAlloc(nullptr, allocation, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!s.workspace) {
        terminal(s, AccountVideoError::memory, GetLastError());
        return false;
    }
    auto* w = &pcm_owner(s);
    if (mp3) {
        auto& owner = *static_cast<Mp3Workspace*>(s.workspace);
        owner.allocation_bytes = allocation;
        const auto decoded = mp3_decoder_initialize(&owner.decoder,
            {reinterpret_cast<u8*>(s.workspace) + offset, state_bytes},
            source_rate(s), encoded_audio_channels(*s.encoded_metadata));
        if (decoded != Mp3Error::none) {
            terminal(s, AccountVideoError::decoder, static_cast<u32>(decoded));
            return false;
        }
        const auto indexed = mp3_stream_build_index(s.encoded_metadata->mp3, &owner.index,
            {owner.points, kMp3DefaultSeekPoints}, s.source_cancelled, s.source_cancel_context);
        if (indexed != Mp3Error::none) {
            terminal(s, AccountVideoError::demux, static_cast<u32>(indexed));
            return false;
        }
    }
    if (!start_audio(s, &w->audio, w->ring, encoded_audio_channels(*s.encoded_metadata), source_rate(s)))
        return false;
    if (!pcm_window(s, *w)) {
        if (s.status.error == AccountVideoError::none)
            terminal(s, AccountVideoError::timestamp);
        return false;
    }
    return true;
}
bool workspace(AccountVideoSession& s) noexcept {
    if (s.workspace)
        return true;
    if (s.status.kind == AccountPlaybackKind::ogg_opus)
        return opus_workspace(s);
    if (pcm_backed(s))
        return pcm_workspace(s);
    auto* w = static_cast<Workspace*>(
        VirtualAlloc(nullptr, sizeof(Workspace), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!w) {
        terminal(s, AccountVideoError::memory, GetLastError());
        return false;
    }
    s.workspace = w;
    const auto* track = s.fragmented_audio ? &s.fragmented_audio->source->track : nullptr;
    const auto asc = track ? track->configuration : mp4_audio_demux_asc(s.audio_metadata);
    const u32 rate = track ? track->sample_rate : s.audio_metadata.sample_rate;
    const u32 channels = track ? track->channels : s.audio_metadata.channels;
    const auto decoder = aac_decoder_initialize(&w->decoder, asc, rate, channels);
    if (!decoder.ok()) {
        terminal(s, AccountVideoError::decoder, static_cast<u32>(decoder.native_status));
        return false;
    }
    if (!start_audio(s, &w->audio, w->ring, channels, rate)) return false;
    if (!audio_window(s, *w)) {
        terminal(s, AccountVideoError::timestamp);
        return false;
    }
    return true;
}
bool drop_frame(AccountVideoSession& s) noexcept {
    if (!s.held)
        return true;
    if (!mp4_decoder_acknowledge_frame(s.video)) {
        terminal(s, AccountVideoError::decoder);
        return false;
    }
    s.held = s.published = false;
    s.status.frame_pending = false;
    s.held_frame = {};
    return true;
}
bool request_audio_state(AccountVideoSession& s) noexcept {
    // After a seek, show/copy the actual target video before starting its audio
    // clock. This avoids advancing audio while the GPU is still in preroll.
    const bool play =
        account_video_audio_should_play(s.desired_play && !data_pending(s), s.seeking, s.video_seek_ready);
    if (play == requested_play(s))
        return true;
    const auto value = play ? media_audio_play(audio_owner(s)) : media_audio_pause(audio_owner(s));
    if (!value.ok()) {
        terminal(s, AccountVideoError::audio, value.native_error);
        return false;
    }
    requested_play(s) = play;
    return true;
}
bool reset(AccountVideoSession& s, i64 target) noexcept {
    if (s.published || s.playback_epoch == ~u64{0})
        return false;
    if (!drop_frame(s))
        return false;
    if (standalone(s)) {
        u64 frame{}, stamp{};
        if (!frame_count(target, source_rate(s), &frame) ||
            !media_audio_clock_time(minimum(frame, source_total_frames(s)), source_rate(s),
                                    &stamp) ||
            stamp > static_cast<u64>(kLimit))
            return false;
        target = static_cast<i64>(stamp);
    } else if (s.video) {
        Mp4SeekPoint point{};
        const auto movie = mp4_decoder_movie(*s.video);
        const i64 video_target = minimum(target / 10, static_cast<i64>(movie.duration_us));
        // A shared live origin can trim whole video files when the audio
        // playlist arrives later. Start at its preceding IDR, keeping only
        // required decode preroll instead of replaying the retained history.
        const bool start = (!video_target && !(movie.live && movie.first_presentation_valid &&
                                               movie.first_presentation_time_us < 0)) ||
                           (movie.first_presentation_time_us > 0 && video_target < movie.first_presentation_time_us);
        const auto sought = start ? (mp4_decoder_seek_start(s.video) ? Mp4DemuxError::none
                                      : s.video->native.demux_error == Mp4DemuxError::data_pending ? Mp4DemuxError::data_pending
                                                                                               : Mp4DemuxError::invalid_input)
                                  : mp4_decoder_seek(s.video, video_target, &point);
        if (sought != Mp4DemuxError::none && sought != Mp4DemuxError::data_pending)
            return false;
        // The IDR payload has not arrived: service retries once it has.
        s.seek_pending = sought == Mp4DemuxError::data_pending;
        s.seek_start_pending = start;
        s.pending_seek_us = video_target;
    }
    s.video_pending = s.audio_pending = false;
    s.video_started = true;
    ++s.playback_epoch;
    s.status.playback_epoch = s.playback_epoch;
    s.seek_target = target;
    s.status.position_100ns = target;
    s.video_eof = s.audio_eof = s.video_seek_ready = false;
    // Audio-only AAC (m4a) shares the MP4 audio path without a picture track.
    if (standalone(s) || !s.video)
        s.video_eof = true;
    s.seeking = s.status.seeking = true;
    s.status.ended = false;
    s.status.clock_valid = false;
    s.qpc_origin = target;
    if (!qpc(&s.qpc_anchor)) {
        terminal(s, AccountVideoError::clock);
        return false;
    }
    if (auto* owner = audio_owner(s)) {
        if (s.status.kind == AccountPlaybackKind::ogg_opus) {
            auto& w = *static_cast<OpusWorkspace*>(s.workspace);
            auto flushed = opus_decoder_reset(&w.decoder);
            if (flushed.ok())
                flushed = opus_decoder_gain(&w.decoder, opus_source(s)->output_gain_q8);
            if (!flushed.ok()) {
                terminal(s, AccountVideoError::decoder, static_cast<u32>(flushed.native_status));
                return false;
            }
        } else if (s.status.kind == AccountPlaybackKind::mp3) {
            const auto flushed = mp3_decoder_reset(&static_cast<Mp3Workspace*>(s.workspace)->decoder);
            if (flushed != Mp3Error::none) {
                terminal(s, AccountVideoError::decoder, static_cast<u32>(flushed));
                return false;
            }
        } else if (!standalone(s)) {
            const auto flushed = aac_decoder_flush(&static_cast<Workspace*>(s.workspace)->decoder);
            if (!flushed.ok()) {
                terminal(s, AccountVideoError::decoder, static_cast<u32>(flushed.native_status));
                return false;
            }
        }
        const auto audio = media_audio_reset(owner, s.playback_epoch, target);
        if (!audio.ok()) {
            terminal(s, AccountVideoError::audio, audio.native_error);
            return false;
        }
        const bool window = pcm_backed(s)
                                ? pcm_window(s, pcm_owner(s))
                                : s.status.kind == AccountPlaybackKind::ogg_opus
                                ? opus_window(s, *static_cast<OpusWorkspace*>(s.workspace))
                                : audio_window(s, *static_cast<Workspace*>(s.workspace));
        if (!window) {
            if (s.status.error == AccountVideoError::none)
                terminal(s, AccountVideoError::timestamp);
            return false;
        }
    }
    return true;
}
void refresh(AccountVideoSession& s) noexcept {
    if (s.status.has_audio) {
        if (auto* owner = audio_owner(s)) {
            const auto a = media_audio_status(owner);
            if (a.error != MediaAudioError::none && s.status.error == AccountVideoError::none)
                terminal(s, AccountVideoError::audio, a.native_error);
            const auto observation = account_video_audio_observation(a, s.playback_epoch);
            const bool current = observation.epoch_ready;
            s.audio_epoch_ready = current;
            s.status.playing = observation.playing;
            s.status.paused = observation.paused;
            s.status.clock_valid = observation.clock_valid;
            s.status.volume_valid = observation.volume_valid;
            s.status.applied_volume = a.applied_volume;
            if (!current)
                s.status.buffering = s.desired_play || s.seeking;
            if (current) {
                const bool ended = observation.ended;
                if (a.clock_valid)
                    s.status.position_100ns =
                        minimum(s.status.duration_100ns, maximum(i64{0}, a.presentation_time_100ns));
                else if (s.seeking)
                    s.status.position_100ns = s.seek_target;
                const bool primed = a.written_frames || !wanted_frames(s);
                const bool native_ready = s.desired_play ? (a.playing && a.clock_valid) || ended : a.paused;
                if (s.seeking && s.video_seek_ready && primed && native_ready) {
                    s.seeking = s.status.seeking = false;
                }
                s.audio_eof = ended;
                s.status.buffering = a.buffering || s.seeking;
                const float wanted = s.status.muted ? 0 : s.status.desired_volume;
                const bool actual_volume = a.volume_valid && a.applied_volume == wanted;
                const bool play_done = s.desired_play ? (a.playing || ended) : a.paused;
                const bool gain_command = s.command_kind == AccountVideoCommand::volume ||
                                          s.command_kind == AccountVideoCommand::mute;
                if (!s.seeking && play_done && (!gain_command || actual_volume) &&
                    s.status.error == AccountVideoError::none)
                    s.status.observed_command = s.status.desired_command;
            }
        } else {
            s.audio_epoch_ready = false;
            s.status.playing = false;
            s.status.paused = true;
            s.status.clock_valid = false;
            s.status.buffering = s.desired_play || s.seeking;
        }
    } else {
        s.status.playing = s.desired_play;
        s.status.paused = !s.desired_play;
        if (s.desired_play && !s.seeking && !data_pending(s)) {
            u64 now{};
            i64 position{};
            if (!qpc(&now) ||
                !account_video_qpc_position(s.qpc_origin, s.qpc_anchor, now, s.qpc_frequency, &position))
                terminal(s, AccountVideoError::clock);
            else {
                s.status.position_100ns = minimum(s.status.duration_100ns, position);
                s.status.clock_valid = true;
            }
        }
        if (s.seeking && s.video_seek_ready) {
            s.seeking = s.status.seeking = false;
            s.qpc_origin = s.seek_target;
            s.status.position_100ns = s.seek_target;
            if (!qpc(&s.qpc_anchor))
                terminal(s, AccountVideoError::clock);
            else
                s.status.clock_valid = true;
        }
        s.status.buffering = s.seeking;
        s.status.volume_valid = false;
        if (!s.seeking && s.status.error == AccountVideoError::none)
            s.status.observed_command = s.status.desired_command;
    }
    if (data_pending(s) && s.status.error == AccountVideoError::none) {
        // Keep the requested transport visible while the network catches up.
        s.status.buffering = true;
        if (s.desired_play) {
            s.status.playing = true;
            s.status.paused = false;
        }
    }
    if (s.video_eof &&
        (!s.status.has_audio ? s.status.position_100ns >= s.status.duration_100ns : s.audio_eof)) {
        s.status.ended = true;
        s.desired_play = s.status.playing = false;
        s.status.buffering = false;
    }
    // Native pause acknowledgement can arrive later. It must not resurrect a
    // spinner for a source that has already failed permanently.
    if (s.status.error != AccountVideoError::none)
        s.status.seeking = s.status.buffering = false;
}
bool write(AccountVideoSession& s, Workspace& w, u32 frames, u32 offset) noexcept {
    const auto value = media_audio_write(&w.audio, s.playback_epoch,
                                         {w.pcm + offset * w.channels, frames * w.channels});
    if (!value.ok()) {
        if (value.error != MediaAudioError::capacity)
            terminal(s, AccountVideoError::audio, value.native_error);
        return false;
    }
    w.queued_frames += frames;
    // Audio-only AAC has no GPU picture acknowledgement. The target becomes
    // ready only after its trimmed PCM (or edit-list silence) reaches the ring.
    if (!s.video) s.video_seek_ready = true;
    return true;
}
bool output(AccountVideoSession& s, Workspace& w) noexcept {
    const auto block = aac_decoder_read(&w.decoder, {reinterpret_cast<u8*>(w.pcm), sizeof(w.pcm)});
    if (block.error == AacDecodeError::need_input) {
        w.reading = false;
        if (w.draining) {
            terminal(s, AccountVideoError::decoder, static_cast<u32>(E_UNEXPECTED));
            return false;
        }
        return true;
    }
    if (block.error == AacDecodeError::end_of_stream) {
        w.decoded_end = true;
        w.reading = false;
        if (w.source_frames < w.window.end_source_frame) {
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
            w.diagnostic.error_site = 1;
#endif
            terminal(s, AccountVideoError::timestamp);
        }
        return s.status.error == AccountVideoError::none;
    }
    if (!block.ok()) {
        terminal(s, AccountVideoError::decoder, static_cast<u32>(block.native_status));
        return false;
    }
    Mp4AudioFrameTimeBounds bounds{};
    const bool mapped = mp4_audio_frame_time_bounds_100ns(w.timeline, w.source_frames, &bounds);
    const i64 expected = bounds.floor_100ns;
    bool matches =
        mapped && account_video_audio_timestamp_within_bounds(block.presentation_time_100ns, bounds);
    if (mapped && !matches && w.preceding_frames && w.preceding_frames <= w.source_frames) {
        i64 preceding{};
        if (mp4_audio_frame_time_100ns(w.timeline, w.source_frames - w.preceding_frames, &preceding))
            matches =
                account_video_audio_timestamp_matches(block.presentation_time_100ns, expected, preceding,
                                                      w.preceding_frames, w.timeline.sample_rate);
    }
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
    auto& d = w.diagnostic;
    ++d.outputs;
    d.source_frames = w.source_frames;
    d.window_first = w.window.first_source_frame;
    d.window_end = w.window.end_source_frame;
    d.native_timestamp = block.presentation_time_100ns;
    d.expected_timestamp = expected;
    d.block_frames = block.frames;
    d.packet_index = w.packet_index;
    d.packet_count = w.packet_count;
    d.stamped = block.timestamp_valid;
    d.mapped = mapped;
#endif
    if (!block.timestamp_valid || !mapped || !matches ||
        block.frames > u64{w.packet_count} * w.frames_per_packet - w.source_frames) {
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
        w.diagnostic.error_site = 2;
#endif
        terminal(s, AccountVideoError::timestamp);
        return false;
    }
    const u64 end = w.source_frames + block.frames,
              first = maximum(w.source_frames, w.window.first_source_frame),
              last = minimum(end, w.window.end_source_frame);
    if (last > first) {
        w.pending_offset = static_cast<u32>(first - w.source_frames);
        w.pending_frames = static_cast<u32>(last - first);
    }
    w.source_frames = end;
    w.preceding_frames = block.frames;
    return true;
}
bool audio_service(AccountVideoSession& s, bool (*cancelled)(void*) noexcept, void* context,
                   bool* interrupted) noexcept {
    if (!workspace(s))
        return false;
    auto& w = *static_cast<Workspace*>(s.workspace);
    if (w.seek_pending) {
        const auto sought = mp4_fragment_audio_seek(s.fragmented_audio, w.packet_index);
        if (sought == Mp4DemuxError::data_pending) { set_pending(s, &s.audio_pending, true); return false; }
        if (sought != Mp4DemuxError::none) { terminal(s, AccountVideoError::demux, static_cast<u32>(sought)); return false; }
        w.seek_pending = false; set_pending(s, &s.audio_pending, false);
    }
    u32 packets{}, calls{}, writes{};
    while (packets < 8 && calls < 24 && writes < 4 && s.status.error == AccountVideoError::none) {
        if (cancelled && cancelled(context)) {
            *interrupted = true;
            return true;
        }
        const auto a = media_audio_status(&w.audio);
        if (a.error != MediaAudioError::none) {
            terminal(s, AccountVideoError::audio, a.native_error);
            break;
        }
        if (w.pending_frames) {
            if (!a.free_frames)
                return false;
            const u32 frames = static_cast<u32>(minimum<u64>(
                minimum(kMediaAudioMaximumWriteFrames, minimum(a.free_frames, w.pending_frames)),
                w.wanted_frames - w.queued_frames));
            if (!frames) {
                w.pending_frames = 0;
                continue;
            }
            if (!write(s, w, frames, w.pending_offset))
                return false;
            w.pending_offset += frames;
            w.pending_frames -= frames;
            ++writes;
            continue;
        }
        if (w.silence_frames || (w.decoded_end && w.queued_frames < w.wanted_frames)) {
            if (!a.free_frames)
                return false;
            const u64 remaining = w.silence_frames ? w.silence_frames : w.wanted_frames - w.queued_frames;
            const u32 frames = static_cast<u32>(
                minimum<u64>(minimum(a.free_frames, kMediaAudioMaximumWriteFrames), remaining));
            for (u32 i = 0; i < frames * w.channels; ++i)
                w.pcm[i] = 0;
            if (!write(s, w, frames, 0))
                return false;
            if (w.silence_frames)
                w.silence_frames -= frames;
            ++writes;
            continue;
        }
        if (w.decoded_end) {
            if (s.status.live && !mp4_decoder_movie(*s.video).duration_complete) {
                set_pending(s, &s.audio_pending, true);
                return false;
            }
            if (!w.sealed) {
                const auto value = media_audio_end(&w.audio, s.playback_epoch);
                if (!value.ok()) {
                    terminal(s, AccountVideoError::audio, value.native_error);
                    break;
                }
                w.sealed = true;
                if (!s.video) s.video_seek_ready = true;
            }
            return false;
        }
        if (w.reading) {
            ++calls;
            if (!output(s, w))
                break;
            continue;
        }
        if (u64{w.packet_index} * w.frames_per_packet >= w.window.end_source_frame ||
            w.packet_index == w.packet_count) {
            if (s.status.live && s.fragmented_audio) {
                Mp4FragmentAudioPacket packet{};
                const auto edge = mp4_fragment_audio_peek(s.fragmented_audio, &packet);
                if (edge == Mp4DemuxError::data_pending) { set_pending(s, &s.audio_pending, true); return false; }
                if (edge != Mp4DemuxError::end_of_stream) { terminal(s, AccountVideoError::demux, static_cast<u32>(edge)); break; }
                set_pending(s, &s.audio_pending, false);
            }
            if (s.fragmented_audio && mp4_fragment_audio_seek(s.fragmented_audio, w.packet_count) != Mp4DemuxError::none) {
                terminal(s, AccountVideoError::demux); break;
            }
            const auto value = aac_decoder_drain(&w.decoder);
            if (!value.ok()) {
                terminal(s, AccountVideoError::decoder, static_cast<u32>(value.native_status));
                break;
            }
            w.draining = w.reading = true;
            ++calls;
            continue;
        }
        const auto before = w.demux;
        ConstSlice<u8> bytes{}; i64 start{}; u64 duration{};
        Mp4DemuxError error{};
        if (s.fragmented_audio) {
            Mp4FragmentAudioPacket packet{};
            error = mp4_fragment_audio_peek(s.fragmented_audio, &packet);
            if (error == Mp4DemuxError::none && packet.first_source_frame != u64{w.packet_index} * w.frames_per_packet)
                error = Mp4DemuxError::malformed;
            bytes = packet.bytes; start = packet.presentation_time_100ns; duration = packet.duration_100ns;
        } else {
            const auto unit = mp4_audio_demux_next(&w.demux);
            error = unit.error; bytes = unit.bytes;
            if (unit.ok() && !mp4_demux_available(before.samples, unit.bytes)) error = Mp4DemuxError::data_pending;
            i64 end{};
            if (error == Mp4DemuxError::none && (unit.sample_index != w.packet_index ||
                !mp4_audio_frame_time_100ns(w.timeline, u64{w.packet_index} * w.frames_per_packet, &start) ||
                !mp4_audio_frame_time_100ns(w.timeline, (u64{w.packet_index} + 1) * w.frames_per_packet, &end) || end <= start))
                error = Mp4DemuxError::unsupported;
            if (error == Mp4DemuxError::none) duration = static_cast<u64>(end) - static_cast<u64>(start);
        }
        if (error == Mp4DemuxError::data_pending) {
            w.demux = before; set_pending(s, &s.audio_pending, true); return false;
        }
        if (s.audio_pending) set_pending(s, &s.audio_pending, false);
        if (error != Mp4DemuxError::none || !duration) {
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
            w.diagnostic.error_site = 3;
#endif
            terminal(s, AccountVideoError::timestamp); break;
        }
        const auto value = aac_decoder_submit(&w.decoder, bytes, start, duration);
        if (value.error == AacDecodeError::need_output) {
            w.demux = before;
            w.reading = true;
            ++calls;
            if (++w.input_retries > 2) {
                terminal(s, AccountVideoError::decoder, static_cast<u32>(value.native_status));
                break;
            }
            continue;
        }
        if (!value.ok()) {
            terminal(s, AccountVideoError::decoder, static_cast<u32>(value.native_status));
            break;
        }
        if (s.fragmented_audio && !mp4_fragment_audio_commit(s.fragmented_audio)) {
            terminal(s, AccountVideoError::demux); break;
        }
        ++w.packet_index;
        w.input_retries = 0;
        w.reading = true;
        ++packets;
    }
    return s.status.error == AccountVideoError::none;
}
bool pcm_service(AccountVideoSession& s, bool (*cancelled)(void*) noexcept, void* context,
                 bool* interrupted) noexcept {
    if (!workspace(s))
        return false;
    auto& w = pcm_owner(s);
    const u32 channels = encoded_audio_channels(*s.encoded_metadata);
    u32 blocks{}, writes{};
    while (blocks < 8 && writes < 4 && s.status.error == AccountVideoError::none) {
        if (cancelled && cancelled(context)) {
            *interrupted = true;
            return true;
        }
        const auto audio = media_audio_status(&w.audio);
        if (audio.error != MediaAudioError::none) {
            terminal(s, AccountVideoError::audio, audio.native_error);
            break;
        }
        if (w.pending_frames) {
            if (!audio.free_frames)
                return false;
            const u32 frames = minimum(audio.free_frames, w.pending_frames);
            const auto written = media_audio_write(&w.audio, s.playback_epoch,
                {w.pcm + w.pending_offset * channels, frames * channels});
            if (!written.ok()) {
                if (written.error != MediaAudioError::capacity)
                    terminal(s, AccountVideoError::audio, written.native_error);
                return false;
            }
            w.queued_frames += frames;
            w.pending_offset += frames;
            w.pending_frames -= frames;
            s.video_seek_ready = true;
            ++writes;
            continue;
        }
        if (w.decoded_end) {
            if (w.queued_frames != w.wanted_frames) {
                terminal(s, AccountVideoError::timestamp);
                break;
            }
            if (!w.sealed) {
                const auto ended = media_audio_end(&w.audio, s.playback_epoch);
                if (!ended.ok()) {
                    terminal(s, AccountVideoError::audio, ended.native_error);
                    break;
                }
                w.sealed = true;
                s.video_seek_ready = true;
            }
            return false;
        }
        if (s.source_cancelled && s.source_cancelled(s.source_cancel_context)) {
            *interrupted = true;
            return true;
        }
        if (s.status.kind == AccountPlaybackKind::mp3) {
            auto& mp3 = *static_cast<Mp3Workspace*>(s.workspace);
            const auto before = mp3.demux;
            const auto packet = mp3_stream_next(&mp3.demux);
            if (packet.error == Mp3Error::end_of_stream) {
                w.decoded_end = true;
                continue;
            }
            if (!packet.ok() || !packet.duration_frames || packet.duration_frames > kMp3MaximumFrames ||
                packet.trim_front_frames > packet.duration_frames ||
                packet.trim_back_frames > packet.duration_frames - packet.trim_front_frames ||
                packet.decoded_first_frame > ~u64{0} - packet.duration_frames) {
                terminal(s, AccountVideoError::demux, static_cast<u32>(packet.error));
                break;
            }
            if (cancelled && cancelled(context)) {
                mp3.demux = before;
                *interrupted = true;
                return true;
            }
            const auto decoded = mp3_decoder_decode(&mp3.decoder, packet.bytes,
                                                     {w.pcm, kMediaAudioMaximumWriteFrames * 2});
            ++blocks;
            const u32 frames = packet.duration_frames - packet.trim_front_frames - packet.trim_back_frames;
            if (decoded.error == Mp3Error::need_preroll && !frames) {
                w.source_frames = packet.decoded_first_frame + packet.duration_frames;
                continue;
            }
            if (!decoded.ok() || decoded.frames != packet.duration_frames) {
                terminal(s, AccountVideoError::decoder, static_cast<u32>(decoded.error));
                break;
            }
            if (frames > w.wanted_frames - minimum(w.queued_frames, w.wanted_frames)) {
                terminal(s, AccountVideoError::timestamp);
                break;
            }
            w.pending_offset = packet.trim_front_frames;
            w.pending_frames = frames;
            w.source_frames = packet.decoded_first_frame + packet.duration_frames;
            continue;
        }
        const auto& source = s.encoded_metadata->wave;
        const auto decoded = wave_pcm_decode(source, w.source_frames, kMediaAudioMaximumWriteFrames,
                                              {w.pcm, kMediaAudioMaximumWriteFrames * 2});
        ++blocks;
        if (!decoded.ok() || !decoded.frames || decoded.frames > source.total_frames - w.source_frames) {
            terminal(s, AccountVideoError::decoder, static_cast<u32>(decoded.error));
            break;
        }
        w.pending_offset = 0;
        w.pending_frames = decoded.frames;
        w.source_frames += decoded.frames;
        w.decoded_end = w.source_frames == source.total_frames;
    }
    return s.status.error == AccountVideoError::none;
}
bool opus_service(AccountVideoSession& s, bool (*cancelled)(void*) noexcept, void* context,
                  bool* interrupted) noexcept {
    if (!workspace(s))
        return false;
    auto& w = *static_cast<OpusWorkspace*>(s.workspace);
    u32 packets{}, writes{};
    while (packets < 8 && writes < 4 && s.status.error == AccountVideoError::none) {
        if (cancelled && cancelled(context)) {
            *interrupted = true;
            return true;
        }
        const auto audio = media_audio_status(&w.audio);
        if (audio.error != MediaAudioError::none) {
            terminal(s, AccountVideoError::audio, audio.native_error);
            break;
        }
        if (w.pending_frames) {
            if (!audio.free_frames)
                return false;
            const u32 frames =
                minimum(kMediaAudioMaximumWriteFrames, minimum(audio.free_frames, w.pending_frames));
            const auto written =
                media_audio_write(&w.audio, s.playback_epoch,
                                  {w.pcm + w.pending_offset * w.demux.channels, frames * w.demux.channels});
            if (!written.ok()) {
                if (written.error != MediaAudioError::capacity)
                    terminal(s, AccountVideoError::audio, written.native_error);
                return false;
            }
            w.queued_frames += frames;
            w.pending_offset += frames;
            w.pending_frames -= frames;
            s.video_seek_ready = true;
            ++writes;
            continue;
        }
        if (w.decoded_end) {
            if (w.queued_frames != w.wanted_frames) {
                terminal(s, AccountVideoError::timestamp);
                break;
            }
            if (!w.sealed) {
                const auto ended = media_audio_end(&w.audio, s.playback_epoch);
                if (!ended.ok()) {
                    terminal(s, AccountVideoError::audio, ended.native_error);
                    break;
                }
                w.sealed = true;
                s.video_seek_ready = true;
            }
            return false;
        }
        const auto before = w.demux;
        const auto packet = ogg_opus_demux_next(&w.demux, {w.packet, sizeof(w.packet)});
        if (packet.error == OggOpusError::end_of_stream) {
            w.decoded_end = true;
            continue;
        }
        if (!packet.ok() || packet.duration_frames > kOpusDecoderMaximumFrames ||
            packet.trim_front_frames > packet.duration_frames ||
            packet.trim_back_frames > packet.duration_frames - packet.trim_front_frames ||
            packet.decoded_first_frame > ~u64{0} - packet.duration_frames) {
            terminal(s, AccountVideoError::demux, static_cast<u32>(packet.error));
            break;
        }
        if (cancelled && cancelled(context)) {
            w.demux = before;
            *interrupted = true;
            return true;
        }
        const auto decoded =
            opus_decoder_decode(&w.decoder, packet.bytes, {w.pcm16, kOpusDecoderMaximumFrames * 2});
        ++packets;
        if (!decoded.ok() || decoded.frames != packet.duration_frames) {
            terminal(s, AccountVideoError::decoder, static_cast<u32>(decoded.native_status));
            break;
        }
        for (u32 i = 0; i < decoded.frames * w.demux.channels; ++i)
            w.pcm[i] = w.pcm16[i] / 32768.0f;
        const u64 first = maximum(w.first_frame, packet.decoded_first_frame + packet.trim_front_frames);
        const u64 last = minimum(w.end_frame, packet.decoded_first_frame + packet.duration_frames -
                                                  packet.trim_back_frames);
        if (last > first) {
            w.pending_offset = static_cast<u32>(first - packet.decoded_first_frame);
            w.pending_frames = static_cast<u32>(last - first);
        }
        w.source_frames = packet.decoded_first_frame + packet.duration_frames;
    }
    return s.status.error == AccountVideoError::none;
}
} // namespace
AccountVideoAudioObservation account_video_audio_observation(const MediaAudioStatus& a,
                                                             u64 epoch) noexcept {
    const bool current = epoch && a.observed_epoch == epoch;
    return {current,
            current && a.playing,
            current && a.paused,
            current && a.clock_valid,
            current && a.volume_valid,
            current && a.ended && !a.playing && a.consumed_frames == a.written_frames};
}
bool account_video_audio_should_play(bool desired, bool seeking, bool ready) noexcept {
    return desired && (!seeking || ready);
}
bool account_video_audio_timestamp_within_bounds(i64 native,
                                                 const Mp4AudioFrameTimeBounds& bounds) noexcept {
    const i64 floor = bounds.floor_100ns, ceil = bounds.ceil_100ns;
    return ceil >= floor && (ceil == floor || (floor < kLimit && ceil == floor + 1)) &&
           (native == floor || native == ceil);
}
bool account_video_audio_timestamp_matches(i64 native, i64 exact, i64 preceding, u32 frames,
                                           u32 rate) noexcept {
    if (native == exact)
        return true;
    if (!frames || frames > kAacDecoderMaximumOutputBytes / 4 || rate < 8000 || rate > 48000)
        return false;
    const i64 duration = static_cast<i64>(u64{frames} * 10000000 / rate);
    if (preceding > kLimit - duration)
        return false;
    const i64 decomposed = preceding + duration;
    // floor(a+b)-floor(a)-floor(b) is exactly0 or1, including signed a.
    // Require the actual preceding source interval's decomposition; accepting
    // +/-1 generally would hide unrelated offset or repeated-rounding drift.
    return decomposed < kLimit && exact == decomposed + 1 && native == decomposed;
}
AccountVideoFrameDue account_video_frame_due(i64 pts, u64 duration, i64 position) noexcept {
    if (position < 0 || !duration || duration > static_cast<u64>(kLimit) ||
        (pts >= 0 && duration > static_cast<u64>(kLimit - pts)))
        return AccountVideoFrameDue::invalid;
    const i64 end = pts + static_cast<i64>(duration);
    if (end <= position)
        return AccountVideoFrameDue::expired;
    if (pts > position)
        return AccountVideoFrameDue::future;
    return AccountVideoFrameDue::ready;
}
bool account_video_qpc_position(i64 origin, u64 anchor, u64 now, u64 frequency, i64* position) noexcept {
    if (!position || origin < 0 || now < anchor)
        return false;
    u64 elapsed{};
    if (!media_audio_clock_time(now - anchor, frequency, &elapsed) ||
        elapsed > static_cast<u64>(kLimit - origin))
        return false;
    *position = origin + static_cast<i64>(elapsed);
    return true;
}
AccountVideoResult account_video_session_initialize(AccountVideoSession* s, Mp4Decoder* video,
                                                    const AccountVideoIdentity& id, HANDLE event,
                                                    Mp4SourceAvailability audio_availability, Mp4FragmentAudio* fragmented_audio) noexcept {
    if (!s || s->initialized || s->workspace || !video || !video->ready || !identity_valid(id) || !event)
        return result(AccountVideoError::invalid_input);
    const auto source = mp4_decoder_source(*video);
    const auto movie = mp4_decoder_movie(*video);
    if (!source.data || !source.count || !movie.duration_us || !movie.first_presentation_valid ||
        movie.duration_us > static_cast<u64>(kLimit) / 10)
        return result(AccountVideoError::invalid_input);
    Mp4AudioDemuxer audio{};
    if (fragmented_audio && (!fragmented_audio->source || !mp4_fragment_source_valid(*fragmented_audio->source) ||
        !fragmented_audio->source->track.audio || !fragmented_audio->packet_count ||
        fragmented_audio->packet_index > fragmented_audio->packet_count))
        return result(AccountVideoError::invalid_input);
    auto error = fragmented_audio ? Mp4DemuxError::none : mp4_audio_demux_initialize(&audio, source, audio_availability);
    // A codec without a decoder here (ALAC, AC-3, LPCM, spatial APAC) plays
    // the picture silently, as a browser does; a broken audio table still fails.
    if (error == Mp4DemuxError::unsupported)
        error = Mp4DemuxError::no_audio;
    if (error != Mp4DemuxError::none && error != Mp4DemuxError::no_audio)
        return result(AccountVideoError::unsupported_audio, static_cast<u32>(error));
    i64 duration = static_cast<i64>(movie.duration_us * 10);
    if (error == Mp4DemuxError::none) {
        Mp4AudioSampleWindow window{};
        if (!(fragmented_audio ? mp4_audio_sample_window(fragmented_audio->timeline, 0, &window)
                               : mp4_audio_demux_sample_window(audio, 0, &window)))
            return result(AccountVideoError::timestamp);
        duration = maximum(duration, window.duration_100ns);
    }
    Win32LargeInteger frequency{};
    if (!QueryPerformanceFrequency(frequency.native()) || frequency.QuadPart <= 0)
        return result(AccountVideoError::clock);
    const auto identity = id;
    *s = {};
    s->video = video;
    s->lane_event = event;
    s->audio_metadata = audio;
    s->fragmented_audio = fragmented_audio;
    s->status.identity = identity;
    s->status.duration_100ns = duration;
    s->status.has_audio = error == Mp4DemuxError::none;
    s->status.live = movie.live;
    s->status.playback_epoch = s->playback_epoch = 1;
    s->qpc_frequency = static_cast<u64>(frequency.QuadPart);
    s->thread_id = GetCurrentThreadId();
    s->initialized = true;
    return result(AccountVideoError::none, 0, true, true);
}
AccountVideoResult account_video_session_initialize_fragment_audio(AccountVideoSession* s, Mp4FragmentAudio* audio,
    const AccountVideoIdentity& id, HANDLE event) noexcept {
    if (!s || s->initialized || s->workspace || !audio || !audio->source || !identity_valid(id) || !event ||
        !mp4_fragment_source_valid(*audio->source) || !audio->source->track.audio ||
        (audio->source->segments && audio->source->segments->live) || !audio->packet_count ||
        audio->packet_index > audio->packet_count)
        return result(AccountVideoError::invalid_input);
    Mp4AudioSampleWindow window{};
    if (!mp4_audio_sample_window(audio->timeline, 0, &window) || window.duration_100ns <= 0)
        return result(AccountVideoError::timestamp);
    Win32LargeInteger frequency{};
    if (!QueryPerformanceFrequency(frequency.native()) || frequency.QuadPart <= 0)
        return result(AccountVideoError::clock);
    const auto identity = id;
    *s = {};
    s->fragmented_audio = audio;
    s->lane_event = event;
    s->status.identity = identity;
    s->status.duration_100ns = window.duration_100ns;
    s->status.has_audio = true;
    s->status.playback_epoch = s->playback_epoch = 1;
    s->video_eof = true;
    s->qpc_frequency = static_cast<u64>(frequency.QuadPart);
    s->thread_id = GetCurrentThreadId();
    s->initialized = true;
    return result(AccountVideoError::none, 0, true, true);
}
AccountVideoResult account_video_session_initialize_audio(AccountVideoSession* s,
                                                          const EncodedAudioMetadata* metadata,
                                                          const AccountVideoIdentity& id,
                                                          HANDLE event) noexcept {
    u64 duration{};
    if (!s || s->initialized || s->workspace || !metadata || !encoded_audio_ready(*metadata) ||
        !encoded_audio_bytes(*metadata).data || !encoded_audio_bytes(*metadata).count ||
        (encoded_audio_channels(*metadata) != 1 && encoded_audio_channels(*metadata) != 2) ||
        encoded_audio_rate(*metadata) < 8000 || encoded_audio_rate(*metadata) > 192000 ||
        !identity_valid(id) || !event ||
        !media_audio_clock_time(encoded_audio_total_frames(*metadata), encoded_audio_rate(*metadata),
                                &duration) || duration > static_cast<u64>(kLimit))
        return result(AccountVideoError::invalid_input);
    AccountPlaybackKind kind{};
    switch (metadata->format) {
    case MediaAttachmentFormat::ogg_opus: kind = AccountPlaybackKind::ogg_opus; break;
    case MediaAttachmentFormat::mp3: kind = AccountPlaybackKind::mp3; break;
    case MediaAttachmentFormat::wav: kind = AccountPlaybackKind::wave_pcm; break;
    case MediaAttachmentFormat::m4a: kind = AccountPlaybackKind::mp4; break;
    default: return result(AccountVideoError::invalid_input);
    }
    Win32LargeInteger frequency{};
    if (kind == AccountPlaybackKind::mp4 && (!QueryPerformanceFrequency(frequency.native()) || frequency.QuadPart <= 0))
        return result(AccountVideoError::clock);
    const auto identity = id;
    *s = {};
    s->encoded_metadata = metadata;
    s->lane_event = event;
    s->status.identity = identity;
    s->status.kind = kind;
    s->status.duration_100ns = static_cast<i64>(duration);
    s->status.has_audio = true;
    s->status.playback_epoch = s->playback_epoch = 1;
    s->video_eof = true;
    if (kind == AccountPlaybackKind::mp4) {
        // No picture decoder: the AAC workspace and its sample window drive playback.
        s->audio_metadata = metadata->m4a;
        s->qpc_frequency = static_cast<u64>(frequency.QuadPart);
    }
    s->thread_id = GetCurrentThreadId();
    s->initialized = true;
    return result(AccountVideoError::none, 0, true, true);
}
AccountVideoResult account_video_session_control(AccountVideoSession* s, const AccountVideoIdentity& id,
                                                 u64 serial, AccountVideoCommand command, i64 target,
                                                 float volume, bool muted) noexcept {
    if (!s || !thread(*s) || !serial ||
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
    if (s && thread(*s) && s->status.error == AccountVideoError::none)
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
    if (!s || !thread(*s) || !account_video_identity_equal(s->status.identity, id) ||
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
    return s && thread(*s) ? s->status : AccountVideoStatus{};
}
AccountVideoAudioProgress account_video_session_audio_progress(const AccountVideoSession* s) noexcept {
    if (!s || !thread(*s) || !s->workspace)
        return {};
    const auto* owner = audio_owner(*s);
    const auto a = media_audio_status(owner);
    if (a.observed_epoch != s->playback_epoch)
        return {};
    return {a.observed_epoch, a.written_frames, a.submitted_frames, a.consumed_frames};
}
AccountVideoResult account_video_session_release(AccountVideoSession* s, u32 timeout) noexcept {
    if (!s)
        return result(AccountVideoError::invalid_input);
    if (!s->initialized && !s->workspace) {
        *s = {};
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
        if (!VirtualFree(s->workspace, 0, MEM_RELEASE))
            return result(AccountVideoError::release, GetLastError(), true);
    }
    *s = {};
    return result(AccountVideoError::none, 0, true, true);
}
u32 account_video_session_owned_bytes(const AccountVideoSession* s) noexcept {
    if (!s || !s->workspace)
        return 0;
    if (s->status.kind == AccountPlaybackKind::wave_pcm)
        return sizeof(PcmWorkspace);
    if (s->status.kind == AccountPlaybackKind::mp3)
        return static_cast<const Mp3Workspace*>(s->workspace)->allocation_bytes;
    return s->status.kind == AccountPlaybackKind::ogg_opus
               ? static_cast<const OpusWorkspace*>(s->workspace)->allocation_bytes
               : sizeof(Workspace);
}
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
AccountVideoTimestampDiagnostic account_video_session_timestamp_diagnostic(
    const AccountVideoSession* s) noexcept {
    if (!s || !thread(*s) || !s->workspace || standalone(*s))
        return {};
    return static_cast<const Workspace*>(s->workspace)->diagnostic;
}
AccountVideoProbeSnapshot account_video_session_probe_snapshot(const AccountVideoSession* s) noexcept {
    if (!s || !thread(*s))
        return {};
    AccountVideoProbeSnapshot out{};
    out.session = s->status;
    out.desired_play = s->desired_play;
    out.video_eof = s->video_eof;
    out.held = s->held;
    out.published = s->published;
    out.seeking = s->seeking;
    out.audio_epoch_ready = s->audio_epoch_ready;
    if (s->workspace && pcm_backed(*s)) {
        const auto& w = pcm_owner(*s);
        out.audio = media_audio_status(&w.audio);
        out.source_frames = w.source_frames;
        out.queued_frames = w.queued_frames;
        out.wanted_frames = w.wanted_frames;
        out.pending_frames = w.pending_frames;
        out.decoded_end = w.decoded_end;
        out.sealed = w.sealed;
        out.requested_play = w.requested_play;
        return out;
    }
    if (s->workspace && s->status.kind == AccountPlaybackKind::ogg_opus) {
        const auto& w = *static_cast<const OpusWorkspace*>(s->workspace);
        out.audio = media_audio_status(&w.audio);
        out.source_frames = w.source_frames;
        out.queued_frames = w.queued_frames;
        out.wanted_frames = w.wanted_frames;
        out.pending_frames = w.pending_frames;
        out.decoded_end = w.decoded_end;
        out.sealed = w.sealed;
        out.requested_play = w.requested_play;
        return out;
    }
    if (const auto* w = static_cast<const Workspace*>(s->workspace)) {
        out.audio = media_audio_status(&w->audio);
        out.source_frames = w->source_frames;
        out.queued_frames = w->queued_frames;
        out.wanted_frames = w->wanted_frames;
        out.pending_frames = w->pending_frames;
        out.reading = w->reading;
        out.draining = w->draining;
        out.decoded_end = w->decoded_end;
        out.sealed = w->sealed;
        out.requested_play = w->requested_play;
    }
    return out;
}
#endif
} // namespace maicord
