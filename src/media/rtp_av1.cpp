#include "maicord/media/rtp_av1.hpp"
#include "rtp_video_assembly_detail.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using namespace rtp_detail;
constexpr u8 kZ = 0x80, kY = 0x40, kN = 0x08, kSizeBit = 0x02, kExtensionBit = 0x04;
// assembly.fu_type: an OBU starts at fu_offset (the last one written).
constexpr u8 kOpen = 1;
// Bounded so a payload of empty elements cannot cost a large frame of state.
constexpr u32 kElements = 64;

struct Element {
    u32 offset, size;
};

// Element sizes are below the 1500-byte payload, so at most five bytes.
bool read_leb128(ConstSlice<u8> data, u32* offset, u32* value) noexcept {
    u64 result = 0;
    for (u32 i = 0; i < 5 && *offset < data.count; ++i) {
        const u8 byte = data[(*offset)++];
        result |= u64{byte & 0x7fu} << (7 * i);
        if (!(byte & 0x80)) {
            if (result > ~u32{0})
                return false;
            *value = static_cast<u32>(result);
            return true;
        }
    }
    return false;
}

u32 leb128_bytes(u32 value) noexcept {
    u32 bytes = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++bytes;
    }
    return bytes;
}

// Gives the open OBU a minimal obu_size unless it already carries one, which
// must then match its payload. An empty or truncated OBU damages the unit.
bool close_obu(RtpVideoAssembly& a) noexcept {
    if (a.fu_type != kOpen)
        return true;
    a.fu_type = 0;
    u8* obu = a.output.data + a.fu_offset;
    const u32 total = a.bytes - a.fu_offset;
    if (!total)
        return false;
    const u32 header = 1u + ((obu[0] & kExtensionBit) ? 1u : 0u);
    if (total < header)
        return false;
    u32 payload = total - header;
    if (obu[0] & kSizeBit) {
        u32 at = a.fu_offset + header, size = 0;
        return read_leb128({a.output.data, a.bytes}, &at, &size) && size == a.bytes - at;
    }
    const u32 extra = leb128_bytes(payload);
    if (!fits(a, extra))
        return false;
    copy_bytes(obu + header + extra, obu + header, payload); // memmove.
    for (u32 i = 0; i < extra; ++i, payload >>= 7)
        obu[header + i] = static_cast<u8>((payload & 0x7f) | (i + 1 < extra ? 0x80 : 0));
    obu[0] |= kSizeBit;
    a.bytes += extra;
    return true;
}

void append(RtpVideoAssembly& a, const u8* data, u32 bytes) noexcept {
    if (!bytes)
        return;
    if (a.bytes == a.fu_offset)
        complete(a, (data[0] >> 3) & 15); // The OBU header byte.
    put(a, data, bytes);
}

// Splits the payload after the aggregation header into OBU elements.
u32 elements(ConstSlice<u8> payload, Element* list, u32 capacity) noexcept {
    const u32 declared = (payload[0] >> 4) & 3;
    u32 offset = 1, count = 0;
    while (offset < payload.count) {
        if (count == capacity)
            return ~u32{0};
        u32 size = payload.count - offset;
        if (count + 1 != declared &&
            (!read_leb128(payload, &offset, &size) || size > payload.count - offset))
            return ~u32{0};
        list[count++] = {offset, size};
        offset += size;
    }
    return count;
}
} // namespace

RtpDepacketize rtp_av1_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload, bool gap_before) noexcept {
    if (!assembly)
        return RtpDepacketize::damaged;
    auto& a = *assembly;
    if (gap_before)
        drop_fragment(a);
    if (!payload.data || !payload.count) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 aggregation = payload[0];
    const bool continues = (aggregation & kZ) != 0;
    Element list[kElements];
    const u32 count = elements(payload, list, kElements);
    if (continues != a.fu_active || count == ~u32{0} || (!count && ((aggregation >> 4) & 3) != 1)) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    if (!count && !continues && !(aggregation & kY))
        return reject(a, RtpDepacketize::damaged); // An announced OBU that never arrives.
    u64 needed = 0;
    for (u32 i = 0; i < count; ++i)
        needed += list[i].size + 5u; // Room for closing the OBU before each element.
    if (needed + 5u > ~u32{0} || !fits(a, static_cast<u32>(needed + 5u))) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::overflow);
    }
    if (aggregation & kN)
        a.nal_types |= kRtpAv1NewSequence;
    for (u32 i = 0; i < count || (!count && !continues); ++i) {
        if (i || !continues) {
            if (!close_obu(a))
                return reject(a, RtpDepacketize::damaged);
            a.fu_offset = a.bytes;
            a.fu_type = kOpen;
        }
        if (!count)
            break;
        const u8* data = payload.data + list[i].offset;
        if (a.bytes == a.fu_offset && list[i].size && (data[0] & 0x80))
            return reject(a, RtpDepacketize::damaged); // obu_forbidden_bit.
        append(a, data, list[i].size);
    }
    a.fu_active = (aggregation & kY) != 0;
    return RtpDepacketize::appended;
}

bool rtp_av1_starts_temporal_unit(ConstSlice<u8> payload) noexcept {
    if (!payload.data || payload.count < 2 || (payload[0] & kZ))
        return false;
    if (payload[0] & kN)
        return true;
    u32 offset = 1, size = payload.count - 1;
    if (((payload[0] >> 4) & 3) != 1 &&
        (!read_leb128(payload, &offset, &size) || size > payload.count - offset))
        return false;
    if (!size)
        return false;
    const u8 type = (payload[offset] >> 3) & 15;
    return type == kAv1ObuSequenceHeader || type == kAv1ObuTemporalDelimiter ||
           type == kAv1ObuFrameHeader || type == kAv1ObuFrame;
}
} // namespace maicord
