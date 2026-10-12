#include "maicord/protocol/rtcp_feedback.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
void store16(u8* output, u32 value) noexcept {
    output[0] = static_cast<u8>(value >> 8);
    output[1] = static_cast<u8>(value);
}
// Validates space for `bytes` (a multiple of four) at offset and writes the
// common header: V=2, no padding, count/FMT, packet type and word length - 1.
u8* header(Slice<u8> output, u32 offset, u32 bytes, u8 count, u8 type) noexcept {
    if (!output.data || !byte_range_valid({output.data, output.count}) || offset > output.count ||
        bytes > output.count - offset || bytes < 4 || (bytes & 3) || count > 31)
        return nullptr;
    u8* packet = output.data + offset;
    packet[0] = static_cast<u8>(0x80 | count);
    packet[1] = type;
    store16(packet + 2, bytes / 4 - 1);
    return packet;
}
} // namespace

u32 rtcp_write_receiver_report(Slice<u8> output, u32 offset, u32 sender_ssrc,
                               ConstSlice<RtcpReportBlock> blocks) noexcept {
    if (blocks.count > kRtcpMaxReportBlocks || (blocks.count && !blocks.data))
        return 0;
    const u32 bytes = 8 + blocks.count * kRtcpReportBlockBytes;
    u8* packet = header(output, offset, bytes, static_cast<u8>(blocks.count), kRtcpReceiverReport);
    if (!packet)
        return 0;
    byte_store_be32(packet + 4, sender_ssrc);
    for (u32 i = 0; i != blocks.count; ++i) {
        const auto& block = blocks[i];
        u8* entry = packet + 8 + i * kRtcpReportBlockBytes;
        byte_store_be32(entry, block.ssrc);
        // RFC 3550 6.4.1: 24-bit signed cumulative loss, saturated.
        const i32 lost = block.cumulative_lost > 0x7fffff    ? 0x7fffff
                         : block.cumulative_lost < -0x800000 ? -0x800000
                                                             : block.cumulative_lost;
        byte_store_be32(entry + 4, (u32{block.fraction_lost} << 24) | (static_cast<u32>(lost) & 0xffffff));
        byte_store_be32(entry + 8, block.extended_highest_sequence);
        byte_store_be32(entry + 12, block.jitter);
        byte_store_be32(entry + 16, block.last_sender_report);
        byte_store_be32(entry + 20, block.delay_since_last_sender_report);
    }
    return offset + bytes;
}

u32 rtcp_write_pli(Slice<u8> output, u32 offset, u32 sender_ssrc, u32 media_ssrc) noexcept {
    u8* packet = header(output, offset, kRtcpPliBytes, kRtcpFormatPli, kRtcpPayloadFeedback);
    if (!packet)
        return 0;
    byte_store_be32(packet + 4, sender_ssrc);
    byte_store_be32(packet + 8, media_ssrc);
    return offset + kRtcpPliBytes;
}

u32 rtcp_write_fir(Slice<u8> output, u32 offset, u32 sender_ssrc, u32 media_ssrc, u8 sequence) noexcept {
    u8* packet = header(output, offset, kRtcpFirBytes, kRtcpFormatFir, kRtcpPayloadFeedback);
    if (!packet)
        return 0;
    // RFC 5104 4.3.1: the common "media source" SSRC is zero; the FCI names it.
    byte_store_be32(packet + 4, sender_ssrc);
    byte_store_be32(packet + 8, 0);
    byte_store_be32(packet + 12, media_ssrc);
    byte_store_be32(packet + 16, u32{sequence} << 24);
    return offset + kRtcpFirBytes;
}

u32 rtcp_write_nack(Slice<u8> output, u32 offset, u32 sender_ssrc, u32 media_ssrc,
                    ConstSlice<RtcpNackItem> items) noexcept {
    if (!items.count || !items.data || items.count > 253)
        return 0;
    const u32 bytes = 12 + items.count * 4;
    u8* packet = header(output, offset, bytes, kRtcpFormatGenericNack, kRtcpTransportFeedback);
    if (!packet)
        return 0;
    byte_store_be32(packet + 4, sender_ssrc);
    byte_store_be32(packet + 8, media_ssrc);
    for (u32 i = 0; i != items.count; ++i) {
        store16(packet + 12 + i * 4, items[i].packet_id);
        store16(packet + 14 + i * 4, items[i].bitmask);
    }
    return offset + bytes;
}

u32 rtcp_nack_pack(ConstSlice<u16> sequences, Slice<RtcpNackItem> items, u32* consumed) noexcept {
    u32 count = 0, index = 0;
    if (sequences.data && items.data) {
        while (index != sequences.count && count != items.count) {
            RtcpNackItem item{sequences[index++], 0};
            while (index != sequences.count) {
                const u16 distance = static_cast<u16>(sequences[index] - item.packet_id);
                if (distance == 0) { // Duplicate input: already represented.
                    ++index;
                    continue;
                }
                if (distance > 16)
                    break;
                item.bitmask = static_cast<u16>(item.bitmask | (1u << (distance - 1)));
                ++index;
            }
            items[count++] = item;
        }
    }
    if (consumed)
        *consumed = index;
    return count;
}

u32 rtcp_read_sender_reports(ConstSlice<u8> compound, Slice<RtcpSenderInfo> output) noexcept {
    if (!byte_range_valid(compound) || (compound.count && !compound.data))
        return 0;
    u32 offset = 0, count = 0;
    while (compound.count - offset >= 4) {
        const u8* part = compound.data + offset;
        const u32 bytes = (u32{byte_load_be16(part + 2)} + 1) * 4;
        if ((part[0] >> 6) != 2 || bytes > compound.count - offset)
            break;
        // SR: header, sender SSRC, 20-byte sender info, then report blocks.
        if (part[1] == kRtcpSenderReport && bytes >= 28 && count != output.count && output.data) {
            auto& info = output[count++];
            info.ssrc = byte_load_be32(part + 4);
            info.ntp_seconds = byte_load_be32(part + 8);
            info.ntp_fraction = byte_load_be32(part + 12);
            info.rtp_timestamp = byte_load_be32(part + 16);
            info.packet_count = byte_load_be32(part + 20);
            info.octet_count = byte_load_be32(part + 24);
        }
        offset += bytes;
    }
    return count;
}
} // namespace maicord
