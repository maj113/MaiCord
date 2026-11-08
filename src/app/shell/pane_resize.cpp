#include "input.hpp"
#include "../account/account_presentation.hpp"
#include "../account/login/ui.hpp"
#include "../account/account_text_retry.hpp"
#include "overlay_input.hpp"
#include "state.hpp"
#include "maicord/ui/layout/splitter.hpp"


namespace maicord::app_detail {
namespace {
float* pane_width(PaneResizeKind kind) noexcept {
    return kind == PaneResizeKind::channels ? &g_app.channel_pane_width : &g_app.member_pane_width;
}
// Pointer events can arrive much faster than display slots. Keep the newest
// requested width; the presentation path performs one reflow.
void set_pane_width(PaneResizeKind kind, float width) noexcept {
    float* target = pane_width(kind);
    if (width == *target)
        return;
    *target = width;
    g_app.pane_layout_pending = true;
    application_request_frame();
}
UiSplitterLimits pane_resize_limits(PaneResizeKind kind) noexcept {
    const float other = kind == PaneResizeKind::channels
                            ? (g_app.member_pane_visible ? g_app.member_pane_width : 0.0f)
                            : g_app.channel_pane_width;
    // The chat column (or the thread browser covering it) keeps its minimum.
    return ui_splitter_limits(ui_rect_width(application_viewport_dips()) -
                              kGuildChannelShellLayoutMetrics.guild_rail_width - other -
                              kMinimumAccountMessageWidthDips);
}
} // namespace

bool handle_pane_resize_drag(const WindowEvent& event) noexcept {
    if (g_app.pane_resize == PaneResizeKind::none) {
        return false;
    }
    if (!input_held(event)) {
        end_pane_resize();
        return true;
    }
    const PaneResizeKind kind = g_app.pane_resize;
    const float x = client_dip_x(event);
    const UiSplitterLimits limits = pane_resize_limits(kind);
    set_pane_width(kind, kind == PaneResizeKind::channels
                             ? ui_splitter_drag_width(kGuildChannelShellLayoutMetrics.guild_rail_width,
                                                      UiSplitterEdge::right, x, limits)
                             : ui_splitter_drag_width(application_viewport_dips().right, UiSplitterEdge::left, x,
                                                      limits));
    return true;
}

void end_pane_resize() noexcept {
    const PaneResizeKind kind = g_app.pane_resize;
    g_app.pane_resize = PaneResizeKind::none;
    if (kind == PaneResizeKind::none)
        return;
    // Persist once per completed drag, never per pointer sample.
    if (*pane_width(kind) != g_app.pane_resize_origin_width)
        g_app.pane_widths_persist_pending = true;
    application_release_capture();
}

bool cancel_pane_resize() noexcept {
    const PaneResizeKind kind = g_app.pane_resize;
    if (kind == PaneResizeKind::none)
        return false;
    set_pane_width(kind, g_app.pane_resize_origin_width);
    g_app.pane_resize = PaneResizeKind::none;
    application_release_capture();
    return true;
}

bool apply_pending_pane_layout() noexcept {
    if (!g_app.pane_layout_pending)
        return true;
    g_app.pane_layout_pending = false;
    // The shell layout measures the composer, which takes the edit geometry the sign-in field shares; while the
    // form shows, measure its field again so the caret and pointer hit tests keep a layout (as a resize does).
    if (synchronize_shell_layout() && refresh_account_text_for_layout() &&
        (!g_app.login.visible || application_synchronize_login_view()))
        return true;
    g_app.pane_layout_pending = true;
    return false;
}
} // namespace maicord::app_detail
