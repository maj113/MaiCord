// platform_settings_read and window_fatal_message on Windows: the system values the application reads again after a
// settings change, and the message box of the startup failures.
#include "maicord/platform/window/win32.hpp"

#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/registry.hpp"

#include <limits.h>

namespace maicord {
namespace {

// What changes only with a settings change (WM_SETTINGCHANGE), read once and again after win32_settings_reset: the
// registry, SystemParametersInfo and the double-click and caret values are not worth a system call per reader
// (event.hpp: settings_pointer and settings_caret). read_cached fills all but system_idle_ms, which
// platform_settings_read reads each time.
PlatformSettings g_cached;
bool g_cached_valid;

void read_cached() noexcept {
    const UINT interval = GetDoubleClickTime();
    g_cached.double_click_ms = interval ? interval : 500;
    g_cached.double_click_dx_px = static_cast<u32>(maximum(GetSystemMetrics(SM_CXDOUBLECLK), 1));
    g_cached.double_click_dy_px = static_cast<u32>(maximum(GetSystemMetrics(SM_CYDOUBLECLK), 1));
    g_cached.caret_blink_ms = GetCaretBlinkTime();
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    g_cached.wheel_lines = lines;
    g_cached.wheel_page = lines == WHEEL_PAGESCROLL;
    BOOL animation = FALSE;
    g_cached.client_area_animation =
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0) != FALSE && animation;
    // The app colour mode; without the value Windows draws apps light.
    DWORD light = 1, bytes = sizeof(light);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    g_cached.dark_scheme = status == ERROR_SUCCESS && light == 0;
    g_cached_valid = true;
}

} // namespace

void win32_settings_reset() noexcept {
    g_cached_valid = false;
}

PlatformSettings platform_settings_read() noexcept {
    if (!g_cached_valid)
        read_cached();
    PlatformSettings settings = g_cached;
    LASTINPUTINFO input{sizeof(input), 0};
    // dwTime is the low half of the tick count that clock_monotonic_milliseconds returns whole.
    settings.system_idle_ms = GetLastInputInfo(&input)
                                  ? static_cast<DWORD>(clock_monotonic_milliseconds()) - input.dwTime
                                  : kSystemIdleUnknown;
    return settings;
}

void window_fatal_message(Utf8View title, Utf8View text) noexcept {
    u16 wide_title[64], wide_text[256];
    u32 written = 0, next = 0;
    if (!utf8_copy_utf16_range(title, 0, title.bytes, {wide_title, 63}, &written, &next))
        written = 0;
    wide_title[written] = 0;
    if (!utf8_copy_utf16_range(text, 0, text.bytes, {wide_text, 255}, &written, &next))
        written = 0;
    wide_text[written] = 0;
    MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide_text), reinterpret_cast<const wchar_t*>(wide_title),
                MB_OK | MB_ICONERROR);
}

} // namespace maicord
