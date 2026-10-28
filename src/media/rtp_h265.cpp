#include "maicord/media/rtp_h265.hpp"
#include "rtp_video_assembly_detail.hpp"

namespace maicord {
namespace {
using namespace rtp_detail;
constexpr u8 kAggregation = 48, kFragmentation = 49;
u8 nal_type(const u8* header) noexcept {
    return (header[0] >> 1) & 0x3f;
}
// forbidden_zero_bit clear and nuh_temporal_id_plus1 nonzero.
bool valid_header(const u8* header) noexcept {
    return !(header[0] & 0x80) && (header[1] & 7);
}
bool plain_nal(const u8* header) noexcept {
    return valid_header(header) && nal_type(header) < kAggregation;
}
bool starts_nal(u8 type, const u8* rest, u32 rest_bytes) noexcept {
    if (type == kH265NalVps || type == kH265NalSps || type == kH265NalPps || type == 35 || type == 39)
        return true;
    return type <= 31 && rest_bytes && (rest[0] & 0x80); // first_slice_segment_in_pic_flag
}
} // namespace

RtpDepacketize rtp_h265_push(RtpVideoAssembly* assembly, ConstSlice<u8> payload, bool gap_before) noexcept {
    if (!assembly)
        return RtpDepacketize::damaged;
    auto& a = *assembly;
    if (gap_before)
        drop_fragment(a);
    if (!payload.data || payload.count < 2 || !valid_header(payload.data)) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 type = nal_type(payload.data);
    if (type != kFragmentation)
        drop_fragment(a);
    if (type < kAggregation) {
        if (payload.count > ~u32{0} - 4 || !fits(a, payload.count + 4))
            return reject(a, RtpDepacketize::overflow);
        start_code(a);
        put(a, payload.data, payload.count);
        complete(a, type);
        return RtpDepacketize::appended;
    }
    // RFC 7798 4.4.2: an AP carries at least two aggregation units; accept
    // one defensively since its bytes are still a complete NAL unit.
    if (type == kAggregation)
        return append_aggregate(a, payload, 2, 2, plain_nal, nal_type);
    if (type != kFragmentation)
        return reject(a, RtpDepacketize::damaged); // PACI and unassigned types.
    if (payload.count < 4) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    const u8 fu = payload[2];
    const bool start = (fu & 0x80) != 0, end = (fu & 0x40) != 0;
    const u8 fu_type = fu & 0x3f;
    if ((start && end) || fu_type >= kAggregation) {
        drop_fragment(a);
        return reject(a, RtpDepacketize::damaged);
    }
    // The payload header carries F, LayerId and TID; FuType replaces Type.
    const u8 reconstructed[2]{static_cast<u8>((payload[0] & 0x81) | (fu_type << 1)), payload[1]};
    return append_fragment(a, payload, 3, start, end, fu_type, reconstructed, 2);
}

bool rtp_h265_starts_access_unit(ConstSlice<u8> payload) noexcept {
    if (!payload.data || payload.count < 2 || !valid_header(payload.data))
        return false;
    const u8 type = nal_type(payload.data);
    if (type < kAggregation)
        return starts_nal(type, payload.data + 2, payload.count - 2);
    if (type == kAggregation) {
        if (payload.count < 6)
            return false;
        const u32 size = load16(payload.data + 2);
        if (size < 2 || size > payload.count - 4 || !plain_nal(payload.data + 4))
            return false;
        return starts_nal(nal_type(payload.data + 4), payload.data + 6, size - 2);
    }
    if (type == kFragmentation && payload.count >= 3 && (payload[2] & 0x80) && !(payload[2] & 0x40))
        return starts_nal(payload[2] & 0x3f, payload.data + 3, payload.count - 3);
    return false;
}
} // namespace maicord
