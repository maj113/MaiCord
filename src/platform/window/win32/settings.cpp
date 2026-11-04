// platform_settings_read and window_fatal_message on Windows: the system values the application reads again after a
// settings change, and the message box of the startup failures.
#include "maicord/platform/window/win32.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/registry.hpp"

#include <limits.h>

namespace maicord {
namespace {

// What changes only with a settings change (WM_SETTINGCHANGE), read once and again after win32_settings_reset: the
// registry and the SystemParametersInfo values are not worth a system call per reader.
struct Cached {
    u32 wheel_lines;
    bool valid, wheel_page, animation, dark;
} g_cached;

void read_cached() noexcept {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    g_cached.wheel_lines = lines;
    g_cached.wheel_page = lines == WHEEL_PAGESCROLL;
    BOOL animation = FALSE;
    g_cached.animation = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0) != FALSE && animation;
    // The app colour mode; without the value Windows draws apps light.
    DWORD light = 1, bytes = sizeof(light);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    g_cached.dark = status == ERROR_SUCCESS && light == 0;
    g_cached.valid = true;
}

} // namespace

void win32_settings_reset() noexcept {
    g_cached.valid = false;
}

PlatformSettings platform_settings_read() noexcept {
    if (!g_cached.valid)
        read_cached();
    PlatformSettings settings{};
    const UINT interval = GetDoubleClickTime();
    settings.double_click_ms = interval ? interval : 500;
    settings.double_click_dx_px = static_cast<u32>(maximum(GetSystemMetrics(SM_CXDOUBLECLK), 1));
    settings.double_click_dy_px = static_cast<u32>(maximum(GetSystemMetrics(SM_CYDOUBLECLK), 1));
    settings.caret_blink_ms = GetCaretBlinkTime();
    settings.wheel_lines = g_cached.wheel_lines;
    settings.wheel_page = g_cached.wheel_page;
    settings.client_area_animation = g_cached.animation;
    settings.dark_scheme = g_cached.dark;
    LASTINPUTINFO input{sizeof(input), 0};
    settings.system_idle_ms = GetLastInputInfo(&input) ? GetTickCount() - input.dwTime : kSystemIdleUnknown;
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
