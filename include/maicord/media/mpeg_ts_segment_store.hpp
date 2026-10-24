#pragma once
#include "maicord/media/mp4_segment_store.hpp"

namespace maicord {
// Native AVC/AAC transport adapter for the existing bounded sample store. The
// first complete file supplies configuration and ordinal zero; no MP4 container
// is generated. Also accepts HLS packed AAC with its mandatory ID3 timestamp.
// A compact sample table shares the candidate payload allocation.
// Empty owner only; failure releases all temporary ownership and preserves it.
// A nonzero reference places the first 33-bit timestamp in the companion
// track's epoch. Later files unwrap against this store's accepted endpoint.
Mp4DemuxError mpeg_ts_segment_store_initialize(Mp4SegmentStore*, ConstSlice<u8>, bool audio,
    u32 advertised_files, u64 duration_us, u64 clock_reference_90k = 0) noexcept;
// Same demand/serial and active-slot fence as ordinary segment receipt. The
// advertised video interval supplies its final picture duration; the next file
// must meet that exact endpoint. AAC uses counted 1024-frame rational timing.
// No full-segment staging buffer or per-PES allocation. Failure preserves active
// bytes but invalidates/empties the replaceable candidate and old seek proposals.
Mp4DemuxError mpeg_ts_segment_store_accept(Mp4SegmentStore*, u32 number, u64 serial,
    ConstSlice<u8>, u64 duration_us) noexcept;
} // namespace maicord
