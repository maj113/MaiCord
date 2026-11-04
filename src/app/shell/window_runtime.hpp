#pragma once

#include "maicord/core/base.hpp"

#include "maicord/platform/window/win32.hpp"

namespace maicord::app_detail {

// The window's sink: every window event of the main window, in the order the overlays and the shell take it.
void application_window_event(void* context, const WindowEvent& event, WindowEventResult* result) noexcept;

// Whether a modal (logout confirmation or retry) owns all input.
bool application_input_blocked() noexcept;

// What the application keeps for itself in the window procedure: the state observer, the caption frame, UI
// Automation and the shell integration (win32.hpp).
const Win32Hooks* application_window_hooks() noexcept;

} // namespace maicord::app_detail
