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
} // namespace maicord::rtp_detail
