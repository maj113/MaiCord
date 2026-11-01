#pragma once

namespace maicord {
// A shader phase has no media identity/viewport publication. All causes that
// can change those sources retain the ordinary bounded completion/scan path.
struct BackgroundFrameWork {
    bool background_requested;
    bool previous_damage;
    bool account_work;
    bool dispatched_messages;
    bool media_animation_due;
    bool video_due;
    bool smooth_scrolling;
    bool other_visual_due;
    bool media_scan_pending;
};
constexpr bool background_frame_reuses_static_media(BackgroundFrameWork work) noexcept {
    return work.background_requested && !work.previous_damage && !work.account_work &&
           !work.dispatched_messages && !work.media_animation_due && !work.video_due &&
           !work.smooth_scrolling && !work.other_visual_due && !work.media_scan_pending;
}
} // namespace maicord
