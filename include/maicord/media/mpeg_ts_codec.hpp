#pragma once
#include "maicord/media/mpeg_ts.hpp"

namespace maicord {
struct MpegTsAacFrame {
    ConstSlice<u8> payload{};
    u32 bytes{}, sample_rate{};
    u8 configuration[2]{}, channels{};
};
// One complete single-block AAC-LC ADTS frame at the front of input. No copy;
// bytes includes its header. Caller walks every frame in each PES, preserving
// its timestamp and the 1024-sample rational clock. CRC/multiple-block/PCE and
// unsupported profiles are explicit; implicit SBR is not inferred from a header.
MpegTsError mpeg_ts_aac_frame(ConstSlice<u8>, MpegTsAacFrame*) noexcept;
struct PackedAudioSegment { ConstSlice<u8> payload{}; u64 timestamp{}; };
// HLS packed AAC: bounded ID3v2.3/v2.4 PRIV timestamp followed by ADTS.
// Requires the standard 33-bit transport clock; never guesses timing from
// playlist durations. Borrows payload, validates its first AAC frame, and
// preserves output on failure. The segment store validates every later frame.
MpegTsError packed_aac_segment(ConstSlice<u8>, PackedAudioSegment*) noexcept;
struct MpegTsAvcConfiguration {
    u8 bytes[1024]{};
    u32 count{}, width{}, height{};
    u16 sar_width{}, sar_height{};
};
// Initial Annex B access unit containing one SPS/PPS pair. Validates both with
// the native H.264 parser and builds the small AVC configuration used by the
// existing GPU decoder. Distinct parameter-set pairs require resynchronization.
MpegTsError mpeg_ts_avc_configuration(ConstSlice<u8>, MpegTsAvcConfiguration*) noexcept;
// Repack Annex B NALs with four-byte lengths for the existing attachment codec.
// One access unit, <=512 NALs, no allocation. Buffers must be disjoint, or the
// destination must begin at least 512 bytes before input (bounded compaction).
// At most one extra byte per NAL replaces its three-byte prefix. The
// output byte count is transactional; scratch may change on failure. IDR proof
// and picture/slice validation remain with the native codec, not transport flags.
MpegTsError mpeg_ts_avc_sample(ConstSlice<u8>, Slice<u8>, u32* bytes) noexcept;
} // namespace maicord
