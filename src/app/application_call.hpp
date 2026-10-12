#pragma once
#include "maicord/core/base.hpp"
#include "maicord/ui/geometry.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Call view: participant tiles for the current voice or DM call over the
// content column, or the whole window in borderless fullscreen. Opened from
// the voice card, a voice channel row or a participant with their camera on;
// any navigation (a channel, friends, search, pins, threads) closes it.
//
// Opens the view for channel_id (the current call when zero), focused on
// focus_user when nonzero. A join in flight shows the connecting state.
bool application_call_open(u64 channel_id, u64 focus_user) noexcept;
void application_call_close() noexcept;
bool application_call_visible() noexcept;
// Per presented frame, after the panels published: layout and renderer binding.
bool application_call_synchronize() noexcept;
// Video receive: wants follow tile visibility; new pictures are presented.
// Polls frames only when due (application_call_next_wake).
bool application_call_pump(bool* repaint) noexcept;
// Voice pump cadence: wants only (also while the window is hidden).
void application_call_voice_pump() noexcept;
u64 application_call_next_wake() noexcept;
bool application_call_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Text in voice: the call's share of the content column while its chat shows
// beside it (zero otherwise). The shell layout reserves it from `available`.
float application_call_chat_split(float available) noexcept;
// While the call view covers the shell title bar (open, no chat split): true,
// with the header slot for the channel search field of the given width in
// *field (empty: no room or fullscreen, so the search hides).
bool application_call_header_search(float width, UiRect* field) noexcept;
// Focuses tile_id in borderless fullscreen, or leaves fullscreen when it is already.
void application_call_toggle_tile_fullscreen(u64 tile_id) noexcept;
void application_call_release() noexcept;
} // namespace maicord::app_detail
