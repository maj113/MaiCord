#include "../src/app/application_state.hpp"
#include "../src/app/application_shell_input.hpp"
#include "../src/app/application_account_presentation.hpp"
#include "../src/app/application_input_modality.hpp"
#include "maicord/ui/splitter.hpp"
#include "maicord/core/module.hpp"
namespace maicord::app_detail {
namespace { u32 layouts, text_updates; bool fail_layout; }
bool synchronize_shell_layout() noexcept { ++layouts; return !fail_layout; }
bool refresh_account_text(bool) noexcept { ++text_updates; return true; }
float client_dip_x(LPARAM point) noexcept { return static_cast<float>(static_cast<short>(LOWORD(point))) * 96.0f / g_app.renderer.dpi; }
}
namespace maicord {
void test_pane_resize(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    const HWND saved_window = g_app.window;
    const bool saved_live_resize = g_app.renderer.live_resize;
    const bool saved_pending = g_app.frame_pending;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                                 L"Frame scheduling test", WS_POPUP | WS_VISIBLE,
                                 -32000, -32000, 64, 64, nullptr, nullptr, module_instance(), nullptr);
    check(window != nullptr, "create offscreen frame scheduling window");
    if (window) {
        g_app.window = window;
        g_app.renderer.live_resize = false;
        g_app.frame_pending = false;
        ValidateRect(window, nullptr);
        for (u32 i = 0; i < 1000; ++i) application_request_frame();
        check(g_app.frame_pending && !GetUpdateRect(window, nullptr, FALSE),
              "UI request burst schedules a GPU frame without a Win32 update region");
        g_app.frame_pending = false;
        g_app.renderer.live_resize = true;
        check(application_request_frame() && g_app.frame_pending && GetUpdateRect(window, nullptr, FALSE),
              "modal sizing preserves its native paint wakeup");
        ValidateRect(window, nullptr);
        DestroyWindow(window);
    }
    g_app.window = saved_window;
    g_app.renderer.live_resize = saved_live_resize;
    g_app.frame_pending = saved_pending;
    g_app.renderer.dpi = 96; g_app.channel_pane_width = 180; g_app.member_pane_width = 180;
    g_app.member_pane_visible = true;
    // The account fixture viewport is 800 x 600. Large bursts still have one
    // final width and one convergence pass at presentation, including release.
    constexpr PaneResizeKind panes[]{PaneResizeKind::channels, PaneResizeKind::members};
    for (auto pane : panes) {
        g_app.pane_resize = pane; layouts = text_updates = 0;
        for (u32 i = 0; i < 1000; ++i) {
            const u32 x = pane == PaneResizeKind::channels ? 260 + i % 70 : 520 + i % 70;
            check(handle_pane_resize_drag(MK_LBUTTON, MAKELPARAM(x, 300)), "pane drag accepts pointer sample");
        }
        const float channel = g_app.channel_pane_width, member = g_app.member_pane_width;
        check(!layouts && !text_updates && g_app.pane_layout_pending && g_app.frame_pending, "pointer burst schedules presentation without synchronous layout");
        end_pane_resize();
        check(apply_pending_pane_layout() && layouts == 1 && text_updates == 1 && !g_app.pane_layout_pending &&
            g_app.channel_pane_width == channel && g_app.member_pane_width == member,
            "release retains final width and converges exactly once");
        check(apply_pending_pane_layout() && layouts == 1 && text_updates == 1, "unchanged presentation skips pane layout");
    }
    // Shared splitter primitive: grip slop, clamping and pointer mapping.
    constexpr UiRect sidebar{72, 48, 312, 600};
    check(ui_splitter_hit(sidebar, UiSplitterEdge::right, 308.f, 300.f) &&
              ui_splitter_hit(sidebar, UiSplitterEdge::right, 315.9f, 300.f) &&
              !ui_splitter_hit(sidebar, UiSplitterEdge::right, 316.f, 300.f) &&
              !ui_splitter_hit(sidebar, UiSplitterEdge::right, 307.9f, 300.f) &&
              !ui_splitter_hit(sidebar, UiSplitterEdge::right, 312.f, 47.f) &&
              !ui_splitter_hit(sidebar, UiSplitterEdge::right, 312.f, 600.f) &&
              ui_splitter_hit(sidebar, UiSplitterEdge::left, 72.f, 300.f) &&
              !ui_splitter_hit({10, 0, 10, 100}, UiSplitterEdge::left, 10.f, 50.f),
          "splitter grip straddles exactly its edge within the pane's rows");
    volatile float zero = 0.f;
    const float not_a_number = zero / zero;
    constexpr auto wide = ui_splitter_limits(2000.f);
    constexpr auto cramped = ui_splitter_limits(100.f);
    check(wide.minimum == kUiSplitterMinimumPaneDips && wide.maximum == kUiSplitterMaximumPaneDips &&
              cramped.minimum == cramped.maximum && cramped.maximum == kUiSplitterMinimumPaneDips &&
              ui_splitter_limits(300.f).maximum == 300.f &&
              ui_splitter_clamp(not_a_number, wide) == wide.minimum &&
              ui_splitter_drag_width(72.f, UiSplitterEdge::right, 9000.f, wide) == 420.f &&
              ui_splitter_drag_width(72.f, UiSplitterEdge::right, -9000.f, wide) == 180.f &&
              ui_splitter_drag_width(72.f, UiSplitterEdge::right, 372.f, wide) == 300.f &&
              ui_splitter_drag_width(1280.f, UiSplitterEdge::left, 1000.f, wide) == 280.f,
          "splitter limits keep a minimum, shrink with space and map the pointer to a clamped width");
    check(ui_splitter_persisted_width(311.6f) == 312 && ui_splitter_persisted_width(0.f) == 0 &&
              ui_splitter_persisted_width(not_a_number) == 0 &&
              ui_splitter_restored_width(0, 240.f, wide) == 240.f &&
              ui_splitter_restored_width(312, 240.f, wide) == 312.f &&
              ui_splitter_restored_width(1000, 240.f, wide) == 420.f &&
              ui_splitter_restored_width(120, 240.f, wide) == 180.f,
          "splitter widths persist as whole DIPs and restore clamped, zero meaning the default");
    // Drag clamps to the live limits (800 DIP fixture keeps the chat column's
    // minimum), release marks one persistence, Escape restores the origin.
    const float rail = kGuildChannelShellLayoutMetrics.guild_rail_width;
    g_app.member_pane_width = 180;
    const float channel_maximum = ui_splitter_limits(ui_rect_width(application_viewport_dips()) - rail -
                                                     g_app.member_pane_width - kMinimumAccountMessageWidthDips)
                                      .maximum;
    g_app.channel_pane_width = 200; g_app.pane_resize_origin_width = 200; g_app.pane_widths_persist_pending = false;
    g_app.pane_resize = PaneResizeKind::channels;
    check(handle_pane_resize_drag(MK_LBUTTON, MAKELPARAM(4000, 300)) && g_app.channel_pane_width == channel_maximum &&
              handle_pane_resize_drag(MK_LBUTTON, MAKELPARAM(0, 300)) &&
              g_app.channel_pane_width == kUiSplitterMinimumPaneDips &&
              handle_pane_resize_drag(MK_LBUTTON, MAKELPARAM(static_cast<int>(rail) + 190, 300)) &&
              g_app.channel_pane_width == 190.f,
          "channel splitter (thread browser left edge) clamps between its minimum and the live maximum");
    check(cancel_pane_resize() && g_app.pane_resize == PaneResizeKind::none && g_app.channel_pane_width == 200.f &&
              !g_app.pane_widths_persist_pending && !cancel_pane_resize(),
          "Escape restores the drag's origin width without persisting");
    g_app.pane_resize = PaneResizeKind::members; g_app.pane_resize_origin_width = g_app.member_pane_width;
    end_pane_resize();
    check(!g_app.pane_widths_persist_pending, "unchanged release does not persist");
    g_app.pane_resize = PaneResizeKind::members;
    const float member_expected = ui_splitter_drag_width(
        application_viewport_dips().right, UiSplitterEdge::left, 610.f,
        ui_splitter_limits(ui_rect_width(application_viewport_dips()) - rail - g_app.channel_pane_width -
                           kMinimumAccountMessageWidthDips));
    check(member_expected != 180.f && handle_pane_resize_drag(MK_LBUTTON, MAKELPARAM(610, 300)) &&
              g_app.member_pane_width == member_expected &&
              handle_pane_resize_drag(0, MAKELPARAM(500, 300)) && g_app.pane_resize == PaneResizeKind::none &&
              g_app.pane_widths_persist_pending && g_app.member_pane_width == member_expected,
          "member splitter follows the pointer and a button-less move ends the drag with one pending save");
    g_app.pane_widths_persist_pending = false; g_app.pane_layout_pending = false; layouts = text_updates = 0;
    g_app.pane_layout_pending = true; fail_layout = true;
    check(!apply_pending_pane_layout() && g_app.pane_layout_pending, "failed pane layout remains pending");
    fail_layout = false;
    check(apply_pending_pane_layout() && !g_app.pane_layout_pending, "pane layout retries after failure");
    g_app.renderer.dpi = 0; g_app.channel_pane_width = g_app.member_pane_width = 0; g_app.member_pane_visible = false; g_app.frame_pending = false;
    bool visible = keyboard_focus_after_message(false, WM_KEYDOWN, VK_TAB, false);
    check(visible && keyboard_focus_after_message(visible, WM_MOUSEMOVE, 0, false), "keyboard focus survives pointer hover");
    visible = keyboard_focus_after_message(visible, WM_LBUTTONDOWN, 0, false);
    check(!visible && !keyboard_focus_after_message(visible, WM_CHAR, 'a', false), "click and ordinary typing show no focus outline");
    check(keyboard_focus_after_message(visible, WM_KEYDOWN, VK_DOWN, false) &&
        keyboard_focus_after_message(visible, WM_SYSKEYDOWN, VK_F10, true) &&
        !keyboard_focus_after_message(true, WM_RBUTTONDOWN, 0, false), "keyboard navigation restores focus and context click hides it");
}
}
