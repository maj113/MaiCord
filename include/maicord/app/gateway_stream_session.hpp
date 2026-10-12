#pragma once
#include "maicord/app/gateway_stream_control.hpp"
#include "maicord/app/voice_receive.hpp"

namespace maicord {
// One watched Go Live stream: the Gateway control plus its own voice
// connection (own DAVE session, group = rtc_channel_id) and receive pipeline.
// Stream audio plays on a separate playback bus (its own WASAPI stream) whose
// volume is independent of the call. Stream video goes to the call's decoder
// sink under a tagged SSRC so it never collides with call SSRCs from the other
// media server: voice_video_frame(snapshot.video_ssrc) renders it.
inline constexpr u32 kGatewayStreamVideoSsrcTag = 0x8000'0000u;
inline constexpr u32 kGatewayStreamVolumeDefault = 100;
inline constexpr u32 kGatewayStreamVolumeMaximum = 200;
// The call view's receive want for the watched stream rides voice_video_wanted
// with the streamer's ID tagged (snowflakes never set bit 63): op 15 on the
// stream connection instead of the call's. Tile IDs and presented pictures use
// the same tagged ID, so a stream never collides with its owner's camera.
inline constexpr u64 kGatewayStreamVideoUserTag = 1ull << 63;
constexpr u64 gateway_stream_video_user(u64 owner_id) noexcept {
    return owner_id ? owner_id | kGatewayStreamVideoUserTag : 0;
}
constexpr bool gateway_stream_video_user_tagged(u64 user_id) noexcept {
    return (user_id & kGatewayStreamVideoUserTag) != 0;
}
constexpr u64 gateway_stream_video_owner(u64 user_id) noexcept {
    return user_id & ~kGatewayStreamVideoUserTag;
}

struct GatewayStreamSession {
    GatewayStreamControl control;
    VoiceConnection connection;
    VoiceReceive receive;
    VideoFrameSink target; // The call decoders; tagged stream frames go here.
    u64 frames_untagged;   // Stream SSRCs that already carry the tag bit (dropped).
    u32 volume_percent;    // Stream bus volume, 0..200 once volume_set.
    bool volume_set;       // False: kGatewayStreamVolumeDefault.
};

// Worker-only. Drives the stream connection after the call's drive: applies
// the voice-channel guard, connects/reconnects, attaches media and audio,
// observes progress (STREAM_PING on loss). output carries the applied output
// device and call volume; the stream bus volume multiplies on top.
void gateway_stream_session_drive(GatewayStreamSession* session, const GatewayVoiceControl& voice, NetworkLoop* loop,
                                  const VoiceAudioSettings& output, u64 now_ms) noexcept;
bool gateway_stream_session_owns(const GatewayStreamSession& session, const NetworkCompletion& completion) noexcept;
void gateway_stream_session_completion(GatewayStreamSession* session, NetworkLoop* loop,
                                       const NetworkCompletion& completion, u64 now_ms) noexcept;
// True while anything (connection, receive, queued commands) needs drives.
bool gateway_stream_session_busy(const GatewayStreamSession& session) noexcept;
bool gateway_stream_session_drained(const GatewayStreamSession& session) noexcept;
u64 gateway_stream_session_deadline(const GatewayStreamSession& session) noexcept;
// Installs the call decoders as the stream video target (worker).
void gateway_stream_session_set_target(GatewayStreamSession* session, VideoFrameSink target) noexcept;
void gateway_stream_session_set_volume(GatewayStreamSession* session, u32 percent) noexcept;
// Worker: whether the watched stream's video is wanted (its tile is visible).
// Unwanted asks the stream connection for quality 0 (op 15); audio continues.
void gateway_stream_session_want_video(GatewayStreamSession* session, u64 owner_id, bool wanted) noexcept;
// The stream bus: the call's output device, gain = output * volume / 100
// (clamped to 200 %).
VoiceAudioSettings gateway_stream_bus_settings(const VoiceAudioSettings& output, u32 volume_percent) noexcept;
// The sink the stream's video pipeline delivers to: tags SSRCs, forwards to target.
VideoFrameSink gateway_stream_session_frames(GatewayStreamSession* session) noexcept;
// Any thread: routes a decoder keyframe request by SSRC tag.
void gateway_stream_session_keyframe(VoiceVideoSink* call, GatewayStreamSession* session, u32 ssrc) noexcept;
// Transport teardown without Gateway traffic (fresh session/shutdown). The
// receive path stays allocated (lent UDP buffers) until the connection drains.
void gateway_stream_session_reset(GatewayStreamSession* session, GatewayStreamEnd end) noexcept;
void gateway_stream_session_release(GatewayStreamSession* session) noexcept;

GatewayClientStreamSnapshot gateway_stream_session_snapshot(const GatewayStreamSession& session,
                                                            u64 self_user_id) noexcept;
} // namespace maicord
