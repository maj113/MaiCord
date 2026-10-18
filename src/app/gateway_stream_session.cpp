#include "maicord/app/gateway_stream_session.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool push_tagged(void* context, u32 ssrc, ConstSlice<u8> frame, u32 rtp_timestamp, bool keyframe,
                 bool intact) noexcept {
    auto& session = *static_cast<GatewayStreamSession*>(context);
    if (ssrc & kGatewayStreamVideoSsrcTag) {
        ++session.frames_untagged;
        return false;
    }
    return session.target.push_frame &&
           session.target.push_frame(session.target.context, ssrc | kGatewayStreamVideoSsrcTag, frame, rtp_timestamp,
                                     keyframe, intact);
}
void end_tagged(void* context, u32 ssrc) noexcept {
    auto& session = *static_cast<GatewayStreamSession*>(context);
    if (!(ssrc & kGatewayStreamVideoSsrcTag) && session.target.end_stream)
        session.target.end_stream(session.target.context, ssrc | kGatewayStreamVideoSsrcTag);
}
u32 volume(const GatewayStreamSession& session) noexcept {
    return session.volume_set ? session.volume_percent : kGatewayStreamVolumeDefault;
}
} // namespace

void gateway_stream_session_drive(GatewayStreamSession* session, const GatewayVoiceControl& voice, NetworkLoop* loop,
                                  const VoiceAudioSettings& output, u64 now_ms) noexcept {
    auto& control = session->control;
    // Watching requires being in the stream's voice channel.
    gateway_stream_voice(&control, voice.setup.channel_id, voice.setup.phase != VoiceSetupPhase::idle);
    gateway_stream_expire(&control, now_ms);
    voice_connection_drive(&session->connection, control.setup, loop, now_ms);
    session->receive.video.frames = gateway_stream_session_frames(session);
    voice_receive_configure(&session->receive, gateway_stream_bus_settings(output, volume(*session)));
    voice_receive_drive(&session->receive, &session->connection, voice, now_ms);
    const auto& connection = session->connection;
    gateway_stream_observe(
        &control, {session->receive.media_revision && session->receive.media_revision == connection.ready_revision,
                   connection.reconnect_pending,
                   connection.error != VoiceConnectionError::none && !connection.reconnect_pending &&
                       voice_connection_drained(connection)});
}

VoiceAudioSettings gateway_stream_bus_settings(const VoiceAudioSettings& output, u32 volume_percent) noexcept {
    VoiceAudioSettings bus = output;
    const u32 gain = static_cast<u32>(output.output_gain_percent) * volume_percent / 100;
    bus.output_gain_percent = static_cast<u16>(gain > kGatewayStreamVolumeMaximum ? kGatewayStreamVolumeMaximum : gain);
    return bus;
}

VideoFrameSink gateway_stream_session_frames(GatewayStreamSession* session) noexcept {
    return {session, push_tagged, end_tagged};
}

bool gateway_stream_session_owns(const GatewayStreamSession& session, const NetworkCompletion& completion) noexcept {
    return voice_connection_owns(session.connection, completion);
}

void gateway_stream_session_completion(GatewayStreamSession* session, NetworkLoop* loop,
                                       const NetworkCompletion& completion, u64 now_ms) noexcept {
    voice_connection_completion(&session->connection, session->control.setup, loop, completion, now_ms);
}

bool gateway_stream_session_busy(const GatewayStreamSession& session) noexcept {
    return gateway_stream_active(session.control) || !voice_connection_drained(session.connection) ||
           session.connection.command.data || session.receive.media_revision;
}

bool gateway_stream_session_drained(const GatewayStreamSession& session) noexcept {
    return voice_connection_drained(session.connection);
}

u64 gateway_stream_session_deadline(const GatewayStreamSession& session) noexcept {
    return earliest_deadline(earliest_deadline(gateway_stream_deadline(session.control),
                                               voice_connection_deadline(session.connection)),
                             voice_receive_deadline(session.receive));
}

void gateway_stream_session_set_target(GatewayStreamSession* session, VideoFrameSink target) noexcept {
    // Streams delivered to a previous decoder set end there first.
    if (session->target.push_frame && session->target.context != target.context)
        voice_video_sink_stop(&session->receive.video);
    session->target = target;
}

void gateway_stream_session_set_volume(GatewayStreamSession* session, u32 percent) noexcept {
    session->volume_percent = percent > kGatewayStreamVolumeMaximum ? kGatewayStreamVolumeMaximum : percent;
    session->volume_set = true;
}

void gateway_stream_session_want_video(GatewayStreamSession* session, u64 owner_id, bool wanted) noexcept {
    auto& sink = session->receive.video;
    sink.unwanted_count = 0;
    if (!wanted && owner_id) {
        sink.unwanted[0] = owner_id;
        sink.unwanted_count = 1;
    }
}

void gateway_stream_session_keyframe(VoiceVideoSink* call, GatewayStreamSession* session, u32 ssrc) noexcept {
    if (ssrc & kGatewayStreamVideoSsrcTag)
        voice_video_sink_request_keyframe(&session->receive.video, ssrc & ~kGatewayStreamVideoSsrcTag);
    else
        voice_video_sink_request_keyframe(call, ssrc);
}

void gateway_stream_session_reset(GatewayStreamSession* session, GatewayStreamEnd end) noexcept {
    gateway_stream_reset(&session->control, end);
    voice_connection_stop(&session->connection);
    voice_receive_stop(&session->receive);
}

void gateway_stream_session_release(GatewayStreamSession* session) noexcept {
    voice_connection_stop(&session->connection);
    voice_receive_stop(&session->receive);
    gateway_stream_release(&session->control);
}

GatewayClientStreamSnapshot gateway_stream_session_snapshot(const GatewayStreamSession& session,
                                                            u64 self_user_id) noexcept {
    const auto& control = session.control;
    const auto& connection = session.connection;
    GatewayClientStreamSnapshot snapshot{};
    snapshot.key = control.key;
    snapshot.generation = control.setup.generation;
    snapshot.rtc_server_id = control.rtc_server_id;
    snapshot.viewer_count = control.viewer_count;
    snapshot.volume_percent = static_cast<u16>(volume(session));
    snapshot.phase = control.phase;
    snapshot.end = control.end;
    snapshot.delete_reason = control.delete_reason;
    snapshot.paused = control.paused;
    if (!gateway_stream_active(control))
        return snapshot;
    snapshot.voice_close_code = connection.socket.close_code;
    snapshot.socket_phase = connection.socket.phase;
    snapshot.connection_error = connection.error;
    snapshot.udp_phase = connection.udp.phase;
    snapshot.frames_delivered = session.receive.video.stats.delivered;
    snapshot.frames_rejected = session.receive.video.stats.dave_rejected;
    VoiceVideoUser users[kVoiceVideoMaximumUsers];
    const u32 count = voice_video_users(connection, &session.receive.video, self_user_id, {users, kVoiceVideoMaximumUsers});
    for (u32 i = 0; i != count; ++i)
        if (users[i].user_id == control.key.owner_id && !(users[i].ssrc & kGatewayStreamVideoSsrcTag)) {
            snapshot.video_ssrc = users[i].ssrc | kGatewayStreamVideoSsrcTag;
            snapshot.video_frames = users[i].frames;
            snapshot.video_user_id = users[i].user_id;
        }
    const auto& participants = connection.participants;
    for (u32 i = 0; i != participants.source_count && participants.sources; ++i) {
        const auto& source = participants.sources[i];
        snapshot.audio |= source.kind == VoiceSourceKind::audio && source.active &&
                          source.user_id == control.key.owner_id;
    }
    return snapshot;
}
} // namespace maicord
