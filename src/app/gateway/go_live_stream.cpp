#include "maicord/app/gateway/go_live_stream.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/media/audio/opus_encoder.hpp"
#include "maicord/platform/crypto/random.hpp"

namespace maicord {
namespace {
constexpr u32 kSlots = kGoLiveUnitQueue; // Encoded units waiting for the worker.
constexpr u32 kAudioPackets = 8;   // Opus packets waiting for the worker (160 ms).
constexpr u32 kStageBytes = kGoLiveFrameBytes + kGoLiveFrameBytes / 4 + 4;
constexpr u32 kSoundshare = 2;     // Speaking flag of stream audio.
}

struct GoLiveSlot {
    u32 offset, bytes, rtp_timestamp; // offset: in GoLiveMedia::units
    GpuStreamCodec codec;
    bool keyframe;
};
struct GoLiveOpusPacket {
    u32 frame;
    u16 bytes;
    u8 data[kOpusEncoderMaximumPacketBytes];
};
struct GoLiveMedia {
    SrwLock lock;
    void (*wake)(void* context) noexcept;
    void* wake_context;
    u8* units; // kGoLiveUnitRingBytes (go_live_unit_place)
    GoLiveSlot slots[kSlots];
    u32 slot_head, slot_count, dropped;
    GoLiveEnd ended;
    bool minimized, minimized_changed;
    u32 width, height;
    // Stream audio (one producer thread): framing and Opus.
    GoLiveOpusPacket audio[kAudioPackets];
    u32 audio_head, audio_count, audio_frame, pcm_count;
    i16 pcm[kOpusEncoderFrameSamples * 2];
    OpusEncoder opus;
};

namespace {
void wake(GoLiveMedia* media) noexcept {
    if (media->wake)
        media->wake(media->wake_context);
}
u64 microseconds() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}
constexpr GpuStreamCodec kCodecs[]{GpuStreamCodec::h264, GpuStreamCodec::h265, GpuStreamCodec::av1};
VoiceVideoCodec voice_codec(GpuStreamCodec codec) noexcept {
    return codec == GpuStreamCodec::av1    ? VoiceVideoCodec::av1
           : codec == GpuStreamCodec::h265 ? VoiceVideoCodec::h265
                                           : VoiceVideoCodec::h264;
}
u32 voice_bits(u32 encodable) noexcept {
    u32 bits = 0;
    for (const auto codec : kCodecs)
        if (encodable & gpu_stream_codec_bit(codec))
            bits |= voice_video_codec_bit(voice_codec(codec));
    return bits;
}
bool active(const GatewayGoLive& live) noexcept {
    return live.state != GoLiveState::idle && live.state != GoLiveState::ended;
}
StreamBitrate bounds(const GoLiveSettings& settings) noexcept {
    return stream_quality_bitrate({settings.resolution, settings.fps});
}

// Once per stream (buffers, teardown, end reasons), compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
GoLiveMedia* media_create(GatewayGoLive& live) noexcept {
    auto* block = static_cast<u8*>(pages_allocate(sizeof(GoLiveMedia) + kGoLiveUnitRingBytes));
    if (!block)
        return nullptr;
    auto* media = reinterpret_cast<GoLiveMedia*>(block);
    media->units = block + sizeof(GoLiveMedia);
    media->wake = live.wake;
    media->wake_context = live.wake_context;
    OpusEncoderProfile profile;
    profile.bitrate = kGoLiveAudioBitrate;
    profile.channels = 2;
    opus_encoder_start(&media->opus, profile);
    return media;
}
void media_release(GoLiveMedia* media) noexcept {
    if (media)
        pages_release(media);
}

void stop_producers(GatewayGoLive& live) noexcept {
    if (live.capturing)
        go_live_producer_capture_stop(live.backend_context);
    if (live.sounding)
        go_live_producer_sound_stop(live.backend_context);
    if (live.encoding)
        go_live_producer_encoder_stop(live.backend_context);
    live.capturing = live.sounding = live.encoding = false;
}
void free_buffers(GatewayGoLive& live) noexcept {
    media_release(live.media);
    live.media = nullptr;
    if (live.seal)
        pages_release(live.seal);
    live.seal = nullptr;
    go_live_sender_stop(&live.sender);
}
void end_session(GatewayGoLive& live, GoLiveEnd end) noexcept {
    stop_producers(live);
    free_buffers(live);
    voice_connection_stop(&live.connection);
    live.media_revision = 0;
    live.state = GoLiveState::ended;
    live.end = end;
    live.pump_due_ms = 0;
}
// Leaves the stream (the server hears of it) and ends the session.
void stop_session(GatewayGoLive& live, GoLiveEnd end) noexcept {
    gateway_stream_stop(&live.control, GatewayStreamEnd::stopped);
    end_session(live, end);
}
GoLiveEnd end_of(const GatewayStreamControl& control) noexcept {
    switch (control.end) {
    case GatewayStreamEnd::stopped:
        return GoLiveEnd::stopped;
    case GatewayStreamEnd::deleted:
        return control.delete_reason == GatewayStreamDeleteReason::stream_full    ? GoLiveEnd::stream_full
               : control.delete_reason == GatewayStreamDeleteReason::unauthorized ? GoLiveEnd::not_allowed
                                                                                   : GoLiveEnd::deleted;
    case GatewayStreamEnd::left_channel:
        return GoLiveEnd::left_channel;
    case GatewayStreamEnd::timeout:
        return GoLiveEnd::timeout;
    default:
        return GoLiveEnd::connection_failed;
    }
}
#pragma clang attribute pop
GpuStreamEncoderSettings encoder_settings(const GatewayGoLive& live) noexcept {
    const auto extent = stream_encode_extent(live.settings.resolution, live.capture_width, live.capture_height);
    return {extent.width, extent.height, live.settings.fps,
            live.sender.history ? live.sender.bandwidth.target : bounds(live.settings).start};
}
void apply_pause(GatewayGoLive& live) noexcept {
    const bool paused = live.user_paused || live.minimized;
    if (paused == live.paused)
        return;
    live.paused = paused;
    gateway_stream_set_paused(&live.control, paused);
    if (!paused)
        live.awaiting_keyframe = live.keyframes.wanted = true;
}
// The process stream audio follows: the shared window's, none for a screen (every process).
u32 sound_process(const CaptureSource& source) noexcept {
    return source.kind == CaptureSourceKind::window ? source.process_id : 0;
}
void start_sound(GatewayGoLive& live) noexcept {
    live.sounding = live.backend_context && go_live_producer_sound(live.backend_context, live.media, live.sound_process);
}

// RTCP from the stream server (receiver reports, NACK, PLI/FIR, REMB, transport feedback).
bool receive(void* context, ConstSlice<u8> packet, const VoiceReceivedPacket& received) noexcept {
    auto& live = *static_cast<GatewayGoLive*>(context);
    if (received.transport.layout.kind == MediaPacketKind::rtcp && live.sender.history)
        go_live_sender_rtcp(&live.sender, packet, microseconds());
    return true;
}
VoiceUdpMediaSend send_packet(void* context, ConstSlice<u8> packet) noexcept {
    auto& live = *static_cast<GatewayGoLive*>(context);
    return voice_connection_media_send(&live.connection, live.media_revision, packet);
}

// The codec the server asked us to send, or the best common one without an answer.
bool negotiated(const GatewayGoLive& live, GpuStreamCodec* codec) noexcept {
    switch (live.connection.media.video_codec) {
    case VoiceVideoCodec::h264: *codec = GpuStreamCodec::h264; break;
    case VoiceVideoCodec::h265: *codec = GpuStreamCodec::h265; break;
    case VoiceVideoCodec::av1: *codec = GpuStreamCodec::av1; break;
    default: {
        u32 offered = 0;
        for (const auto candidate : kCodecs)
            if (live.connection.media.video_offer & voice_video_codec_bit(voice_codec(candidate)))
                offered |= gpu_stream_codec_bit(candidate);
        return gpu_stream_encoder_pick(live.encodable, offered, codec);
    }
    }
    return (live.encodable & gpu_stream_codec_bit(*codec)) != 0;
}
// (Re)creates the encoder for the negotiated codec once the picture size is known.
bool ensure_encoder(GatewayGoLive& live) noexcept {
    GpuStreamCodec codec{};
    if (!negotiated(live, &codec))
        return false;
    if (live.encoding && codec == live.codec)
        return true;
    if (!live.capture_width || !live.capture_height || live.sender.frame_active)
        return true; // Not yet: no picture, or the old codec's frame is still on the wire.
    if (live.encoding)
        go_live_producer_encoder_stop(live.backend_context);
    live.encoding = live.backend_context &&
                    go_live_producer_encoder(live.backend_context, live.media, codec, encoder_settings(live));
    if (!live.encoding)
        return false;
    live.codec = codec;
    live.encoder_held = false; // A new encoder encodes until live_media holds it.
    live.bitrate_applied = encoder_settings(live).bitrate;
    const u8 payload = voice_connection_video_payload_type(live.connection, voice_codec(codec));
    live.sender.streams.codec = static_cast<RtpVideoPacketCodec>(codec);
    live.sender.streams.video_payload_type = payload;
    live.sender.streams.rtx_payload_type = static_cast<u8>(payload + 1);
    live.awaiting_keyframe = true;
    return payload != 0;
}
bool attach(GatewayGoLive& live, u64 now_us) noexcept {
    auto& media = live.connection.media;
    if (!media.video_ssrc)
        return false; // The server assigned no video stream.
    if (!voice_connection_media_start(&live.connection, {live.receive_buffer, sizeof(live.receive_buffer)},
                                      {live.send_buffer, sizeof(live.send_buffer)}, receive, &live,
                                      kVoiceUdpMediaReceiveSlots))
        return true; // Not yet current; the next drive retries.
    const GoLiveSendStreams streams{media.ssrc, media.video_ssrc, media.video_rtx_ssrc, 0, 0, RtpVideoPacketCodec::h264};
    if (!go_live_sender_start(&live.sender, streams, bounds(live.settings), now_us))
        return false;
    live.media_revision = live.connection.ready_revision;
    live.announced = {};
    live.speaking_sent = 0;
    // A new connection renegotiated the codec: the encoder starts again for it.
    if (live.encoding)
        go_live_producer_encoder_stop(live.backend_context);
    live.encoding = false;
    return true;
}
// Op 5 then op 12 whenever they changed, one per idle socket.
void signal(GatewayGoLive& live, u64 now_ms) noexcept {
    auto& connection = live.connection;
    if (connection.socket.sending != VoiceSocketOutput::none || !connection.identify_submitted)
        return;
    const u32 flags = live.sounding && !live.paused ? kSoundshare : 0;
    if (live.speaking_sent != flags + 1) {
        if (voice_connection_speaking(&connection, flags, now_ms) == VoiceSocketSend::accepted)
            live.speaking_sent = flags + 1;
        return;
    }
    const auto extent = stream_encode_extent(live.settings.resolution, live.capture_width, live.capture_height);
    const VoiceVideoSendStream stream{connection.media.video_ssrc,
                                      connection.media.video_rtx_ssrc,
                                      bounds(live.settings).maximum,
                                      static_cast<u16>(live.settings.resolution ? extent.width : 0),
                                      static_cast<u16>(live.settings.resolution ? extent.height : 0),
                                      live.settings.fps,
                                      100,
                                      !live.paused};
    if (stream.ssrc == live.announced.ssrc && stream.active == live.announced.active &&
        stream.max_bitrate == live.announced.max_bitrate && stream.max_width == live.announced.max_width &&
        stream.max_height == live.announced.max_height && stream.max_framerate == live.announced.max_framerate)
        return;
    if (voice_connection_video(&connection, stream, now_ms) == VoiceSocketSend::accepted)
        live.announced = stream;
}
// Producer events: end, minimize, picture size.
void media_events(GatewayGoLive& live) noexcept {
    auto& m = *live.media;
    AcquireSRWLockExclusive(m.lock.native());
    const GoLiveEnd ended = m.ended;
    const bool minimized_changed = m.minimized_changed, minimized = m.minimized;
    m.minimized_changed = false;
    const u32 width = m.width, height = m.height, dropped = m.dropped;
    m.dropped = 0;
    ReleaseSRWLockExclusive(m.lock.native());
    if (ended != GoLiveEnd::none) {
        stop_session(live, ended);
        return;
    }
    if (dropped) {
        // The dropped unit may have been the answer to the last request.
        live.keyframes.wanted = live.awaiting_keyframe = true;
        live.keyframes.pending = false;
    }
    if (minimized_changed) {
        live.minimized = minimized;
        apply_pause(live);
    }
    if (width && height && (width != live.capture_width || height != live.capture_height)) {
        live.capture_width = width;
        live.capture_height = height;
        if (live.encoding)
            go_live_producer_configure(live.backend_context, encoder_settings(live));
    }
}
void drain_video(GatewayGoLive& live, u64 now_ms) noexcept {
    auto& m = *live.media;
    for (;;) {
        const Slice<u8> output = go_live_sender_frame_buffer(&live.sender);
        if (!output.data)
            return;
        AcquireSRWLockExclusive(m.lock.native());
        const bool any = m.slot_count != 0;
        const u32 index = m.slot_head;
        const GoLiveSlot slot = m.slots[index];
        ReleaseSRWLockExclusive(m.lock.native());
        if (!any)
            return;
        const ConstSlice<u8> unit{m.units + slot.offset, slot.bytes};
        if (slot.keyframe)
            live.keyframes.pending = false;
        // Nothing goes out paused, before DAVE allows it, in another codec,
        // or (after a gap) until a keyframe.
        bool send = !live.paused && live.dave_ready && live.encoding && slot.codec == live.codec;
        if (send && live.awaiting_keyframe && !slot.keyframe) {
            send = false;
            live.keyframes.wanted = true;
        }
        if (send) {
            const auto sealed = voice_connection_dave_encrypt_video(
                &live.connection, static_cast<DaveVideoCodec>(slot.codec), unit, {live.seal, kStageBytes},
                {live.seal + kStageBytes, kStageBytes}, output);
            if (sealed.ok() && go_live_sender_frame(&live.sender, sealed.bytes,
                                                    slot.rtp_timestamp + live.video_timestamp_offset, slot.keyframe)) {
                if (slot.keyframe) {
                    live.awaiting_keyframe = false;
                    live.last_keyframe_ms = now_ms;
                }
            } else
                live.awaiting_keyframe = live.keyframes.wanted = true;
        }
        AcquireSRWLockExclusive(m.lock.native());
        m.slot_head = (m.slot_head + 1) % kSlots;
        --m.slot_count;
        ReleaseSRWLockExclusive(m.lock.native());
    }
}
// Frames share one DAVE nonce counter (libdave's Encryptor keeps one per sender), and a receiver opens a frame when
// its last packet arrives: audio sealed after a video frame but sent before the frame finished would reach a
// receiver first, and that receiver's replay window would then refuse the video frame as a replay (the first one
// under a key it has just been given, or the first it sees after joining). So audio is sealed only while no video
// frame is on the wire; it waits in the producer's queue (160 ms) meanwhile.
void drain_audio(GatewayGoLive& live) noexcept {
    auto& m = *live.media;
    while (!live.sender.frame_active) {
        GoLiveOpusPacket packet;
        AcquireSRWLockExclusive(m.lock.native());
        const bool any = m.audio_count != 0;
        if (any) {
            copy_bytes(&packet, &m.audio[m.audio_head], 8 + m.audio[m.audio_head].bytes);
            m.audio_head = (m.audio_head + 1) % kAudioPackets;
            --m.audio_count;
        }
        ReleaseSRWLockExclusive(m.lock.native());
        if (!any)
            return;
        if (live.paused || !live.dave_ready || !live.sounding)
            continue;
        u8 sealed[kOpusEncoderMaximumPacketBytes + 64];
        const auto result =
            voice_connection_dave_encrypt(&live.connection, {packet.data, packet.bytes}, {sealed, sizeof(sealed)});
        if (result.ok())
            go_live_sender_audio(&live.sender, {sealed, result.bytes},
                                 live.audio_timestamp_base + packet.frame * kOpusEncoderFrameSamples);
    }
}
void live_media(GatewayGoLive& live, u64 now_ms) noexcept {
    const u64 now_us = microseconds();
    const bool ready = voice_connection_dave_ready(live.connection) && !live.connection.dave_recovering;
    if (ready && !live.dave_ready)
        live.awaiting_keyframe = live.keyframes.wanted = true; // Viewers decrypt from here on.
    live.dave_ready = ready;
    if (!ensure_encoder(live)) {
        stop_session(live, GoLiveEnd::encoder_failed);
        return;
    }
    // Nobody watches: the encoder holds (the self-preview keeps its pictures). The first viewer's keyframe is asked for
    // by the release itself, as Discord starts encoding when a viewer arrives.
    const bool hold = live.control.viewer_count == 0;
    if (live.encoding && hold != live.encoder_held) {
        go_live_producer_hold(live.backend_context, hold);
        live.encoder_held = hold;
        if (!hold) {
            live.awaiting_keyframe = live.keyframes.pending = true;
            live.keyframes.requested_ms = now_ms;
        }
    }
    signal(live, now_ms);
    drain_audio(live);
    drain_video(live, now_ms);
    if (go_live_sender_take_keyframe(&live.sender))
        live.keyframes.wanted = true;
    const u32 interval = live.connection.media.keyframe_interval_ms;
    if (interval && live.last_keyframe_ms && now_ms - live.last_keyframe_ms >= interval)
        live.keyframes.wanted = true;
    if (live.awaiting_keyframe && now_ms - live.keyframes.requested_ms >= kGoLiveKeyframeIntervalMs)
        live.keyframes.wanted = true;
    if (live.encoding && !live.paused && !live.encoder_held && go_live_keyframe_due(&live.keyframes, now_ms))
        go_live_producer_keyframe(live.backend_context);
    const u32 target = live.sender.bandwidth.target;
    if (live.encoding && (target > live.bitrate_applied + live.bitrate_applied / 20 ||
                          target + live.bitrate_applied / 20 < live.bitrate_applied)) {
        go_live_producer_bitrate(live.backend_context, target);
        live.bitrate_applied = target;
    }
    const u64 next_us = go_live_sender_pump(&live.sender, {&live, send_packet}, now_us);
    live.pump_due_ms = next_us ? now_ms + (next_us > now_us ? (next_us - now_us + 999) / 1000 : 0) : 0;
}
} // namespace

void go_live_media_unit(GoLiveMedia* media, const GpuStreamAccessUnit& unit) noexcept {
    AcquireSRWLockExclusive(media->lock.native());
    const u32 index = (media->slot_head + media->slot_count) % kSlots;
    const GoLiveSlot& newest = media->slots[(index + kSlots - 1) % kSlots];
    const u32 bytes = static_cast<u32>(unit.data.count);
    const u32 offset = bytes && bytes <= kGoLiveFrameBytes
                           ? go_live_unit_place(media->slots[media->slot_head].offset, newest.offset + newest.bytes,
                                                media->slot_count, bytes)
                           : kGoLiveNoRoom;
    if (offset == kGoLiveNoRoom)
        ++media->dropped;
    ReleaseSRWLockExclusive(media->lock.native());
    if (offset != kGoLiveNoRoom) {
        // Only this producer writes the ring outside the queued units, which the worker only ever frees.
        copy_bytes(media->units + offset, unit.data.data, bytes);
        AcquireSRWLockExclusive(media->lock.native());
        media->slots[index] = {offset, bytes, unit.rtp_timestamp, unit.codec, unit.keyframe};
        ++media->slot_count;
        ReleaseSRWLockExclusive(media->lock.native());
    }
    wake(media);
}

void go_live_media_audio(GoLiveMedia* media, ConstSlice<i16> stereo) noexcept {
    constexpr u32 kFrame = kOpusEncoderFrameSamples * 2;
    for (u32 at = 0; at != stereo.count;) {
        const u32 take = minimum(kFrame - media->pcm_count, stereo.count - at);
        copy_bytes(media->pcm + media->pcm_count, stereo.data + at, take * 2u);
        media->pcm_count += take;
        at += take;
        if (media->pcm_count != kFrame)
            continue;
        media->pcm_count = 0;
        GoLiveOpusPacket packet;
        packet.bytes = static_cast<u16>(opus_encode(&media->opus, media->pcm, packet.data));
        packet.frame = media->audio_frame++;
        AcquireSRWLockExclusive(media->lock.native());
        if (media->audio_count == kAudioPackets) { // The worker fell behind: the oldest is late anyway.
            media->audio_head = (media->audio_head + 1) % kAudioPackets;
            --media->audio_count;
        }
        copy_bytes(&media->audio[(media->audio_head + media->audio_count) % kAudioPackets], &packet,
                   8 + packet.bytes);
        ++media->audio_count;
        ReleaseSRWLockExclusive(media->lock.native());
        wake(media);
    }
}

void go_live_media_ended(GoLiveMedia* media, GoLiveEnd reason) noexcept {
    AcquireSRWLockExclusive(media->lock.native());
    if (media->ended == GoLiveEnd::none)
        media->ended = reason;
    ReleaseSRWLockExclusive(media->lock.native());
    wake(media);
}

void go_live_media_minimized(GoLiveMedia* media, bool minimized) noexcept {
    AcquireSRWLockExclusive(media->lock.native());
    media->minimized = minimized;
    media->minimized_changed = true;
    ReleaseSRWLockExclusive(media->lock.native());
    wake(media);
}

void go_live_media_capture_size(GoLiveMedia* media, u32 width, u32 height) noexcept {
    AcquireSRWLockExclusive(media->lock.native());
    media->width = width;
    media->height = height;
    ReleaseSRWLockExclusive(media->lock.native());
    wake(media);
}

// Once per stream or user command.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool gateway_go_live_command(GatewayGoLive* live, const GoLiveCommand& command, u64 self_user_id,
                             Utf8View session, u64 now_ms) noexcept {
    switch (command.kind) {
    case GoLiveCommandKind::start: {
        if (active(*live))
            stop_session(*live, GoLiveEnd::stopped);
        const auto key = gateway_stream_key_for(command.guild_id, command.channel_id, self_user_id);
        if (!gateway_stream_key_valid(key) || !command.settings.fps)
            return false;
        bool known = false;
        live->encodable =
            live->backend_context ? go_live_producer_encodable(live->backend_context, &known) : 0;
        live->settings = command.settings;
        live->sound_process = sound_process(command.source);
        live->user_paused = live->minimized = live->paused = false;
        live->media_revision = 0;
        live->capture_width = command.system_picker ? 0 : command.source.width;
        live->capture_height = command.system_picker ? 0 : command.source.height;
        live->keyframes = {};
        live->dave_ready = false;
        live->awaiting_keyframe = true;
        live->state = GoLiveState::ended;
        if (!(live->encodable & gpu_stream_codec_bit(GpuStreamCodec::h264))) {
            live->end = GoLiveEnd::no_hardware_encoder;
            return true;
        }
        u32 random[2]{};
        live->media = media_create(*live);
        live->seal = static_cast<u8*>(pages_allocate(u64{kStageBytes} * 2));
        if (!live->media || !live->seal || !secure_random_fill({reinterpret_cast<u8*>(random), 8}).ok()) {
            free_buffers(*live);
            live->end = GoLiveEnd::capture_unavailable;
            return true;
        }
        live->video_timestamp_offset = random[0];
        live->audio_timestamp_base = random[1];
        live->capturing = live->backend_context &&
                          go_live_producer_capture(live->backend_context, live->media, command.source,
                                                   command.system_picker, command.settings.fps);
        if (!live->capturing) {
            free_buffers(*live);
            live->end = GoLiveEnd::capture_unavailable;
            return true;
        }
        if (command.settings.sound)
            start_sound(*live);
        if (!live->control.setup.user_id)
            gateway_stream_bind(&live->control, self_user_id, session);
        if (!gateway_stream_create_own(&live->control, key, now_ms)) {
            end_session(*live, GoLiveEnd::connection_failed);
            return false;
        }
        live->state = GoLiveState::starting;
        live->end = GoLiveEnd::none;
        return true;
    }
    case GoLiveCommandKind::stop:
        if (!active(*live))
            return false;
        stop_session(*live, GoLiveEnd::stopped);
        return true;
    case GoLiveCommandKind::update: {
        if (!active(*live) || !command.settings.fps)
            return false;
        const bool sound = command.settings.sound;
        live->settings = command.settings;
        if (sound && !live->sounding)
            start_sound(*live);
        else if (!sound && live->sounding) {
            go_live_producer_sound_stop(live->backend_context);
            live->sounding = false;
        }
        if (live->sender.history) {
            const u32 target = live->sender.bandwidth.target;
            live->sender.bandwidth.bounds = bounds(live->settings);
            go_live_bandwidth_remb(&live->sender.bandwidth, live->sender.bandwidth.remb); // Re-clamps the target.
            live->bitrate_applied = target == live->sender.bandwidth.target ? live->bitrate_applied : 0;
        }
        if (live->encoding)
            go_live_producer_configure(live->backend_context, encoder_settings(*live));
        return true;
    }
    case GoLiveCommandKind::change_source:
        if (!active(*live))
            return false;
        if (live->capturing)
            go_live_producer_capture_stop(live->backend_context);
        if (live->sounding)
            go_live_producer_sound_stop(live->backend_context);
        live->sounding = false;
        live->sound_process = sound_process(command.source);
        live->capturing = live->backend_context &&
                          go_live_producer_capture(live->backend_context, live->media, command.source,
                                                   command.system_picker, live->settings.fps);
        if (!live->capturing) {
            stop_session(*live, GoLiveEnd::capture_unavailable);
            return true;
        }
        if (live->settings.sound)
            start_sound(*live);
        live->minimized = false;
        apply_pause(*live);
        live->awaiting_keyframe = live->keyframes.wanted = true;
        return true;
    case GoLiveCommandKind::pause:
    case GoLiveCommandKind::resume:
        if (!active(*live))
            return false;
        live->user_paused = command.kind == GoLiveCommandKind::pause;
        apply_pause(*live);
        return true;
    }
    return false;
}
#pragma clang attribute pop

void gateway_go_live_drive(GatewayGoLive* live, const GatewayVoiceControl& voice, NetworkLoop* loop,
                           u64 now_ms) noexcept {
    auto& control = live->control;
    gateway_stream_voice(&control, voice.setup.channel_id, voice.setup.phase != VoiceSetupPhase::idle);
    gateway_stream_expire(&control, now_ms);
    if (active(*live) && live->media)
        media_events(*live);
    if (active(*live) && control.phase == GatewayStreamPhase::ended)
        end_session(*live, end_of(control));
    live->connection.video_encodable = voice_bits(live->encodable);
    live->connection.video_decodable = live->decodable;
    voice_connection_drive(&live->connection, control.setup, loop, now_ms);
    if (!active(*live))
        return;
    auto& connection = live->connection;
    if (live->media_revision && live->media_revision != connection.ready_revision) {
        go_live_sender_stop(&live->sender); // The transport went away; a new one starts afresh.
        live->media_revision = 0;
    }
    if (!live->media_revision && connection.media.has_description && !attach(*live, microseconds())) {
        stop_session(*live, GoLiveEnd::connection_failed);
        return;
    }
    const bool attached = live->media_revision && live->media_revision == connection.ready_revision;
    gateway_stream_observe(&control, {attached, connection.reconnect_pending,
                                      connection.error != VoiceConnectionError::none && !connection.reconnect_pending &&
                                          voice_connection_drained(connection)});
    switch (control.phase) {
    case GatewayStreamPhase::requested:
    case GatewayStreamPhase::awaiting_server:
        live->state = GoLiveState::starting;
        break;
    case GatewayStreamPhase::connecting:
        live->state = GoLiveState::connecting;
        break;
    case GatewayStreamPhase::reconnecting:
        live->state = GoLiveState::reconnecting;
        break;
    case GatewayStreamPhase::active:
        live->state = GoLiveState::live;
        break;
    default:
        end_session(*live, end_of(control));
        return;
    }
    if (attached)
        live_media(*live, now_ms);
}

bool gateway_go_live_busy(const GatewayGoLive& live) noexcept {
    return active(live) || gateway_stream_active(live.control) || !voice_connection_drained(live.connection) ||
           live.connection.command.data;
}

u64 gateway_go_live_deadline(const GatewayGoLive& live) noexcept {
    u64 deadline = earliest_deadline(gateway_stream_deadline(live.control), voice_connection_deadline(live.connection));
    deadline = earliest_deadline(deadline, live.pump_due_ms);
    if (active(live) && live.encoding && !live.paused && !live.encoder_held &&
        (live.awaiting_keyframe || live.keyframes.wanted))
        deadline = earliest_deadline(deadline, go_live_keyframe_deadline(live.keyframes));
    return deadline;
}

// Once per snapshot poll, reset or release.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool gateway_go_live_publish(GatewayGoLive* live, GoLiveSnapshot* output) noexcept {
    GoLiveSnapshot s{};
    bool known = false;
    s.key = live->control.key;
    s.viewer_count = active(*live) ? live->control.viewer_count : 0;
    copy_bytes(s.viewer_ids, live->control.viewer_ids, minimum(s.viewer_count, kGoLiveViewerIds) * 8u);
    s.encodable = voice_bits(live->backend_context ? go_live_producer_encodable(live->backend_context, &known) : 0);
    s.encodable_known = known;
    s.settings = live->settings;
    s.state = live->state;
    s.end = live->end;
    s.paused = live->paused;
    s.generation = live->published.generation;
    const bool changed = !bytes_equal(&s, &live->published, sizeof(s));
    if (changed) {
        s.generation = live->published.generation + 1;
        live->published = s;
    }
    *output = live->published;
    return changed;
}

void gateway_go_live_reset(GatewayGoLive* live, GatewayStreamEnd end) noexcept {
    gateway_stream_reset(&live->control, end);
    if (active(*live))
        end_session(*live, end == GatewayStreamEnd::session ? GoLiveEnd::connection_failed : end_of(live->control));
    voice_connection_stop(&live->connection);
}

void gateway_go_live_release(GatewayGoLive* live) noexcept {
    stop_producers(*live);
    free_buffers(*live);
    voice_connection_stop(&live->connection);
    gateway_stream_release(&live->control);
    live->state = GoLiveState::idle;
    live->end = GoLiveEnd::none;
}
#pragma clang attribute pop
} // namespace maicord
