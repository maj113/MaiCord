#pragma once
#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Main-loop tick: projects mentions/unread into the taskbar overlay badge and
// flashes the button for notifications admitted while unfocused.
void application_taskbar_attention_tick(u64 now_ms) noexcept;
// A message notification passed policy; flashes on the next tick if unfocused.
void application_taskbar_attention_notify() noexcept;
// Returns true for TaskbarButtonCreated.
bool application_taskbar_attention_window_message(UINT message) noexcept;
void application_taskbar_attention_release() noexcept;
// Last counted mentions (plus friend requests) and unread state; the tray reuses them.
void application_taskbar_attention_counts(u32* mentions, bool* unread) noexcept;
} // namespace maicord::app_detail
