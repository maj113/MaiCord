// The application's accessibility server on Linux: none yet. The snapshots (accessibility_describe.cpp) are portable;
// what exposes them to assistive technology on Windows is UI Automation (accessibility.cpp), and on Linux that would be
// AT-SPI over D-Bus. Until then no client can ask, so the window never reports an accessibility object and the frame
// hook has nothing to publish.
#include "../accessibility.hpp"

namespace maicord::app_detail {

bool application_accessibility_window_message(UINT, WPARAM, LPARAM, LRESULT*) noexcept {
    return false;
}

void application_accessibility_frame_presented() noexcept {}

void application_accessibility_shutdown() noexcept {}

} // namespace maicord::app_detail
