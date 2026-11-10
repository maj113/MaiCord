#include "maicord/protocol/voice/rtcp_sender.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {
void report_blocks(const u8* blocks, u32 count, u32 bytes, const RtcpFeedbackVisitor& visitor) noexcept {
    for (u32 i = 0; i != count && (i + 1) * kRtcpReportBlockBytes <= bytes; ++i) {
        const u8* entry = blocks + i * kRtcpReportBlockBytes;
        const u32 loss = byte_load_be32(entry + 4);
        RtcpReportBlock block{byte_load_be32(entry), static_cast<u8>(loss >> 24),
                              static_cast<i32>(loss << 8) >> 8, byte_load_be32(entry + 8),
                              byte_load_be32(entry + 12), byte_load_be32(entry + 16), byte_load_be32(entry + 20)};
        visitor.report(visitor.context, block);
    }
}
// RTPFB FMT 15 FCI (after the media SSRC): base sequence, status count,
// reference time, feedback count, chunks, then receive deltas.
void transport(const u8* fci, u32 bytes, const RtcpFeedbackVisitor& visitor) noexcept {
    if (bytes < 8)
        return;
    const u16 base = byte_load_be16(fci);
    const u32 count = byte_load_be16(fci + 2);
    const i32 reference = static_cast<i32>(byte_load_be32(fci + 4) & 0xffffff00) >> 8;
    if (!count || count > kRtcpTransportFeedbackMaxPackets)
        return;
    u8 status[kRtcpTransportFeedbackMaxPackets];
    u32 statuses = 0, at = 8;
    while (statuses < count) {
        if (bytes - at < 2)
            return;
        const u32 chunk = byte_load_be16(fci + at);
        at += 2;
        if (!(chunk & 0x8000)) {
            const u8 symbol = static_cast<u8>((chunk >> 13) & 3);
            for (u32 run = chunk & 0x1fff; run && statuses < count; --run)
                status[statuses++] = symbol;
        } else if (!(chunk & 0x4000)) {
            for (u32 i = 0; i != 14 && statuses < count; ++i)
                status[statuses++] = static_cast<u8>((chunk >> (13 - i)) & 1);
        } else
            for (u32 i = 0; i != 7 && statuses < count; ++i)
                status[statuses++] = static_cast<u8>((chunk >> (12 - 2 * i)) & 3);
    }
    RtcpTransportArrival arrivals[kRtcpTransportFeedbackMaxPackets];
    i64 time = i64{reference} * 64'000;
    for (u32 i = 0; i != count; ++i) {
        arrivals[i] = {static_cast<u16>(base + i), false, 0};
        if (status[i] == 1) {
            if (bytes - at < 1)
                return;
            time += i64{fci[at++]} * 250;
        } else if (status[i] == 2) {
            if (bytes - at < 2)
                return;
            time += i64{static_cast<i16>(byte_load_be16(fci + at))} * 250;
            at += 2;
        } else if (status[i] == 3)
            return;
        if (status[i])
            arrivals[i] = {static_cast<u16>(base + i), true, time};
    }
    visitor.transport(visitor.context, {arrivals, count});
}
} // namespace

u32 rtcp_write_sender_report(Slice<u8> output, u32 offset, const RtcpSenderReport& report) noexcept {
    if (offset > output.count || output.count - offset < kRtcpSenderReportBytes)
        return 0;
    u8* packet = output.data + offset;
    packet[0] = 0x80;
    packet[1] = kRtcpSenderReport;
    packet[2] = 0;
    packet[3] = kRtcpSenderReportBytes / 4 - 1;
    byte_store_be32(packet + 4, report.ssrc);
    byte_store_be32(packet + 8, report.ntp_seconds);
    byte_store_be32(packet + 12, report.ntp_fraction);
    byte_store_be32(packet + 16, report.rtp_timestamp);
    byte_store_be32(packet + 20, report.packets);
    byte_store_be32(packet + 24, report.octets);
    return offset + kRtcpSenderReportBytes;
}

void rtcp_read_feedback(ConstSlice<u8> compound, const RtcpFeedbackVisitor& visitor) noexcept {
    u32 offset = 0;
    while (compound.count - offset >= 4) {
        const u8* part = compound.data + offset;
        const u32 bytes = (u32{byte_load_be16(part + 2)} + 1) * 4;
        if ((part[0] >> 6) != 2 || bytes > compound.count - offset)
            return;
        const u8 format = part[0] & 31, type = part[1];
        const u8* fci = part + 12;
        const u32 fci_bytes = bytes >= 12 ? bytes - 12 : 0;
        const u32 media = bytes >= 12 ? byte_load_be32(part + 8) : 0;
        if (type == kRtcpReceiverReport && visitor.report && bytes >= 8)
            report_blocks(part + 8, format, bytes - 8, visitor);
        else if (type == kRtcpSenderReport && visitor.report && bytes >= kRtcpSenderReportBytes)
            report_blocks(part + kRtcpSenderReportBytes, format, bytes - kRtcpSenderReportBytes, visitor);
        else if (type == kRtcpTransportFeedback && bytes >= 12) {
            if (format == kRtcpFormatGenericNack && visitor.nack)
                for (u32 at = 0; at + 4 <= fci_bytes; at += 4) {
                    const u16 id = byte_load_be16(fci + at), mask = byte_load_be16(fci + at + 2);
                    visitor.nack(visitor.context, media, id);
                    for (u32 bit = 0; bit != 16; ++bit)
                        if (mask >> bit & 1)
                            visitor.nack(visitor.context, media, static_cast<u16>(id + bit + 1));
                }
            else if (format == kRtcpFormatTransportFeedback && visitor.transport)
                transport(fci, fci_bytes, visitor);
        } else if (type == kRtcpPayloadFeedback && bytes >= 12) {
            if (format == kRtcpFormatPli && visitor.keyframe)
                visitor.keyframe(visitor.context, media);
            else if (format == kRtcpFormatFir && visitor.keyframe)
                for (u32 at = 0; at + 8 <= fci_bytes; at += 8)
                    visitor.keyframe(visitor.context, byte_load_be32(fci + at));
            else if (format == kRtcpFormatApplication && visitor.remb && fci_bytes >= 8 && fci[0] == 'R' &&
                     fci[1] == 'E' && fci[2] == 'M' && fci[3] == 'B') {
                const u32 field = byte_load_be32(fci + 4);
                visitor.remb(visitor.context, u64{field & 0x3ffff} << ((field >> 18) & 63));
            }
        }
        offset += bytes;
    }
}
} // namespace maicord
