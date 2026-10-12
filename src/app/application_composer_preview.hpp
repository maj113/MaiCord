#pragma once
#include "maicord/app/account_media.hpp"

namespace maicord::app_detail {
// Optional thumbnails for the composer upload strip. Images decode on the
// account media lanes (never on the UI thread) into the preview atlas; MP4/MOV
// files get a GPU-decoded first-frame poster reduced to the same extent. A
// missing, unsupported or failed preview leaves the file-type card.
struct ApplicationComposerPreview {
    MediaAssetKey key;
    bool ready, failed;
};
ApplicationComposerPreview application_composer_preview_view(u64 draft_id) noexcept;
// Format whose codec MaiCord owns for this filename, or unknown.
MediaAttachmentFormat application_composer_preview_format(Utf8View filename) noexcept;
// Admits current active-channel drafts and retires everything else.
bool application_composer_preview_request(u64 now_ms, u64 unix_now_seconds) noexcept;
bool application_composer_preview_key_current(MediaAssetKey) noexcept;
bool application_composer_preview_frame_current(const AccountMediaFrame&) noexcept;
void application_composer_preview_frame_uploaded(const AccountMediaFrame&) noexcept;
void application_composer_preview_rejected(MediaAssetKey) noexcept;
// The decoded thumbnail found no preview atlas room (every entry pinned by
// visible media): wait until retry_at_ms instead of re-decoding on the next
// 100 ms poll. A ready or failed tile is unaffected.
void application_composer_preview_deferred(MediaAssetKey, u64 retry_at_ms) noexcept;
// Retires one draft's preview (0: all) and makes its atlas entry evictable.
void application_composer_preview_retire(u64 draft_id = 0) noexcept;
void application_composer_preview_release() noexcept;
u64 application_composer_preview_next_wake() noexcept;
u32 application_composer_preview_revision() noexcept;
struct ApplicationComposerPreviewMetrics {
    u32 active, pending, ready, failed;
    u64 admissions, retirements;
};
ApplicationComposerPreviewMetrics application_composer_preview_metrics() noexcept;

// Local-preview routing shared by the forum form and the composer strip.
bool application_local_preview_key_current(MediaAssetKey) noexcept;
bool application_local_preview_frame_current(const AccountMediaFrame&) noexcept;
void application_local_preview_frame_uploaded(const AccountMediaFrame&) noexcept;
void application_local_preview_rejected(MediaAssetKey) noexcept;
void application_local_preview_deferred(MediaAssetKey, u64 retry_at_ms) noexcept;
} // namespace maicord::app_detail
