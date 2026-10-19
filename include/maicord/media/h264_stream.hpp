#pragma once

#include "maicord/media/h264_output_queue.hpp"
#include "maicord/media/h264_references.hpp"

namespace maicord {

// Live H.264 (RTP camera/screen) bitstream state, CPU side only. Input is one
// depacketized Annex B frame per call (3- or 4-byte start codes); parameter
// sets may repeat, change or use any id mid-stream. Nothing here touches the
// GPU: the platform decoder executes each H264StreamDecode and reports back.

// Coded-area budget for live streams: 3840x2160 and 4096x2160 fit, 4096x4096
// does not. Each side is also capped at 4096 (DXVA H.264 level 5.1/5.2 range).
inline constexpr u32 kH264StreamMaximumDimension = 4096;
inline constexpr u64 kH264StreamMaximumPixels = u64{4096} * 2304;
inline constexpr u32 kH264StreamMaximumSlices = 512;
inline constexpr u32 kH264StreamMaximumPpsBytes = 512;
inline constexpr u32 kH264StreamSpsSlots = 4;
inline constexpr u32 kH264StreamPpsSlots = 16;
// Output pictures held back when the SPS has no bitstream_restriction and the
// stream turns out to reorder (B-frames). Streams without reordering stay at 0.
inline constexpr u8 kH264StreamAdaptiveReorder = 4;
inline constexpr u8 kH264StreamMaximumSurfaces = 26;

// Annex B splitter. Returns NALs without start code or trailing zero bytes;
// empty NALs are skipped. A NAL with the forbidden bit set returns malformed
// with the cursor advanced past it so the caller can drop the picture.
struct H264AnnexBCursor {
    ConstSlice<u8> bytes;
    u32 offset;
};
H264Nal h264_annexb_next(H264AnnexBCursor* cursor) noexcept;

// Bounded SPS/PPS tables keyed by id with least-recently-stored eviction.
// SPS are validated when stored; PPS keep raw bytes and are parsed against
// the referenced SPS whenever a picture activates them (a PPS depends on SPS
// scaling lists and profile, and either may be replaced independently).
struct H264StreamSps {
    H264Sps parsed;
    u32 stored;
    bool valid;
};
struct H264StreamPps {
    u8 raw[kH264StreamMaximumPpsBytes];
    u32 bytes, stored;
    u8 id, sps_id;
    bool valid;
};
struct H264ParameterSets {
    H264StreamSps sps[kH264StreamSpsSlots];
    H264StreamPps pps[kH264StreamPpsSlots];
    u32 clock;
    H264Error last_sps_error;
};
// SPS (type 7) or PPS (type 8) NAL. Failure leaves an existing entry for the
// same id invalidated (it may no longer be relied on) and reports the error.
H264Error h264_parameter_sets_store(H264ParameterSets* sets, ConstSlice<u8> nal) noexcept;
H264Error h264_parameter_sets_resolve(const H264ParameterSets& sets, u8 pps_id, H264Sps* sps,
                                      H264Pps* pps) noexcept;
// Same decoding process (DPB, POC, slice syntax, scaling); VUI/crop may differ.
bool h264_sps_decode_compatible(const H264Sps& a, const H264Sps& b) noexcept;

// One primary coded picture gathered from Annex B input. Slices reference the
// caller's input bytes by offset (NAL header included, start code excluded).
struct H264StreamNalRange {
    u32 offset, size;
};
struct H264StreamPicture {
    H264Sps sps;
    H264Pps pps;
    H264Slice first;
    H264StreamNalRange slices[kH264StreamMaximumSlices];
    u32 slice_count, bitstream_bytes; // bitstream_bytes includes 3-byte start codes.
    bool all_intra;
    H264Error error; // Reason when the scan reports a damaged picture.
};
struct H264StreamScanner {
    H264AnnexBCursor cursor;
    H264Nal pending;
    bool has_pending;
};
enum class H264ScanStatus : u8 { end, picture, damaged };
inline H264StreamScanner h264_stream_scanner(ConstSlice<u8> annexb) noexcept {
    return {{annexb, 0}, {}, false};
}
// Applies parameter sets in bitstream order and returns each access unit
// (H.264 7.4.1.2.3 boundaries: AUD/SEI/SPS/PPS after a VCL NAL, a slice of a
// different picture, end of sequence/stream, end of input). A damaged picture
// (missing parameter sets, malformed/truncated slice header, missing leading
// slices, data partitioning, too many slices) must not be submitted.
H264ScanStatus h264_stream_scan(H264ParameterSets* sets, H264StreamScanner* scanner,
                                H264StreamPicture* picture) noexcept;

struct H264StreamOutputInfo {
    u32 rtp_timestamp;
    bool keyframe;
};
struct H264StreamState {
    H264ParameterSets sets;
    H264Sps sps; // Active since the last accepted IDR.
    H264ReferenceState references;
    H264OutputQueue output;
    H264StreamOutputInfo info[32]; // Per surface, for pictures awaiting output.
    i32 last_output_poc;
    u32 held;         // Surfaces the GPU layer presents; never decoded into.
    u8 surfaces;      // Allocated by the GPU layer; 0 before the first IDR.
    u8 reorder;       // Current output delay in pictures.
    u8 reorder_limit; // Upper bound for adaptive reorder growth.
    bool active, waiting, drain, output_started, unsupported;
    u32 decoded, dropped, damaged, gaps, late;
};
// Starts waiting for an IDR without forgetting parameter sets. Also the
// response to a rejected picture or a lost decoded one: the DPB can no longer
// be trusted.
void h264_stream_reset(H264StreamState* state) noexcept;

enum class H264StreamAction : u8 {
    decode,           // Drain outputs, reinitialize if asked, submit, then commit.
    request_keyframe, // Picture dropped; decoding resumes at the next IDR.
    unsupported,      // IDR uses features/geometry the decoder does not handle.
};
struct H264StreamDecode {
    H264ReferencePlan plan;
    u8 surface;
    u8 surfaces_needed;
    bool reinitialize; // New geometry or DPB size: recreate decoder and surfaces.
};
// No state is changed for a picture that will be decoded except that pending
// outputs may be scheduled for draining (h264_stream_pop) before submission.
H264StreamAction h264_stream_prepare(H264StreamState* state, const H264StreamPicture& picture,
                                     H264StreamDecode* decode) noexcept;
// GPU layer reports the surface count actually allocated (>= surfaces_needed).
void h264_stream_set_surfaces(H264StreamState* state, u8 surfaces) noexcept;
// The GPU accepted the picture.
void h264_stream_commit(H264StreamState* state, const H264StreamPicture& picture,
                        const H264StreamDecode& decode, u32 rtp_timestamp) noexcept;
// Damaged input while decoding: drop it and wait for the next IDR.
void h264_stream_lost(H264StreamState* state) noexcept;

struct H264StreamOutput {
    i32 poc;
    u32 rtp_timestamp;
    u8 surface;
    bool keyframe;
};
// Next picture in output order once the reorder delay allows (or while
// draining). Pictures that arrive behind an already displayed POC are dropped
// and grow the adaptive reorder delay instead of being shown out of order.
// The surface stays valid until the next submission.
bool h264_stream_pop(H264StreamState* state, H264StreamOutput* output) noexcept;

static_assert(sizeof(H264StreamPicture) <= 6'144);
static_assert(sizeof(H264StreamState) <= 20'480);

} // namespace maicord
