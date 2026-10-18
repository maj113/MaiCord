#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/ui/caption_bar.hpp"

namespace maicord::app_detail {

// The main window draws its own caption: WM_NCCALCSIZE extends the client over
// the system caption and keeps the DWM side and bottom frame. The renderer's
// content starts application_caption_height() pixels below the client origin.
// Every offset below is zero until application_caption_enable, so other windows
// and test fixtures that share the window procedure keep client coordinates.

// Before CreateWindowExW: the creation-time WM_NCCALCSIZE already applies it.
void application_caption_enable() noexcept;
// Frame messages (WM_NCCALCSIZE, WM_NCHITTEST, the caption buttons, the
// window menu and activation). Runs before every other route.
bool application_caption_window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                        LRESULT* result) noexcept;
// Pixels between the client origin and the content.
u32 application_caption_height() noexcept;
// Client mouse coordinates to content coordinates.
LPARAM application_caption_content_point(LPARAM client) noexcept;
// Screen and content conversions: application_content_point.hpp.
// Outer window size for a content size at a DPI (minimum track size).
SIZE application_caption_window_size(HWND window, u32 dpi, LONG content_width,
                                     LONG content_height) noexcept;
// Per frame: follows theme and DPI changes of the height and publishes the
// caption state and title to the renderer.
void application_caption_synchronize() noexcept;
// Fullscreen hides the caption; leaving it restores it.
void application_caption_set_hidden(bool hidden) noexcept;
// Minimize, maximize/restore or close, as the caption buttons do.
bool application_caption_command(UiCaptionPart part) noexcept;
// The bar state; the hovered part's tooltip shows once its delay passed.
UiCaptionBarState application_caption_state() noexcept;
// When the hovered part's tooltip is due, zero when none is pending.
u64 application_caption_tooltip_wake() noexcept;
// Caption buttons in content DIPs (negative rows), for accessibility.
UiCaptionBarLayout application_caption_layout() noexcept;

} // namespace maicord::app_detail
