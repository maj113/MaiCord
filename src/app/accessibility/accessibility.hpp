#pragma once

#include "maicord/core/win32_types.hpp"

namespace maicord {
struct WindowEvent;
}

namespace maicord::app_detail {

#if defined(_WIN32)
// Deferred UI Automation actions run from this posted message, never inside
// a UI Automation call.
inline constexpr UINT kApplicationAccessibilityMessage = kWindowMessageApp + 0x3A;

// WM_GETOBJECT (UI Automation root) and the deferred action message.
bool application_accessibility_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
#else
// The window sink's view of every event (Linux): the deferred AT-SPI actions arrive as wake_accessibility and run here;
// input makes the next snapshot urgent; the destroyed window shuts the server down. True when the event was consumed.
bool application_accessibility_window_event(const WindowEvent& event) noexcept;
#endif
// After each presented frame: snapshot and raise events while a client listens.
// A single flag test until a UI Automation client first asks for the window (Linux: until an AT-SPI client has called).
void application_accessibility_frame_presented() noexcept;
void application_accessibility_shutdown() noexcept;

} // namespace maicord::app_detail
