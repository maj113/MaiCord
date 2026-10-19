#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/protocol/media_packet.hpp"

namespace maicord {
// RFC 3550 / RFC 4585 / RFC 5104 RTCP packets for the video receive path. The
// writers produce PLAINTEXT RTCP; transport encryption (rtpsize: the first
// eight octets stay clear) is applied by voice_packet_seal like any RTCP.
inline constexpr u8 kRtcpSenderReport = 200;
inline constexpr u8 kRtcpReceiverReport = 201;
inline constexpr u8 kRtcpTransportFeedback = 205; // RTPFB
inline constexpr u8 kRtcpPayloadFeedback = 206;   // PSFB
inline constexpr u8 kRtcpFormatGenericNack = 1;   // RTPFB FMT
inline constexpr u8 kRtcpFormatPli = 1;           // PSFB FMT
inline constexpr u8 kRtcpFormatFir = 4;           // PSFB FMT
inline constexpr u32 kRtcpMaxReportBlocks = 31;
inline constexpr u32 kRtcpReportBlockBytes = 24;
inline constexpr u32 kRtcpPliBytes = 12;
inline constexpr u32 kRtcpFirBytes = 20;

struct RtcpReportBlock {
    u32 ssrc;
    u8 fraction_lost;
    i32 cumulative_lost; // Clamped to the 24-bit signed wire range.
    u32 extended_highest_sequence, jitter, last_sender_report, delay_since_last_sender_report;
};
struct RtcpNackItem {
    u16 packet_id, bitmask; // PID and BLP (bit i: PID + i + 1 also lost).
};
struct RtcpSenderInfo {
    u32 ssrc, ntp_seconds, ntp_fraction, rtp_timestamp, packet_count, octet_count;
};

// Each writer appends one packet at `offset` and returns the new end offset,
// or 0 when the output is too small or arguments are invalid (nothing written).
u32 rtcp_write_receiver_report(Slice<u8> output, u32 offset, u32 sender_ssrc,
                               ConstSlice<RtcpReportBlock> blocks) noexcept;
u32 rtcp_write_pli(Slice<u8> output, u32 offset, u32 sender_ssrc, u32 media_ssrc) noexcept;
u32 rtcp_write_fir(Slice<u8> output, u32 offset, u32 sender_ssrc, u32 media_ssrc, u8 sequence) noexcept;
// One generic NACK packet; 1..253 FCI entries.
u32 rtcp_write_nack(Slice<u8> output, u32 offset, u32 sender_ssrc, u32 media_ssrc,
                    ConstSlice<RtcpNackItem> items) noexcept;
// Packs sequence numbers given in ascending (unwrapped) order into PID/BLP
// entries. Returns the entry count; *consumed receives the number of input
// sequences represented (less than the input when items run out).
u32 rtcp_nack_pack(ConstSlice<u16> sequences, Slice<RtcpNackItem> items, u32* consumed) noexcept;
// LSR field value (middle 32 bits of the NTP timestamp).
constexpr u32 rtcp_compact_ntp(u32 ntp_seconds, u32 ntp_fraction) noexcept {
    return (ntp_seconds << 16) | (ntp_fraction >> 16);
}
// Extracts sender info from every SR in an already framing-validated compound
// packet (media_packet_plaintext). Returns the number of SRs written.
u32 rtcp_read_sender_reports(ConstSlice<u8> compound, Slice<RtcpSenderInfo> output) noexcept;

// RTP header extensions of a transport-opened packet (layout from
// media_packet_plaintext): one-byte (0xBEDE) or two-byte (0x100x) elements.
struct RtpHeaderExtensions {
    ConstSlice<u8> value; // Element `id`, when found.
    u16 ids;              // Bit n: element ID n (< 16) present.
    bool found;
};
RtpHeaderExtensions rtp_header_extensions(ConstSlice<u8> packet, const MediaPacketLayout& layout, u8 id) noexcept;

// Transport-wide congestion control feedback (draft-holmer-rmcat-transport-
// wide-cc-extensions-01): RTPFB FMT 15 reporting the arrival of every RTP
// packet stamped with a transport-wide sequence number. The SFU's downlink
// bandwidth estimate (and so the forwarded layer and pacing) depends on it.
inline constexpr u8 kRtcpFormatTransportFeedback = 15; // RTPFB FMT
// Native UDP extension ID of the transport-wide sequence number (2 bytes).
inline constexpr u8 kRtpTransportSequenceExtensionId = 5;
inline constexpr u32 kRtcpTransportFeedbackMaxPackets = 512; // Statuses per feedback packet.
inline constexpr u32 kRtcpTransportFeedbackWindow = 1024;    // Remembered sequences (power of two).
inline constexpr u32 kRtcpTransportFeedbackIntervalUs = 50'000;
struct RtcpTransportFeedbackWrite {
    u32 end, packets; // New end offset and statuses written; packets 0: nothing written.
};
// One feedback packet for base_sequence + i, i < arrivals.count, where
// arrivals[i] is the arrival time in microseconds (0: not received). Chunks
// are run-length or two-bit status vectors; deltas are 250 us ticks from the
// 64 ms reference time of the first received packet; zero padding (no P bit)
// ends the last word. Writes the longest prefix that fits the output and the
// 16-bit delta range, trimmed to end at a received packet.
RtcpTransportFeedbackWrite rtcp_write_transport_feedback(Slice<u8> output, u32 offset, u32 sender_ssrc,
                                                         u32 media_ssrc, u16 base_sequence, u8 feedback_count,
                                                         ConstSlice<u64> arrivals) noexcept;
// Receive-side record of transport sequence numbers. Zero-initialize (or
// reset) per transport. Feedback starts at the first unreported sequence; a
// late packet below it rewinds the start so its arrival is reported too.
struct RtcpTransportFeedbackState {
    u64 arrivals[kRtcpTransportFeedbackWindow]; // By extended sequence; 0: not received.
    u64 highest, next;                          // Extended sequences; next: first unreported.
    u64 next_feedback_us;
    u64 packets, feedback; // Recorded packets and feedback packets built (diagnostics).
    u32 media_ssrc;        // SSRC of the latest stamped packet.
    u8 feedback_count;
    bool started;
};
void rtcp_transport_feedback_reset(RtcpTransportFeedbackState* state) noexcept;
void rtcp_transport_feedback_record(RtcpTransportFeedbackState* state, u16 sequence, u32 media_ssrc,
                                    u64 arrival_us) noexcept;
// Earliest time a feedback packet is due (0: nothing to report).
u64 rtcp_transport_feedback_deadline(const RtcpTransportFeedbackState& state) noexcept;
// Appends one feedback packet at offset when due; returns the new end offset
// (offset when nothing was due or fitted). Building commits the reported range.
u32 rtcp_transport_feedback_build(RtcpTransportFeedbackState* state, Slice<u8> output, u32 offset,
                                  u32 sender_ssrc, u64 now_us) noexcept;
} // namespace maicord
