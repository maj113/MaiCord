#include "application_scrolling.hpp"
#include "maicord/core/memory.hpp"

#include "application_account_presentation.hpp"
#include "application_message_history.hpp"
#include "application_state.hpp"
#include "application_read_state.hpp"
#include "application_commands.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/scrollbar.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
bool observe_read_after_scroll() noexcept {
    if (!g_app.account_ui) return true;
    const u32 revision = g_app.account_ui->navigation_view.revision;
    if (!application_read_state_observe(clock_monotonic_milliseconds())) return false;
    if (revision != g_app.account_ui->navigation_view.revision) {
        if (!refresh_account_text()) return false;
        application_request_frame();
    }
    return true;
}
} // namespace

static void invalidate_if_scrolled(bool changed) noexcept {
    if (changed && g_app.window && refresh_account_text(true)) {
        application_request_frame();
    }
}

bool request_older_history_if_near_start() noexcept {
    if (!g_app.account_ui || !message_history_browsing_ready(g_app.message_catchup.state)) {
        return true;
    }
    const MessageTimeline& timeline = g_app.message_view.timeline;
    if (!message_history_older_prefetch_needed(timeline)) {
        return true;
    }
    const ChatStateStore& state = g_app.account_ui->chat_state;
    const ChannelRecord* channel =
        chat_state_store_find_channel(state, state.selected_channel_id);
    if (!channel || (channel->actions & channel_action_read_history) == 0) {
        return true;
    }
    if (!message_history_older_demand(&g_app.message_older, &timeline, channel->id,
                                      kMessageHistoryRequestLimit)) {
        log_write(LogLevel::error, "rest", "message_history_older_demand_failed");
        return false;
    }
    return pump_message_history();
}

bool scroll_message_view_by(i64 delta_units, bool repaint) noexcept {
    bool changed = false;
    if (!message_scene_scroll_by(&g_app.message_scene, delta_units, ui_extent_units(1.0f), &changed)) {
        return false;
    }
    if (repaint) invalidate_if_scrolled(changed);
    if (delta_units < 0 && g_app.message_view.timeline.newer_refetch_required)
        g_app.message_catchup.incremental = true;
    if (delta_units < 0 && !request_older_history_if_near_start()) {
        return false;
    }
    if (delta_units > 0 && message_history_newer_prefetch_needed(g_app.message_view.timeline)) {
        // The resident edge is not the live edge. Keep the visible anchor while
        // loading the next page instead of enabling live-tail following here.
        g_app.message_view.timeline.follow_bottom = false;
        if (!pump_message_history()) return false;
    }
    return observe_read_after_scroll();
}

bool scroll_message_view_to(bool bottom) noexcept {
    bool changed = false;
    const bool ok = bottom ? message_scene_scroll_to_bottom(&g_app.message_scene, &changed)
                           : message_scene_scroll_to_start(&g_app.message_scene, &changed);
    if (!ok) {
        return false;
    }
    invalidate_if_scrolled(changed);
    if (bottom) g_app.message_catchup.incremental = false;
    if (!bottom && !request_older_history_if_near_start()) {
        return false;
    }
    if (bottom && !pump_message_history()) return false;
    return observe_read_after_scroll();
}

bool application_message_jump_to_present() noexcept {
    const u64 channel_id = g_app.account_ui ? g_app.account_ui->active_channel_id : 0;
    // Jump to present, as in the official client: a detached window is replaced by the latest
    // page instead of paging forward through everything in between.
    if (channel_id && message_history_jump_to_present(&g_app.message_catchup, &g_app.message_older,
                                                      &g_app.message_pump, channel_id,
                                                      kMessageHistoryRequestLimit)) {
        reset_value<AttachmentRefresh>(&g_app.attachment_refresh);
        reset_value<ReplyReferenceRefresh>(&g_app.reply_refresh);
        g_app.message_scene.dirty = true;
        if (g_app.window && refresh_account_text(true))
            application_request_frame();
    }
    return scroll_message_view_to(true);
}

static UiScrollbarGeometry application_scrollbar_geometry(ScrollbarDragKind kind) noexcept {
    if (kind == ScrollbarDragKind::messages && g_app.message_scene.active &&
        g_app.message_view.timeline.list) {
        return ui_scrollbar_geometry(g_app.message_scene.viewport,
                                     ui_extent_dips(g_app.message_view.timeline.scroll_units),
                                     ui_extent_dips(g_app.message_view.timeline.list->total_units));
    }
    if (kind == ScrollbarDragKind::channels && g_app.shell_navigation_scene.active) {
        return ui_scrollbar_geometry(g_app.shell_navigation_scene.viewport,
                                     g_app.shell_navigation_scene.scroll_dips,
                                     g_app.shell_navigation_scene.content_height);
    }
    if (kind == ScrollbarDragKind::members && g_app.member_list_scene.active) {
        return ui_scrollbar_geometry(g_app.member_list_scene.viewport, g_app.member_list_scene.scroll_dips,
                                     g_app.member_list_scene.content_height);
    }
    // The scrollable menu's bar, as popup presentation paints it; the command
    // picker's bar spans its virtual body, which its owner scrolls.
    if (kind == ScrollbarDragKind::popup && g_app.popup_scene.active && g_app.popup_scene.picker_scroll_extent > 0)
        return ui_scrollbar_geometry(popup_scene_picker_body(g_app.popup_scene),
                                     g_app.popup_scene.picker_scroll_offset, g_app.popup_scene.picker_scroll_extent);
    if (kind == ScrollbarDragKind::popup && g_app.popup_scene.active && g_app.popup_scene.scroll_max > 0) {
        return ui_scrollbar_geometry(g_app.popup_scene.rectangle, g_app.popup_scene.scroll_offset,
                                     ui_rect_height(g_app.popup_scene.rectangle) + g_app.popup_scene.scroll_max);
    }
    return UiScrollbarGeometry{};
}

static bool apply_scrollbar_pointer(float pointer_y) noexcept {
    const ScrollbarDragKind kind = g_app.scrollbar_drag;
    const UiScrollbarGeometry geometry = application_scrollbar_geometry(kind);
    if (kind == ScrollbarDragKind::none || !geometry.visible) {
        return false;
    }
    const float target =
        ui_scrollbar_scroll_from_pointer(geometry, pointer_y, g_app.scrollbar_thumb_grab_offset);
    if (kind == ScrollbarDragKind::messages) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const u64 current = g_app.message_view.timeline.scroll_units;
        const u64 desired = minimum(static_cast<u64>(ui_extent_units(target)),
                                    g_app.message_view.timeline.list->total_units);
        if (desired == current) {
            return true;
        }
        const i64 delta =
            desired > current
                ? static_cast<i64>(minimum(desired - current, static_cast<u64>(~0ull >> 1u)))
                : -static_cast<i64>(minimum(current - desired, static_cast<u64>(~0ull >> 1u)));
        return scroll_message_view_by(delta);
    }
    bool changed = false;
    if (kind == ScrollbarDragKind::popup) {
        // The thumb tracks the pointer directly, like every other list.
        ui_scroll_host_cancel(&g_app.scroll_host);
        if (g_app.popup_scene.picker_scroll_extent > 0) {
            if (!application_commands_scroll(false, target, &changed))
                return false;
        } else if (!popup_scene_scroll(&g_app.popup_scene, target - g_app.popup_scene.scroll_offset, &changed))
            return false;
    } else if (kind == ScrollbarDragKind::channels) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        if (!g_app.account_ui) {
            return false;
        }
        const ShellNavigationSceneResult result =
            shell_navigation_scene_scroll(&g_app.shell_navigation_scene, &g_app.account_ui->navigation_view,
                                          target - g_app.shell_navigation_scene.scroll_dips);
        if (!result.ok()) {
            return false;
        }
        changed = result.changed;
    } else {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const MemberListSceneResult result = member_list_scene_scroll(
            &g_app.member_list_scene, target - g_app.member_list_scene.scroll_dips);
        if (!result.ok()) {
            return false;
        }
        changed = result.changed;
    }
    if (changed &&
        (!refresh_account_text(true) || (g_app.window && !application_request_frame()))) {
        return false;
    }
    return true;
}

bool handle_scrollbar_begin(float pointer_x, float pointer_y) noexcept {
    if (g_app.login.visible || g_app.scrollbar_drag != ScrollbarDragKind::none) {
        return false;
    }
    // An open menu floats above the panes, so its bar wins.
    const ScrollbarDragKind order[]{ScrollbarDragKind::popup, ScrollbarDragKind::members,
                                    ScrollbarDragKind::channels, ScrollbarDragKind::messages};
    for (u32 index = 0; index < 4; ++index) {
        const UiScrollbarGeometry geometry = application_scrollbar_geometry(order[index]);
        if (!ui_scrollbar_track_hit(geometry, pointer_x, pointer_y)) {
            continue;
        }
        g_app.scrollbar_drag = order[index];
        g_app.scrollbar_thumb_grab_offset = ui_scrollbar_thumb_hit(geometry, pointer_x, pointer_y)
                                                ? pointer_y - geometry.thumb.top
                                                : ui_rect_height(geometry.thumb) * 0.5f;
        SetCapture(g_app.window);
        return apply_scrollbar_pointer(pointer_y);
    }
    return false;
}

bool handle_scrollbar_drag(WPARAM buttons, float pointer_y) noexcept {
    if (g_app.scrollbar_drag == ScrollbarDragKind::none) {
        return false;
    }
    if ((buttons & MK_LBUTTON) != 0) {
        return apply_scrollbar_pointer(pointer_y);
    }
    g_app.scrollbar_drag = ScrollbarDragKind::none;
    g_app.scrollbar_thumb_grab_offset = 0.0f;
    if (GetCapture() == g_app.window) {
        ReleaseCapture();
    }
    return true;
}

void end_scrollbar_drag() noexcept {
    const bool held = g_app.scrollbar_drag != ScrollbarDragKind::none;
    g_app.scrollbar_drag = ScrollbarDragKind::none;
    g_app.scrollbar_thumb_grab_offset = 0.0f;
    if (held && GetCapture() == g_app.window) {
        ReleaseCapture();
    }
}

bool handle_message_key(WPARAM key) noexcept {
    const u64 viewport = g_app.message_view.timeline.viewport_units;
    const u64 page = viewport > g_app.message_view.timeline.default_item_units
                         ? viewport - g_app.message_view.timeline.default_item_units
                         : viewport;
    const u64 bounded_page = minimum(page, static_cast<u64>(~0ull >> 1));
    switch (key) {
    case VK_UP:
        return scroll_message_view_by(-static_cast<i64>(g_app.message_view.timeline.default_item_units));
    case VK_DOWN:
        return scroll_message_view_by(static_cast<i64>(g_app.message_view.timeline.default_item_units));
    case VK_PRIOR:
        return scroll_message_view_by(-static_cast<i64>(bounded_page));
    case VK_NEXT:
        return scroll_message_view_by(static_cast<i64>(bounded_page));
    case VK_HOME:
        return scroll_message_view_to(false);
    case VK_END:
        return scroll_message_view_to(true);
    default:
        return false;
    }
}

} // namespace maicord::app_detail

