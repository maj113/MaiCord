#pragma once
#include "maicord/core/base.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord {
struct UiAccessWidgetInfo;
}

namespace maicord::app_detail {
// A one-to-one DM header's identity, its avatar and name: a click opens the user's full profile; hovering shows the
// names popout (their username with "AKA", then each other nickname they use in a server you share, with those
// servers' pictures) at once, closing 250 ms after the pointer leaves both the identity and the popout. Without
// another nickname there is no popout.
bool application_private_header_window_event(const WindowEvent& event) noexcept;
// Closes the popout when its conversation, a modal surface or the sign-in takes over; advances its entrance.
void application_private_header_pump() noexcept;
u64 application_private_header_next_wake() noexcept;
// Keyboard: the identity is a stop of the header controls; focus shows the popout, Enter and Space open the profile.
bool application_private_header_focused() noexcept;
bool application_private_header_activate() noexcept;
// Screen readers: Expand shows the popout, Collapse closes it (Invoke is the click that opens the profile).
void application_private_header_popout(bool show) noexcept;
inline constexpr u64 kPrivateHeaderAccessItem = 0x5048'4944'454e'5459ull;
void application_private_header_access(UiAccessWidgetInfo* info) noexcept;
} // namespace maicord::app_detail
