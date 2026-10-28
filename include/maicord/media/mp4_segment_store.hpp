#pragma once
#include "maicord/core/reserved_bytes.hpp"
#include "maicord/media/mp4_fragment.hpp"

namespace maicord {
struct Mp4SegmentSlot {
    ReservedBytes storage{};
    Mp4MediaSegment media{};
    u32 number{}, bytes{};
};
// Lane-owned, fixed-address storage for separately fetched media files. Only
// the current file and one seek/read-ahead candidate are resident. The active
// slot remains pinned until explicit adoption. No codec, HTTP or worker here.
// Zero initialize; release only after all borrowed packets/decoders retire.
struct Mp4SegmentStore {
    ReservedBytes initialization{};
    Mp4FragmentTrack track{};
    Mp4SegmentSlot slots[2]{};
    u64 generation{}, request_serial{}, nominal_duration{};
    u64 seek_time{};
    u32 initialization_bytes{}, count{}, nominal_timescale{}, demand{};
    u32 seek_number{}, seek_attempts{};
    u8 active{2};
    i32 seek_direction{};
    bool pending{}, seek_active{};
    // Optional borrowed playlist lookup; supplies a file hint in microseconds
    // relative to the first media timestamp. Container timing stays authoritative.
    void* seek_context{};
    bool (*locate)(void*, u64, u32*) noexcept{};
    u64 available_end{}, available_presentation_end{};
    u32 first_available{}, received_count{}; // Highest accepted ordinal plus one; final duration is known only after its body arrives.
    bool live{}, ended{}, refresh_pending{}, transport{};
};
static_assert(sizeof(Mp4SegmentStore) <= 1024);
// Copies the bounded initialization once. Segment pages are allocated only on
// receipt: <=8 MiB per video slot, <=1 MiB per audio slot. No per-packet work.
Mp4DemuxError mp4_segment_store_initialize(Mp4SegmentStore*, ConstSlice<u8> initialization,
    bool audio, u32 count, u64 nominal_duration, u32 nominal_timescale, bool live = false) noexcept;
// Extend stable live file ordinals after a validated playlist refresh. Cached
// active bytes survive removal from the advertised window. An uncached expired
// ordinal needs caller resynchronization; it must never be silently renumbered.
Mp4DemuxError mp4_segment_store_extend(Mp4SegmentStore*, u32 first_available, u32 count, bool ended) noexcept;
bool mp4_segment_store_overlaps(const Mp4SegmentStore&, const void*, u32) noexcept;
// A file's interval must not overlap a resident neighbour, and live neighbours abut exactly.
bool mp4_segment_store_range_fits(const Mp4SegmentStore&, u32 number, const Mp4FragmentRange&) noexcept;
const Mp4SegmentSlot* mp4_segment_store_find(const Mp4SegmentStore&, u32 number) noexcept;
// A missing lookup publishes demand. Repeating the same demand preserves its
// serial; retarget/cancel makes prior responses stale. No active bytes change.
Mp4DemuxError mp4_segment_store_request(Mp4SegmentStore*, u32 number) noexcept;
Mp4DemuxError mp4_segment_store_accept(Mp4SegmentStore*, u32 number, u64 request_serial, ConstSlice<u8>) noexcept;
bool mp4_segment_store_retain(Mp4SegmentStore*, u32 number) noexcept;
void mp4_segment_store_cancel(Mp4SegmentStore*) noexcept;
void mp4_segment_store_release(Mp4SegmentStore*) noexcept;
} // namespace maicord
