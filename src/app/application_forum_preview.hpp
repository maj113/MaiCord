#pragma once
#include "application_forum_post.hpp"
#include "maicord/app/account_media.hpp"
namespace maicord::app_detail {
struct ApplicationForumPreview {
    MediaAssetKey key;
    bool ready, failed;
};
ApplicationForumPreview application_forum_preview_view(const ApplicationForumPostTicket&,
                                                       u64 draft_id) noexcept;
bool application_forum_preview_key_current(MediaAssetKey) noexcept;
bool application_forum_preview_request(u64 now_ms, u64 unix_now_seconds) noexcept;
bool application_forum_preview_frame_current(const AccountMediaFrame&) noexcept;
void application_forum_preview_frame_uploaded(const AccountMediaFrame&) noexcept;
void application_forum_preview_rejected(MediaAssetKey) noexcept;
void application_forum_preview_retire(const ApplicationForumPostTicket&, u64 draft_id = 0) noexcept;
void application_forum_preview_release() noexcept;
u64 application_forum_preview_next_wake() noexcept;
u32 application_forum_preview_revision() noexcept;
struct ApplicationForumPreviewMetrics {
    u32 owner_bytes, committed_bytes, active, pending, ready, failed;
    u64 admissions;
};
ApplicationForumPreviewMetrics application_forum_preview_metrics() noexcept;
} // namespace maicord::app_detail
