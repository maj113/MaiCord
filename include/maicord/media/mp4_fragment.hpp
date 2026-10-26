#pragma once
#include "maicord/media/mp4_demux.hpp"

namespace maicord {
// Borrowed, immutable metadata; no allocation or copied per-sample index.
// Offsets and timestamps remain in source bytes / track ticks, without rounding.
struct Mp4FragmentRange {
    u64 offset{}, time{}, duration{};
    u32 bytes{}, index{}, sap_delta{};
    u8 sap_type{};
    bool starts_with_sap{};
};
struct Mp4FragmentIndex {
    ConstSlice<u8> entries{};
    u64 first_offset{}, first_time{}, duration{};
    u32 track_id{}, timescale{}, count{};
};
// One complete sidx box and its absolute source offset. Flat references only;
// every referenced segment must fit the supplied entity length. Transactional.
Mp4DemuxError mp4_fragment_index_initialize(ConstSlice<u8> sidx, u64 offset, u64 entity_bytes,
                                           Mp4FragmentIndex*) noexcept;
bool mp4_fragment_index_range(const Mp4FragmentIndex&, u32 index, Mp4FragmentRange*) noexcept;
// Constant-time successor to a range returned by this index. Transactional;
// caller preserves the previous range and the immutable index it belongs to.
bool mp4_fragment_index_follow(const Mp4FragmentIndex&, const Mp4FragmentRange&, Mp4FragmentRange*) noexcept;
// Returns the containing segment, not a guarantee of an independently decodable
// picture. The decoder must inspect SAP metadata and verify its random-access NAL.
bool mp4_fragment_index_seek(const Mp4FragmentIndex&, u64 track_time, Mp4FragmentRange*) noexcept;

struct Mp4FragmentDefaults {
    u32 track_id{}, duration{}, bytes{}, flags{};
};
Mp4DemuxError mp4_fragment_defaults(ConstSlice<u8> trex, Mp4FragmentDefaults*) noexcept;
struct Mp4FragmentTrack {
    ConstSlice<u8> configuration{}; // Borrowed avcC or decoded, validated AAC ASC.
    Mp4FragmentDefaults defaults{};
    u64 media_duration{}, empty_edit{}, edit_duration{};
    i64 media_start{};
    u32 timescale{}, movie_timescale{}, width{}, height{}, sar_width{}, sar_height{}, sample_rate{};
    u16 frames_per_packet{};
    u8 channels{}, nal_length_bytes{}, rotation{};
    bool audio{}, edited{};
};
static_assert(sizeof(Mp4FragmentTrack) <= 112);
// Complete ftyp/moov initialization segment, at most 1 MiB. Selects one clear
// AVC or AAC track, validates matching trex, empty sample tables and local data
// references, and reuses the ordinary MP4 codec-description checks. Timing stays
// in its original track/movie ticks; zero edit_duration means an open-ended edit.
// No allocation; source must remain immutable/alive through its configuration
// borrow. Failure leaves output untouched. A media duration of zero is unknown.
Mp4DemuxError mp4_fragment_track_initialize(ConstSlice<u8> initialization, bool audio,
                                           Mp4FragmentTrack*) noexcept;
struct Mp4FragmentSample {
    u64 offset{}, decode_time{};
    i64 presentation_time{};
    u32 bytes{}, duration{}, index{};
    bool sync{};
};
struct Mp4FragmentRun {
    u64 offset{};
    u32 table{}, count{}, flags{}, first_flags{};
};
struct Mp4Fragment {
    ConstSlice<u8> metadata{};
    Mp4FragmentDefaults defaults{};
    Mp4FragmentRun runs[8]{};
    u64 first_time{}, duration{}, decode_time{}, media_cursor{};
    u32 count{}, index{}, run_count{}, run_index{}, run_sample{};
};
static_assert(sizeof(Mp4Fragment) <= 288);
// One selected clear track in a moof followed by one mdat. Up to 32 unique
// track identities; later tracks need an explicit or moof-relative base.
// Only moof and the mdat
// header need be resident in prefix; segment_bytes includes the payload. Up to
// eight runs and 65536 samples, with ordered disjoint payload ranges. No sample
// bytes are accessed. tfdt is required; encrypted auxiliary data is unsupported.
// A caller must separately admit the initialization segment's codec/encryption.
// Failure leaves output unchanged; input metadata must outlive the cursor.
Mp4DemuxError mp4_fragment_initialize(ConstSlice<u8> prefix, u64 source_offset, u64 segment_bytes,
                                     const Mp4FragmentDefaults&, Mp4Fragment*) noexcept;
void mp4_fragment_reset(Mp4Fragment*) noexcept;
Mp4DemuxError mp4_fragment_next(Mp4Fragment*, Mp4FragmentSample*) noexcept;
struct Mp4MediaSegment {
    static constexpr bool kZeroDefault = true; // Every member defaults to zero: reset_value clears bytes.
    Mp4FragmentRange range{};
    Mp4Fragment fragment{};
    u64 presentation_end{};
    u32 samples{}, next_fragment{};
};
// One complete independently fetched media file: optional styp/flat sidx,
// bounded emsg and up to 1024 moof/mdat pairs, 65536 samples in the selected
// track. Offsets are local to this file; clocks remain track ticks. Without
// sidx, the exact decode interval comes from the selected track's fragments.
// The clear initialization track must already be validated. At most 8 MiB,
// no allocation or copy; input and track configuration outlive the cursor.
// Failure preserves output. Audio index and decode clocks must agree exactly;
// video presentation timing may differ from decode timing due to reordering.
Mp4DemuxError mp4_media_segment_initialize(ConstSlice<u8>, const Mp4FragmentTrack&, Mp4MediaSegment*) noexcept;
// Read the next moof/mdat at a validated file offset, skipping bounded emsg.
// Returns a borrowed cursor plus the next byte offset, without retaining a
// fragment table. EOF and failure preserve both outputs.
Mp4DemuxError mp4_media_segment_fragment(ConstSlice<u8>, const Mp4FragmentTrack&, u32 offset,
    Mp4Fragment*, u32* next_offset) noexcept;
} // namespace maicord
