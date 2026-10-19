#include "application_toast.hpp"
#include "application_account_presentation.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/layer_presentation.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
// Text owner for the toast: one root over the window.
struct ToastHost {
    UiWidgetManager widgets;
    UiWidget root;
    UiLayoutNode root_layout;
    UiRect viewport;
    bool ready, failed;
};
ToastHost g_host{};

bool animate() noexcept {
    return !g_app.renderer.live_resize && g_app.window && IsWindowVisible(g_app.window) &&
           !IsIconic(g_app.window);
}
bool reduced_motion() noexcept {
    return g_app.preferences.reduced_motion || g_app.renderer.reduced_motion;
}
bool bind(UiRect viewport) noexcept {
    if (!g_host.ready) {
        if (g_host.failed)
            return false;
        UiWidgetReference owner{};
        g_host.ready =
            ui_widget_manager_initialize_fill_root(
                &g_host.widgets, {&g_host.root, 1}, {&g_host.root_layout, 1}, utf8_literal("Toast"),
                ui_layout_node_visible | ui_layout_node_allow_overlap | ui_layout_node_clip_children,
                &owner) &&
            ui_toast_bind_text_owner(&g_app.toasts, owner);
        g_host.failed = !g_host.ready;
        if (!g_host.ready)
            return false;
    }
    if (!ui_rect_equal(viewport, g_host.viewport)) {
        if (!ui_widget_manager_layout_rectangle(&g_host.widgets, viewport).ok())
            return false;
        g_host.viewport = viewport;
    }
    return true;
}
} // namespace

bool application_toast_synchronize(u64 now_ms) noexcept {
    auto& toasts = g_app.toasts;
    // Nothing queued or on screen: no work, no wake.
    if (!toasts.count && !ui_rect_has_area(toasts.rectangle))
        return true;
    const UiRect viewport = g_app.renderer_ready ? application_viewport_dips() : UiRect{};
    // A minimized window keeps the queue's time; nothing is placed until it returns.
    if (ui_rect_has_area(viewport) && !bind(viewport)) {
        ui_toast_clear(&toasts);
        return false;
    }
    const u32 revision = toasts.revision;
    const UiToastEnvironment environment{&application_theme(),
                                         g_app.account_ui ? &g_app.account_ui->text_measure_cache : nullptr,
                                         viewport, animate(), reduced_motion()};
    const bool changed = ui_toast_update(&toasts, now_ms, environment);
    g_app.renderer.toast_scene = &toasts;
    renderer_set_toast_opacity(&g_app.renderer, toasts.progress);
    if (toasts.revision != revision)
        application_repaint();
    else if (changed)
        application_request_frame();
    return true;
}

u64 application_toast_next_wake() noexcept {
    const auto& toasts = g_app.toasts;
    if (!toasts.count && !ui_rect_has_area(toasts.rectangle))
        return 0;
    return ui_toast_next_wake(toasts, clock_monotonic_milliseconds(), animate());
}

void application_toast_release() noexcept {
    if (g_app.renderer.toast_scene == &g_app.toasts)
        g_app.renderer.toast_scene = nullptr;
    ui_toast_clear(&g_app.toasts);
    g_app.toasts.text_owner = {};
    g_host = {};
}

} // namespace maicord::app_detail
