#include "maicord/protocol/media_packet.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
bool header(ConstSlice<u8> packet, MediaPacketLayout& value) noexcept {
    if (!byte_range_valid(packet) || packet.count < 8 || (packet[0] >> 6) != 2)
        return false;
    value = {};
    value.padded = (packet[0] & 0x20) != 0;
    if (packet[1] >= 192 && packet[1] <= 223) {
        value.kind = MediaPacketKind::rtcp;
        value.type = packet[1];
        value.clear_bytes = 8;
        value.ssrc = byte_load_be32(packet.data + 4);
    } else {
        if (packet.count < 12)
            return false;
        value.kind = MediaPacketKind::rtp;
        value.type = packet[1] & 0x7f;
        // RFC 5761 forbids these payload types under RTP/RTCP multiplexing.
        if (value.type >= 64 && value.type <= 95)
            return false;
        value.marker = (packet[1] & 0x80) != 0;
        value.sequence = byte_load_be16(packet.data + 2);
        value.timestamp = byte_load_be32(packet.data + 4);
        value.ssrc = byte_load_be32(packet.data + 8);
        value.csrc_count = packet[0] & 15;
        value.clear_bytes = 12 + value.csrc_count * 4;
        if (packet.count < value.clear_bytes)
            return false;
        if (packet[0] & 0x10) {
            if (packet.count - value.clear_bytes < 4)
                return false;
            const u8* preamble = packet.data + value.clear_bytes;
            value.extension_profile = byte_load_be16(preamble);
            value.extension_bytes = byte_load_be16(preamble + 2) * 4u;
            value.clear_bytes += 4;
        }
    }
    return true;
}
bool destination(ConstSlice<u8> packet, const MediaPacketLayout* output) noexcept {
    return output && !byte_ranges_overlap(packet, {reinterpret_cast<const u8*>(output), sizeof(*output)});
}
} // namespace

bool media_packet_header(ConstSlice<u8> packet, MediaPacketLayout* output) noexcept {
    MediaPacketLayout value{};
    if (!destination(packet, output) || !header(packet, value))
        return false;
    *output = value;
    return true;
}

bool media_packet_plaintext(ConstSlice<u8> packet, MediaPacketLayout* output) noexcept {
    MediaPacketLayout value{};
    if (!destination(packet, output) || !header(packet, value))
        return false;
    if (value.kind == MediaPacketKind::rtp) {
        if (value.extension_bytes > packet.count - value.clear_bytes)
            return false;
        value.payload_offset = value.clear_bytes + value.extension_bytes;
        value.payload_bytes = packet.count - value.payload_offset;
        if (value.padded) {
            if (!value.payload_bytes)
                return false;
            value.padding_bytes = packet[packet.count - 1];
            if (!value.padding_bytes || value.padding_bytes > value.payload_bytes)
                return false;
            value.payload_bytes -= value.padding_bytes;
        }
    } else {
        u32 offset = 0;
        while (offset != packet.count) {
            if (packet.count - offset < 4)
                return false;
            const u8* part = packet.data + offset;
            const u32 bytes = (u32{byte_load_be16(part + 2)} + 1) * 4;
            if ((part[0] >> 6) != 2 || part[1] < 192 || part[1] > 223 || bytes > packet.count - offset ||
                (offset == 0 && bytes < 8))
                return false;
            if (part[0] & 0x20) {
                // RTCP padding belongs only to the last packet in a compound.
                const u8 padding = part[bytes - 1];
                if (offset + bytes != packet.count || !padding || (padding & 3) ||
                    padding > bytes - (offset == 0 ? 8u : 4u))
                    return false;
                value.padding_bytes = padding;
                value.padded = true;
            }
            offset += bytes;
        }
        value.payload_offset = 8;
        value.payload_bytes = packet.count - 8;
    }
    *output = value;
    return true;
}
} // namespace maicord
