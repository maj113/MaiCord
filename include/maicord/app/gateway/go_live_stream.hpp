#pragma once
#include "maicord/app/gateway/stream_control.hpp"
#include "maicord/app/gateway/voice_control.hpp"
#include "maicord/app/voice/connection.hpp"
#include "maicord/app/voice/go_live_send.hpp"
#include "maicord/app/voice/go_live_session.hpp"
#include "maicord/platform/encode/gpu_stream_encoder.hpp"

namespace maicord {
// The local user's Go Live stream on the gateway worker, behind the UI's
// GoLiveSession (go_live_session.hpp):
//   STREAM_CREATE (op 18) -> STREAM_CREATE + STREAM_SERVER_UPDATE -> the
//   stream server connection as a sender (Identify with the screen stream,
//   encode codecs in Select Protocol, Ready's video/RTX SSRCs, Speaking with
//   SOUNDSHARE, Video op 12, DAVE) -> capture -> GPU encoder -> DAVE ->
//   RTP/RTCP (go_live_send.hpp); pause is STREAM_SET_PAUSED (op 22) and op 12
//   inactive; stop is STREAM_DELETE (op 19).
// The producers (capture, encoder, stream audio) run on their own threads and
// hand over through GoLiveMedia; everything else is worker-only.
struct GoLiveMedia;

// What makes pictures and sound: go_live_devices.cpp (capture, the GPU encoder, stream audio) with its GoLiveDevices as
// the context (GatewayGoLive::backend_context); a test that links the stream code defines these itself with a
// synthetic producer. Every call is on the worker except preview (any thread).
// gpu_stream_codec_bit set of the hardware encoders; *known is false until there is a device to probe.
u32 go_live_producer_encodable(void* context, bool* known) noexcept;
// Starts capturing into media (the picker's source, or the system picker offering 1 << source.kind); false when
// nothing could start.
bool go_live_producer_capture(void* context, GoLiveMedia* media, const CaptureSource& source, bool system_picker,
                              u32 max_fps) noexcept;
void go_live_producer_capture_stop(void* context) noexcept;
// Stream audio of the source (process_id 0: everything but MaiCord).
bool go_live_producer_sound(void* context, GoLiveMedia* media, u32 process_id) noexcept;
void go_live_producer_sound_stop(void* context) noexcept;
bool go_live_producer_encoder(void* context, GoLiveMedia* media, GpuStreamCodec codec,
                              const GpuStreamEncoderSettings& settings) noexcept;
void go_live_producer_configure(void* context, const GpuStreamEncoderSettings& settings) noexcept;
void go_live_producer_bitrate(void* context, u32 bits_per_second) noexcept;
void go_live_producer_keyframe(void* context) noexcept;
void go_live_producer_encoder_stop(void* context) noexcept;
// The newest picture, and the sequence it carries (it changes with every new picture).
bool go_live_producer_preview(void* context, GpuVideoFrame* output, u32* sequence) noexcept;

// Producer side, any thread. Units queue (up to 16, in 8 MiB) while the
// sender paces out a large one, as libwebrtc's pacer queues: a keyframe takes
// several frame intervals on the wire. A unit that finds no room is dropped
// and a keyframe is asked for.
void go_live_media_unit(GoLiveMedia* media, const GpuStreamAccessUnit& unit) noexcept;
// 48 kHz stereo PCM16 of any length, framed into 20 ms Opus packets here
// (one producer thread).
void go_live_media_audio(GoLiveMedia* media, ConstSlice<i16> stereo) noexcept;
void go_live_media_ended(GoLiveMedia* media, GoLiveEnd reason) noexcept;
void go_live_media_minimized(GoLiveMedia* media, bool minimized) noexcept;
// The captured picture's size; the encoder extent follows it.
void go_live_media_capture_size(GoLiveMedia* media, u32 width, u32 height) noexcept;

inline constexpr u32 kGoLiveAudioBitrate = 128'000;
struct GatewayGoLive {
    GatewayStreamControl control;
    VoiceConnection connection;
    GoLiveSender sender;
    void* backend_context; // The producers' context.
    GoLiveMedia* media;
    u8* seal; // Video DAVE staging: prepared, then scratch.
    // Worker wake-up for producers (posted from their threads).
    void (*wake)(void* context) noexcept;
    void* wake_context;
    GoLiveSettings settings;
    u32 sound_process; // The shared window's process, which stream audio follows (0: every process).
    GoLiveSnapshot published; // Last snapshot; its generation bumps on change.
    u64 media_revision, last_keyframe_ms, pump_due_ms;
    GoLiveKeyframePacer keyframes;
    u32 encodable, decodable; // gpu_stream_codec_bit encoders; voice_video_codec_bit decoders (offered too).
    u32 capture_width, capture_height, bitrate_applied, video_timestamp_offset, audio_timestamp_base;
    VoiceVideoSendStream announced; // Last op 12 sent (ssrc 0: none yet).
    u32 speaking_sent;              // Last op 5 flags + 1 (0: none sent).
    GpuStreamCodec codec;
    GoLiveState state;
    GoLiveEnd end;
    bool capturing, sounding, encoding, user_paused, minimized, paused;
    bool awaiting_keyframe, dave_ready;
    u8 receive_buffer[kVoiceUdpMediaReceiveSlots * 2048];
    u8 send_buffer[2048];
};

// Worker. Applies one UI command; self_user_id and session bind the control
// the first time. False for a command that does not apply.
bool gateway_go_live_command(GatewayGoLive* live, const GoLiveCommand& command, u64 self_user_id,
                             Utf8View session, u64 now_ms) noexcept;
// After the call's drive: the voice-channel guard, the connection, media
// attachment, producers' output, DAVE, RTP and RTCP.
void gateway_go_live_drive(GatewayGoLive* live, const GatewayVoiceControl& voice, NetworkLoop* loop,
                           u64 now_ms) noexcept;
bool gateway_go_live_busy(const GatewayGoLive& live) noexcept;
u64 gateway_go_live_deadline(const GatewayGoLive& live) noexcept;
// The UI snapshot; true when it changed since the last call (generation bumped).
bool gateway_go_live_publish(GatewayGoLive* live, GoLiveSnapshot* output) noexcept;
// Gateway session gone: ends without Gateway traffic. Producers stop now.
void gateway_go_live_reset(GatewayGoLive* live, GatewayStreamEnd end) noexcept;
// Shutdown, after the connection drained.
void gateway_go_live_release(GatewayGoLive* live) noexcept;
} // namespace maicord
