#pragma once
#include "maicord/app/video_frame_sink.hpp"
#include "maicord/app/video_rtp_receive.hpp"
#include "maicord/app/voice_connection.hpp"

namespace maicord {
// Call video receive between the voice transport and a decoder:
//   mapped video/RTX RTP -> RTP core (reorder, RTX, NACK/PLI, assembly)
//   -> DAVE (the sender's shared audio/video epoch) -> VideoFrameSink.
// Worker-owned except voice_video_sink_request_keyframe. The RTP core and the
// DAVE frame buffers are allocated when the first video packet of a negotiated
// codec arrives and are freed by stop (leave/disconnect).
inline constexpr u32 kVoiceVideoFeedbackBytes = 1200;
inline constexpr u32 kVoiceVideoMaximumUsers = 16;
inline constexpr u32 kVoiceVideoUnwantedUsers = 32;
// DAVE stage for one assembled frame from a currently mapped video SSRC.
// Production binds voice_connection_dave_decrypt_frame; tests substitute.
using VoiceVideoDecrypt = DaveMediaResult (*)(void* context, u64 user_id, u32 ssrc, ConstSlice<u8> frame,
                                              Slice<u8> scratch, Slice<u8> output) noexcept;
struct VoiceVideoStats {
    u64 packets, rtcp, ignored, frames, delivered, dave_rejected, dropped, keyframe_requests, feedback_sent,
        streams_ended;
};
// A remote participant announcing an active camera or screen stream (op 12).
struct VoiceVideoUser {
    u64 user_id;
    u32 ssrc;   // Highest-quality active video SSRC of the user.
    u32 frames; // Frames assembled for it so far; zero until media arrives.
    VoiceSourceKind kind;
    u8 quality;
};
struct VoiceVideoSink {
    VideoRtpReceiver rtp;
    VideoFrameSink frames;
    VoiceVideoDecrypt decrypt;
    void* decrypt_context;
    // Optional worker wake-up after a cross-thread keyframe request.
    void (*wake)(void* context) noexcept;
    void* wake_context;
    u8* buffer; // DAVE output then scratch, 2 * kVideoRtpFrameBytes.
    LONG pending_keyframes[kVideoRtpMaxStreams]; // Interlocked only: SSRCs queued by decoders.
    u32 ended[kVideoRtpMaxStreams]; // Streams delivered to the decoder, for end_stream.
    u64 unwanted[kVoiceVideoUnwantedUsers]; // Users whose video is stopped (op 15 quality 0).
    u64 deadline_ms;
    u32 ended_count, unwanted_count, feedback_bytes;
    VoiceVideoCodec codec;
    VoiceVideoStats stats;
    u8 feedback[kVoiceVideoFeedbackBytes]; // Built RTCP waiting for the transport.
    bool rtp_ready;
};
// True for RTP sources that belong to the video pipeline (camera, screen and
// their RTX; RTX of audio is not negotiated and is ignored by the RTP core).
bool voice_video_source(const VoiceSsrcBinding& source) noexcept;
// One transport-opened, mapped RTP packet of a video source. Assembled frames
// are DAVE-opened and pushed synchronously. False when ignored.
bool voice_video_sink_packet(VoiceVideoSink* sink, const VoiceReceivedPacket& packet, ConstSlice<u8> plaintext,
                             u64 now_us) noexcept;
// Transport-opened RTCP (sender reports for receiver-report timing).
void voice_video_sink_rtcp(VoiceVideoSink* sink, ConstSlice<u8> plaintext, u64 now_us) noexcept;
// After voice_connection_drive while media is attached (media_current): binds
// the negotiated codec, applies keyframe requests, ends streams whose SSRC no
// longer maps to a video source, sends due RTCP feedback and updates the op 15
// sink wants. now_us uses the same clock as packet delivery.
void voice_video_sink_drive(VoiceVideoSink* sink, VoiceConnection* connection, bool media_current, u64 now_ms,
                            u64 now_us) noexcept;
// Ends every stream, releases the RTP core and frame buffers. Bindings and
// per-user wants stay (a reconnect renumbers SSRCs but keeps preferences).
void voice_video_sink_stop(VoiceVideoSink* sink) noexcept;
// Any thread. The request is sent with the next drive.
void voice_video_sink_request_keyframe(VoiceVideoSink* sink, u32 ssrc) noexcept;
u64 voice_video_sink_deadline(const VoiceVideoSink& sink) noexcept;
// Remote users with an active video source, sorted by user ID (self excluded).
u32 voice_video_users(const VoiceConnection& connection, const VoiceVideoSink* sink, u64 self_user_id,
                      Slice<VoiceVideoUser> output) noexcept;
} // namespace maicord
