#include "application_commands.hpp"
#include "application_content_point.hpp"
#include "application_cursor.hpp"
#include "application_scrolling.hpp"
#include "application_profile.hpp"
#include "application_account_presentation.hpp"
#include "application_message_history.hpp"
#include "application_message_interaction.hpp"
#include "application_state.hpp"
#include "application_retained_projection.hpp"
#include "application_settings.hpp"
#include "application_theme_editor.hpp"
#include "application_pinned_messages.hpp"
#include "application_thread_browser.hpp"
#include "application_inbox.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/scroll_host.hpp"

#include "maicord/core/win32_user.hpp"

#include <limits.h>

namespace maicord::app_detail {
namespace {
// A moved message viewport takes the scroll-only account refresh.
bool refresh_scrolled_projection(bool messages_changed) noexcept {
    return messages_changed ? refresh_account_scroll() && application_request_frame()
                            : refresh_retained_projection(false);
}
bool finish_immediate_scroll(bool messages_changed) noexcept {
    if (!g_app.scroll_host.immediate)
        return true;
    if (g_app.reaction_viewer.active)
        return refresh_reaction_viewer_presentation();
    if (application_thread_browser_active())
        return application_thread_browser_refresh_after_scroll();
    return refresh_scrolled_projection(messages_changed);
}
enum Surface : u64 {
    messages,
    members,
    guilds,
    channels,
    composer,
    command_categories,
    suggestions,
    reaction_users,
    reaction_types,
    profile_details,
    profile_peers,
    pinned_messages,
    thread_browser,
    inbox,
    dm_profile
};

// Virtualized surfaces describe their full range, not just materialized rows.
// There is no wheel policy or animation state in these adapters.
bool read_surface(void*, u64 key, UiScrollRange* range) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return false;
    const bool profile = g_app.profile && g_app.profile->scene.active;
    if (profile && key != profile_details && key != profile_peers)
        return false;
    if (key == profile_details || key == profile_peers) {
        if (!profile)
            return false;
        const auto binding = profile_scene_scroll_binding(&g_app.profile->scene, key == profile_peers);
        return binding.read(binding.context, binding.key, range);
    }
    if (key == dm_profile) {
        auto* panel = g_app.renderer.dm_profile_panel;
        if (!panel || !panel->active || g_app.popup_scene.active || g_app.reaction_viewer.active)
            return false;
        const auto binding = profile_scene_scroll_binding(panel);
        return binding.read(binding.context, binding.key, range);
    }
    if (application_thread_browser_read_blocked() &&
        (key == messages || key == members || key == composer || key == pinned_messages))
        return false;
    if (key <= composer && (g_app.reaction_viewer.active || g_app.popup_scene.active))
        return false;
    switch (key) {
    case inbox:
        return application_inbox_scroll_range(range);
    case thread_browser:
        return application_thread_browser_scroll_range(range);
    case pinned_messages:
        if (g_app.popup_scene.active || g_app.reaction_viewer.active ||
            g_app.image_viewer.scene.active || application_settings_active() || g_app.text_form)
            return false;
        return application_pinned_messages_scroll_range(range);
    case composer: {
        const auto& scene = g_app.message_composer_scene;
        const auto& edit = g_app.renderer.composer_layout;
        const auto* node = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
        if (!scene.active || !scene.model || !edit.active || !node || g_app.image_viewer.scene.active ||
            edit.layout_height <= edit.height)
            return false;
        *range = {node->content_rectangle,
                  edit.scroll_y,
                  edit.layout_height - edit.height,
                  edit.caret.height > 0 ? edit.caret.height : edit.em_size,
                  1,
                  0,
                  scene.model->document_identity};
        return true;
    }
    case messages: {
        const auto& scene = g_app.message_scene;
        const auto& timeline = g_app.message_view.timeline;
        if (!scene.active || !timeline.list)
            return false;
        *range = {scene.viewport,
                  static_cast<double>(timeline.scroll_units),
                  static_cast<double>(timeline.list->total_units > timeline.viewport_units
                                          ? timeline.list->total_units - timeline.viewport_units
                                          : 0),
                  static_cast<double>(ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height)),
                  static_cast<double>(ui_extent_units(1.0f)),
                  timeline.scroll_origin_units,
                  timeline.reset_generation};
        return true;
    }
    case members:
        if (!g_app.member_list_scene.active)
            return false;
        *range = {g_app.member_list_scene.viewport, g_app.member_list_scene.scroll_dips,
                  maximum(0.0f, g_app.member_list_scene.content_height -
                                    ui_rect_height(g_app.member_list_scene.viewport)),
                  kMemberListRowHeight, 1};
        return true;
    case guilds:
        if (!g_app.guild_rail_scene.active)
            return false;
        *range = {g_app.guild_rail_scene.viewport, g_app.guild_rail_scene.scroll_dips,
                  maximum(0.0f, g_app.guild_rail_scene.content_height -
                                    ui_rect_height(g_app.guild_rail_scene.viewport)),
                  32, 1};
        return true;
    case channels:
        if (!g_app.shell_navigation_scene.active)
            return false;
        *range = {g_app.shell_navigation_scene.viewport, g_app.shell_navigation_scene.scroll_dips,
                  maximum(0.0f, g_app.shell_navigation_scene.content_height -
                                    ui_rect_height(g_app.shell_navigation_scene.viewport)),
                  32, 1};
        return true;
    case command_categories:
        return application_commands_scroll_range(true, range);
    case suggestions:
        if (g_app.popup_owner_kind == PopupOwnerKind::commands)
            return application_commands_scroll_range(false, range);
        if (!g_app.popup_scene.active || (g_app.popup_owner_kind != PopupOwnerKind::autocomplete &&
                                          g_app.popup_scene.kind != PopupKind::context_menu &&
                                          g_app.popup_scene.kind != PopupKind::popover))
            return false;
        *range = {g_app.popup_scene.rectangle, g_app.popup_scene.scroll_offset,
                  g_app.popup_scene.scroll_max, kPopupRowHeight, 1};
        return true;
    case reaction_users:
    case reaction_types: {
        const auto& viewer = g_app.reaction_viewer;
        if (!viewer.active)
            return false;
        const bool users = key == reaction_users;
        UiRect viewport = viewer.body;
        const float split = viewer.panel.left + (ui_rect_width(viewer.panel) < 400 ? 112.0f : 138.0f);
        if (users)
            viewport.left = split;
        else
            viewport.right = split;
        *range = {viewport, users ? viewer.user_scroll : viewer.aggregate_scroll,
                  reaction_viewer_scroll_maximum(viewer, users),
                  users ? kReactionViewerUserRowHeight : kReactionViewerAggregateRowHeight, 1};
        return true;
    }
    default:
        return false;
    }
}

// Scrolls a panel with its own range accessors; reports whether the position moved.
bool scroll_panel(bool (*scroll_to)(double) noexcept, bool (*read_range)(UiScrollRange*) noexcept, double offset,
                  double before, bool* changed) noexcept {
    UiScrollRange after{};
    if (!scroll_to(offset) || !read_range(&after))
        return false;
    *changed = before != after.offset;
    return true;
}

bool write_surface(void*, u64 key, double offset, bool* changed) noexcept {
    UiScrollRange before{};
    if (!read_surface(nullptr, key, &before))
        return false;
    *changed = false;
    switch (key) {
    case inbox:
        return scroll_panel(application_inbox_scroll_to, application_inbox_scroll_range, offset, before.offset,
                            changed);
    case thread_browser:
        return scroll_panel(application_thread_browser_scroll_to, application_thread_browser_scroll_range,
                            offset, before.offset, changed);
    case pinned_messages:
        return scroll_panel(application_pinned_messages_scroll_to, application_pinned_messages_scroll_range,
                            offset, before.offset, changed);
    case profile_details:
    case profile_peers: {
        const auto binding = profile_scene_scroll_binding(&g_app.profile->scene, key == profile_peers);
        return binding.write(binding.context, binding.key, offset, changed);
    }
    case dm_profile: {
        const auto binding = profile_scene_scroll_binding(g_app.renderer.dm_profile_panel);
        return binding.write(binding.context, binding.key, offset, changed);
    }
    case composer: {
        auto& edit = g_app.renderer.composer_layout;
        if (!text_edit_layout_scroll(&edit, static_cast<float>(offset - before.offset)))
            return false;
        *changed = before.offset != edit.scroll_y;
        return true;
    }
    case messages: {
        const i64 next = static_cast<i64>(offset + 0.5);
        const bool ok = scroll_message_view_by(
            next - static_cast<i64>(g_app.message_view.timeline.scroll_units), false);
        *changed = before.offset != g_app.message_view.timeline.scroll_units;
        return ok;
    }
    case members: {
        const auto result =
            member_list_scene_scroll(&g_app.member_list_scene, static_cast<float>(offset - before.offset));
        *changed = result.changed;
        return result.ok();
    }
    case guilds: {
        const auto result =
            guild_rail_scene_scroll(&g_app.guild_rail_scene, &g_app.account_ui->navigation_view,
                                    static_cast<float>(offset - before.offset));
        *changed = result.changed;
        return result.ok();
    }
    case channels: {
        const auto result =
            shell_navigation_scene_scroll(&g_app.shell_navigation_scene, &g_app.account_ui->navigation_view,
                                          static_cast<float>(offset - before.offset));
        *changed = result.changed;
        return result.ok();
    }
    case command_categories:
        return application_commands_scroll(true, offset, changed);
    case suggestions:
        if (g_app.popup_owner_kind == PopupOwnerKind::commands)
            return application_commands_scroll(false, offset, changed);
        return popup_scene_scroll(&g_app.popup_scene, static_cast<float>(offset - before.offset), changed);
    case reaction_users:
    case reaction_types:
        return reaction_viewer_set_scroll(&g_app.reaction_viewer, key == reaction_users,
                                          static_cast<float>(offset), changed);
    default:
        return false;
    }
}
} // namespace

void update_scrollbar_hover(float x, float y) noexcept {
    UiRect viewport{};
    constexpr Surface order[] = {
        inbox,
        profile_peers, profile_details, command_categories,
        suggestions,   reaction_types,  reaction_users,    thread_browser, pinned_messages, composer, guilds,
        channels,      members,         dm_profile,        messages};
    for (const auto surface : order) {
        UiScrollRange range{};
        if (read_surface(nullptr, surface, &range) && ui_rect_contains(range.viewport, x, y)) {
            viewport = range.viewport;
            break;
        }
    }
    if (!ui_rect_equal(viewport, g_app.renderer.scrollbar_viewport)) {
        g_app.renderer.scrollbar_viewport = viewport;
        if (g_app.window)
            application_request_frame();
    }
}

bool handle_scroll_wheel(WPARAM wparam, LPARAM lparam) noexcept {
    const u64 message_scroll_before = g_app.message_view.timeline.scroll_units;
    POINT pixel{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (!application_screen_to_content(&pixel))
        return false;
    const double scale = 96.0 / maximum(g_app.renderer.dpi, 1u);
    const UiPoint point{static_cast<float>(pixel.x * scale), static_cast<float>(pixel.y * scale)};
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const i32 delta = static_cast<short>(HIWORD(wparam));
    const u64 now = application_now_us();
    const auto wheel = [&](u64 surface) noexcept {
        return ui_scroll_host_wheel(&g_app.scroll_host, {nullptr, surface, read_surface, write_surface}, point,
                                    delta, lines, lines == WHEEL_PAGESCROLL, now, scale);
    };
    const auto finish = [&](UiScrollDispatch result, const char* failure, bool messages_changed) noexcept {
        if (result == UiScrollDispatch::failed)
            log_write(LogLevel::error, "ui", failure);
        if (!finish_immediate_scroll(messages_changed))
            log_write(LogLevel::error, "ui", "immediate_scroll_refresh_failed");
    };
    // The inbox popout floats above every pane. Its body takes the same
    // animated wheel policy as the timeline; the rest of the panel absorbs
    // wheel input so nothing scrolls underneath it.
    if (const auto* popout = g_app.renderer.inbox_scene;
        popout && application_inbox_active() && ui_inbox_scene_contains(*popout, point.x, point.y)) {
        finish(wheel(inbox), "inbox_scroll_dispatch_failed", false);
        return true;
    }
    // The browser owns the entire main-content panel, including headers and
    // an empty list. Covered message/member descendants never receive its wheel.
    if (application_thread_browser_active() &&
        ui_rect_contains(application_thread_browser_viewport(), point.x, point.y) &&
        !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
        !(g_app.profile && g_app.profile->scene.active)) {
        finish(wheel(thread_browser), "thread_browser_scroll_dispatch_failed", false);
        return true;
    }
    // The pin panel covers timeline/member descendants. Route its virtual
    // body first, including when it has no scroll extent, so wheel input cannot
    // reach a covered retained child underneath it.
    const auto pins = wheel(pinned_messages);
    if (pins != UiScrollDispatch::unhandled) {
        finish(pins, "pinned_scroll_dispatch_failed", false);
        return true;
    }
    // Retained descendants route before their virtualized parent surfaces.
    // Passing a virtual owner excludes only that node, not its descendants.
    struct Tree {
        UiWidgetManager* manager;
        UiWidgetHandle virtual_owner;
        bool active;
    };
    const bool base = !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
                      !(g_app.profile && g_app.profile->scene.active);
    const bool content = base && !application_thread_browser_read_blocked();
    Tree trees[] = {
        {&g_app.reaction_viewer.widgets, ui_widget_invalid_handle(), g_app.reaction_viewer.active},
        {&g_app.popup_scene.widgets, g_app.popup_scene.panel,
         g_app.popup_scene.active && !g_app.reaction_viewer.active &&
             g_app.popup_owner_kind != PopupOwnerKind::commands},
        {&g_app.message_composer_scene.widgets, ui_widget_invalid_handle(),
         content && g_app.message_composer_scene.active},
        {&g_app.message_scene.widgets, g_app.message_scene.root, content && g_app.message_scene.active},
        {&g_app.member_list_scene.widgets, g_app.member_list_scene.root,
         content && g_app.member_list_scene.active},
        {&g_app.guild_rail_scene.widgets, g_app.guild_rail_scene.root,
         base && g_app.guild_rail_scene.active},
        {&g_app.shell_navigation_scene.widgets, g_app.shell_navigation_scene.root,
         base && g_app.shell_navigation_scene.active},
    };
    // The composer upload strip scrolls horizontally under the vertical wheel.
    if (content && g_app.message_composer_scene.active) {
        const auto strip = message_composer_scene_attachment_view(g_app.message_composer_scene, false);
        if (strip.tiles.count && ui_rect_contains(strip.strip, point.x, point.y)) {
            if (message_composer_scene_attachment_scroll(&g_app.message_composer_scene,
                                                         -static_cast<float>(delta) / WHEEL_DELTA * 96.f) &&
                !refresh_account_frame(true))
                log_write(LogLevel::error, "ui", "attachment_strip_scroll_refresh_failed");
            return true;
        }
    }
    for (const auto& tree : trees) {
        if (!tree.active)
            continue;
        const auto result =
            ui_scroll_host_wheel_widgets(&g_app.scroll_host, tree.manager, point, delta, lines,
                                         lines == WHEEL_PAGESCROLL, now, scale, tree.virtual_owner);
        if (result != UiScrollDispatch::unhandled) {
            finish(result, "retained_scroll_dispatch_failed",
                   g_app.message_view.timeline.scroll_units != message_scroll_before);
            return true;
        }
    }
    constexpr Surface order[] = {profile_peers,     profile_details, reaction_types,     reaction_users,
                                 command_categories, suggestions,
                                 thread_browser, pinned_messages,    composer,       guilds,             channels, members,
                                 dm_profile,     messages};
    for (const auto surface : order) {
        const auto result = wheel(surface);
        if (result != UiScrollDispatch::unhandled) {
            finish(result, "virtual_scroll_dispatch_failed",
                   g_app.message_view.timeline.scroll_units != message_scroll_before);
            return true;
        }
    }
    // Modal wheel input cannot leak into the message timeline underneath.
    if (g_app.profile && g_app.profile->scene.active)
        return true;
    if (g_app.reaction_viewer.active)
        return true;
    if (g_app.popup_scene.active) {
        if (!ui_rect_contains(g_app.popup_scene.rectangle, point.x, point.y))
            dismiss_popup();
        return true;
    }
    return false;
}

bool reveal_command_row(float top, float bottom) noexcept {
    return ui_scroll_host_reveal(&g_app.scroll_host, {nullptr, suggestions, read_surface, write_surface},
                                 top, bottom, application_now_us(),
                                 96.0 / maximum(g_app.renderer.dpi, 1u)) != UiScrollDispatch::failed;
}

bool queue_profile_scroll(bool right, i32 delta) noexcept {
    const auto surface = right ? profile_peers : profile_details;
    UiScrollRange range{};
    if (!read_surface(nullptr, surface, &range))
        return false;
    const UiPoint point{range.viewport.left + 1, range.viewport.top + 1};
    return ui_scroll_host_wheel(&g_app.scroll_host, {nullptr, surface, read_surface, write_surface}, point,
                                delta, 1, false, application_now_us(),
                                96.0 / maximum(g_app.renderer.dpi, 1u)) != UiScrollDispatch::failed;
}

bool animate_guild_rail_scroll_from(float before) noexcept {
    auto& rail = g_app.guild_rail_scene;
    if (!g_app.account_ui || !rail.active || g_app.scroll_host.immediate || rail.scroll_dips == before)
        return true;
    const float after = rail.scroll_dips;
    const auto* view = &g_app.account_ui->navigation_view;
    // Restoring keeps the focused guild id; its row refocuses as it materializes.
    if (!guild_rail_scene_update(&rail, view, rail.viewport, before).ok())
        return false;
    const auto result = ui_scroll_host_scroll_to(
        &g_app.scroll_host, {nullptr, guilds, read_surface, write_surface}, after, application_now_us(),
        96.0 / maximum(g_app.renderer.dpi, 1u));
    if (result == UiScrollDispatch::handled)
        return refresh_retained_projection(false);
    // A covering surface owns scrolling right now: keep the jump.
    return guild_rail_scene_update(&rail, view, rail.viewport, after).ok() &&
           result != UiScrollDispatch::failed;
}

bool animate_popup_scroll_from(float before) noexcept {
    auto& popup = g_app.popup_scene;
    // Command popups scroll their own child viewports, not the menu panel.
    if (!popup.active || g_app.popup_owner_kind == PopupOwnerKind::commands || g_app.scroll_host.immediate ||
        popup.scroll_offset == before)
        return true;
    const float after = popup.scroll_offset;
    bool changed = false;
    if (!popup_scene_scroll(&popup, before - after, &changed))
        return true;
    const auto result = ui_scroll_host_scroll_to(
        &g_app.scroll_host, {nullptr, suggestions, read_surface, write_surface}, after, application_now_us(),
        96.0 / maximum(g_app.renderer.dpi, 1u));
    if (result == UiScrollDispatch::handled)
        return true;
    return popup_scene_scroll(&popup, after - popup.scroll_offset, &changed) && result != UiScrollDispatch::failed;
}

namespace {
// Live cost of a menu glide: per-tick refresh time, logged once it settles, so
// a slow projection (which would make the glide look like a jump) shows up.
struct PopupGlideCost {
    u32 ticks;
    u64 total_us, max_us;
};
PopupGlideCost popup_glide;
void popup_glide_settled() noexcept {
    if (!popup_glide.ticks)
        return;
    log_u64(LogLevel::info, "scroll", "popup_glide_ticks", popup_glide.ticks);
    log_u64(LogLevel::info, "scroll", "popup_glide_tick_max_us", popup_glide.max_us);
    log_u64(LogLevel::info, "scroll", "popup_glide_tick_mean_us", popup_glide.total_us / popup_glide.ticks);
    popup_glide = {};
}
} // namespace

bool tick_smooth_scrolls(u64 now_us) noexcept {
    const u64 message_scroll_before = g_app.message_view.timeline.scroll_units;
    bool changed = false;
    if (!ui_scroll_host_tick(&g_app.scroll_host, now_us, &changed))
        return false;
    const bool popup_glide_tick = changed && g_app.popup_scene.active && !application_settings_active();
    if (!popup_glide_tick)
        popup_glide_settled();
    const u64 refresh_start = popup_glide_tick ? application_now_us() : 0;
    struct PopupGlideSample {
        bool active;
        u64 start;
        ~PopupGlideSample() {
            if (!active)
                return;
            const u64 elapsed = application_now_us() - start;
            ++popup_glide.ticks;
            popup_glide.total_us += elapsed;
            popup_glide.max_us = maximum(popup_glide.max_us, elapsed);
            if (!ui_scroll_host_active(g_app.scroll_host))
                popup_glide_settled();
        }
    } sample{popup_glide_tick, refresh_start};
    if (g_app.reaction_viewer.active) {
        const auto more = reaction_viewer_controller_request_more(&g_app.reaction_viewer_controller);
        if (more.ok() && more.changed) {
            changed = true;
            if (!pump_message_history())
                return false;
        }
    }
    if (!changed)
        return true;
    // Each glide step moves content under a still pointer; the next presented
    // frame resolves the cursor again so a hover affordance cannot keep it.
    application_cursor_invalidate();
    if (application_settings_active())
        return application_settings_synchronize() && application_theme_editor_synchronize() &&
               application_request_frame();
    if (g_app.reaction_viewer.active)
        return refresh_reaction_viewer_presentation();
    if (application_thread_browser_active())
        return application_thread_browser_refresh_after_scroll();
    return refresh_scrolled_projection(g_app.message_view.timeline.scroll_units != message_scroll_before);
}
} // namespace maicord::app_detail
