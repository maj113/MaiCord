#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class MpegTsError : u8 { none, invalid_input, malformed, unsupported, capacity, end_of_stream };
inline constexpr u16 kMpegTsNoPid = 8191;
inline constexpr u32 kMpegTsMaximumBytes = 8 * 1024 * 1024;
struct MpegTsProgram {
    u16 number{}, pmt_pid{kMpegTsNoPid}, pcr_pid{kMpegTsNoPid};
    u16 video_pid{kMpegTsNoPid}, audio_pid{kMpegTsNoPid};
};
// One complete clear HLS transport segment. Validates packet/adaptation bounds,
// CRC-protected PAT/PMT (including split sections), one program, and any
// declared PCR. Explicit no-PCR tracks still require valid PES timestamps.
// Selects at most one AVC and one ADTS AAC stream; ignores timed metadata.
// No allocation. Unsupported codecs, encryption or discontinuities are explicit.
// Input remains borrowed; failure preserves output. At most 8 MiB, 188-byte packets.
MpegTsError mpeg_ts_program(ConstSlice<u8>, MpegTsProgram*) noexcept;
struct MpegTsReader {
    ConstSlice<u8> source{};
    u32 offset{}, previous{~u32{0}};
    u16 pid{kMpegTsNoPid};
    u8 counter{};
    bool duplicated{};
};
struct MpegTsPes {
    ConstSlice<u8> payload{};
    u64 pts{}, dts{}; // Original 33-bit, 90 kHz timestamps; no invented duration.
    bool random_access{}; // Transport hint only; codec must still validate an IDR.
};
// Initialize the reader with the same immutable source validated above and a
// selected program PID. Collects one complete PES into caller-owned storage;
// payload borrows that storage after its small PES header. No per-packet table
// or allocation. Output storage must not overlap input, reader or result.
// Failure/EOF preserves reader and result; scratch contents may change.
// A duplicate packet is accepted once, missing/out-of-order packets fail.
MpegTsError mpeg_ts_next(MpegTsReader*, Slice<u8> scratch, MpegTsPes*) noexcept;
// Map a raw 33-bit timestamp to the nearest epoch around an accepted reference.
// Rejects an ambiguous half-wrap, negative result or overflow. Transactional.
bool mpeg_ts_unwrap(u64 raw, u64 reference, u64*) noexcept;
} // namespace maicord
