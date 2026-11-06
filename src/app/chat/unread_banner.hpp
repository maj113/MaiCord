#pragma once
#include "maicord/core/base.hpp"

#include "maicord/platform/window/event.hpp"

namespace maicord {
struct UiUnreadBannerScene;
}

namespace maicord::app_detail {
// Frame-time projection of the selected channel's unread bar and jump bar.
void application_unread_banner_synchronize(u64 now_ms) noexcept;
u64 application_unread_banner_next_wake() noexcept;
// Hover, cursor and primary press over the bars; other messages pass through.
bool application_unread_banner_window_event(const WindowEvent& event) noexcept;
// Plain Escape: marks the channel read and/or jumps to present when useful.
bool application_unread_banner_escape() noexcept;
void application_unread_banner_release() noexcept;
} // namespace maicord::app_detail
