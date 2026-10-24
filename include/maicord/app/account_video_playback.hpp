#pragma once
#include "maicord/app/account_video_session.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
struct AccountVideoSelection {
    MediaAssetKey key{}; AccountVideoIdentity identity{};
    u16 collection_track{}; // Zero accepts the resident track; otherwise exact one-based index.
};
struct AccountVideoControl {
    AccountVideoIdentity identity{};u64 command_serial{};i64 seek_time_100ns{};
    float volume{1};AccountVideoCommand command{};bool muted{};
};
// Buffered video ranges in 1/10000 of the duration; a complete source is one full range.
inline constexpr u32 kAccountVideoBufferedRanges = 4;
struct AccountVideoBuffered {
    u16 begin[kAccountVideoBufferedRanges]{}, end[kAccountVideoBufferedRanges]{};
    u8 count{};
};
struct AccountVideoPlaybackStatus {
    MediaAssetKey key{};AccountVideoStatus playback{};bool selected{};
    // The selected progressive source needs a refreshed signed URL (403/410 or expiry);
    // the owner resubmits the attachment request with a forced refresh.
    bool source_refresh{};
    AccountVideoBuffered buffered{};
    u16 collection_count{}, collection_track{};
    bool collection_more{};
};
// The GPU source is borrowed until exact acknowledgement. Acknowledgement means
// the renderer copy has been queued, or the caller has discarded the frame.
struct AccountVideoFrame {
    MediaAssetKey key{};AccountVideoIdentity identity{};const GpuVideoFrame* gpu{};
    u64 playback_epoch{},frame_serial{};i64 presentation_time_us{};u64 duration_us{};
    u16 display_width{},display_height{};
};
}
