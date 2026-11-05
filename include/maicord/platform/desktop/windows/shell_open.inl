// The Windows bodies of maicord/platform/desktop/shell_open.hpp. Included by shell_open.hpp only.
#include "maicord/platform/window/win32.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {

__forceinline void desktop_alert_beep() noexcept {
    MessageBeep(MB_OK);
}

__forceinline bool desktop_open_target(Window owner, const wchar_t* target) noexcept {
    return reinterpret_cast<usize>(
               ShellExecuteW(win32_hwnd(owner), L"open", target, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

__forceinline bool desktop_reveal_file(const wchar_t* path) noexcept {
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path);
    if (!item)
        return false;
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
    return true;
}

__forceinline bool desktop_open_notification_settings(Window owner) noexcept {
    return desktop_open_target(owner, L"ms-settings:notifications");
}

} // namespace maicord
