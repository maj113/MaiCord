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
// A rejected frame keeps its display slot, and a visual deadline that only a drawn frame moves stays due. The loop
// would then wait zero and draw again at once, for as long as the frame fails. So the loop tries the first three
// rejections in a row again on the next display frame. It tries each later one after a delay that doubles from
// 16 ms to 250 ms. Until that time the loop presents nothing, and no frame request or visual deadline wakes it.
void application_frame_retry_schedule(u64 now_ms) noexcept;
bool application_frame_retry_waiting(u64 now_ms) noexcept;
bool application_refresh_relative_timestamps_if_due() noexcept;

} // namespace maicord::app_detail
