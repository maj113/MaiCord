#include "maicord/app/voice_receive.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
DaveMediaResult connection_decrypt(void* context, const VoiceReceivedPacket& packet, ConstSlice<u8> frame,
                                   Slice<u8> scratch, Slice<u8> output) noexcept {
    // DAVE epochs are timed on the worker's GetTickCount64 clock.
    return voice_connection_dave_decrypt(static_cast<VoiceConnection*>(context), packet, frame, scratch, output,
                                         GetTickCount64());
}
DaveMediaResult connection_decrypt_frame(void* context, u64 user_id, u32 ssrc, ConstSlice<u8> frame,
                                         Slice<u8> scratch, Slice<u8> output) noexcept {
    return voice_connection_dave_decrypt_frame(static_cast<VoiceConnection*>(context), user_id, ssrc, frame, scratch,
                                               output, GetTickCount64());
}
bool transport_receive(void* context, ConstSlice<u8> bytes, const VoiceReceivedPacket& packet) noexcept {
    auto& receive = *static_cast<VoiceReceive*>(context);
    // Undecodable or unauthorized media is a loss, never a transport failure.
    voice_receive_packet(&receive, packet, bytes, connection_decrypt, receive.connection,
                         voice_playback_now_us());
    return true;
}
void render(void* context, Slice<i16> pcm, u64 now_us) noexcept {
    voice_playback_render(static_cast<VoicePlayback*>(context), pcm, now_us);
}
bool media_attachable(const VoiceConnection& connection) noexcept {
    return connection.media.has_description && connection.identify_submitted &&
           connection.socket.phase == VoiceSocketPhase::open && connection.ready_revision &&
           connection.ready_revision == connection.udp_revision && connection.udp.phase == VoiceUdpPhase::ready &&
           voice_udp_media_drained(connection.media_io);
}
} // namespace

VoiceReceiveResult voice_receive_packet(VoiceReceive* receive, const VoiceReceivedPacket& packet,
                                        ConstSlice<u8> plaintext, VoiceReceiveDecrypt decrypt, void* context,
                                        u64 now_us) noexcept {
    if (!receive || !decrypt)
        return VoiceReceiveResult::invalid;
    const auto& layout = packet.transport.layout;
    if (layout.kind == MediaPacketKind::rtcp) {
        if (packet.transport.ok())
            voice_video_sink_rtcp(&receive->video, plaintext, now_us);
        return VoiceReceiveResult::ignored;
    }
    // Video RTP is assembled before DAVE; the frame stage decrypts it.
    if (packet.transport.ok() && layout.kind == MediaPacketKind::rtp && voice_video_source(packet.source))
        return voice_video_sink_packet(&receive->video, packet, plaintext, now_us)
                   ? VoiceReceiveResult::video
                   : VoiceReceiveResult::ignored;
    if (!packet.transport.ok() || layout.kind != MediaPacketKind::rtp || layout.type != kVoiceOpusPayloadType ||
        packet.source.kind != VoiceSourceKind::audio || !packet.member.user_id || !receive->playback.block)
        return VoiceReceiveResult::ignored;
    // rtpsize: the clear header covers fixed header, CSRCs and the extension
    // preamble; extension elements were encrypted and precede the Opus payload.
    if (!layout.payload_bytes || layout.payload_offset > plaintext.count ||
        layout.payload_bytes > plaintext.count - layout.payload_offset)
        return VoiceReceiveResult::invalid;
    const ConstSlice<u8> frame{plaintext.data + layout.payload_offset, layout.payload_bytes};
    const auto opened = decrypt(context, packet, frame, {receive->dave_scratch, sizeof(receive->dave_scratch)},
                                {receive->dave_output, sizeof(receive->dave_output)});
    if (!opened.ok() || !opened.bytes || opened.bytes > sizeof(receive->dave_output) ||
        !voice_playback_push(&receive->playback, layout.ssrc, packet.member.user_id, packet.member.speaking,
                             layout.sequence, layout.timestamp, {receive->dave_output, opened.bytes}, now_us)) {
        ++receive->stats.lost;
        return VoiceReceiveResult::lost;
    }
    ++receive->stats.queued;
    return VoiceReceiveResult::queued;
}

void voice_receive_drive(VoiceReceive* receive, VoiceConnection* connection, const GatewayVoiceControl& voice,
                         u64 now_ms) noexcept {
    if (!receive || !connection)
        return;
    receive->connection = connection;
    receive->video.decrypt = connection_decrypt_frame;
    receive->video.decrypt_context = connection;
    if (media_attachable(*connection) &&
        (receive->playback.block || voice_playback_initialize(&receive->playback))) {
        // A new Ready/Description may renumber SSRCs; drop old streams.
        voice_playback_clear(&receive->playback);
        voice_video_sink_stop(&receive->video);
        if (voice_connection_media_start(connection, {receive->receive_buffer, sizeof(receive->receive_buffer)},
                                         {receive->send_buffer, sizeof(receive->send_buffer)}, transport_receive,
                                         receive, kVoiceUdpMediaReceiveSlots)) {
            receive->media_revision = connection->ready_revision;
            if (!voice_audio_output_running(receive->output))
                voice_audio_output_start_device(&receive->output, receive->output_device, render,
                                                &receive->playback);
        }
    }
    // The call lives until the connection drains without a pending reconnect.
    if (receive->media_revision && voice_connection_drained(*connection) && !connection->reconnect_pending) {
        voice_receive_stop(receive);
        return;
    }
    voice_playback_set_deafened(&receive->playback,
                                voice.desired.self_deaf || (voice.setup.flags & gateway_voice_deaf));
    voice_video_sink_drive(&receive->video, connection,
                           receive->media_revision && receive->media_revision == connection->ready_revision, now_ms,
                           voice_playback_now_us());
}

void voice_receive_stop(VoiceReceive* receive) noexcept {
    if (!receive)
        return;
    voice_audio_output_stop(&receive->output);
    voice_playback_release(&receive->playback);
    voice_video_sink_stop(&receive->video);
    receive->video.unwanted_count = 0;
    receive->media_revision = 0;
    receive->speaking_deadline_ms = 0;
}

u32 voice_receive_speaking(VoiceReceive* receive, Slice<u64> users, u64 now_ms) noexcept {
    if (!receive)
        return 0;
    const u64 now_us = voice_playback_now_us();
    u64 next_us = 0;
    const u32 count = voice_playback_speaking(&receive->playback, now_us, users, &next_us);
    receive->speaking_deadline_ms = next_us > now_us ? now_ms + (next_us - now_us + 999) / 1000 : 0;
    return count;
}

u64 voice_receive_deadline(const VoiceReceive& receive) noexcept {
    const u64 video = voice_video_sink_deadline(receive.video);
    return !video || (receive.speaking_deadline_ms && receive.speaking_deadline_ms < video) ? receive.speaking_deadline_ms
                                                                                         : video;
}

bool voice_receive_set_user(VoiceReceive* receive, u64 user_id, u32 gain_percent, bool muted) noexcept {
    return receive && voice_playback_set_user(&receive->playback, user_id, gain_percent, muted);
}

void voice_receive_configure(VoiceReceive* receive, const VoiceAudioSettings& settings) noexcept {
    if (!receive)
        return;
    voice_playback_set_master(&receive->playback, settings.output_gain_percent);
    if (voice_device_id_equal(settings.output_device, receive->output_device))
        return;
    copy_bytes(receive->output_device, settings.output_device, sizeof(receive->output_device));
    receive->output_device[kVoiceDeviceIdCharacters - 1] = 0;
    if (voice_audio_output_running(receive->output)) {
        voice_audio_output_stop(&receive->output);
        voice_audio_output_start_device(&receive->output, receive->output_device, render, &receive->playback);
    }
}
} // namespace maicord
