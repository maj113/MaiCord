#pragma once
#include "maicord/media/video/rtp_video_assembly.hpp"

namespace maicord::rtp_detail {
inline bool fits(const RtpVideoAssembly& assembly, u32 bytes) noexcept {
    return assembly.output.data && assembly.bytes <= assembly.output.count &&
           bytes <= assembly.output.count - assembly.bytes;
}
inline void put(RtpVideoAssembly& assembly, const u8* data, u32 bytes) noexcept {
    copy_bytes(assembly.output.data + assembly.bytes, data, bytes);
    assembly.bytes += bytes;
}
inline void complete(RtpVideoAssembly& assembly, u32 type) noexcept {
    assembly.nal_types |= u64{1} << (type & 63);
}
inline RtpDepacketize reject(RtpVideoAssembly& assembly, RtpDepacketize result) noexcept {
    assembly.damaged = true;
    return result;
}
} // namespace maicord::rtp_detail
