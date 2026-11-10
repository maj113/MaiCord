#include "maicord/platform/window/win32.hpp"

// The application's half of the Windows window procedure (platform/window/win32.hpp) for the tests that create a real
// window without the application: no caption controller (the system frame), no shell hook, no modal.
namespace maicord {

bool win32_shell_message(HWND, UINT, WPARAM, LPARAM, LRESULT*) noexcept {
    return false;
}
bool win32_input_blocked() noexcept {
    return false;
}
bool win32_caption_enabled() noexcept {
    return false;
}
u32 win32_caption_height() noexcept {
    return 0;
}
bool win32_caption_hidden() noexcept {
    return true;
}
FrameHit win32_caption_hit(float, float) noexcept {
    return FrameHit::client;
}
void win32_caption_maximized(bool) noexcept {}
void win32_caption_active(bool) noexcept {}
void win32_caption_hover(FrameHit) noexcept {}
void win32_caption_leave() noexcept {}
void win32_caption_press(FrameHit) noexcept {}
bool win32_caption_release(FrameHit) noexcept {
    return false;
}
bool win32_caption_title_bar(FrameBarRects*) noexcept {
    return false;
}

} // namespace maicord
