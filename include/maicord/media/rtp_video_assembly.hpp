#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Annex B access-unit builder shared by the H.264 (RFC 6184) and H.265
// (RFC 7798) depacketizers. Every NAL unit is written with a FOUR-byte start
// code: DAVE's unencrypted-range offsets are defined over this exact form.
enum class RtpDepacketize : u8 {
    appended,
    damaged,  // Malformed/unsupported/out-of-order data: nothing from this payload was kept.
    overflow, // Output capacity exceeded: nothing from this payload was kept.
};
struct RtpVideoAssembly {
    Slice<u8> output;
    u64 nal_types;   // Bit n: a complete NAL unit of type n was emitted.
    u32 bytes;       // Annex B bytes written so far.
    u32 fu_offset;   // Start code offset of the fragmented NAL in progress.
    u8 fu_type;
    bool fu_active;  // A start fragment was seen; end not yet.
    bool damaged;    // Something was dropped: the access unit is not intact.
};
inline void rtp_video_assembly_begin(RtpVideoAssembly* assembly, Slice<u8> output) noexcept {
    *assembly = {};
    assembly->output = output;
}
// Ends the access unit. An unterminated fragmentation unit is removed and the
// unit is marked damaged. Returns the Annex B byte count.
inline u32 rtp_video_assembly_finish(RtpVideoAssembly* assembly) noexcept {
    if (assembly->fu_active) {
        assembly->bytes = assembly->fu_offset;
        assembly->fu_active = false;
        assembly->damaged = true;
    }
    return assembly->bytes;
}
} // namespace maicord
