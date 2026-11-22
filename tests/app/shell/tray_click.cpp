// A click on the notification-area icon (issue #31): the window in use hides to the tray, any other window comes to
// the front (desktop_tray_click). The headless window gives the show state the client's sink reads, and the activation
// events arrive as the sink observes them (src/app/shell/window_state.hpp).
#include "../../support/test_output.hpp"
#include "../../support/window_fixture.hpp"
#include "shell/window_state.hpp"

MAICORD_TEST_ENTRY(maicord_tray_click_entry) {
    using namespace maicord;
    using namespace maicord::app_detail;
    u32 failures = 0;
    const auto check = [&failures](bool valid, const char* label) noexcept { test_check(&failures, valid, label); };

    const Window window = window_test::open();
    window_show(window, ShowMode::show);
    application_window_observe_position(window);
    application_window_observe_activation(true, 1'000);
    check(application_window_tray_click(true, 61'000) == DesktopTrayClick::hide,
          "the shown, active window hides to the tray");
    check(application_window_tray_click(false, 61'000) == DesktopTrayClick::reveal,
          "without the icon the window never hides");

    // Windows: the press on the notification area activates the taskbar before the click arrives.
    application_window_observe_activation(false, 70'000);
    check(application_window_tray_click(true, 70'120) == DesktopTrayClick::hide,
          "a window the click itself deactivated hides");
    check(application_window_tray_click(true, 72'000) == DesktopTrayClick::reveal,
          "a shown window behind other windows comes to the front");

    window_show(window, ShowMode::hide);
    application_window_observe_position(window);
    check(application_window_tray_click(true, 70'120) == DesktopTrayClick::reveal, "a hidden window shows");
    window_test::close();

    // The headless window has no minimized state.
    check(desktop_tray_click(true, true, true, true, 0) == DesktopTrayClick::reveal, "a minimized window is restored");
    test_exit(failures, "tray click");
}
