#include "shell/state.hpp"
#include "shell/window.hpp"
#include "shell/window_state.hpp"
#include "../../support/window_fixture.hpp"
#include "../../support/test_output.hpp"

// The window's minimum size follows the layout: the sign-in form has a small one, the signed-in layout (rail, channel
// list, members, messages) a larger one, and the shell tells the window system again when it changes. Wayland keeps
// only what xdg_toplevel.set_min_size last said, so the shell calls application_window_refresh_minimum_size every
// turn; the headless window records each window_set_min_size.

namespace maicord::app_detail {
// The caption bar lives in the window shell's own sources, which this test does not link: its rows are a fixed height.
void application_caption_enable() noexcept {}
void application_caption_synchronize() noexcept {}
void application_caption_set_hidden(bool) noexcept {}
u32 application_caption_pixels(u32 dpi) noexcept {
    return 32 * dpi / 96;
}
bool application_platform_window_create(Window*, const char**) noexcept {
    return false;
}
void application_platform_desktop_fonts() noexcept {}
} // namespace maicord::app_detail

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_window_minimum_entry) {
    using namespace maicord;
    using namespace maicord::app_detail;
    // The headless window reports 96 DPI: one pixel per DIP. Every height is 480 DIPs of content and the 32-pixel
    // caption.
    g_app.window = window_test::open();

    // Signed out: the sign-in form's minimum.
    g_app.account_ui = nullptr;
    g_app.login.visible = true;
    application_window_refresh_minimum_size();
    const NullWindowProbe login = null_window_probe();
    check(login.min_size_calls == 1 && login.min_width == 560 && login.min_height == 512,
          "the sign-in form sets its own minimum size");

    // Signing in switches to the main layout: the 64-DIP rail, 240-DIP channel list and member list and the 320-DIP
    // message minimum.
    alignas(8) static char account_ui_stand_in[8];
    g_app.account_ui = reinterpret_cast<decltype(g_app.account_ui)>(account_ui_stand_in);
    g_app.login.visible = false;
    g_app.channel_pane_width = g_app.member_pane_width = 240;
    g_app.member_pane_visible = true;
    application_window_refresh_minimum_size();
    const NullWindowProbe main_layout = null_window_probe();
    check(main_layout.min_size_calls == 2 && main_layout.min_width == 864 && main_layout.min_height == 512,
          "switching from sign-in to the main layout sends the larger minimum size");

    // Hiding the member list lowers it.
    g_app.member_pane_visible = false;
    application_window_refresh_minimum_size();
    const NullWindowProbe without_members = null_window_probe();
    check(without_members.min_width == 624, "hiding the member list lowers the minimum size");

    // Back to sign-in (a log out) restores the form's.
    g_app.login.visible = true;
    application_window_refresh_minimum_size();
    check(null_window_probe().min_width == login.min_width, "returning to sign-in restores its minimum size");

    // No window, nothing to tell.
    const u32 calls = null_window_probe().min_size_calls;
    g_app.window = {};
    application_window_refresh_minimum_size();
    check(null_window_probe().min_size_calls == calls, "without a window nothing is sent");
    g_app.account_ui = nullptr;
    window_test::close();
    test_exit(g_failures, "linux_window_minimum");
}
