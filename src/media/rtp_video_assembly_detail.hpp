#pragma once
#include "maicord/media/rtp_video_assembly.hpp"

namespace maicord::rtp_detail {
inline bool fits(const RtpVideoAssembly& assembly, u32 bytes) noexcept {
    return assembly.output.data && assembly.bytes <= assembly.output.count &&
           bytes <= assembly.output.count - assembly.bytes;
}
inline void put(RtpVideoAssembly& assembly, const u8* data, u32 bytes) noexcept {
    copy_bytes(assembly.output.data + assembly.bytes, data, bytes);
    assembly.bytes += bytes;
}
inline void start_code(RtpVideoAssembly& assembly) noexcept {
    u8* out = assembly.output.data + assembly.bytes;
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    assembly.bytes += 4;
}
inline void complete(RtpVideoAssembly& assembly, u32 type) noexcept {
    assembly.nal_types |= u64{1} << (type & 63);
}
inline RtpDepacketize reject(RtpVideoAssembly& assembly, RtpDepacketize result) noexcept {
    assembly.damaged = true;
    return result;
}
inline u32 load16(const u8* data) noexcept {
    return u32{data[0]} << 8 | data[1];
}
// H.264 STAP-A and H.265 AP: payload[first, count) holds units of a 16-bit size and one plain NAL
// unit of at least min_size bytes. The whole aggregate is validated before any of it is written;
// plain(header) accepts a unit's NAL header and nal_type(header) names its type.
template <typename Plain, typename NalType>
inline RtpDepacketize append_aggregate(RtpVideoAssembly& assembly, ConstSlice<u8> payload, u32 first, u32 min_size,
                                       Plain plain, NalType nal_type) noexcept {
    u32 offset = first, total = 0, units = 0;
    while (offset != payload.count) {
        if (payload.count - offset < 2)
            return reject(assembly, RtpDepacketize::damaged);
        const u32 size = load16(payload.data + offset);
        offset += 2;
        if (size < min_size || size > payload.count - offset || !plain(payload.data + offset))
            return reject(assembly, RtpDepacketize::damaged);
        total += 4 + size;
        offset += size;
        ++units;
    }
    if (!units)
        return reject(assembly, RtpDepacketize::damaged);
    if (!fits(assembly, total))
        return reject(assembly, RtpDepacketize::overflow);
    offset = first;
    while (offset != payload.count) {
        const u32 size = load16(payload.data + offset);
        offset += 2;
        start_code(assembly);
        put(assembly, payload.data + offset, size);
        complete(assembly, nal_type(payload.data + offset));
        offset += size;
    }
    return RtpDepacketize::appended;
}
// H.264 FU-A and H.265 FU, after the caller validated the fragment header (not both start and end,
// a plain NAL type). payload[skip, count) is the fragment; a start writes the start code and the
// reconstructed NAL header first.
inline RtpDepacketize append_fragment(RtpVideoAssembly& assembly, ConstSlice<u8> payload, u32 skip, bool start,
                                      bool end, u8 type, const u8* header, u32 header_bytes) noexcept {
    const u32 data = payload.count - skip;
    if (start) {
        drop_fragment(assembly); // A start before the previous end: that NAL is lost.
        if (!fits(assembly, data + 4 + header_bytes))
            return reject(assembly, RtpDepacketize::overflow);
        assembly.fu_offset = assembly.bytes;
        assembly.fu_type = type;
        assembly.fu_active = true;
        start_code(assembly);
        put(assembly, header, header_bytes);
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
} // namespace maicord::rtp_detail
