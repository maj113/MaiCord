#pragma once

// Shared between the files of the Windows window backend.
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/win32.hpp"

namespace maicord::win32_detail {

// The cursor position (client pixels) the window procedure resolved for the WM_SETCURSOR being dispatched, so
// every layer that asks where the pointer is reads it (window_pointer_position) instead of querying the system
// again.
void dispatch_cursor_begin(HWND window, float x, float y) noexcept;
void dispatch_cursor_end() noexcept;
// The cursor of the WM_SETCURSOR being dispatched to `window`, when there is one.
bool dispatch_cursor(HWND window, float* x, float* y) noexcept;
// The window procedure.
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;

// Delivers an event to the sink of the window created by win32_window_create.
bool deliver(const WindowEvent& event, WindowEventResult* result) noexcept;
// Records the main window for win32_main_window.
void set_main_window(HWND window) noexcept;

// The frame messages of a window whose client extends over the system caption (caption_frame.cpp). True when handled;
// *result is then the LRESULT.
bool caption_frame_message(const Win32CaptionOps& ops, HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                           LRESULT* result) noexcept;
// The outer size of `window` for a client of this size; `ops` is null for the system frame.
void frame_outer_size(const Win32CaptionOps* ops, HWND window, u32 client_width, u32 client_height, u32* width,
                      u32* height) noexcept;
// The caption controller of the window, null for the system frame.
const Win32CaptionOps* caption_ops() noexcept;

} // namespace maicord::win32_detail
