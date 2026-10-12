#include "maicord/app/voice_video_sink.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u32 kBufferBytes = 2 * kVideoRtpFrameBytes;
VideoRtpCodec rtp_codec(VoiceVideoCodec codec) noexcept {
    return codec == VoiceVideoCodec::h264   ? VideoRtpCodec::h264
           : codec == VoiceVideoCodec::h265 ? VideoRtpCodec::h265
                                            : VideoRtpCodec::none;
}
// Select Protocol offers one decode codec at index 0: PT 101, RTX 102.
bool bind_codec(VoiceVideoSink& sink) noexcept {
    const auto codec = rtp_codec(sink.codec);
    if (codec == VideoRtpCodec::none)
        return video_rtp_set_codecs(&sink.rtp, {});
    const VideoRtpCodecBinding binding{kVoiceVideoPayloadTypeBase, kVoiceVideoPayloadTypeBase + 1, codec};
    return video_rtp_set_codecs(&sink.rtp, {&binding, 1});
}
void end_stream(VoiceVideoSink& sink, u32 index) noexcept {
    const u32 ssrc = sink.ended[index];
    sink.ended[index] = sink.ended[--sink.ended_count];
    sink.ended[sink.ended_count] = 0;
    ++sink.stats.streams_ended;
    if (sink.frames.end_stream)
        sink.frames.end_stream(sink.frames.context, ssrc);
}
void deliver(void* context, const VideoRtpFrame& frame) noexcept {
    auto& sink = *static_cast<VoiceVideoSink*>(context);
    ++sink.stats.frames;
    if (!sink.decrypt || !frame.annex_b.count || frame.annex_b.count > kVideoRtpFrameBytes) {
        ++sink.stats.dropped;
        return;
    }
    if (!sink.buffer) {
        sink.buffer = static_cast<u8*>(VirtualAlloc(nullptr, kBufferBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!sink.buffer) {
            ++sink.stats.dropped;
            return;
        }
    }
    const auto opened = sink.decrypt(sink.decrypt_context, frame.user_id, frame.ssrc, frame.annex_b,
                                     {sink.buffer + kVideoRtpFrameBytes, kVideoRtpFrameBytes},
                                     {sink.buffer, kVideoRtpFrameBytes});
    if (!opened.ok() || !opened.bytes || opened.bytes > kVideoRtpFrameBytes) {
        // Without this frame the decoder lacks a reference: ask for a fresh
        // keyframe (throttled by the RTP core), e.g. once DAVE keys arrive.
        ++sink.stats.dave_rejected;
        video_rtp_request_keyframe(&sink.rtp, frame.ssrc);
        return;
    }
    bool known = false;
    for (u32 i = 0; i != sink.ended_count && !known; ++i)
        known = sink.ended[i] == frame.ssrc;
    if (!known && sink.ended_count != kVideoRtpMaxStreams)
        sink.ended[sink.ended_count++] = frame.ssrc;
    if (sink.frames.push_frame &&
        sink.frames.push_frame(sink.frames.context, frame.ssrc, {sink.buffer, opened.bytes}, frame.rtp_timestamp,
                               frame.keyframe, !frame.references_lost))
        ++sink.stats.delivered;
    else
        ++sink.stats.dropped;
}
bool ensure_rtp(VoiceVideoSink& sink, u32 local_ssrc) noexcept {
    if (sink.rtp_ready)
        return true;
    if (rtp_codec(sink.codec) == VideoRtpCodec::none || !video_rtp_initialize(&sink.rtp, local_ssrc, deliver, &sink))
        return false;
    if (!bind_codec(sink)) {
        video_rtp_release(&sink.rtp);
        return false;
    }
    sink.rtp_ready = true;
    return true;
}
bool current_video(const VoiceConnection& connection, u32 ssrc, u64 user_id) noexcept {
    const auto source = voice_participants_source(connection.participants, ssrc);
    return source.user_id == user_id &&
           (source.kind == VoiceSourceKind::video || source.kind == VoiceSourceKind::screen);
}
void update_wants(VoiceVideoSink& sink, VoiceConnection& connection) noexcept {
    VoiceSinkWant wants[kVoiceSinkWantsMaximum];
    u32 count = 0;
    const auto& participants = connection.participants;
    for (u32 i = 0; i != participants.source_count && count != kVoiceSinkWantsMaximum; ++i) {
        const auto& source = participants.sources[i];
        if (source.kind != VoiceSourceKind::video && source.kind != VoiceSourceKind::screen)
            continue;
        for (u32 j = 0; j != sink.unwanted_count; ++j)
            if (sink.unwanted[j] == source.user_id) {
                wants[count++] = {source.ssrc, 0};
                break;
            }
    }
    voice_connection_set_sink_wants(&connection, kVoiceSinkWantsDefaultQuality, {wants, count});
}
} // namespace

bool voice_video_source(const VoiceSsrcBinding& source) noexcept {
    return source.user_id && (source.kind == VoiceSourceKind::video || source.kind == VoiceSourceKind::screen ||
                              source.kind == VoiceSourceKind::rtx);
}

bool voice_video_sink_packet(VoiceVideoSink* sink, const VoiceReceivedPacket& packet, ConstSlice<u8> plaintext,
                             u64 now_us) noexcept {
    if (!sink)
        return false;
    const auto& layout = packet.transport.layout;
    if (!packet.transport.ok() || layout.kind != MediaPacketKind::rtp || !voice_video_source(packet.source) ||
        packet.member.user_id != packet.source.user_id || !packet.source.primary_ssrc ||
        !ensure_rtp(*sink, sink->rtp.local_ssrc)) {
        ++sink->stats.ignored;
        return false;
    }
    ++sink->stats.packets;
    const VideoRtpSource source{packet.source.user_id, packet.source.primary_ssrc,
                                packet.source.kind == VoiceSourceKind::rtx};
    const auto pushed = video_rtp_receive(&sink->rtp, source, layout, plaintext, now_us);
    if (pushed == VideoRtpPush::ignored || pushed == VideoRtpPush::invalid) {
        ++sink->stats.ignored;
        return false;
    }
    return true;
}

void voice_video_sink_rtcp(VoiceVideoSink* sink, ConstSlice<u8> plaintext, u64 now_us) noexcept {
    if (!sink || !sink->rtp_ready)
        return;
    ++sink->stats.rtcp;
    video_rtp_receive_rtcp(&sink->rtp, plaintext, now_us);
}

void voice_video_sink_drive(VoiceVideoSink* sink, VoiceConnection* connection, bool media_current, u64 now_ms,
                            u64 now_us) noexcept {
    if (!sink)
        return;
    sink->deadline_ms = 0;
    if (!connection)
        return;
    const auto codec = connection->media.has_description ? connection->media.video_codec : VoiceVideoCodec::none;
    if (codec != sink->codec) {
        sink->codec = codec;
        if (sink->rtp_ready)
            bind_codec(*sink);
    }
    update_wants(*sink, *connection);
    if (!sink->rtp_ready)
        return;
    video_rtp_set_local_ssrc(&sink->rtp, connection->media.ssrc);
    for (auto& pending : sink->pending_keyframes)
        if (const u32 ssrc = static_cast<u32>(InterlockedExchange(&pending, 0));
            ssrc && video_rtp_request_keyframe(&sink->rtp, ssrc))
            ++sink->stats.keyframe_requests;
    // A stream ends when its SSRC no longer maps to that user's video (leave,
    // op 12 replacement, new Ready) or the RTP core evicted it.
    for (auto& stream : sink->rtp.streams)
        if (stream.slots && !current_video(*connection, stream.ssrc, stream.user_id))
            video_rtp_remove_stream(&sink->rtp, stream.ssrc);
    for (u32 i = 0; i < sink->ended_count;)
        if (!video_rtp_find_stream(sink->rtp, sink->ended[i]))
            end_stream(*sink, i);
        else
            ++i;
    video_rtp_tick(&sink->rtp, now_us);
    if (!media_current) {
        sink->feedback_bytes = 0;
    } else {
        if (!sink->feedback_bytes)
            sink->feedback_bytes = video_rtp_feedback(&sink->rtp, now_us, {sink->feedback, sizeof(sink->feedback)});
        if (sink->feedback_bytes) {
            const auto sent = voice_connection_media_send(connection, connection->ready_revision,
                                                          {sink->feedback, sink->feedback_bytes});
            if (sent != VoiceUdpMediaSend::busy) {
                if (sent == VoiceUdpMediaSend::accepted)
                    ++sink->stats.feedback_sent;
                sink->feedback_bytes = 0;
            }
        }
    }
    // Busy feedback retries shortly; otherwise wake for the RTP core's timers.
    const u64 due = video_rtp_deadline(sink->rtp);
    if (sink->feedback_bytes)
        sink->deadline_ms = now_ms + 2;
    else if (due)
        sink->deadline_ms = due <= now_us ? now_ms : now_ms + (due - now_us + 999) / 1000;
}

void voice_video_sink_stop(VoiceVideoSink* sink) noexcept {
    if (!sink)
        return;
    while (sink->ended_count)
        end_stream(*sink, sink->ended_count - 1);
    if (sink->rtp_ready)
        video_rtp_release(&sink->rtp);
    if (sink->buffer) {
        SecureZeroMemory(sink->buffer, kBufferBytes);
        VirtualFree(sink->buffer, 0, MEM_RELEASE);
    }
    sink->buffer = nullptr;
    for (auto& pending : sink->pending_keyframes)
        InterlockedExchange(&pending, 0);
    sink->rtp_ready = false;
    sink->codec = VoiceVideoCodec::none;
    sink->feedback_bytes = 0;
    sink->deadline_ms = 0;
}

void voice_video_sink_request_keyframe(VoiceVideoSink* sink, u32 ssrc) noexcept {
    if (!sink || !ssrc)
        return;
    bool queued = false;
    for (auto& pending : sink->pending_keyframes) {
        const LONG previous = InterlockedCompareExchange(&pending, static_cast<LONG>(ssrc), 0);
        if (!previous || static_cast<u32>(previous) == ssrc) {
            queued = true;
            break;
        }
    }
    if (queued && sink->wake)
        sink->wake(sink->wake_context);
}

u64 voice_video_sink_deadline(const VoiceVideoSink& sink) noexcept {
    return sink.deadline_ms;
}

u32 voice_video_users(const VoiceConnection& connection, const VoiceVideoSink* sink, u64 self_user_id,
                      Slice<VoiceVideoUser> output) noexcept {
    if (!output.data && output.count)
        return 0;
    u32 count = 0;
    const auto& participants = connection.participants;
    for (u32 i = 0; i != participants.source_count; ++i) {
        const auto& source = participants.sources[i];
        if ((source.kind != VoiceSourceKind::video && source.kind != VoiceSourceKind::screen) || !source.active ||
            !source.user_id || source.user_id == self_user_id)
            continue;
        u32 at = 0;
        while (at != count && output[at].user_id < source.user_id)
            ++at;
        if (at != count && output[at].user_id == source.user_id) {
            // Camera before screen, then the highest simulcast quality.
            auto& user = output[at];
            const bool better = (user.kind == VoiceSourceKind::screen && source.kind == VoiceSourceKind::video) ||
                                (user.kind == source.kind && source.quality > user.quality);
            if (better)
                user = {source.user_id, source.ssrc, 0, source.kind, source.quality};
            continue;
        }
        if (count == output.count)
            continue;
        for (u32 k = count; k > at; --k)
            output[k] = output[k - 1];
        output[at] = {source.user_id, source.ssrc, 0, source.kind, source.quality};
        ++count;
    }
    for (u32 i = 0; i != count; ++i)
        if (const auto* stream = sink && sink->rtp_ready ? video_rtp_find_stream(sink->rtp, output[i].ssrc) : nullptr)
            output[i].frames = static_cast<u32>(minimum<u64>(stream->stats.frames, ~u32{0}));
    return count;
}
} // namespace maicord
