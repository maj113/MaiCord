#include "maicord/media/rtp_h264.hpp"
#include "rtp_video_assembly_detail.hpp"

namespace maicord {
namespace {
using namespace rtp_detail;
constexpr u8 kStapA = 24, kFuA = 28;
// A NAL unit header that may appear on its own or inside an aggregate.
bool plain_nal(u8 header) noexcept {
    const u8 type = header & 0x1f;
    return !(header & 0x80) && type >= 1 && type <= 23;
}
bool starts_nal(u8 type, const u8* rest, u32 rest_bytes) noexcept {
    if (type == 6 || type == 7 || type == 8 || type == 9)
        return true;
    // Slice header: first_mb_in_slice ue(v) is zero exactly when its first bit is one.
    return type >= 1 && type <= 5 && rest_bytes && (rest[0] & 0x80);
}
} // namespace

RtpDepacketize rtp_h264_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload, bool gap_before) noexcept {
    if (!assembly)
        return RtpDepacketize::damaged;
    auto& a = *assembly;
    if (gap_before)
        drop_fragment(a);
    if (!payload.data || !payload.count) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 header = payload[0];
    const u8 type = header & 0x1f;
    if ((header & 0x80) || type != kFuA)
        drop_fragment(a); // Anything but an in-order FU-A continuation ends a fragment.
    if (header & 0x80)
        return reject(a, RtpDepacketize::damaged);
    if (type >= 1 && type <= 23) {
        if (payload.count > ~u32{0} - 4 || !fits(a, payload.count + 4))
            return reject(a, RtpDepacketize::overflow);
        start_code(a);
        put(a, payload.data, payload.count);
        complete(a, type);
        return RtpDepacketize::appended;
    }
    if (type == kStapA) {
        // Validate the whole aggregate before writing any of it.
        u32 offset = 1, total = 0, units = 0;
        while (offset != payload.count) {
            if (payload.count - offset < 2)
                return reject(a, RtpDepacketize::damaged);
            const u32 size = load16(payload.data + offset);
            offset += 2;
            if (!size || size > payload.count - offset || !plain_nal(payload[offset]))
                return reject(a, RtpDepacketize::damaged);
            total += 4 + size;
            offset += size;
            ++units;
        }
        if (!units)
            return reject(a, RtpDepacketize::damaged);
        if (!fits(a, total))
            return reject(a, RtpDepacketize::overflow);
        offset = 1;
        while (offset != payload.count) {
            const u32 size = load16(payload.data + offset);
            offset += 2;
            start_code(a);
            put(a, payload.data + offset, size);
            complete(a, payload[offset] & 0x1f);
            offset += size;
        }
        return RtpDepacketize::appended;
    }
    if (type != kFuA)
        return reject(a, RtpDepacketize::damaged); // STAP-B, MTAP, FU-B and reserved types.
    if (payload.count < 3) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 fu = payload[1];
    const bool start = (fu & 0x80) != 0, end = (fu & 0x40) != 0;
    const u8 nal_type = fu & 0x1f;
    // RFC 6184 5.8: a whole NAL unit never travels in one FU, and FU-A carries
    // only non-aggregate types.
    if ((start && end) || nal_type < 1 || nal_type > 23) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u32 data = payload.count - 2;
    if (start) {
        drop_fragment(a); // A start before the previous end: that NAL is lost.
        if (!fits(a, data + 5))
            return reject(a, RtpDepacketize::overflow);
        a.fu_offset = a.bytes;
        a.fu_type = nal_type;
        a.fu_active = true;
        start_code(a);
        const u8 reconstructed = static_cast<u8>((header & 0xe0) | nal_type);
        put(a, &reconstructed, 1);
    } else {
        if (!a.fu_active)
            return reject(a, RtpDepacketize::damaged);
        if (a.fu_type != nal_type) {
            drop_fragment(a);
            return reject(a, RtpDepacketize::damaged);
        }
        if (!fits(a, data)) {
            drop_fragment(a);
            return reject(a, RtpDepacketize::overflow);
        }
    }
    put(a, payload.data + 2, data);
    if (end) {
        a.fu_active = false;
        complete(a, nal_type);
    }
    return RtpDepacketize::appended;
}

bool rtp_h264_starts_access_unit(ConstSlice<u8> payload) noexcept {
    if (!payload.data || !payload.count || (payload[0] & 0x80))
        return false;
    const u8 type = payload[0] & 0x1f;
    if (type >= 1 && type <= 23)
        return starts_nal(type, payload.data + 1, payload.count - 1);
    if (type == kStapA) {
        if (payload.count < 4)
            return false;
        const u32 size = load16(payload.data + 1);
        if (!size || size > payload.count - 3 || !plain_nal(payload[3]))
            return false;
        return starts_nal(payload[3] & 0x1f, payload.data + 4, size - 1);
    }
    if (type == kFuA && payload.count >= 2 && (payload[1] & 0x80) && !(payload[1] & 0x40))
        return starts_nal(payload[1] & 0x1f, payload.data + 2, payload.count - 2);
    return false;
}
} // namespace maicord
