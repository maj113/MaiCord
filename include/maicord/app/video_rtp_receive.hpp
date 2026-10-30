#pragma once
#include "maicord/protocol/media_packet.hpp"
#include "maicord/protocol/rtcp_feedback.hpp"

namespace maicord {
// Per-SSRC RTP video receive: sequence unwrap, reorder window, RTX (RFC 4588)
// unwrap, access-unit assembly by timestamp/marker, loss recovery with generic
// NACK (RFC 4585) and keyframe requests (PLI, or FIR per RFC 5104), and RTCP
// receiver reports. Input is transport-opened (rtpsize) plaintext RTP whose
// source was already mapped by the voice connection; output frames are the
// depacketized bytes DAVE then decrypts: Annex B with 4-byte start codes for
// H.264/H.265, OBUs for AV1 (rtp_av1.hpp).
inline constexpr u32 kVideoRtpMaxStreams = 16;
inline constexpr u32 kVideoRtpMaxCodecs = 8;
// Reorder/recovery window per stream (power of two). A 2 MiB access unit at
// ~1.2 KB per packet needs ~1750 packets, so the window holds a whole cap.
// Storage starts at a fraction of it and doubles as sequences span more slots.
inline constexpr u32 kVideoRtpPacketSlots = 2048;
inline constexpr u32 kVideoRtpPacketBytes = 1500;    // Largest stored RTP payload.
inline constexpr u32 kVideoRtpFrameBytes = 2u << 20; // Access-unit cap (Annex B bytes).
// A single gap wider than this is not NACKed: the receiver requests a
// keyframe and skips it at once.
inline constexpr u32 kVideoRtpMaxNackGap = 256;
inline constexpr u32 kVideoRtpMaxNackItems = 64; // FCI entries per stream per RTCP packet.

enum class VideoRtpCodec : u8 {
    none,
    h264,
    h265,
    av1
};
// From the negotiated codec list (opcode 1/4): media payload type, its RTX
// payload type (0 when absent) and codec.
struct VideoRtpCodecBinding {
    u8 payload_type, rtx_payload_type;
    VideoRtpCodec codec;
};
// The connection's SSRC mapping for one packet: media_ssrc is the video SSRC
// (for RTX, the binding's primary SSRC); rtx marks a retransmission stream.
struct VideoRtpSource {
    u64 user_id;
    u32 media_ssrc;
    bool rtx;
};
enum class VideoRtpPush : u8 {
    stored,
    recovered, // RTX filled a gap.
    duplicate,
    late,      // Older than the assembly point.
    padding,   // Payload-less packet: sequence space only.
    ignored,   // Unknown payload type, oversized payload or no stream capacity.
    invalid
};
struct VideoRtpFrame {
    u64 user_id;
    ConstSlice<u8> annex_b; // Borrowed until the sink returns.
    u32 ssrc, rtp_timestamp;
    VideoRtpCodec codec;  // Of the frame's payload type.
    bool keyframe;        // Contains an IDR (H.264) / IRAP (H.265) picture, or starts an AV1 sequence.
    bool references_lost; // Loss since the last delivered keyframe (or none yet).
};
// Called synchronously from video_rtp_receive/video_rtp_tick for complete,
// intact access units in RTP order. The sink may call
// video_rtp_request_keyframe but must not receive, tick, remove or release.
using VideoRtpFrameSink = void (*)(void* context, const VideoRtpFrame& frame) noexcept;
struct VideoRtpConfig {
    u32 nack_delay_us;      // Reorder grace before the first NACK of a missing packet.
    u32 nack_retry_us;      // Interval between NACKs of the same packet.
    u32 give_up_us;         // A missing packet older than this is declared lost.
    u32 start_grace_us;     // A new stream waits this long for packets reordered before its first.
    u32 keyframe_retry_us;  // Minimum interval between keyframe requests per stream.
    u32 report_interval_us; // Receiver report period.
    u32 idle_stream_us;     // Idle streams may be evicted for new SSRCs.
    u8 max_nacks;           // NACKs per missing packet.
    bool use_fir;           // FIR instead of PLI.
};
VideoRtpConfig video_rtp_default_config() noexcept;
struct VideoRtpStreamStats {
    u64 rtx_packets, late, oversized, discarded_packets, frames, damaged_frames, losses;
};
struct VideoRtpSlot {
    u64 sequence; // Extended sequence of the entry (valid unless empty).
    u64 time_us;  // Arrival (present) or detection (missing) time.
    u64 nack_us;  // Next NACK time while missing.
    u32 timestamp;
    u16 bytes;
    u8 state, payload_type, nacks;
    bool marker;
};
struct VideoRtpStream {
    VideoRtpSlot* slots; // One VirtualAlloc block of `capacity` slots then their payloads.
    u8* payloads;
    u64 user_id;
    u64 first, base, highest; // Extended sequences; base is the next to assemble.
    u64 received, expected_prior, received_prior;
    u64 last_packet_us, last_sender_report_us, keyframe_request_us;
    u64 next_nack_us, oldest_missing_us;
    u32 ssrc, rtx_ssrc, missing;
    u32 last_sender_report, prev_timestamp, last_timestamp, last_transit, jitter_timestamp;
    u32 consecutive_late, capacity; // capacity: allocated slots, a power of two up to kVideoRtpPacketSlots.
    i32 jitter_q4;
    u8 fir_sequence;
    bool started, prev_known, prev_marker, has_last_timestamp, has_transit;
    bool keyframe_needed, keyframe_seen, references_lost;
    VideoRtpStreamStats stats;
};
struct VideoRtpReceiver {
    VideoRtpStream streams[kVideoRtpMaxStreams];
    VideoRtpCodecBinding codecs[kVideoRtpMaxCodecs];
    VideoRtpConfig config;
    VideoRtpFrameSink sink;
    void* sink_context;
    u8* frame; // kVideoRtpFrameBytes assembly buffer shared by all streams.
    u64 next_report_us;
    u32 codec_count, local_ssrc;
};
// Zero-initialize, then initialize. local_ssrc is the RTCP sender SSRC (the
// Ready audio SSRC). Allocates the shared frame buffer; streams allocate
// their window (~0.4 MiB, up to ~3.1 MiB) on first packet and free it on removal/release.
bool video_rtp_initialize(VideoRtpReceiver* receiver, u32 local_ssrc, VideoRtpFrameSink sink,
                          void* context) noexcept;
void video_rtp_release(VideoRtpReceiver* receiver) noexcept;
void video_rtp_set_local_ssrc(VideoRtpReceiver* receiver, u32 local_ssrc) noexcept;
// Replaces the payload-type table; false (unchanged) for invalid/duplicate entries.
bool video_rtp_set_codecs(VideoRtpReceiver* receiver, ConstSlice<VideoRtpCodecBinding> codecs) noexcept;
// One transport-opened RTP packet (layout from voice_packet_open). Unknown
// payload types are ignored. May deliver frames to the sink.
VideoRtpPush video_rtp_receive(VideoRtpReceiver* receiver, const VideoRtpSource& source,
                               const MediaPacketLayout& layout, ConstSlice<u8> plaintext, u64 now_us) noexcept;
// Transport-opened compound RTCP: records sender reports for LSR/DLSR.
void video_rtp_receive_rtcp(VideoRtpReceiver* receiver, ConstSlice<u8> plaintext, u64 now_us) noexcept;
// Declares timed-out gaps lost and delivers frames they were blocking.
void video_rtp_tick(VideoRtpReceiver* receiver, u64 now_us) noexcept;
// Earliest time tick/feedback has work (0: none).
u64 video_rtp_deadline(const VideoRtpReceiver& receiver) noexcept;
// Builds one plaintext compound RTCP packet (RR first, then NACK and PLI/FIR)
// when a report, NACK or keyframe request is due; returns its size or 0.
// Building commits the NACK/keyframe/report schedule: if the transport is
// busy, retain and resend these bytes rather than building again.
u32 video_rtp_feedback(VideoRtpReceiver* receiver, u64 now_us, Slice<u8> output) noexcept;
// Decoder/DAVE failure path: ask the sender of media_ssrc for a keyframe.
bool video_rtp_request_keyframe(VideoRtpReceiver* receiver, u32 media_ssrc) noexcept;
void video_rtp_remove_stream(VideoRtpReceiver* receiver, u32 media_ssrc) noexcept;
const VideoRtpStream* video_rtp_find_stream(const VideoRtpReceiver& receiver, u32 media_ssrc) noexcept;
} // namespace maicord
