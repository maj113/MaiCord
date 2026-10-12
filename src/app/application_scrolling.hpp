#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
bool request_older_history_if_near_start() noexcept;

bool scroll_message_view_by(i64 delta_units, bool repaint = true) noexcept;
bool scroll_message_view_to(bool bottom) noexcept;
// "Jump to Present": a window detached from the live tail (after a jump or
// deep scrollback) is replaced by the latest page; otherwise scrolls to bottom.
bool application_message_jump_to_present() noexcept;
bool handle_scrollbar_begin(float pointer_x, float pointer_y) noexcept;
bool handle_scrollbar_drag(WPARAM buttons, float pointer_y) noexcept;
void end_scrollbar_drag() noexcept;
bool reveal_command_row(float top, float bottom) noexcept;
bool queue_profile_scroll(bool right, i32 delta) noexcept;
bool handle_scroll_wheel(WPARAM wparam, LPARAM lparam) noexcept;
bool handle_message_key(WPARAM key) noexcept;
bool tick_smooth_scrolls(u64 now_us) noexcept;
// A keyboard move or scroll-into-view already placed the rail or menu at its
// target. With smooth scrolling on, replay that move from `before` with the
// wheel curve (the frame loop ticks it); with it off, the jump stands.
bool animate_guild_rail_scroll_from(float before) noexcept;
bool animate_popup_scroll_from(float before) noexcept;
void update_scrollbar_hover(float x, float y) noexcept;

} // namespace maicord::app_detail
