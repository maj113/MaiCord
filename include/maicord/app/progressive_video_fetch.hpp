#pragma once

#include "maicord/media/progressive_source.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/protocol/cdn_http.hpp"

namespace maicord {
struct SegmentedVideoFetch;
struct DashTrack;

// Transport-neutral HTTP Range driver for one progressive MP4 attachment. The
// owner submits the ranges it returns on its CDN client and feeds every
// response back; the sparse source is the decoder's borrowed encoded span.
enum class ProgressiveVideoFetchState : u8 { empty, probing, ready, refresh_required, failed };
enum class ProgressiveVideoKind : u8 { movie, fragmented_video, fragmented_audio };

enum class ProgressiveVideoAccept : u8 {
    ignored,          // Stale, cancelled or retryable transport failure.
    progress,         // Range stored.
    whole_entity,     // The server ignored Range: the caller decodes the 200 body itself.
    refresh_required, // 403/410: resubmit after a refreshed signed URL.
    failed,
};

enum class ProgressiveVideoStep : u8 { idle, submit, cancel, ready, refresh_required, failed };
struct ProgressiveVideoNext {
    CdnByteRange range;
    ProgressiveVideoStep step;
};

inline constexpr u32 kProgressiveVideoMaximumRefreshes = 3;
inline constexpr u32 kProgressiveVideoMaximumTransportFailures = 3;

struct ProgressiveVideoFetch {
    SegmentedVideoFetch* segmented;
    ProgressiveSource source;
    // Table-only video cursor over source for buffered ranges.
    Mp4Demuxer index;
    Mp4FragmentSource fragment;
    Mp4FragmentAudio audio;
    u8 target[kCdnMaximumTargetBytes];
    // A refreshed URL offered by the UI thread; guarded by the video runtime lock.
    char offered[kCdnMaximumTargetBytes];
    u64 expires_at_seconds;
    u64 inflight_id;
    CdnByteRange inflight;
    u64 cache_expires_at_seconds;
    u32 target_bytes;
    u32 offered_bytes;
    CdnHost host;
    CdnImageType image_type;
    ProgressiveVideoKind kind;
    ProgressiveVideoFetchState state;
    u8 refreshes;
    u8 transport_failures;
    bool cancelling;
    bool index_ready;
    bool cache_store_allowed;
    bool cache_must_revalidate;
    bool cached;
    // Only signed attachments have an API refresh route. An ordinary embed
    // returning 403/410 must end, rather than wait forever for an impossible refresh.
    bool refreshable;
};

// The first range is the head probe; later ranges follow the probe and cursors.
bool progressive_video_fetch_begin(ProgressiveVideoFetch*, ConstSlice<u8> target, CdnHost,
                                   u64 expires_at_seconds, ProgressiveVideoKind = ProgressiveVideoKind::movie) noexcept;
bool progressive_video_fetch_begin_segments(ProgressiveVideoFetch*, Utf8View manifest_url, const DashTrack&, bool audio) noexcept;
bool progressive_video_fetch_pending(const ProgressiveVideoFetch*) noexcept;
u64 progressive_video_fetch_generation(const ProgressiveVideoFetch*) noexcept;
// Rebuilds fragment metadata from retained initialization after joined retirement.
bool progressive_video_fetch_restore(ProgressiveVideoFetch*) noexcept;
void progressive_video_fetch_retire(ProgressiveVideoFetch*) noexcept;
void progressive_video_fetch_release(ProgressiveVideoFetch*) noexcept;
ProgressiveVideoNext progressive_video_fetch_next(ProgressiveVideoFetch*, bool read_ahead,
                                                  u64 now_seconds) noexcept;
void progressive_video_fetch_submitted(ProgressiveVideoFetch*, u64 request_id, CdnByteRange) noexcept;
void progressive_video_fetch_cancelling(ProgressiveVideoFetch*) noexcept;
ProgressiveVideoAccept progressive_video_fetch_accept(ProgressiveVideoFetch*, u64 request_id,
                                                      bool transport_ok, const CdnResponseResult&,
                                                      ConstSlice<u8> body, u64 initial_clock_90k = 0) noexcept;
// Installs a refreshed signed target; the entity length must stay the same.
bool progressive_video_fetch_refresh(ProgressiveVideoFetch*, ConstSlice<u8> target, CdnHost,
                                     u64 expires_at_seconds) noexcept;
// Buffered video ranges in 1/10000 of duration_us.
u32 progressive_video_fetch_buffered(ProgressiveVideoFetch*, u64 duration_us, u16* begin, u16* end,
                                     u32 capacity) noexcept;

} // namespace maicord
