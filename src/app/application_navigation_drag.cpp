#include "application_navigation_drag.hpp"
#include "application_account_presentation.hpp"
#include "application_content_input.hpp"
#include "application_guild_folders.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_shell_input.hpp"
#include "application_state.hpp"
#include "application_tooltip.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/channel_reorder.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/navigation_drag.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
struct DragOwner {
    NavigationDrag drag;
    LPARAM press; // replayed as an ordinary click when no drag happens
    u64 visual_source; // the row ID painted dimmed (folder header row IDs differ from folder IDs)
    u64 guild_id;      // channel drags: the guild whose list is shown
    u64 last_tick_ms;
    bool captured;
};
DragOwner g_drag{};

constexpr u32 kRowCapacity = kGuildRailRowCapacity > kShellNavigationRowCapacity ? kGuildRailRowCapacity
                                                                                  : kShellNavigationRowCapacity;
NavigationDragRow g_rows[kRowCapacity + 1];

bool account_ready() noexcept {
    return g_app.account_ui && !g_app.login.visible && !g_app.popup_scene.active && !g_app.text_form;
}
NavigationDragRowKind guild_kind(const GuildViewRow& row) noexcept {
    if (row.unavailable)
        return NavigationDragRowKind::none;
    if (row.folder_role == GuildFolderRole::header)
        return application_guild_folders_folder_id(row) ? NavigationDragRowKind::folder : NavigationDragRowKind::none;
    return row.folder_role == GuildFolderRole::member ? NavigationDragRowKind::nested_guild
                                                      : NavigationDragRowKind::guild;
}
u64 guild_item_id(const GuildViewRow& row) noexcept {
    return row.folder_role == GuildFolderRole::header ? application_guild_folders_folder_id(row) : row.id;
}
// Rearranging channels needs Manage Channels on the server; a category the
// move touches needs it there as well.
bool manage_channels(u64 guild_id) noexcept {
    return guild_id && guild_context_can_create_channel(g_app.account_ui->chat_state, guild_id, 0);
}
NavigationDragRowKind channel_kind(const ChannelViewRow& row, u64 guild_id) noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, row.id);
    if (!channel || channel->guild_id != guild_id)
        return NavigationDragRowKind::none;
    switch (channel_sort_group(row.type)) {
    case ChannelSortGroup::text:
        return NavigationDragRowKind::text;
    case ChannelSortGroup::voice:
        return NavigationDragRowKind::voice;
    case ChannelSortGroup::category:
        return guild_context_can_create_channel(g_app.account_ui->chat_state, 0, row.id)
                   ? NavigationDragRowKind::category
                   : NavigationDragRowKind::none;
    default:
        return NavigationDragRowKind::none;
    }
}
u32 guild_rows() noexcept {
    const auto& scene = g_app.guild_rail_scene;
    u32 count = 0;
    for (u32 i = 0; i < scene.count && count < kRowCapacity; ++i) {
        const auto& row = scene.rows[i];
        const auto* layout = guild_rail_scene_layout_node(scene, row);
        if (!layout || row.reveal <= 0)
            continue;
        g_rows[count++] = {guild_item_id(row.view), layout->rectangle, guild_kind(row.view)};
    }
    return count;
}
u32 channel_rows() noexcept {
    const auto& scene = g_app.shell_navigation_scene;
    u32 count = 0;
    for (u32 i = 0; i < scene.count && count < kRowCapacity; ++i) {
        const auto& row = scene.rows[i];
        const auto* layout = shell_navigation_scene_layout_node(scene, row);
        if (!layout)
            continue;
        g_rows[count++] = {row.view.id, layout->rectangle, channel_kind(row.view, g_drag.guild_id)};
    }
    return count;
}
void publish(const NavigationDragVisual& visual) noexcept {
    NavigationDragVisual painted = visual;
    painted.source_id = g_drag.visual_source;
    if (g_drag.drag.list == NavigationDragList::guilds)
        g_app.guild_rail_scene.drag = painted;
    else
        g_app.shell_navigation_scene.drag = painted;
    g_app.renderer.retained_presentation.valid = false;
    application_request_frame();
}
void clear_visuals() noexcept {
    g_app.guild_rail_scene.drag = {};
    g_app.shell_navigation_scene.drag = {};
    g_app.renderer.retained_presentation.valid = false;
    application_request_frame();
}
void retarget() noexcept {
    if (!navigation_drag_dragging(g_drag.drag) || !g_app.account_ui)
        return;
    const float y = g_drag.drag.pointer.y;
    NavigationDragTarget target{};
    if (g_drag.drag.list == NavigationDragList::guilds) {
        const u32 count = guild_rows();
        target = navigation_drag_guild_target(g_drag.drag, {g_rows, count}, y, kGuildRailRowGapDips);
    } else {
        const u32 count = channel_rows();
        target = navigation_drag_channel_target(g_drag.drag, {g_rows, count}, y);
        // Only drops the server accepts get an indicator.
        if (target.drop != NavigationDropKind::none) {
            const auto drop = target.drop == NavigationDropKind::into    ? ChannelReorderDrop::into
                              : target.drop == NavigationDropKind::before ? ChannelReorderDrop::before
                                                                          : ChannelReorderDrop::after;
            if (!channel_reorder_valid(g_app.account_ui->chat_state, g_drag.drag.source_id, target.id, drop))
                target = {};
        }
    }
    publish(navigation_drag_update_target(&g_drag.drag, target));
}
void end_capture() noexcept {
    if (g_drag.captured) {
        g_drag.captured = false;
        if (GetCapture() == g_app.window)
            ReleaseCapture();
    }
}
void commit(const NavigationDragTarget& target, NavigationDragList list, u64 source, NavigationDragRowKind kind,
            u64 guild_id) noexcept {
    if (!g_app.account_ui)
        return;
    if (list == NavigationDragList::guilds) {
        const auto drop = target.drop == NavigationDropKind::combine ? GuildFolderDrop::combine
                          : target.drop == NavigationDropKind::before ? GuildFolderDrop::before
                                                                      : GuildFolderDrop::after;
        const auto result = application_guild_folders_move({source, kind == NavigationDragRowKind::folder},
                                                           {target.id, target.kind == NavigationDragRowKind::folder},
                                                           drop);
        if (result != GuildFolderEditError::none && result != GuildFolderEditError::unchanged)
            log_u64(LogLevel::info, "navigation_drag", "guild_move_refused", static_cast<u64>(result));
        return;
    }
    if (!manage_channels(guild_id))
        return;
    const auto drop = target.drop == NavigationDropKind::into    ? ChannelReorderDrop::into
                      : target.drop == NavigationDropKind::before ? ChannelReorderDrop::before
                                                                  : ChannelReorderDrop::after;
    static GuildAdminPosition positions[kGuildAdminMaximumPositions];
    const u32 count = channel_reorder_plan(g_app.account_ui->chat_state, source, target.id, drop,
                                           {positions, kGuildAdminMaximumPositions});
    if (count == ~0u) {
        log_write(LogLevel::info, "navigation_drag", "channel_move_refused");
        return;
    }
    if (count)
        application_guild_lifecycle_reorder_channels(guild_id, {positions, count});
}
} // namespace

bool application_navigation_drag_press(LPARAM lparam) noexcept {
    if (!account_ready() || g_drag.drag.phase != NavigationDragPhase::idle)
        return false;
    const float x = client_dip_x(lparam), y = client_dip_y(lparam);
    const UiPoint point{x, y};
    const auto rail = guild_rail_scene_hit_test(g_app.guild_rail_scene, x, y);
    if (rail.id && ui_rect_contains(g_app.guild_rail_scene.viewport, x, y)) {
        const auto* row = guild_rail_scene_resolve(g_app.guild_rail_scene, rail.widget);
        const auto kind = row ? guild_kind(row->view) : NavigationDragRowKind::none;
        if (kind == NavigationDragRowKind::none)
            return false;
        navigation_drag_press(&g_drag.drag, NavigationDragList::guilds, guild_item_id(row->view), kind, point);
        g_drag.visual_source = row->view.id;
    } else {
        const auto& state = g_app.account_ui->chat_state;
        if (state.home_selected || !manage_channels(state.selected_guild_id) ||
            !ui_rect_contains(g_app.shell_navigation_scene.viewport, x, y))
            return false;
        const auto hit = shell_navigation_scene_hit_test(g_app.shell_navigation_scene, x, y);
        const auto* row = hit.id ? shell_navigation_scene_resolve(g_app.shell_navigation_scene, hit.widget) : nullptr;
        // Presses on a voice row's connected users keep their own behaviour.
        if (!row)
            return false;
        if (const auto* layout = shell_navigation_scene_layout_node(g_app.shell_navigation_scene, *row);
            !layout || (row->view.voice_count && y >= layout->rectangle.top + shell_navigation_voice_top(row->view)))
            return false;
        g_drag.guild_id = state.selected_guild_id;
        const auto kind = channel_kind(row->view, g_drag.guild_id);
        if (kind == NavigationDragRowKind::none)
            return false;
        navigation_drag_press(&g_drag.drag, NavigationDragList::channels, row->view.id, kind, point);
        g_drag.visual_source = row->view.id;
    }
    if (g_drag.drag.phase != NavigationDragPhase::armed) {
        g_drag = {};
        return false;
    }
    g_drag.press = lparam;
    return true;
}

bool application_navigation_drag_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (g_drag.drag.phase == NavigationDragPhase::idle)
        return false;
    switch (message) {
    case WM_MOUSEMOVE: {
        if (!(wparam & MK_LBUTTON)) {
            // The button came up outside the window: no click, no drop.
            application_navigation_drag_cancel();
            return false;
        }
        const UiPoint point{client_dip_x(lparam), client_dip_y(lparam)};
        if (navigation_drag_move(&g_drag.drag, point)) {
            SetCapture(g_app.window);
            g_drag.captured = true;
            g_drag.last_tick_ms = clock_monotonic_milliseconds();
            application_tooltip_clear();
        }
        if (!navigation_drag_dragging(g_drag.drag))
            return false;
        retarget();
        return true;
    }
    case WM_LBUTTONUP: {
        if (!navigation_drag_dragging(g_drag.drag)) {
            // A press without a drag is an ordinary click on its row.
            const LPARAM press = g_drag.press;
            g_drag = {};
            activate_navigation_press(press);
            return true;
        }
        g_drag.drag.pointer = {client_dip_x(lparam), client_dip_y(lparam)};
        retarget();
        const auto list = g_drag.drag.list;
        const auto kind = g_drag.drag.source_kind;
        const u64 source = g_drag.drag.source_id, guild = g_drag.guild_id;
        NavigationDragTarget target{};
        const bool dropped = navigation_drag_release(&g_drag.drag, &target);
        g_drag.drag = {};
        end_capture();
        g_drag = {};
        clear_visuals();
        if (dropped)
            commit(target, list, source, kind, guild);
        return true;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && navigation_drag_dragging(g_drag.drag)) {
            application_navigation_drag_cancel();
            return true;
        }
        return navigation_drag_dragging(g_drag.drag);
    case WM_RBUTTONDOWN:
        if (navigation_drag_dragging(g_drag.drag)) {
            application_navigation_drag_cancel();
            return true;
        }
        return false;
    case WM_CAPTURECHANGED:
        if (g_drag.captured && reinterpret_cast<HWND>(lparam) != g_app.window) {
            g_drag.captured = false;
            application_navigation_drag_cancel();
        }
        return false;
    case WM_KILLFOCUS:
        application_navigation_drag_cancel();
        return false;
    default:
        return false;
    }
}

void application_navigation_drag_tick(u64 now_ms) noexcept {
    if (!navigation_drag_dragging(g_drag.drag))
        return;
    if (!account_ready()) {
        application_navigation_drag_cancel();
        return;
    }
    const u64 elapsed = now_ms > g_drag.last_tick_ms ? now_ms - g_drag.last_tick_ms : 0;
    g_drag.last_tick_ms = now_ms;
    const bool guilds = g_drag.drag.list == NavigationDragList::guilds;
    const UiRect viewport = guilds ? g_app.guild_rail_scene.viewport : g_app.shell_navigation_scene.viewport;
    const float delta = navigation_drag_autoscroll(viewport, g_drag.drag.pointer.y, elapsed);
    if (delta == 0)
        return;
    bool changed = false;
    auto& view = g_app.account_ui->navigation_view;
    if (guilds) {
        const auto result = guild_rail_scene_scroll(&g_app.guild_rail_scene, &view, delta);
        changed = result.ok() && result.changed;
    } else {
        const auto result = shell_navigation_scene_scroll(&g_app.shell_navigation_scene, &view, delta);
        changed = result.ok() && result.changed;
    }
    if (changed) {
        refresh_account_text(true);
        retarget();
    }
    // Keep sampling while the pointer rests in the edge band.
    application_request_frame();
}

void application_navigation_drag_cancel() noexcept {
    if (g_drag.drag.phase == NavigationDragPhase::idle)
        return;
    const bool dragging = navigation_drag_dragging(g_drag.drag);
    navigation_drag_cancel(&g_drag.drag);
    end_capture();
    g_drag = {};
    if (dragging)
        clear_visuals();
}
} // namespace maicord::app_detail
