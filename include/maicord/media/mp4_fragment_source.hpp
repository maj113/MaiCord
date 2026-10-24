#pragma once
#include "maicord/media/mp4_fragment.hpp"
#include "maicord/media/progressive_source.hpp"

namespace maicord {
struct Mp4SegmentStore;
struct Mp4FragmentPacket {
    Mp4FragmentSample sample{};
    ConstSlice<u8> bytes{};
    u32 segment{};
};
// Exclusive lane-owned reader of an immutable sparse entity or separate files.
// Initialization stays pinned; the current moof/file has a replaceable lease.
// Do not copy a live owner. Its backing source/store must outlive release;
// this cursor owns no allocation.
struct Mp4FragmentSource {
    ProgressiveSource* source{};
    Mp4SegmentStore* segments{};
    Mp4FragmentTrack track{};
    Mp4FragmentIndex index{};
    Mp4FragmentRange segment{};
    Mp4Fragment fragment{};
    u64 presentation_end{}; // Final file's actual presentation endpoint.
    u32 next_fragment{};
    ProgressiveTrack reader{};
    bool loaded{}, pending{}, ended{}, preparing{};
};
static_assert(sizeof(Mp4FragmentSource) <= 560);
bool mp4_fragment_source_valid(const Mp4FragmentSource&) noexcept;
bool mp4_fragment_source_overlaps(const Mp4FragmentSource&, const void*, u32) noexcept;
ConstSlice<u8> mp4_fragment_source_bytes(const Mp4FragmentSource&) noexcept;
// Short-lived seek proposal. It owns no lease and must be used immediately on
// the same lane, before another read/select/write/eviction. The current source
// cursor and metadata lease remain intact until adoption. Never edit a proposal.
// Discard old borrowed packet views before preparation; new range requests may
// evict old payload. If a seek is cancelled, re-peek the preserved old cursor.
// Pending preparation temporarily retains the new header as well as the old
// current header. Cancel, normal peek, adoption, select or release drops it.
struct Mp4FragmentPosition {
    Mp4FragmentSource* owner{};
    u64 generation{};
    Mp4FragmentRange segment{};
    Mp4Fragment fragment{};
    Mp4FragmentPacket packet{};
    u32 next_fragment{};
};
// Sample is relative to the selected segment, spanning its inner moofs.
Mp4DemuxError mp4_fragment_source_prepare(Mp4FragmentSource*, u32 segment, u32 sample, Mp4FragmentPosition*) noexcept;
void mp4_fragment_source_cancel_prepare(Mp4FragmentSource*) noexcept;
bool mp4_fragment_source_adopt(Mp4FragmentSource*, const Mp4FragmentPosition&) noexcept;
Mp4DemuxError mp4_fragment_position_unit(const Mp4FragmentPosition&, Mp4AccessUnit*) noexcept;
bool mp4_fragment_source_time(const Mp4FragmentSource&, i64 track_ticks, i64* movie_us) noexcept;
// Empty output only. Requires ftyp/moov and a flat sidx within the bounded
// metadata prefix; absent ranges register demand and return data_pending.
// The source owner feeds those ranges through its existing scheduler. No HTTP,
// codec or worker is created. Failure leaves output empty.
Mp4DemuxError mp4_fragment_source_initialize(ProgressiveSource*, bool audio, Mp4FragmentSource*) noexcept;
// Separate-file variant: requests first/last files to establish exact clocks.
// Missing files publish demand in the store. The store outlives this reader.
Mp4DemuxError mp4_fragment_source_initialize_segments(Mp4SegmentStore*, Mp4FragmentSource*) noexcept;
// Publish newly received live file extents without resetting the cursor or
// allocating a sample table. The live edge remains pending until ENDLIST.
bool mp4_fragment_source_refresh(Mp4FragmentSource*) noexcept;
// Exact containing segment in track ticks. Separate files check the two cached
// intervals before using a nominal ordinal hint and inspecting actual indexes.
// Missing files publish demand and preserve both the accepted cursor and the
// seek's search progress.
Mp4DemuxError mp4_fragment_source_find(Mp4FragmentSource*, u64 track_time, Mp4FragmentRange*) noexcept;
// Only successful commit consumes a packet. Repeated peek, missing bytes and
// codec backpressure preserve sample identity. At a consumed segment boundary,
// peek can replace metadata with the next segment's moof. Samples are <=8 MiB;
// each returned byte span remains borrowed until commit, select or release.
// The source owner must not trim or reset readers while a packet is borrowed.
Mp4DemuxError mp4_fragment_source_peek(Mp4FragmentSource*, Mp4FragmentPacket*) noexcept;
// Existing native-video access-unit view, preserving raw track ticks alongside
// the signed movie time after edit mapping. Does not consume the pending packet.
Mp4DemuxError mp4_fragment_source_unit(Mp4FragmentSource*, Mp4AccessUnit*) noexcept;
bool mp4_fragment_source_duration(const Mp4FragmentSource&, u64* microseconds) noexcept;
bool mp4_fragment_source_commit(Mp4FragmentSource*) noexcept;
// Coarse selection, not a decoder seek: clears the pending packet and metadata
// lease and registers the target range. Index count is the EOF sentinel. Caller
// must apply its playback-generation fence and validate the random-access NAL
// before resetting GPU references; sidx SAP flags alone do not prove an IDR.
bool mp4_fragment_source_select(Mp4FragmentSource*, u32 segment) noexcept;
void mp4_fragment_source_release(Mp4FragmentSource*) noexcept;
} // namespace maicord
