// The main window's handle for the parts of the backend that act for the application without a Window: the clipboard
// is owned by it.
#include "maicord/platform/window/win32.hpp"
#include "internal.hpp"

namespace maicord {
namespace {

HWND g_main_window;

} // namespace

namespace win32_detail {

void set_main_window(HWND window) noexcept {
    g_main_window = window;
}

} // namespace win32_detail

HWND win32_main_window() noexcept {
    return g_main_window;
}

} // namespace maicord
