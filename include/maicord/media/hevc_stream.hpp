#pragma once

#include "maicord/media/h264_stream.hpp"
#include "maicord/media/hevc_references.hpp"

namespace maicord {

// Live H.265 (RTP camera/screen) bitstream state, CPU side only: the H.265
// counterpart of H264StreamState. Input is one depacketized Annex B access
// unit per call; VPS/SPS/PPS arrive in-band and may change or reuse ids
// mid-stream. Main profile (8-bit 4:2:0) only; other SPS are unsupported.

// Level 5.x MaxSliceSegmentsPerPicture.
inline constexpr u32 kHevcStreamMaximumSlices = 200;
inline constexpr u32 kHevcStreamSpsSlots = 4;
inline constexpr u32 kHevcStreamPpsSlots = 16;
inline constexpr u32 kHevcStreamMaximumSpsBytes = 1024;
inline constexpr u32 kHevcStreamMaximumPpsBytes = 512;

// Raw SPS/PPS keyed by id with least-recently-stored eviction. A picture's
// chain is parsed when it is activated and cached until either set changes.
struct HevcStreamSps {
    u8 raw[kHevcStreamMaximumSpsBytes];
    u32 bytes, stored;
    u8 id;
    bool valid;
};
struct HevcStreamPps {
    u8 raw[kHevcStreamMaximumPpsBytes];
    u32 bytes, stored;
    u8 id, sps_id;
    bool valid;
};
struct HevcParameterSets {
    HevcStreamSps sps[kHevcStreamSpsSlots];
    HevcStreamPps pps[kHevcStreamPpsSlots];
    HevcConfiguration active;          // The last resolved chain.
    u32 clock, active_sps, active_pps; // Stored stamps of the active chain; 0 when none.
    HevcError last_sps_error;
};
// SPS (33) or PPS (34) NAL. A failing set invalidates a stored set of the same id.
HevcError hevc_parameter_sets_store(HevcParameterSets* sets, ConstSlice<u8> nal) noexcept;
// Makes the chain of pps_id the active configuration.
HevcError hevc_parameter_sets_resolve(HevcParameterSets* sets, u8 pps_id) noexcept;

// One picture gathered from Annex B input; slices reference the caller's
// bytes (NAL header included, start code excluded).
struct HevcStreamPicture {
    HevcPictureHeader header;
    H264StreamNalRange slices[kHevcStreamMaximumSlices];
    u32 slice_count, bitstream_bytes; // bitstream_bytes includes 3-byte start codes.
    u32 last_address;
    bool intra;
    HevcError error; // Reason when the scan reports a damaged picture.
};
struct HevcStreamScanner {
    H264AnnexBCursor cursor;
    H264Nal pending;
    bool has_pending;
};
enum class HevcScanStatus : u8 {
    end,
    picture,
    damaged
};
inline HevcStreamScanner hevc_stream_scanner(ConstSlice<u8> annexb) noexcept {
    return {{annexb, 0}, {}, false};
}
// Applies parameter sets in bitstream order and returns each picture
// (H.265 7.4.2.4.4 boundaries). The active configuration is the returned
// picture's until the next scan. A damaged picture (missing parameter sets or
// leading segments, malformed header, too many slices) must not be submitted.
HevcScanStatus hevc_stream_scan(HevcParameterSets* sets, HevcStreamScanner* scanner,
                                HevcStreamPicture* picture) noexcept;

struct HevcStreamState {
    HevcParameterSets sets;
    HevcReferenceState references;
    H264OutputQueue output;
    H264StreamOutputInfo info[32]; // Per surface, for pictures awaiting output.
    u32 coded_width, coded_height; // Geometry since the last accepted IRAP.
    u32 held;                      // Surfaces the GPU layer presents; never decoded into.
    u8 max_buffering, reorder;
    u8 surfaces; // Allocated by the GPU layer; 0 before the first IRAP.
    bool active, waiting, skip_rasl, drain, unsupported;
    u32 decoded, dropped, damaged, gaps;
};
// Waits for an IRAP without forgetting parameter sets.
void hevc_stream_reset(HevcStreamState* state) noexcept;
// Damaged or rejected input while decoding: drop it and wait for an IRAP.
void hevc_stream_lost(HevcStreamState* state) noexcept;

enum class HevcStreamAction : u8 {
    decode,           // Drain outputs, reinitialize if asked, submit, then commit.
    request_keyframe, // Picture dropped; decoding resumes at the next IRAP.
    unsupported,      // IRAP uses features/geometry the decoder does not handle.
    skip,             // RASL picture of the IRAP decoding started at: not decodable, not an error.
};
struct HevcStreamDecode {
    HevcReferencePlan plan;
    u8 surface, surfaces_needed;
    bool reinitialize; // New geometry or DPB size: recreate decoder and surfaces.
    bool restart;      // IRAP with NoRaslOutputFlag: the DPB starts empty.
};
// Uses sets.active (the picture's chain). Changes nothing for a picture that
// will be decoded except scheduling pending outputs for draining.
HevcStreamAction hevc_stream_prepare(HevcStreamState* state, const HevcStreamPicture& picture,
                                     HevcStreamDecode* decode) noexcept;
void hevc_stream_set_surfaces(HevcStreamState* state, u8 surfaces) noexcept;
// The GPU accepted the picture.
void hevc_stream_commit(HevcStreamState* state, const HevcStreamPicture& picture,
                        const HevcStreamDecode& decode, u32 rtp_timestamp) noexcept;
// Next picture in output order once sps_max_num_reorder_pics allows (or while draining).
bool hevc_stream_pop(HevcStreamState* state, H264StreamOutput* output) noexcept;

static_assert(sizeof(HevcStreamPicture) <= 2'048);
static_assert(sizeof(HevcStreamState) <= 24'576);

} // namespace maicord
