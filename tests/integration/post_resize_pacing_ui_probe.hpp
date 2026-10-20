#pragma once

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "../../tools/perf/probe_window.hpp"
#include "maicord/core/win32_error.hpp"

extern "C" __declspec(dllimport) long __stdcall DwmFlush();

namespace maicord {
namespace post_resize_probe_detail {
// While the display sleeps, DWM composes only every ~250 ms, so Present(1)
// and the frame-latency waitable miss this probe's 200 ms waits. Awake, each
// DwmFlush takes at most one refresh interval (~42 ms even at 24 Hz).
inline bool compositor_asleep() noexcept {
    const Clock clock = clock_initialize();
    if (clock.frequency <= 0 || DwmFlush() < 0)
        return false;
    const auto start = clock_now();
    return DwmFlush() >= 0 && DwmFlush() >= 0 && clock_microseconds(clock, start, clock_now()) > 200'000;
}
struct Session {
    D3D11Renderer* renderer;
    bool (*shell)() noexcept;
    bool (*model)() noexcept;
    u32 enters, exits, sizes;
    bool failed;
};
inline Session* active{};
// This is an offline HWND adapter, not Main's account/input window procedure.
inline bool window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* session = active;
    if (!session)
        return false;
    auto& r = *session->renderer;
    switch (message) {
    case WM_ENTERSIZEMOVE:
        ++session->enters;
        renderer_set_live_resize(&r, true);
        return true;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && LOWORD(lparam) && HIWORD(lparam)) {
            ++session->sizes;
            session->failed |= !renderer_resize(&r, LOWORD(lparam), HIWORD(lparam)) || !session->shell() ||
                               !session->model() || !renderer_present(&r, kDefaultTheme);
        }
        return true;
    case WM_EXITSIZEMOVE: {
        ++session->exits;
        RECT client{};
        session->failed |= !GetClientRect(window, &client) || client.right <= client.left ||
                           client.bottom <= client.top ||
                           !renderer_finish_resize(&r, static_cast<u32>(client.right - client.left),
                                                   static_cast<u32>(client.bottom - client.top));
        // Main's preference application invalidates the old presentation here.
        r.retained_presentation.valid = false;
        return true;
    }
    case WM_PAINT:
        ValidateRect(window, nullptr);
        return true;
    }
    return false;
}
} // namespace post_resize_probe_detail

inline bool post_resize_pacing_ui_probe(D3D11Renderer& r, HWND window, MessageScene& scene,
                                        bool (*shell)() noexcept, bool (*model)() noexcept) noexcept {
    using namespace post_resize_probe_detail;
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "post_resize_pacing_failure_stage", stage);
        }
    } failure{stage};
    if (!window || !shell || !model || !r.graphics.frame_latency_waitable || active)
        return false;
    Session session{&r, shell, model};
    struct Restore {
        D3D11Renderer& renderer;
        ~Restore() noexcept {
            active = nullptr;
            renderer_set_live_resize(&renderer, false);
        }
    } restore{r};
    active = &session;
    renderer_set_live_resize(&r, true);
    probe_window::show(window);
    if (!shell() || !model() || !renderer_present(&r, kDefaultTheme))
        return false;
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    RECT initial{};
    if (!GetClientRect(window, &initial) || session.failed || r.live_resize)
        return false;
    const LONG initial_width = initial.right - initial.left, initial_height = initial.bottom - initial.top;
    // A live drag must not bank DXGI semaphore credits. Those credits let
    // subsequent Present(1) calls queue frames ahead of input indefinitely.
    // Force real paints even at a retained logical size, as modal WM_PAINT does.
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    for (u32 frame = 0; frame < 32; ++frame) {
        r.retained_presentation.valid = false;
        if (!renderer_present(&r, kDefaultTheme))
            return false;
    }
    if (d3d11_device_owner_wait(r.graphics, 200) != WAIT_OBJECT_0)
        return false;
    renderer_set_live_resize(&r, false);
    renderer_accept_begin_frame(&r);
    if (d3d11_device_owner_wait(r.graphics, 0) != WAIT_TIMEOUT) {
        log_write(LogLevel::error, "probe", "resize_accumulated_frame_latency_credits");
        return false;
    }
    // Moving without resizing, and replacing buffers after a slot was already
    // acquired by Main, must preserve that slot instead of waiting for it twice.
    renderer_set_live_resize(&r, true);
    if (!r.begin_frame_permit || !renderer_finish_resize(&r, initial_width, initial_height) ||
        !r.begin_frame_permit || !renderer_resize(&r, initial_width + 1, initial_height) ||
        !r.begin_frame_permit || !renderer_resize(&r, initial_width, initial_height) ||
        !r.begin_frame_permit)
        return false;
    r.retained_presentation.valid = false;
    if (!renderer_present(&r, kDefaultTheme) || r.begin_frame_permit)
        return false;
    const LONG shifts[6][2]{{-1, 0}, {1, 0}, {0, 0}, {0, -1}, {0, 1}, {0, 0}};
    const auto start = clock_monotonic_milliseconds();
    u64 maximum_wait_us = 0, maximum_present_us = 0;
    u32 normal_frames = 0;
    const Clock clock = clock_initialize();
    if (clock.frequency <= 0)
        return false;
    stage = 2;
    for (u32 pass = 0; pass < 6; ++pass) {
        RECT bounds{0, 0, initial_width + shifts[pass][0], initial_height + shifts[pass][1]};
        if (!AdjustWindowRectEx(&bounds, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                                static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE))))
            return false;
        SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
        if (!SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                          SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER))
            return false;
        SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
        if (session.failed || r.live_resize ||
            r.width != static_cast<u32>(initial_width + shifts[pass][0]) ||
            r.height != static_cast<u32>(initial_height + shifts[pass][1]) ||
            r.graphics.buffer_width != r.width || r.graphics.buffer_height != r.height ||
            r.begin_frame_permit || r.previous_buffer_damage_valid)
            return false;
        u32 row_index = ~0u;
        for (u32 i = 0; i < scene.count; ++i) {
            const auto* node = message_scene_layout_node(scene, scene.rows[i].widget);
            if (node && ui_rect_has_area(ui_rect_intersection(node->rectangle, node->clip_rectangle))) {
                row_index = i;
                break;
            }
        }
        if (row_index == ~0u)
            return false;
        const auto text_builds = r.content_text->rebuilds, glyph_builds = r.glyph_text.cache_builds;
        const auto text_uploads = r.text_instance_uploads;
        for (u32 frame = 0; frame < 16; ++frame) {
            if (clock_monotonic_milliseconds() - start > 30'000)
                return false;
            const auto wait_start = clock_now();
            const DWORD waited = d3d11_device_owner_wait(r.graphics, 200);
            const auto wait_us = clock_microseconds(clock, wait_start, clock_now());
            maximum_wait_us = maximum(maximum_wait_us, wait_us);
            if (waited != WAIT_OBJECT_0) {
                log_u64(LogLevel::error, "probe", "post_resize_wait_result", waited);
                log_u64(LogLevel::error, "probe", "post_resize_wait_pass", pass);
                log_u64(LogLevel::error, "probe", "post_resize_wait_frame", frame);
                return false;
            }
            renderer_accept_begin_frame(&r);
            const auto desired = frame & 1 ? scene.rows[row_index].widget : ui_widget_invalid_handle();
            ui_widget_manager_hover(&scene.widgets, desired);
            if (scene.widgets.hovered.slot != desired.slot ||
                scene.widgets.hovered.generation != desired.generation ||
                !renderer_rebuild_content_text(&r, kDefaultTheme))
                return false;
            const auto present_start = clock_now();
            if (!renderer_present(&r, kDefaultTheme) || r.begin_frame_permit || r.live_resize)
                return false;
            maximum_present_us =
                maximum(maximum_present_us, clock_microseconds(clock, present_start, clock_now()));
            ++normal_frames;
            if (r.content_text->rebuilds != text_builds || r.glyph_text.cache_builds != glyph_builds ||
                r.text_instance_uploads != text_uploads)
                return false;
        }
    }
    stage = 3;
    if (session.failed || session.enters != 7 || session.exits != 7 || session.sizes < 6 ||
        normal_frames != 96)
        return false;
    log_u64(LogLevel::info, "probe", "post_resize_normal_paced_frames", normal_frames);
    log_u64(LogLevel::info, "probe", "post_resize_maximum_native_wait_us", maximum_wait_us);
    log_u64(LogLevel::info, "probe", "post_resize_maximum_native_present_us", maximum_present_us);
    log_write(LogLevel::info, "probe", "post_resize_native_clock_and_plain_hover_retention_passed");
    stage = 0;
    return true;
}
} // namespace maicord
