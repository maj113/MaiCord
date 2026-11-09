#pragma once
#include "maicord/core/base.hpp"

#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {
// Input observation never consumes a message, captures the mouse or changes
// logical focus. Main calls synchronize after dispatch/layout and before draw.
void application_tooltip_input(const WindowEvent& event) noexcept;
bool application_tooltip_synchronize(u64 now_ms) noexcept;
u64 application_tooltip_next_wake() noexcept;
void application_tooltip_clear() noexcept;
// A pinned tooltip (the command card) appeared: makes the next frame offer it
// even before the pointer has hovered anything.
void application_tooltip_wake() noexcept;
void application_tooltip_release() noexcept;
// A click at (x, y) DIPs on an attachment's "ALT" badge: opens its description's popout (a click on the badge whose
// popout that press closed leaves it closed). False when no badge is there.
bool application_alt_text_toggle(float x, float y) noexcept;
} // namespace maicord::app_detail
