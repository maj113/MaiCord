#pragma once
#include "maicord/app/voice/udp_media.hpp"
#include "maicord/media/video/rtp_video_packetize.hpp"
#include "maicord/media/video/stream_quality.hpp"
#include "maicord/protocol/voice/rtcp_sender.hpp"

namespace maicord {
// The Go Live media sender on the gateway worker: DAVE-sealed access units
// and Opus packets in, RTP and RTCP out through the stream connection's
// transport (one datagram in flight; a busy transport keeps the datagram for
// the next pump). No threads or I/O of its own; time is the client clock in
// microseconds (the capture timestamps' clock).
//   video: RTP packetization (rtp_video_packetize.hpp) paced at 2.5x the
//     target bitrate, the transport-wide sequence (extension 5) and the
//     playout-delay extension (6) on every packet, a history of sent packets
//     answering generic NACKs with RTX (RFC 4588: original sequence, then the
//     payload, on the RTX SSRC and payload type).
//   audio: one Opus packet per RTP packet (payload type 120, 48 kHz), the
//     Discord speaking extension (9) carrying SOUNDSHARE.
//   RTCP: sender reports each second; PLI/FIR ask for a keyframe; transport
//     feedback, REMB and receiver-report loss drive the bandwidth estimate.
inline constexpr u8 kGoLiveTransportSequenceExtensionId = 5;
inline constexpr u8 kGoLivePlayoutDelayExtensionId = 6;
inline constexpr u8 kGoLiveSpeakingExtensionId = 9;
inline constexpr u8 kGoLiveOpusPayloadType = 120;
inline constexpr u32 kGoLiveHistoryPackets = 1024;  // Power of two.
inline constexpr u32 kGoLiveTransportLog = 4096;    // Power of two.
inline constexpr u32 kGoLiveAudioQueue = 8;
inline constexpr u32 kGoLiveRtxQueue = 64;
// RTP header, extensions, RTX original sequence and payload.
inline constexpr u32 kGoLiveDatagramBytes = 12 + 12 + 2 + kRtpVideoMaximumPayload;
inline constexpr u32 kGoLiveFrameBytes = 2u << 20; // Largest sealed access unit (the receivers' cap).
inline constexpr u64 kGoLiveReportIntervalUs = 1'000'000;

// Send-side bandwidth estimation as WebRTC's: a delay-based estimate from
// transport-wide feedback (trendline of one-way delay variation between
// packet groups with an adaptive overuse threshold: overuse cuts to 85 % of
// the acknowledged rate, normal use grows 8 % a second), a loss-based bound
// (above 10 % loss the rate drops by half the loss; under 2 % it may grow),
// capped by REMB and the quality's bounds.
inline constexpr u32 kGoLiveTrendWindow = 20;
struct GoLiveBandwidth {
    StreamBitrate bounds;
    u32 target, remb, acked_bps;
    i64 group_send_us, group_arrival_us, previous_send_us, previous_arrival_us, first_arrival_us;
    double accumulated, smoothed, threshold;
    double trend_x[kGoLiveTrendWindow], trend_y[kGoLiveTrendWindow];
    u32 trend_count, groups;
    i64 overuse_since_us;
    u8 overuse_hits;
    bool has_group, has_previous, has_first;
    u64 last_update_us, last_threshold_us;
    u64 acked_bytes, acked_since_us;
    u32 lost, received; // Of the current feedback batch.
};
void go_live_bandwidth_reset(GoLiveBandwidth* bandwidth, StreamBitrate bounds, u64 now_us) noexcept;
// One packet of a transport feedback batch, in sequence order: its send time
// and size from our log, its arrival on the receiver's clock (or lost).
void go_live_bandwidth_packet(GoLiveBandwidth* bandwidth, u64 send_us, i64 arrival_us, u32 bytes,
                              bool received) noexcept;
// After a batch: applies the detector and the loss bound to the target.
void go_live_bandwidth_update(GoLiveBandwidth* bandwidth, u64 now_us) noexcept;
// Receiver report loss (fraction / 256) when no transport feedback comes.
void go_live_bandwidth_loss(GoLiveBandwidth* bandwidth, u8 fraction_lost, u64 now_us) noexcept;
void go_live_bandwidth_remb(GoLiveBandwidth* bandwidth, u64 bitrate) noexcept;

struct GoLiveSendStreams {
    u32 audio_ssrc, video_ssrc, rtx_ssrc;
    u8 video_payload_type, rtx_payload_type;
    RtpVideoPacketCodec codec;
};
// Plaintext RTP/RTCP to the transport (voice_connection_media_send).
struct GoLiveSendLink {
    void* context;
    VoiceUdpMediaSend (*send)(void* context, ConstSlice<u8> packet) noexcept;
};
struct GoLiveHistoryEntry {
    u64 retransmitted_us;
    u32 timestamp;
    u16 sequence, bytes;
    bool marker, valid;
    u8 payload[kRtpVideoMaximumPayload];
};
struct GoLiveSentPacket {
    u64 send_us;
    u16 sequence, bytes;
    bool valid;
};
struct GoLiveAudioPacket {
    u32 timestamp;
    u16 bytes;
    u8 data[1275 + 32]; // A DAVE-sealed Opus packet.
};
struct GoLiveSender {
    GoLiveSendStreams streams;
    GoLiveBandwidth bandwidth;
    GoLiveHistoryEntry* history; // kGoLiveHistoryPackets, with the frame buffer in one allocation.
    u8* frame;                   // kGoLiveFrameBytes: the access unit being sent.
    RtpVideoPacketizer packetizer;
    GoLiveSentPacket transport_log[kGoLiveTransportLog];
    GoLiveAudioPacket audio[kGoLiveAudioQueue];
    u16 rtx[kGoLiveRtxQueue];
    u8 datagram[kGoLiveDatagramBytes];
    u64 next_report_us, pace_us;
    i64 budget_bytes;
    u32 audio_head, audio_count, rtx_count, datagram_bytes, frame_timestamp;
    u32 video_octets, video_count, audio_octets, audio_total; // Sender report counters.
    u16 video_sequence, rtx_sequence, audio_sequence, transport_sequence;
    bool frame_active, datagram_video, datagram_transport;
    bool keyframe_wanted; // A PLI/FIR arrived; go_live_sender_take_keyframe consumes it.
};
// Allocates the history and frame buffer; sequences start at random values.
bool go_live_sender_start(GoLiveSender* sender, const GoLiveSendStreams& streams, StreamBitrate bounds,
                          u64 now_us) noexcept;
void go_live_sender_stop(GoLiveSender* sender) noexcept;
// Where the next sealed access unit is written (null while one is still being sent).
Slice<u8> go_live_sender_frame_buffer(GoLiveSender* sender) noexcept;
// Commits bytes written to the frame buffer as the next access unit to send.
bool go_live_sender_frame(GoLiveSender* sender, u32 bytes, u32 rtp_timestamp, bool keyframe) noexcept;
// Queues one sealed Opus packet (false when the queue is full: the oldest
// would be late anyway, so the caller drops this one).
bool go_live_sender_audio(GoLiveSender* sender, ConstSlice<u8> packet, u32 rtp_timestamp) noexcept;
// Transport-opened RTCP from the stream server.
void go_live_sender_rtcp(GoLiveSender* sender, ConstSlice<u8> compound, u64 now_us) noexcept;
// Sends what is due: a held datagram, audio, RTX, video within the pacing
// budget, and the sender report. Returns the time more is due (0: waits for
// input or the transport's completion).
u64 go_live_sender_pump(GoLiveSender* sender, const GoLiveSendLink& link, u64 now_us) noexcept;
// True once since the last call when a receiver asked for a keyframe.
bool go_live_sender_take_keyframe(GoLiveSender* sender) noexcept;
} // namespace maicord
