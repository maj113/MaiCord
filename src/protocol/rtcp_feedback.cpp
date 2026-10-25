#include "maicord/protocol/rtcp_feedback.hpp"
#include "maicord/core/memory.hpp"
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

RtpHeaderExtensions rtp_header_extensions(ConstSlice<u8> packet, const MediaPacketLayout& layout, u8 id) noexcept {
    RtpHeaderExtensions result{};
    if (layout.kind != MediaPacketKind::rtp || !layout.extension_bytes || !byte_range_valid(packet) ||
        !packet.data || layout.clear_bytes > packet.count || layout.extension_bytes > packet.count - layout.clear_bytes)
        return result;
    const bool one_byte = layout.extension_profile == 0xBEDE;
    if (!one_byte && (layout.extension_profile & 0xfff0) != 0x1000)
        return result;
    const u8* at = packet.data + layout.clear_bytes;
    const u8* end = at + layout.extension_bytes;
    while (at < end) {
        if (!*at) { // Padding between elements.
            ++at;
            continue;
        }
        u32 element = 0, bytes = 0;
        if (one_byte) {
            element = *at >> 4;
            bytes = (*at & 15u) + 1;
            if (element == 15)
                break; // Reserved: stop parsing.
            ++at;
        } else {
            if (end - at < 2)
                break;
            element = at[0];
            bytes = at[1];
            at += 2;
        }
        if (bytes > static_cast<u32>(end - at))
            break;
        if (element < 16)
            result.ids = static_cast<u16>(result.ids | (1u << element));
        if (element == id && !result.found) {
            result.value = {at, bytes};
            result.found = true;
        }
        at += bytes;
    }
    return result;
}

namespace {
constexpr u32 kTransportFeedbackHeaderBytes = 20; // Common header, SSRCs, base/count, reference/count.
constexpr u64 kReferenceTickUs = 64'000, kDeltaTickUs = 250;
constexpr u64 kSequenceBase = u64{1} << 32;
constexpr u64 kWindowMask = kRtcpTransportFeedbackWindow - 1;
static_assert((kRtcpTransportFeedbackWindow & kWindowMask) == 0 &&
              kRtcpTransportFeedbackMaxPackets <= kRtcpTransportFeedbackWindow);
enum : u8 {
    not_received,
    small_delta,
    large_delta
};
struct Chunk {
    u16 start, count;
    bool run;
};
} // namespace

RtcpTransportFeedbackWrite rtcp_write_transport_feedback(Slice<u8> output, u32 offset, u32 sender_ssrc,
                                                         u32 media_ssrc, u16 base_sequence, u8 feedback_count,
                                                         ConstSlice<u64> arrivals) noexcept {
    if (!output.data || !byte_range_valid({output.data, output.count}) || offset > output.count ||
        !arrivals.count || !arrivals.data || arrivals.count > kRtcpTransportFeedbackMaxPackets)
        return {0, 0};
    u32 first = 0;
    while (first != arrivals.count && !arrivals[first])
        ++first;
    if (first == arrivals.count)
        return {0, 0};
    u8 status[kRtcpTransportFeedbackMaxPackets];
    i16 delta[kRtcpTransportFeedbackMaxPackets];
    const u64 reference = arrivals[first] / kReferenceTickUs;
    u64 current = reference * kReferenceTickUs;
    u32 count = arrivals.count;
    for (u32 i = 0; i != count; ++i) {
        status[i] = not_received;
        if (!arrivals[i])
            continue;
        // Truncated ticks; the running time advances by what was encoded.
        const i64 ticks = static_cast<i64>(arrivals[i] - current) / static_cast<i64>(kDeltaTickUs);
        if (ticks < -32768 || ticks > 32767) {
            count = i;
            break;
        }
        status[i] = ticks >= 0 && ticks <= 255 ? small_delta : large_delta;
        delta[i] = static_cast<i16>(ticks);
        current += static_cast<u64>(ticks * static_cast<i64>(kDeltaTickUs));
    }
    while (count && status[count - 1] == not_received)
        --count;
    // Greedy chunks: a run of seven or more equal statuses is run-length coded,
    // anything else a two-bit vector of up to seven. Stops at the room left.
    const u32 room = (output.count - offset) & ~3u;
    Chunk chunks[kRtcpTransportFeedbackMaxPackets];
    u32 chunk_count = 0, bytes = kTransportFeedbackHeaderBytes, covered = 0;
    while (covered != count && bytes + 2 <= room) {
        u32 run = 1;
        while (covered + run != count && status[covered + run] == status[covered])
            ++run;
        const u32 length = run >= 7 ? run : minimum(7u, count - covered);
        u32 fitted = 0, size = bytes + 2;
        while (fitted != length && size + status[covered + fitted] <= room)
            size += status[covered + fitted++];
        if (!fitted)
            break;
        chunks[chunk_count++] = {static_cast<u16>(covered), static_cast<u16>(fitted), run >= 7};
        bytes = size;
        covered += fitted;
    }
    // End at a received packet: a trailing zero run is dropped, a vector cut.
    while (chunk_count) {
        auto& last = chunks[chunk_count - 1];
        while (last.count && status[last.start + last.count - 1] == not_received)
            --last.count;
        if (last.count)
            break;
        --chunk_count;
    }
    if (!chunk_count)
        return {0, 0};
    const u32 packets = chunks[chunk_count - 1].start + chunks[chunk_count - 1].count;
    bytes = kTransportFeedbackHeaderBytes + 2 * chunk_count;
    for (u32 i = 0; i != packets; ++i)
        bytes += status[i];
    const u32 padded = (bytes + 3) & ~3u;
    u8* packet = header(output, offset, padded, kRtcpFormatTransportFeedback, kRtcpTransportFeedback);
    if (!packet)
        return {0, 0};
    byte_store_be32(packet + 4, sender_ssrc);
    byte_store_be32(packet + 8, media_ssrc);
    store16(packet + 12, base_sequence);
    store16(packet + 14, packets);
    byte_store_be32(packet + 16, (static_cast<u32>(reference & 0xffffff) << 8) | feedback_count);
    u8* at = packet + kTransportFeedbackHeaderBytes;
    for (u32 c = 0; c != chunk_count; ++c) {
        const auto& chunk = chunks[c];
        u32 value = 0;
        if (chunk.run) {
            value = (u32{status[chunk.start]} << 13) | chunk.count;
        } else {
            value = 0xc000;
            for (u32 k = 0; k != chunk.count; ++k)
                value |= u32{status[chunk.start + k]} << (2 * (6 - k));
        }
        store16(at, value);
        at += 2;
    }
    for (u32 i = 0; i != packets; ++i) {
        if (status[i] == small_delta)
            *at++ = static_cast<u8>(delta[i]);
        else if (status[i] == large_delta) {
            store16(at, static_cast<u16>(delta[i]));
            at += 2;
        }
    }
    while (at != packet + padded)
        *at++ = 0;
    return {offset + padded, packets};
}

void rtcp_transport_feedback_reset(RtcpTransportFeedbackState* state) noexcept {
    if (state)
        reset_value(state);
}

void rtcp_transport_feedback_record(RtcpTransportFeedbackState* state, u16 sequence, u32 media_ssrc,
                                    u64 arrival_us) noexcept {
    if (!state)
        return;
    if (!state->started) {
        for (auto& arrival : state->arrivals)
            arrival = 0;
        state->highest = state->next = kSequenceBase + sequence;
        state->started = true;
    }
    const i16 distance = static_cast<i16>(static_cast<u16>(sequence - static_cast<u16>(state->highest)));
    const u64 extended = state->highest + static_cast<i64>(distance);
    if (extended > state->highest) {
        if (extended - state->highest >= kRtcpTransportFeedbackWindow) {
            for (auto& arrival : state->arrivals)
                arrival = 0;
        } else {
            for (u64 s = state->highest + 1; s != extended; ++s)
                state->arrivals[s & kWindowMask] = 0;
        }
        state->highest = extended;
        // Sequences that left the window are never reported.
        if (state->next + kRtcpTransportFeedbackWindow <= extended)
            state->next = extended - kRtcpTransportFeedbackWindow + 1;
    } else if (state->highest - extended >= kRtcpTransportFeedbackWindow) {
        return;
    } else if (state->arrivals[extended & kWindowMask]) {
        return; // Duplicate: the first arrival counts.
    }
    state->arrivals[extended & kWindowMask] = arrival_us ? arrival_us : 1;
    if (extended < state->next)
        state->next = extended;
    state->media_ssrc = media_ssrc;
    ++state->packets;
}

u64 rtcp_transport_feedback_deadline(const RtcpTransportFeedbackState& state) noexcept {
    if (!state.started || state.next > state.highest)
        return 0;
    return state.next_feedback_us ? state.next_feedback_us : 1;
}

u32 rtcp_transport_feedback_build(RtcpTransportFeedbackState* state, Slice<u8> output, u32 offset,
                                  u32 sender_ssrc, u64 now_us) noexcept {
    if (!state)
        return offset;
    const u64 due = rtcp_transport_feedback_deadline(*state);
    if (!due || now_us < due)
        return offset;
    // A feedback packet needs a received status: skip the start of a loss run
    // longer than one packet (those sequences go unreported).
    u64 received = state->next;
    while (!state->arrivals[received & kWindowMask])
        ++received; // highest is always received
    if (received - state->next >= kRtcpTransportFeedbackMaxPackets)
        state->next = received - (kRtcpTransportFeedbackMaxPackets - 1);
    const u32 count = static_cast<u32>(minimum<u64>(state->highest - state->next + 1, kRtcpTransportFeedbackMaxPackets));
    u64 arrivals[kRtcpTransportFeedbackMaxPackets];
    for (u32 i = 0; i != count; ++i)
        arrivals[i] = state->arrivals[(state->next + i) & kWindowMask];
    const auto written = rtcp_write_transport_feedback(output, offset, sender_ssrc, state->media_ssrc,
                                                       static_cast<u16>(state->next), state->feedback_count,
                                                       {arrivals, count});
    if (!written.packets)
        return offset;
    state->next += written.packets;
    ++state->feedback_count;
    ++state->feedback;
    // A range larger than one packet continues on the next build.
    state->next_feedback_us = state->next <= state->highest ? now_us : now_us + kRtcpTransportFeedbackIntervalUs;
    return written.end;
}
} // namespace maicord
