#pragma once

#include "maicord/app/voice_connection_setup.hpp"
#include "maicord/protocol/gateway_payload.hpp"
#include "maicord/protocol/gateway_stream.hpp"

namespace maicord {
enum class VoiceSocketPhase : u8;
enum class VoiceConnectionError : u8;
enum class VoiceUdpPhase : u8;
// Go Live watch session on the main Gateway (worker-owned, never copied):
//   watch -> STREAM_WATCH -> STREAM_CREATE (media server) ->
//   STREAM_SERVER_UPDATE (token, endpoint) -> stream voice connection ->
//   STREAM_DELETE / stop / leaving the voice channel.
// Only one stream is watched at a time; watching another key first deletes the
// previous one, as the official client does without "allow multiple".
enum class GatewayStreamPhase : u8 {
    idle,
    requested,       // STREAM_WATCH queued or sent; awaiting STREAM_CREATE.
    awaiting_server, // Media server known; awaiting STREAM_SERVER_UPDATE.
    connecting,      // Credentials ready; the stream voice connection negotiates.
    active,          // Stream media is attached.
    reconnecting,    // The stream connection is reconnecting (STREAM_PING sent).
    ended,           // See GatewayStreamEnd.
};
enum class GatewayStreamEnd : u8 {
    none,
    stopped,      // Local stop.
    deleted,      // STREAM_DELETE (see the delete reason).
    left_channel, // The local user left or moved out of the stream's voice channel.
    session,      // A fresh Gateway session invalidated the stream connection.
    timeout,      // No STREAM_CREATE / STREAM_SERVER_UPDATE in time.
    failed,       // The stream voice connection failed permanently.
    invalid_event,
    storage,
};
enum class GatewayStreamCommandKind : u8 {
    none,
    watch,
    remove, // STREAM_DELETE
    ping,
};
struct GatewayStreamCommand {
    GatewayStreamKey key;
    GatewayStreamCommandKind kind;
};
inline constexpr u32 kGatewayStreamCommandQueue = 4;
inline constexpr u64 kGatewayStreamCreateTimeoutMs = 30'000;

struct GatewayStreamControl {
    VoiceConnectionSetup setup; // Stream credentials (server = rtc_server_id).
    GatewayStreamKey key;       // Watched key; kept after the end for the UI.
    GatewayStreamCommand queue[kGatewayStreamCommandQueue];
    u64 rtc_server_id, rtc_channel_id;
    u64 create_deadline_ms;
    u32 queue_head, queue_count;
    u32 viewer_count;
    GatewayStreamPhase phase;
    GatewayStreamEnd end;
    GatewayStreamDeleteReason delete_reason;
    bool paused;
    bool reconnect_seen; // Edge detector: one STREAM_PING per connection loss.
};

// The viewer is identified by the main Gateway session. Rebinding (a fresh
// READY) ends any stream with GatewayStreamEnd::session.
bool gateway_stream_bind(GatewayStreamControl* stream, u64 user_id, Utf8View session) noexcept;
bool gateway_stream_active(const GatewayStreamControl& stream) noexcept;
// False for an invalid key, an unbound owner or a full command queue.
bool gateway_stream_watch(GatewayStreamControl* stream, const GatewayStreamKey& key, u64 now_ms) noexcept;
// Ends the watched stream; sends STREAM_DELETE when the server knows about it.
void gateway_stream_stop(GatewayStreamControl* stream, GatewayStreamEnd end) noexcept;
// Peek/commit like gateway_voice_next: commit only after the whole command
// was accepted by the serialized Gateway send path.
bool gateway_stream_next(const GatewayStreamControl& stream, GatewayStreamCommand* output) noexcept;
bool gateway_stream_submitted(GatewayStreamControl* stream, const GatewayStreamCommand& command) noexcept;
GatewayCommandWriteResult gateway_stream_write(const GatewayStreamCommand& command, Slice<u8> output) noexcept;
void gateway_stream_create(GatewayStreamControl* stream, const GatewayStreamCreate& event, u64 now_ms) noexcept;
void gateway_stream_server(GatewayStreamControl* stream, const GatewayStreamServer& event) noexcept;
void gateway_stream_update(GatewayStreamControl* stream, const GatewayStreamUpdate& event) noexcept;
void gateway_stream_delete(GatewayStreamControl* stream, const GatewayStreamDelete& event) noexcept;
// STREAM_* dispatches. True when the event was a stream event (consumed on
// the worker; the server token never reaches the UI mailbox).
bool gateway_stream_dispatch(GatewayStreamControl* stream, const GatewayEnvelope& envelope, Slice<u32> scratch,
                             u32 maximum_depth, u64 now_ms) noexcept;
// Stream connection progress, after its drive: attached media is active; a
// remote loss with a planned reconnect queues one STREAM_PING; a permanent
// failure ends the stream.
struct GatewayStreamConnectionState {
    bool media_attached, reconnecting, failed;
};
void gateway_stream_observe(GatewayStreamControl* stream, const GatewayStreamConnectionState& state) noexcept;
// Ends the stream when the local voice call is no longer in its channel.
void gateway_stream_voice(GatewayStreamControl* stream, u64 voice_channel_id, bool in_call) noexcept;
bool gateway_stream_expire(GatewayStreamControl* stream, u64 now_ms) noexcept;
u64 gateway_stream_deadline(const GatewayStreamControl& stream) noexcept;
// Drops all state without sending anything (the Gateway session is gone).
void gateway_stream_reset(GatewayStreamControl* stream, GatewayStreamEnd end) noexcept;
void gateway_stream_release(GatewayStreamControl* stream) noexcept;

// Crosses the account module ABI.
struct GatewayClientStreamSnapshot {
    GatewayStreamKey key; // Watched key; kept after the end until the next watch.
    u64 generation;       // Stream credential generation.
    u64 rtc_server_id;
    u64 frames_delivered, frames_rejected; // Decoder hand-offs; DAVE rejections.
    u32 viewer_count;
    u32 video_ssrc;   // Tagged SSRC for voice_video_frame; zero until announced.
    u32 video_frames; // Frames assembled for it so far.
    u16 volume_percent;
    u16 voice_close_code;
    GatewayStreamPhase phase;
    GatewayStreamEnd end;
    GatewayStreamDeleteReason delete_reason;
    bool paused;
    VoiceSocketPhase socket_phase;
    VoiceConnectionError connection_error;
    VoiceUdpPhase udp_phase;
    bool audio; // The streamer announced an audio source (soundshare).
    u64 video_user_id; // The streamer, once its video source is announced.
    constexpr bool operator==(const GatewayClientStreamSnapshot&) const noexcept = default;
};
static_assert(sizeof(GatewayClientStreamSnapshot) == 96);
} // namespace maicord
