#include "application_shell_input.hpp"
#include "application_account_presentation.hpp"
#include "application_state.hpp"
#include "maicord/ui/splitter.hpp"

#include <Windows.h>

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

bool handle_pane_resize_drag(WPARAM buttons, LPARAM lparam) noexcept {
    if (g_app.pane_resize == PaneResizeKind::none) {
        return false;
    }
    if ((buttons & MK_LBUTTON) == 0) {
        end_pane_resize();
        return true;
    }
    const PaneResizeKind kind = g_app.pane_resize;
    const float x = client_dip_x(lparam);
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
    if (GetCapture() == g_app.window)
        ReleaseCapture();
}

bool cancel_pane_resize() noexcept {
    const PaneResizeKind kind = g_app.pane_resize;
    if (kind == PaneResizeKind::none)
        return false;
    set_pane_width(kind, g_app.pane_resize_origin_width);
    g_app.pane_resize = PaneResizeKind::none;
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    return true;
}

bool apply_pending_pane_layout() noexcept {
    if (!g_app.pane_layout_pending)
        return true;
    g_app.pane_layout_pending = false;
    if (synchronize_shell_layout() && refresh_account_text(true))
        return true;
    g_app.pane_layout_pending = true;
    return false;
}
} // namespace maicord::app_detail
