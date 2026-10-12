#pragma once
#include "../src/app/application_window_resize.hpp"

namespace maicord::settings_runtime_test {
inline bool resize_messages_active{};
inline LRESULT CALLBACK resize_window_proc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam) noexcept {
    using namespace app_detail;
    if (resize_messages_active) {
        if (message == WM_ENTERSIZEMOVE) {
            application_begin_window_resize();
            return 0;
        }
        if (message == WM_EXITSIZEMOVE) {
            application_end_window_resize(window);
            return 0;
        }
        if (message == WM_SIZE && wparam != SIZE_MINIMIZED) {
            application_present_resize_projection(true);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
inline void test_window_resize_lifecycle() noexcept {
    using namespace app_detail;
    const auto old_window = g_app.window;
    const auto old_preferences = g_app.preferences;
    auto* old_settings = g_app.settings;
    const auto old_scroll = g_app.scroll_host;
    const auto old_animations = g_app.animations;
    const auto old_retained = g_app.renderer.retained_presentation;
    const auto old_text = g_app.renderer.content_text_retention;
    const auto old_clock = g_app.clock;
    const auto old_start = g_app.process_start;
    const auto old_watchdog = g_app.frame_clock_watchdog_at_us;
    const bool old_fallback = g_app.frame_clock_timer_fallback;
    const u32 old_width = g_app.renderer.width, old_height = g_app.renderer.height;
    const bool old_ready = g_app.renderer_ready, old_loop = g_app.frame_loop_active,
               old_pending = g_app.frame_pending, old_live = g_app.renderer.live_resize,
               old_permit = g_app.renderer.begin_frame_permit, old_reduced = g_app.renderer.reduced_motion,
               old_members = g_app.member_pane_visible;
    constexpr wchar_t name[] = L"MaiCordOwnedResizeLifecycleTest";
    WNDCLASSW type{};
    type.lpfnWndProc = resize_window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = name;
    if (!RegisterClassW(&type)) {
        check(false, "resize lifecycle: owned window class initializes");
        return;
    }
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, name, name, WS_POPUP, -32000, -32000,
                                  320, 180, nullptr, nullptr, type.hInstance, nullptr);
    check(window != nullptr, "resize lifecycle: real offscreen nonactivating HWND initializes");
    if (!window) {
        UnregisterClassW(name, type.hInstance);
        return;
    }
    g_app.window = window;
    g_app.settings = nullptr;
    g_app.scroll_host = {};
    g_app.animations = {};
    AnimationTimelineEntry entries[1]{};
    u32 buckets[2]{};
    check(animation_timeline_initialize(&g_app.animations.timeline, {entries, 1}, {buckets, 2}),
          "resize lifecycle: independent real animation timeline initializes");
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    g_app.renderer_ready = g_app.frame_loop_active = true;
    g_app.renderer.live_resize = g_app.frame_pending = false;
    g_app.renderer.begin_frame_permit = true;
    g_app.preferences.reduced_motion = false;
    g_app.preferences.smooth_scroll = true;
    resize_messages_active = true;
    const u32 presents = observed.resize_sync_presents, finishes = observed.resize_finishes;
    SetWindowPos(window, nullptr, 0, 0, 321, 180, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    check(g_app.frame_pending && observed.resize_sync_presents == presents,
          "resize lifecycle: actual normal WM_SIZE admits a frame without entering synchronous present");
    g_app.frame_pending = false;
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    check(g_app.renderer.live_resize && g_app.renderer.begin_frame_permit &&
              g_app.animations.timeline.suspended,
          "resize lifecycle: actual enter suspends animation and preserves an acquired display permit");
    SetWindowPos(window, nullptr, 0, 0, 322, 181, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    check(observed.resize_sync_presents == presents + 1 && !g_app.frame_pending,
          "resize lifecycle: actual modal WM_SIZE still paints synchronously");
    g_app.frame_clock_watchdog_at_us = 123;
    g_app.frame_clock_timer_fallback = true;
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    check(!g_app.renderer.live_resize && !g_app.animations.timeline.suspended &&
              !g_app.scroll_host.immediate && g_app.renderer.width == 322 && g_app.renderer.height == 181 &&
              observed.resize_finishes == finishes + 1,
          "resize lifecycle: actual exit reads committed client bounds and restores normal "
          "scroll/animation policy");
    check(g_app.frame_pending && !g_app.frame_clock_watchdog_at_us && !g_app.frame_clock_timer_fallback,
          "resize lifecycle: exit requests a fresh paced image and retires the modal clock watchdog");
    g_app.frame_pending = false;
    check(application_present_resize_projection(true) && g_app.frame_pending &&
              observed.resize_sync_presents == presents + 1,
          "resize lifecycle: post-exit resize projection returns to the outer paced frame path");
    g_app.frame_pending = false;
    check(!application_present_resize_projection(false) && !g_app.frame_pending &&
              observed.resize_sync_presents == presents + 1,
          "resize lifecycle: failed preparation cannot present or publish an incomplete frame");
    const u32 finish_failures_before = observed.resize_finish_failures;
    observed.resize_finish_failed = true;
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    check(!g_app.renderer.live_resize && !g_app.animations.timeline.suspended &&
              observed.resize_finish_failures == finish_failures_before + 1,
          "resize lifecycle: rejected renderer finish cannot leave the host in modal resize");
    observed.resize_finish_failed = false;
    g_app.preferences.reduced_motion = true;
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    check(
        !g_app.renderer.live_resize && g_app.animations.timeline.suspended && g_app.scroll_host.immediate,
        "resize lifecycle: exit preserves the user's reduced motion rather than unconditionally resuming");
    resize_messages_active = false;
    DestroyWindow(window);
    UnregisterClassW(name, type.hInstance);
    g_app.window = old_window;
    g_app.preferences = old_preferences;
    g_app.settings = old_settings;
    g_app.scroll_host = old_scroll;
    g_app.animations = old_animations;
    g_app.renderer.retained_presentation = old_retained;
    g_app.renderer.content_text_retention = old_text;
    g_app.renderer.width = old_width;
    g_app.renderer.height = old_height;
    g_app.renderer_ready = old_ready;
    g_app.frame_loop_active = old_loop;
    g_app.frame_pending = old_pending;
    g_app.renderer.live_resize = old_live;
    g_app.renderer.begin_frame_permit = old_permit;
    g_app.renderer.reduced_motion = old_reduced;
    g_app.member_pane_visible = old_members;
    g_app.clock = old_clock;
    g_app.process_start = old_start;
    g_app.frame_clock_watchdog_at_us = old_watchdog;
    g_app.frame_clock_timer_fallback = old_fallback;
}
} // namespace maicord::settings_runtime_test
