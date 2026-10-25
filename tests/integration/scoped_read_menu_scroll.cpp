// The server header dropdown's wheel path in the scoped-read probe window:
// the production wheel router (handle_scroll_wheel) and frame tick
// (tick_smooth_scrolls) drive the real popup scene opened by the real
// header toggle. Smooth scrolling must glide over several frames; with it
// off the same input must jump.
#include "application_guild_menu.hpp"
#include "application_account_presentation.hpp"
#include "application_message_interaction.hpp"
#include "application_scrolling.hpp"
#include "application_state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/theme.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/model/permission_bits.hpp"
#include "../../src/app/account_ui_runtime.hpp"

namespace maicord::scoped_read_native {
using namespace app_detail;
bool resize(u32 width, u32 height, u32 dpi, const Theme& theme) noexcept;
bool click_header() noexcept;
bool fail_at(u32 line, u32 stage) noexcept;
#define MENU_SCROLL_FAIL() fail_at(__LINE__, 421)

namespace {
u64 now_us() noexcept {
    return clock_microseconds(g_app.clock, g_app.process_start, clock_now());
}
// One wheel message as Windows posts it: delta in the high word and the
// cursor in screen pixels.
bool wheel(i16 delta, UiPoint dips) noexcept {
    const float scale = g_app.renderer.dpi / 96.f;
    POINT screen{static_cast<LONG>(dips.x * scale), static_cast<LONG>(dips.y * scale)};
    if (!ClientToScreen(g_app.window, &screen))
        return false;
    const WPARAM wparam = static_cast<WPARAM>(static_cast<u32>(static_cast<u16>(delta)) << 16);
    const LPARAM lparam = static_cast<LPARAM>(static_cast<u32>(static_cast<u16>(screen.x)) |
                                              (static_cast<u32>(static_cast<u16>(screen.y)) << 16));
    return handle_scroll_wheel(wparam, lparam);
}
// Samples 60 Hz frames after `start`; counts frames strictly between the
// starting and settled offsets.
struct Glide {
    u32 intermediate, frames;
    float settled;
};
Glide glide(u64 start, float from) noexcept {
    Glide g{};
    u64 t = start;
    while (g.frames < 40 && ui_scroll_host_active(g_app.scroll_host)) {
        t += 16'667;
        if (!tick_smooth_scrolls(t))
            return {};
        ++g.frames;
        const float offset = g_app.popup_scene.scroll_offset;
        // Moved, but the curve is still running: a visible in-between frame.
        g.intermediate += offset != from && ui_scroll_host_active(g_app.scroll_host);
    }
    g.settled = g_app.popup_scene.scroll_offset;
    return g;
}
} // namespace

bool menu_scroll() noexcept {
    auto& popup = g_app.popup_scene;
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    g_app.scroll_host = {};
    // An administrator (with developer mode) gets every header row; a short
    // window makes them overflow.
    auto& state = g_app.account_ui->chat_state;
    constexpr u64 kGuild = 94001, kAdminRole = 94900;
    g_app.preferences.developer_mode = true;
    if (!chat_state_store_upsert_role(&state, {kAdminRole, kGuild, kPermissionAdministrator, utf8_literal("admin"), 1}))
        return MENU_SCROLL_FAIL();
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    if (!resize(960, 390, 144, kLightTheme) || !click_header())
        return MENU_SCROLL_FAIL();
    // (Overflowing, it is clamped into the window rather than kept below the header.)
    if (!application_guild_menu_header_open() ||
        popup.kind != PopupKind::context_menu || g_app.popup_owner_kind != PopupOwnerKind::guild ||
        !(popup.scroll_max > 64))
        return fail_at(__LINE__, 50000 + static_cast<u32>(popup.scroll_max));
    const UiPoint over{(popup.rectangle.left + popup.rectangle.right) * .5f,
                       (popup.rectangle.top + popup.rectangle.bottom) * .5f};
    // A mouse notch: queued, then several partial frames to its target.
    u64 start = now_us();
    if (!wheel(-120, over) || popup.scroll_offset != 0 || !ui_scroll_host_active(g_app.scroll_host))
        return MENU_SCROLL_FAIL();
    Glide notch = glide(start, 0);
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float row = kPopupRowHeight * static_cast<float>(lines);
    const float notch_target = minimum(row, popup.scroll_max);
    log_u64(LogLevel::info, "menu_scroll", "notch_intermediate_frames", notch.intermediate);
    if (notch.intermediate < 3 || notch.settled < notch_target - .01f || notch.settled > notch_target + .01f)
        return fail_at(__LINE__, 60000 + notch.intermediate * 1000 + static_cast<u32>(notch.settled));
    // Hover follows the animated layout: the row under the cursor is hit.
    const PopupHit hit = popup_scene_hit_test(popup, over.x, over.y);
    PopupItemLayout layout{};
    if (hit.source_index < popup.item_count &&
        (!popup_scene_resolve_item_layout(popup, hit.source_index, &layout) ||
         !ui_rect_contains(layout.row->rectangle, over.x, over.y)))
        return MENU_SCROLL_FAIL();
    // A precision touchpad delta (a quarter notch) also glides.
    const float before = popup.scroll_offset;
    start = now_us();
    if (!wheel(30, over) || popup.scroll_offset != before || !ui_scroll_host_active(g_app.scroll_host))
        return MENU_SCROLL_FAIL();
    const Glide fine = glide(start, before);
    log_u64(LogLevel::info, "menu_scroll", "precision_intermediate_frames", fine.intermediate);
    if (fine.intermediate < 2 || fine.settled < before - row * .25f - .01f || fine.settled > before - row * .25f + .01f)
        return fail_at(__LINE__, 60000 + fine.intermediate * 1000 + static_cast<u32>(fine.settled));
    // Smooth scrolling off: the same notch jumps synchronously.
    g_app.scroll_host.immediate = true;
    const float jump_from = popup.scroll_offset;
    const bool jumped = wheel(-120, over) && !ui_scroll_host_active(g_app.scroll_host) &&
                        popup.scroll_offset == minimum(jump_from + row, popup.scroll_max);
    g_app.scroll_host = {};
    dismiss_popup();
    g_app.preferences.developer_mode = false;
    application_guild_menu_synchronize();
    return jumped || MENU_SCROLL_FAIL();
}
} // namespace maicord::scoped_read_native

// Surfaces this window does not host: inactive, exactly as the client sees
// them while only the shell and a menu are open.
namespace maicord::app_detail {
bool application_commands_scroll(bool, double, bool* changed) noexcept {
    *changed = false;
    return false;
}
bool application_commands_scroll_range(bool, UiScrollRange*) noexcept { return false; }
bool application_inbox_active() noexcept { return false; }
bool application_inbox_scroll_range(UiScrollRange*) noexcept { return false; }
bool application_inbox_scroll_to(double) noexcept { return false; }
bool application_pinned_messages_scroll_range(UiScrollRange*) noexcept { return false; }
bool application_pinned_messages_scroll_to(double) noexcept { return false; }
bool application_settings_active() noexcept { return false; }
bool application_settings_synchronize() noexcept { return true; }
bool application_theme_editor_synchronize() noexcept { return true; }
bool application_thread_browser_active() noexcept { return false; }
bool application_thread_browser_refresh_after_scroll() noexcept { return true; }
bool application_thread_browser_scroll_range(UiScrollRange*) noexcept { return false; }
bool application_thread_browser_scroll_to(double) noexcept { return false; }
UiRect application_thread_browser_viewport() noexcept { return {}; }
bool pump_message_history() noexcept { return true; }
bool refresh_account_scroll(bool) noexcept { return refresh_account_text(true); }
bool refresh_reaction_viewer_presentation() noexcept { return true; }
bool scroll_message_view_by(i64, bool) noexcept { return false; }
} // namespace maicord::app_detail
