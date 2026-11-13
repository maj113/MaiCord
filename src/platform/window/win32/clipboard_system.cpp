// The Win32 clipboard transaction behind clipboard.cpp's writes (win32_clipboard.hpp), alone in this file so that tests
// link a capturing replacement. The clipboard belongs to the main window, whatever the caller.
#include "maicord/platform/window/win32_clipboard.hpp"
#include "maicord/platform/window/win32.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord {

bool clipboard_system_open() noexcept {
    if (!OpenClipboard(win32_main_window()))
        return false;
    if (EmptyClipboard())
        return true;
    CloseClipboard();
    return false;
}

bool clipboard_system_publish(UINT format, HGLOBAL data) noexcept {
    return SetClipboardData(format, data) != nullptr;
}

void clipboard_system_close() noexcept {
    CloseClipboard();
}

} // namespace maicord
