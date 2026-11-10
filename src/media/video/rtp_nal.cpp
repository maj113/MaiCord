#include "maicord/media/video/rtp_h264.hpp"
#include "maicord/media/video/rtp_h265.hpp"
#include "rtp_video_assembly_detail.hpp"

// The H.264 (RFC 6184, packetization mode 1) and H.265 (RFC 7798, no DONL) depacketizers. Both read the same three
// kinds of packet (one NAL unit, an aggregate of units, a fragment of one) and differ in the NAL unit header they
// start with. A NalCodec describes the header: each codec's entry point is the one dispatch inlined with its own
// constants (the per-packet checks fold to what a hand-written copy would be), and the work behind it is shared.
namespace maicord {
namespace {
using namespace rtp_detail;

struct NalCodec {
    u8 header_bytes;                // The NAL unit header: one byte; H.265's carries the layer and temporal ids in two.
    u8 type_shift, type_mask;       // Where the type lies in the first byte, and in a fragment's header byte.
    u8 aggregation, fragmentation;  // STAP-A / AP and FU-A / FU packet types.
    u8 first_plain, last_plain;     // The types of a NAL unit that travels alone, or in an aggregate or fragments.
    u8 fragment_keep;               // Bits of the payload header's first byte a fragment's NAL header keeps.
    u8 first_slice, last_slice;     // Coded slice types: they start an access unit when their first-slice bit is set.
    u64 starts;                     // Types that always start an access unit (parameter sets, delimiter, SEI).
};
// H.264: single NAL unit types 1-23; F and NRI kept; slices 1-5; SEI, SPS, PPS and AUD (6-9) start.
constexpr NalCodec kH264{1, 0, 0x1f, 24, 28, 1, 23, 0xe0, 1, 5, u64{0xf} << 6};
// H.265: types 0-47; F and the layer id's high bit kept; slices 0-31; VPS, SPS, PPS, AUD (32-35) and prefix SEI (39).
constexpr NalCodec kH265{2, 1, 0x3f, 48, 49, 0, 47, 0x81, 0, 31, u64{0xf} << 32 | u64{1} << 39};

u32 type_of(const NalCodec& c, u8 byte) noexcept {
    return (byte >> c.type_shift) & c.type_mask;
}
bool plain_type(const NalCodec& c, u32 type) noexcept {
    return type >= c.first_plain && type <= c.last_plain;
}
// forbidden_zero_bit clear and, for H.265, nuh_temporal_id_plus1 nonzero.
bool valid_header(const NalCodec& c, const u8* header) noexcept {
    return !(header[0] & 0x80) && (c.header_bytes == 1 || (header[1] & 7));
}
// A NAL unit header that may appear on its own or inside an aggregate.
bool plain_nal(const NalCodec& c, const u8* header) noexcept {
    return valid_header(c, header) && plain_type(c, type_of(c, header[0]));
}
bool starts_nal(const NalCodec& c, u32 type, const u8* rest, u32 rest_bytes) noexcept {
    if ((c.starts >> type) & 1)
        return true;
    // first_mb_in_slice ue(v) is zero exactly when its first bit is one; H.265's first_slice_segment_in_pic_flag.
    return type >= c.first_slice && type <= c.last_slice && rest_bytes && (rest[0] & 0x80);
}

void start_code(RtpVideoAssembly& assembly) noexcept {
    u8* out = assembly.output.data + assembly.bytes;
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    assembly.bytes += 4;
}
u32 load16(const u8* data) noexcept {
    return u32{data[0]} << 8 | data[1];
}

// STAP-A and AP: payload[header_bytes, count) holds units of a 16-bit size and one plain NAL unit. The whole aggregate
// is validated before any of it is written.
__declspec(noinline) RtpDepacketize append_aggregate(RtpVideoAssembly& assembly, const NalCodec& c,
                                                     ConstSlice<u8> payload) noexcept {
    u32 offset = c.header_bytes, total = 0, units = 0;
    while (offset != payload.count) {
        if (payload.count - offset < 2)
            return reject(assembly, RtpDepacketize::damaged);
        const u32 size = load16(payload.data + offset);
        offset += 2;
        if (size < c.header_bytes || size > payload.count - offset || !plain_nal(c, payload.data + offset))
            return reject(assembly, RtpDepacketize::damaged);
        total += 4 + size;
        offset += size;
        ++units;
    }
    if (!units)
        return reject(assembly, RtpDepacketize::damaged);
    if (!fits(assembly, total))
        return reject(assembly, RtpDepacketize::overflow);
    offset = c.header_bytes;
    while (offset != payload.count) {
        const u32 size = load16(payload.data + offset);
        offset += 2;
        start_code(assembly);
        put(assembly, payload.data + offset, size);
        complete(assembly, type_of(c, payload.data[offset]));
        offset += size;
    }
    return RtpDepacketize::appended;
}

// FU-A and FU, after the fragment header was validated (not both start and end, a plain NAL type). A start writes the
// start code and the reconstructed NAL unit header first.
__declspec(noinline) RtpDepacketize append_fragment(RtpVideoAssembly& assembly, const NalCodec& c,
                                                    ConstSlice<u8> payload, bool start, bool end, u8 type,
                                                    const u8* header) noexcept {
    const u32 skip = c.header_bytes + 1, data = payload.count - skip;
    if (start) {
        drop_fragment(assembly); // A start before the previous end: that NAL is lost.
        if (!fits(assembly, data + 4 + c.header_bytes))
            return reject(assembly, RtpDepacketize::overflow);
        assembly.fu_offset = assembly.bytes;
        assembly.fu_type = type;
        assembly.fu_active = true;
        start_code(assembly);
        put(assembly, header, c.header_bytes);
    } else {
        if (!assembly.fu_active)
            return reject(assembly, RtpDepacketize::damaged);
        if (assembly.fu_type != type) {
            drop_fragment(assembly);
            return reject(assembly, RtpDepacketize::damaged);
        }
        if (!fits(assembly, data)) {
            drop_fragment(assembly);
            return reject(assembly, RtpDepacketize::overflow);
        }
    }
    put(assembly, payload.data + skip, data);
    if (end) {
        assembly.fu_active = false;
        complete(assembly, type);
    }
    return RtpDepacketize::appended;
}

__forceinline RtpDepacketize push(const NalCodec& c, RtpVideoAssembly* assembly, ConstSlice<u8> payload) noexcept {
    if (!assembly)
        return RtpDepacketize::damaged;
    auto& a = *assembly;
    if (!payload.data || payload.count < c.header_bytes || !valid_header(c, payload.data)) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 type = static_cast<u8>(type_of(c, payload[0]));
    if (type != c.fragmentation)
        drop_fragment(a); // Anything but an in-order fragment continuation ends a fragment.
    if (plain_type(c, type)) {
        if (payload.count > ~u32{0} - 4 || !fits(a, payload.count + 4))
            return reject(a, RtpDepacketize::overflow);
        start_code(a);
        put(a, payload.data, payload.count);
        complete(a, type);
        return RtpDepacketize::appended;
    }
    // An H.265 aggregate carries at least two units (RFC 7798 4.4.2); one is accepted, its bytes being a NAL unit.
    if (type == c.aggregation)
        return append_aggregate(a, c, payload);
    if (type != c.fragmentation)
        return reject(a, RtpDepacketize::damaged); // STAP-B, MTAP, FU-B, PACI and reserved types.
    if (payload.count < c.header_bytes + 2u) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 fu = payload[c.header_bytes];
    const bool start = (fu & 0x80) != 0, end = (fu & 0x40) != 0;
    const u8 nal_type = fu & c.type_mask;
    // A whole NAL unit never travels in one fragment, and fragments carry only plain types.
    if ((start && end) || !plain_type(c, nal_type)) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    // The payload header carries F (and NRI, or the layer id and TID); the fragment header replaces the type.
    const u8 header[2]{static_cast<u8>((payload[0] & c.fragment_keep) | (nal_type << c.type_shift)), payload[1]};
    return append_fragment(a, c, payload, start, end, nal_type, header);
}

__declspec(noinline) bool starts_access_unit(const NalCodec& c, ConstSlice<u8> payload) noexcept {
    const u32 header = c.header_bytes;
    if (!payload.data || payload.count < header || !valid_header(c, payload.data))
        return false;
    const u8 type = static_cast<u8>(type_of(c, payload[0]));
    if (plain_type(c, type))
        return starts_nal(c, type, payload.data + header, payload.count - header);
    if (type == c.aggregation) {
        if (payload.count < 2 * header + 2)
            return false;
        const u32 size = load16(payload.data + header);
        const u8* unit = payload.data + header + 2;
        if (size < header || size > payload.count - header - 2 || !plain_nal(c, unit))
            return false;
        return starts_nal(c, type_of(c, unit[0]), unit + header, size - header);
    }
    if (type == c.fragmentation && payload.count > header && (payload[header] & 0x80) && !(payload[header] & 0x40))
        return starts_nal(c, payload[header] & c.type_mask, payload.data + header + 1, payload.count - header - 1);
    return false;
}
} // namespace

RtpDepacketize rtp_h264_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload) noexcept {
    return push(kH264, assembly, payload);
}
bool rtp_h264_starts_access_unit(ConstSlice<u8> payload) noexcept {
    return starts_access_unit(kH264, payload);
}
RtpDepacketize rtp_h265_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload) noexcept {
    return push(kH265, assembly, payload);
}
bool rtp_h265_starts_access_unit(ConstSlice<u8> payload) noexcept {
    return starts_access_unit(kH265, payload);
}
} // namespace maicord
