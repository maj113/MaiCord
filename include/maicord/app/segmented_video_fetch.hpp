#pragma once
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/protocol/dash_manifest.hpp"
#include "maicord/protocol/hls_playlist.hpp"

namespace maicord {
struct SegmentedVideoFetch {
    Mp4SegmentStore store{};
    DashTrack track{};
    // Owned HLS text follows this owner. Live playback reuses one bounded
    // buffer, replacing it only after validating the complete new snapshot.
    HlsMediaPlaylist playlist{};
    HlsMediaSegment current{};
    char folder[1024]{}, text[544]{};
    u64 serial{};
    u32 folder_bytes{}, number{};
    bool initialization{}, synchronized{};
    Utf8View manifest{};
    u64 sequence_base{}, refresh_at{}, requested_at{};
    bool refreshing{}, youtube{}, vimeo{}, soundcloud{};
};
static_assert(sizeof(SegmentedVideoFetch) <= 3072);
ProgressiveVideoNext segmented_video_next(ProgressiveVideoFetch*, bool read_ahead, u64 now_seconds) noexcept;
ProgressiveVideoAccept segmented_video_accept(ProgressiveVideoFetch*, const CdnResponseResult&, ConstSlice<u8>, u64 initial_clock_90k = 0) noexcept;
void segmented_video_retire(ProgressiveVideoFetch*) noexcept;
void segmented_video_release(ProgressiveVideoFetch*) noexcept;
u32 segmented_video_buffered(ProgressiveVideoFetch*, u64 duration_us, u16* begin, u16* end, u32 capacity) noexcept;
bool progressive_video_fetch_begin_hls(ProgressiveVideoFetch*, Utf8View manifest, Utf8View playlist, bool audio,
    bool live = false, u64 now_seconds = 0) noexcept;
// Active playback only: caller merges this deadline with the existing lane
// wait. Paused/retired players must not call it to schedule background polls.
u32 segmented_video_refresh_delay(const ProgressiveVideoFetch*, u64 now_seconds) noexcept;
// Live startup trims to the later track's first timestamp. data_pending asks
// the shared lane for enough earlier-track media before publishing that origin.
Mp4DemuxError segmented_video_synchronize(ProgressiveVideoFetch* video, ProgressiveVideoFetch* audio) noexcept;
} // namespace maicord
