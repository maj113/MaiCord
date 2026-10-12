#pragma once

#include "maicord/core/slice.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

// Deferred UI Automation actions run from this posted message, never inside
// a UI Automation call.
inline constexpr UINT kApplicationAccessibilityMessage = kWindowMessageApp + 0x3A;

// WM_GETOBJECT (UI Automation root) and the deferred action message.
bool application_accessibility_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept;
// After each presented frame: snapshot and raise events while a client listens.
// A single flag test until a UI Automation client first asks for the window.
void application_accessibility_frame_presented() noexcept;
void application_accessibility_shutdown() noexcept;
// Committed snapshot storage (zero until a UI Automation client first asks).
usize application_accessibility_committed_bytes() noexcept;

} // namespace maicord::app_detail
