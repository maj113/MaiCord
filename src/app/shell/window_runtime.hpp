#pragma once

#include "maicord/core/base.hpp"

#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {

// The window's sink: every window event of the main window, in the order the overlays and the shell take it.
void application_window_event(void* context, const WindowEvent& event, WindowEventResult* result) noexcept;

// Whether a modal (logout confirmation or retry) owns all input.
bool application_input_blocked() noexcept;

} // namespace maicord::app_detail
