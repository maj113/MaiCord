#pragma once
#include "maicord/core/slice.hpp"

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
} // namespace maicord
