#pragma once
#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Input observation never consumes a message, captures the mouse or changes
// logical focus. Main calls synchronize after dispatch/layout and before draw.
void application_tooltip_input(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
bool application_tooltip_synchronize(u64 now_ms) noexcept;
u64 application_tooltip_next_wake() noexcept;
void application_tooltip_clear() noexcept;
void application_tooltip_release() noexcept;
u32 application_tooltip_owner_bytes() noexcept;
} // namespace maicord::app_detail
