#pragma once
#include "maicord/protocol/voice/rtcp_feedback.hpp"

namespace maicord {
// RTCP of a media sender (Go Live): the sender reports it writes and the
// feedback it reads back from the SFU. Plaintext; transport encryption is
// voice_packet_seal's, as for every RTCP packet.
inline constexpr u8 kRtcpFormatFir = 4;             // PSFB FMT
inline constexpr u8 kRtcpFormatApplication = 15;    // PSFB FMT (REMB: "REMB" identifier)
inline constexpr u32 kRtcpSenderReportBytes = 28;   // Without report blocks.
inline constexpr u64 kNtpUnixOffsetSeconds = 2'208'988'800;
struct RtcpSenderReport {
    u32 ssrc, ntp_seconds, ntp_fraction, rtp_timestamp, packets, octets;
};
// Appends one SR without report blocks; 0 when it does not fit.
u32 rtcp_write_sender_report(Slice<u8> output, u32 offset, const RtcpSenderReport& report) noexcept;
// NTP time of a Unix time in microseconds.
constexpr void rtcp_ntp_time(u64 unix_us, u32* seconds, u32* fraction) noexcept {
    *seconds = static_cast<u32>(unix_us / 1'000'000 + kNtpUnixOffsetSeconds);
    *fraction = static_cast<u32>(((unix_us % 1'000'000) << 32) / 1'000'000);
}

// One transport-wide feedback entry (RTPFB FMT 15): whether the packet with
// that transport sequence arrived, and when (microseconds on the receiver's
// own clock: only differences between arrivals are meaningful).
struct RtcpTransportArrival {
    u16 sequence;
    bool received;
    i64 arrival_us;
};
// Calls from rtcp_read_feedback, in packet order. Any may be null.
struct RtcpFeedbackVisitor {
    void* context;
    void (*keyframe)(void* context, u32 media_ssrc) noexcept;           // PLI or FIR.
    void (*nack)(void* context, u32 media_ssrc, u16 sequence) noexcept; // Every lost packet a NACK names.
    void (*remb)(void* context, u64 bitrate) noexcept;                  // Receiver estimate, bits per second.
    void (*report)(void* context, const RtcpReportBlock& block) noexcept; // RR/SR report blocks.
    // One transport-wide feedback packet, then its entries in sequence order.
    void (*transport)(void* context, ConstSlice<RtcpTransportArrival> arrivals) noexcept;
};
// Walks a transport-opened compound RTCP packet (malformed parts end the
// walk; what came before is delivered).
void rtcp_read_feedback(ConstSlice<u8> compound, const RtcpFeedbackVisitor& visitor) noexcept;
} // namespace maicord
