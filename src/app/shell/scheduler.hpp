#pragma once

#include "maicord/core/base.hpp"

namespace maicord::app_detail {

// visual_suspended (minimized/hidden window) drops purely visual deadlines.
u32 application_wait_timeout(u64 now_ms, u64 now_us, bool frame_clock_armed,
                             bool visual_suspended = false) noexcept;
bool application_nonvisual_wake_due(u64 now_ms) noexcept;
u64 application_login_browser_deadline() noexcept;
bool application_smooth_scroll_active() noexcept;
bool application_animation_frame_due(u64 now_us) noexcept;
bool application_effect_frame_due(u64 now_ms) noexcept;
bool application_visual_frame_requested(u64 now_ms, u64 now_us) noexcept;
bool application_refresh_relative_timestamps_if_due() noexcept;

} // namespace maicord::app_detail
