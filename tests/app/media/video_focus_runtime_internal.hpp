#pragma once

#include "chat/search/search.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/account_video_playback.hpp"

namespace maicord::video_focus_runtime_test {

extern u32 confirmations, media_refreshes;

extern UiSearchScene* mutable_search_scene;

using namespace app_detail;

struct AccountFixture {
    AccountVideoPlaybackStatus status{};
    AccountVideoFrame queued_frame{};
    u32 selections{}, commands{}, network_requests{}, gpu_publications{};
    u32 frame_leases{}, frame_acknowledgements{};
    u32 reject_controls{};
    bool frame_pending{};
    AccountMediaAttachmentRequest last_request{};
};
extern AccountFixture account;

extern u32 failures, text_publications, presentation_rejections;

extern bool reject_text_projection;

extern bool settings;

extern bool provider_upload;

extern bool search_current;

extern u32 viewer_opens;

extern bool viewer_allowed;

extern UiRect viewer_preview;

extern u32 downloads;

extern u64 download_message, download_attachment;

extern bool download_snapshot;

extern ApplicationSearchSourceView search_view;

} // namespace maicord::video_focus_runtime_test
