#include "application_shell_input.hpp"
#include "maicord/core/memory.hpp"
#include "application_guild_menu.hpp"
#include "application_cursor.hpp"
#include "application_video.hpp"
#include "application_private_channel.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_profile.hpp"
#include "application_account_menu.hpp"
#include "application_content_input.hpp"
#include "application_channel_snapshots.hpp"
#include "application_retained_projection.hpp"
#include "maicord/ui/voice_panel.hpp"

#include "application_account_presentation.hpp"
#include "application_message_interaction.hpp"
#include "application_thread_browser.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "application_plugins.hpp"

#include "maicord/render/d3d11_renderer.hpp"
#include "../render/d3d11_renderer_interaction_state.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/core/log.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/message_author.hpp"

namespace maicord::app_detail {
namespace {
// The header toolbar badge as the shell paints it: right-aligned in the title
// label across the full title bar height.
UiPluginSlotBadge header_plugin_hit(float x, float y) noexcept {
    const auto* label = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::title_label));
    const auto* title = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::title_bar));
    UiPluginSlotBadge badge{};
    if (!label || !title)
        return badge;
    UiRect line = label->rectangle;
    line.top = title->rectangle.top;
    line.bottom = title->rectangle.bottom;
    if (!ui_plugin_slot_line_badge(kUiPluginSlotHeaderToolbar, 0, 0, 0, label->widget, line, nullptr, &badge) ||
        !ui_rect_contains(badge.badge.rectangle, x, y))
        reset_value(&badge);
    return badge;
}
bool voice_disconnect_contains(float x, float y) noexcept {
    const auto* footer = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::channel_footer));
    return footer && g_app.account_ui && g_app.account_ui->navigation_view.connected_voice_channel_id &&
           ui_rect_contains(ui_voice_disconnect_rectangle(footer->rectangle), x, y);
}
bool leave_voice() noexcept {
    AccountHost& host = g_app.account;
    const GatewayVoiceStateUpdate leave{g_app.account_ui->voice_session.guild_id, 0, false, false, false};
    if (!host.instance || !host.api.voice_request || !host.api.voice_request(host.instance, &leave))
        log_write(LogLevel::warning, "voice", "leave_rejected");
    return true;
}

bool account_identity_contains(float x, float y) noexcept {
    const auto* footer = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::channel_footer));
    if (!footer || !g_app.account_ui || !ui_rect_contains(footer->rectangle, x, y)) return false;
    const auto& users = g_app.account_ui->user_store;
    const auto* self = user_store_find(users, g_app.account_ui->chat_state.self_user_id);
    if (!self) return false;
    if (ui_rect_contains(ui_account_avatar_rectangle(footer->rectangle), x, y))
        return true;
    const Utf8View labels[]{user_store_name(users, *self), user_store_username(users, *self)};
    for (u32 i = 0; i < 2; ++i) {
        const auto label = labels[i];
        if (!label.bytes) continue;
        const TextStyleSpan style{0, label.bytes, i ? text_style_subtext : text_style_bold};
        auto r = footer->rectangle;
        r.left += 52;
        r.right -= 44;
        r.top += i ? 28 : 6;
        r.bottom = i ? footer->rectangle.bottom - 4 : r.top + 22;
        UiTextMeasureValue measured{};
        const bool ready = ui_text_measure_cache_measure(&g_app.account_ui->text_measure_cache,
            {{self->id, hash64_bytes(kHash64Seed, label.data, label.bytes), self->revision, static_cast<u16>(0x4140 + i), 0},
             label, {&style, 1}, {maximum(1.f, ui_rect_width(r)), 64}, true}, &measured).ok();
        r.right = minimum(r.right, r.left + (ready ? measured.size.width + 2 : label.bytes * 8.f));
        if (ui_rect_contains(r, x, y)) return true;
    }
    return false;
}

// The channel sidebar's right edge is also the thread browser's left edge, so
// the same splitter resizes the browser while it covers the chat column.
PaneResizeKind pane_resize_hit(float x, float y) noexcept {
    const UiNode* channels =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::channel_sidebar));
    if (channels && ui_splitter_hit(channels->rectangle, UiSplitterEdge::right, x, y))
        return PaneResizeKind::channels;
    const UiNode* members =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::member_sidebar));
    // Search results and the DM profile take the member slot at a fixed width; no grip there.
    if (members && !d3d11_renderer_search_visible(g_app.renderer) && !g_app.shell_scene.side_panel_profile &&
        ui_splitter_hit(members->rectangle, UiSplitterEdge::left, x, y))
        return PaneResizeKind::members;
    return PaneResizeKind::none;
}

} // namespace

float client_dip_x(LPARAM lparam) noexcept {
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    return static_cast<float>(static_cast<short>(LOWORD(lparam))) * scale;
}

float client_dip_y(LPARAM lparam) noexcept {
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    return static_cast<float>(static_cast<short>(HIWORD(lparam))) * scale;
}

namespace {
// Every client cursor is resolved here in the same front-to-back order as
// input routing. Scenes report semantic shapes; application_set_cursor maps
// them to native cursors.
bool select_client_cursor(i32 pixel_x, i32 pixel_y, UiCursor* out) noexcept {
    if (!g_app.renderer_ready)
        return false;
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const float x = static_cast<float>(pixel_x) * scale;
    const float y = static_cast<float>(pixel_y) * scale;
    const UiPoint point{x, y};
    auto& cursor = *out;
    cursor = UiCursor::arrow;
    // A splitter drag keeps the resize cursor over whatever it crosses.
    if (g_app.pane_resize != PaneResizeKind::none) {
        cursor = UiCursor::resize_we;
        return true;
    }
    // The opaque Settings page owns the entire client surface, including
    // empty space. Never consult text, links or resize grips behind it.
    if (const auto* settings = g_app.renderer.settings_scene; settings && settings->active) {
        cursor = g_app.popup_owner_kind == PopupOwnerKind::settings_choice && g_app.popup_scene.active
                     ? ui_popup_scene_cursor(g_app.popup_scene, x, y)
                     : ui_settings_scene_cursor(*settings, x, y);
        return true;
    }
    if (g_app.profile && g_app.profile->scene.active) {
        const auto& profile = g_app.profile->scene;
        cursor = profile.edit.active &&
                         (g_app.profile->message_dragging || ui_rect_contains(profile.edit_rectangle, x, y))
                     ? UiCursor::ibeam
                     : ui_widget_cursor(profile.widgets, profile_scene_hit_test(profile, x, y));
        return true;
    }
    if (g_app.image_viewer.scene.active) {
        const auto& scene = g_app.image_viewer.scene;
        cursor = ui_widget_manager_cursor(scene.widgets, x, y);
        if (cursor == UiCursor::arrow && !application_video_viewer_cursor(point, &cursor))
            cursor = scene.zoom > 1 ? UiCursor::move : UiCursor::arrow;
        return true;
    }
    if (g_app.reaction_viewer.active) {
        cursor = ui_reaction_viewer_cursor(g_app.reaction_viewer, x, y);
        return true;
    }
    if (application_friends_requests_active()) {
        const auto* requests = g_app.renderer.friends_requests_scene;
        cursor = g_app.popup_scene.active ? ui_popup_scene_cursor(g_app.popup_scene, x, y)
                 : requests               ? ui_friends_scene_cursor(*requests, x, y)
                                          : UiCursor::arrow;
        return true;
    }
    if (const auto* friends = g_app.renderer.friends_scene;
        application_friends_active() && friends && ui_rect_contains(friends->viewport, x, y)) {
        cursor = ui_friends_scene_cursor(*friends, x, y);
        return true;
    }
    if (g_app.login.visible) {
        cursor = ui_login_scene_cursor(g_app.login_scene, x, y);
        return true;
    }
    if (g_app.popup_scene.active) {
        const auto hit = popup_scene_hit_test(g_app.popup_scene, x, y);
        if (g_app.popup_scene.kind != PopupKind::autocomplete || hit.inside) {
            cursor = ui_popup_scene_cursor(g_app.popup_scene, x, y);
            return true;
        }
    }
    // Splitter grips outrank the panels that border them (thread browser, pins).
    if (pane_resize_hit(x, y) != PaneResizeKind::none) {
        cursor = UiCursor::resize_we;
        return true;
    }
    if (const auto* pins = g_app.renderer.pinned_messages_scene;
        pins && ui_pinned_messages_scene_contains(*pins, x, y)) {
        cursor = ui_pinned_messages_scene_cursor(*pins, x, y);
        return true;
    }
    if (const auto* browser = application_thread_browser_scene();
        browser && ui_thread_browser_scene_contains(*browser, x, y)) {
        cursor = ui_thread_browser_scene_cursor(*browser, x, y);
        return true;
    }
    cursor = UiCursor::hand;
    if (message_action_bar_hit_test(g_app.message_action_bar, x, y).action != MessageActionBarAction::none)
        return true;
    const auto composer = message_composer_scene_hit_test(g_app.message_composer_scene, x, y);
    if (composer != MessageComposerControl::none) {
        cursor = ui_action_cursor(true, composer == MessageComposerControl::edit);
        return true;
    }
    ApplicationPluginSurfaceHit surface{};
    if (header_plugin_hit(x, y).action_id || (application_plugins_surface_hit(x, y, &surface) && surface.action_id) ||
        (g_app.member_list_scene.active && member_list_plugin_hit(g_app.member_list_scene, x, y, nullptr).action_id))
        return true;
    const auto guild = guild_rail_scene_hit_test(g_app.guild_rail_scene, x, y);
    if (guild.selectable || guild.folder || guild.add_server || guild.conversation)
        return true;
    // Nonselectable rows with an ID still occlude the lower-priority lists.
    if (!guild.id) {
        const auto channel = shell_navigation_scene_hit_test(g_app.shell_navigation_scene, x, y);
        if (channel.selectable)
            return true;
        if (!channel.id && member_list_scene_hit_test(g_app.member_list_scene, x, y).user_id)
            return true;
    }
    // Inline players, polls, previews, replies and rich buttons own their cards
    // before the text underneath can claim an I-beam.
    if (application_message_content_cursor(x, y, &cursor))
        return true;
    cursor = UiCursor::hand;
    const auto message = message_scene_hit_test(g_app.message_scene, x, y);
    if (application_message_profile_hit_test(x, y).user_id || application_message_guild_tag_hit(x, y))
        return true;
    u32 reaction_index = 0;
    if (message_scene_reaction_index(g_app.message_scene, message, &reaction_index) ||
        renderer_content_hit_test(g_app.renderer, pixel_x, pixel_y).valid)
        return true;
    if (application_voice_hand_cursor(x, y))
        return true;
    if (application_private_channel_title_hit(x, y))
        return true;
    // Shell buttons declare themselves; the footer is a button only over the
    // account identity and voice disconnect targets.
    const auto shell_hit = ui_widget_manager_hit_test(g_app.shell_scene.widgets, x, y);
    const auto* shell = ui_widget_manager_resolve(g_app.shell_scene.widgets, shell_hit);
    if (shell && (shell->id == static_cast<u32>(UiSemantic::channel_footer) + 1u
                      ? account_identity_contains(x, y) || voice_disconnect_contains(x, y)
                      : ui_widget_cursor(g_app.shell_scene.widgets, shell_hit) == UiCursor::hand))
        return true;
    cursor = ui_rect_contains(g_app.message_scene.viewport, x, y) &&
                     renderer_message_text_position_hit_test(g_app.renderer, pixel_x, pixel_y).valid
                 ? UiCursor::ibeam
                 : UiCursor::arrow;
    return true;
}
} // namespace

bool set_client_cursor(i32 pixel_x, i32 pixel_y) noexcept {
    UiCursor cursor{};
    if (!select_client_cursor(pixel_x, pixel_y, &cursor))
        return false;
    application_set_cursor(cursor);
    return true;
}
bool handle_pane_resize_begin(LPARAM lparam) noexcept {
    // Foreground overlays that own the pointer keep it even over a grip.
    if (g_app.login.visible || g_app.pane_resize != PaneResizeKind::none ||
        (g_app.profile && g_app.profile->scene.active) || g_app.image_viewer.scene.active ||
        g_app.reaction_viewer.active) {
        return false;
    }
    const PaneResizeKind hit = pane_resize_hit(client_dip_x(lparam), client_dip_y(lparam));
    if (hit == PaneResizeKind::none) {
        return false;
    }
    if (g_app.popup_scene.active && !dismiss_popup()) {
        return false;
    }
    g_app.pane_resize = hit;
    g_app.pane_resize_origin_width =
        hit == PaneResizeKind::channels ? g_app.channel_pane_width : g_app.member_pane_width;
    SetCapture(g_app.window);
    return true;
}

bool handle_shell_hover(LPARAM lparam) noexcept {
    if (g_app.login.visible) {
        return false;
    }
    const float x = client_dip_x(lparam);
    const float y = client_dip_y(lparam);
    if (pane_resize_hit(x, y) != PaneResizeKind::none)
        return true;
    if (application_plugins_surface_hover(x, y) || application_voice_shell_hover(x, y)) {
        if (g_app.shell_scene.widgets.hovered.generation &&
            ui_widget_manager_hover(&g_app.shell_scene.widgets, ui_widget_invalid_handle()))
            application_request_frame();
        return true;
    }
    const UiWidgetHandle prior = g_app.shell_scene.widgets.hovered;
    UiWidgetHandle hit = ui_widget_manager_hit_test(g_app.shell_scene.widgets, x, y);
    const auto* identity = ui_widget_manager_resolve(g_app.shell_scene.widgets, hit);
    if (identity && identity->id == static_cast<u32>(UiSemantic::channel_footer) + 1u && !account_identity_contains(x, y))
        hit = ui_widget_invalid_handle();
    if (!ui_widget_manager_hover(&g_app.shell_scene.widgets, hit)) {
        return false;
    }
    const UiWidget* widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, hit);
    if (widget && (widget->id == static_cast<u32>(UiSemantic::member_toggle) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::private_create) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::private_add) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::private_call) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::pins_toggle) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::inbox_toggle) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::threads_toggle) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::settings) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::friends_button) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::guild_menu) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::channel_footer) + 1u ||
                   widget->id == static_cast<u32>(UiSemantic::home_button) + 1u)) {
        if (!ui_widget_handle_equal(prior, hit)) {
            application_request_frame();
        }
        return true;
    }
    if (prior.generation != 0) {
        application_request_frame();
    }
    return false;
}

bool handle_navigation_hover(LPARAM lparam) noexcept {
    if (g_app.login.visible) {
        return false;
    }
    const float x = client_dip_x(lparam);
    const float y = client_dip_y(lparam);
    const UiWidgetHandle prior_guild = g_app.guild_rail_scene.widgets.hovered;
    const UiWidgetHandle prior_channel = g_app.shell_navigation_scene.widgets.hovered;
    const GuildRailHit guild = guild_rail_scene_hit_test(g_app.guild_rail_scene, x, y);
    const ShellNavigationHit channel =
        guild.id == 0 ? shell_navigation_scene_hit_test(g_app.shell_navigation_scene, x, y)
                      : ShellNavigationHit{};
    if (!ui_widget_manager_hover(&g_app.guild_rail_scene.widgets,
                                 guild.id != 0 || guild.add_server ? guild.widget : ui_widget_invalid_handle()) ||
        !ui_widget_manager_hover(&g_app.shell_navigation_scene.widgets,
                                 channel.id != 0 ? channel.widget : ui_widget_invalid_handle())) {
        return false;
    }
    const bool changed = !ui_widget_handle_equal(prior_guild, g_app.guild_rail_scene.widgets.hovered) ||
                         !ui_widget_handle_equal(prior_channel, g_app.shell_navigation_scene.widgets.hovered);
    application_channel_prefetch_hover(channel.selectable ? channel.id : 0);
    if (changed) {
        application_request_frame();
    }
    return guild.selectable || guild.add_server || guild.conversation || channel.selectable;
}

bool handle_shell_click(LPARAM lparam) noexcept {
    if (g_app.login.visible) {
        return false;
    }
    if (application_voice_shell_click(client_dip_x(lparam), client_dip_y(lparam)))
        return true;
    if (const auto header = header_plugin_hit(client_dip_x(lparam), client_dip_y(lparam)); header.action_id) {
        application_plugins_slot_activate(header.owner_id, {kUiPluginSlotHeaderToolbar, 0, 0, 0, 0}, header.action_id,
                                          header.badge.rectangle);
        return true;
    }
    // Channel/DM row and server rail badges, account panel buttons.
    if (application_plugins_surface_click(client_dip_x(lparam), client_dip_y(lparam)))
        return true;
    const UiWidgetHandle hit =
        ui_widget_manager_hit_test(g_app.shell_scene.widgets, client_dip_x(lparam), client_dip_y(lparam));
    const UiWidget* widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, hit);
    if (widget && widget->id == static_cast<u32>(UiSemantic::settings) + 1u)
        return application_settings_open();
    if (widget && widget->id == static_cast<u32>(UiSemantic::guild_menu) + 1u)
        return application_guild_menu_toggle(false);
    if (widget && widget->id == static_cast<u32>(UiSemantic::private_create) + 1u)
        return application_private_channel_picker(0);
    if (widget && widget->id == static_cast<u32>(UiSemantic::private_add) + 1u && g_app.account_ui)
        return application_private_channel_picker(g_app.account_ui->chat_state.selected_channel_id);
    if (widget && widget->id == static_cast<u32>(UiSemantic::private_call) + 1u && g_app.account_ui)
        return application_private_channel_call(g_app.account_ui->chat_state.selected_channel_id);
    if (application_private_channel_title_hit(client_dip_x(lparam), client_dip_y(lparam)))
        return application_private_channel_rename(g_app.account_ui->chat_state.selected_channel_id);
    if (widget && widget->id == static_cast<u32>(UiSemantic::friends_button) + 1u)
        return application_friends_show();
    if (widget && widget->id == static_cast<u32>(UiSemantic::threads_toggle) + 1u)
        return application_thread_browser_toggle();
    if (widget && widget->id == static_cast<u32>(UiSemantic::channel_footer) + 1u && g_app.account_ui) {
        if (voice_disconnect_contains(client_dip_x(lparam), client_dip_y(lparam)))
            return leave_voice();
        if (!account_identity_contains(client_dip_x(lparam), client_dip_y(lparam))) return false;
        const auto* node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, hit);
        return application_account_menu_open(
            hit, node ? ui_rect_intersection(node->rectangle, node->clip_rectangle) : UiRect{});
    }
    if (!widget || widget->id != static_cast<u32>(UiSemantic::member_toggle) + 1u) {
        return false;
    }
    // A one-to-one DM toggles the recipient's profile instead (disabled while too narrow).
    if (g_app.shell_scene.side_panel_profile)
        return g_app.shell_scene.side_panel_unavailable ||
               application_preferences_dm_profile_visibility(!g_app.preferences.dm_profile_visible);
    return application_preferences_member_visibility(!g_app.member_pane_visible);
}

bool handle_member_hover(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked()) return false;
    if (!g_app.member_list_scene.active || g_app.login.visible) {
        return false;
    }
    const float x = client_dip_x(lparam);
    const float y = client_dip_y(lparam);
    const UiWidgetHandle prior = g_app.member_list_scene.widgets.hovered;
    const UiWidgetHandle hit = ui_widget_manager_hit_test(g_app.member_list_scene.widgets, x, y);
    if (!ui_widget_manager_hover(&g_app.member_list_scene.widgets, hit)) {
        return false;
    }
    const bool changed = !ui_widget_handle_equal(prior, hit);
    const MemberListHit member = member_list_scene_hit_test(g_app.member_list_scene, x, y);
    if (changed) {
        if (!refresh_retained_projection())
            return false;
    }
    return member.user_id != 0;
}

namespace {
MessageHoverScrollGate g_message_hover_gate{};
// The media pump re-evaluates which avatars play when this changes.
void set_message_avatar_hover(u64 message_id) noexcept {
    message_media_controller_set_avatar_hover(&g_app.message_media, message_id);
}

u64 message_avatar_at(float x, float y) noexcept {
    const MessageScene& scene = g_app.message_scene;
    if (!scene.timeline || !scene.timeline->store || !ui_rect_contains(scene.viewport, x, y))
        return 0;
    for (u32 i = 0; i < scene.count; ++i) {
        const MessageSceneRowWidget& row = scene.rows[i];
        const UiLayoutNode* avatar = row.grouped ? nullptr : message_scene_layout_node(scene, row.avatar);
        if (!avatar || !ui_rect_contains(avatar->rectangle, x, y))
            continue;
        const MessageRecord* record = message_store_resolve(*scene.timeline->store, row.message);
        return record ? record->id : 0;
    }
    return 0;
}

bool clear_message_hover() noexcept {
    set_message_avatar_hover(0);
    bool changed = false;
    if (!message_action_bar_close(&g_app.message_action_bar, &changed))
        return false;
    if (g_app.message_scene.widgets.hovered.generation) {
        if (!ui_widget_manager_hover(&g_app.message_scene.widgets, ui_widget_invalid_handle()))
            return false;
        changed = true;
    }
    return !changed || refresh_retained_projection();
}
} // namespace

bool application_message_hover_observe_scroll() noexcept {
    const auto& timeline = g_app.message_view.timeline;
    if (!g_app.message_scene.active || !timeline.list)
        return true;
    // Anchoring (history prepends, resizes, live-tail growth) moves the
    // origin with the content; only deliberate scrolling changes this offset.
    const double offset = static_cast<double>(timeline.scroll_units) - timeline.scroll_origin_units;
    if (!message_hover_scroll_gate_observe(&g_message_hover_gate, timeline.reset_generation, offset))
        return true;
    return clear_message_hover();
}

bool application_message_hover_leave() noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active)
        return true;
    return clear_message_hover();
}

bool handle_message_hover(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked()) return false;
    if (!g_app.message_scene.active || g_app.login.visible) {
        return false;
    }
    const float x = client_dip_x(lparam);
    const float y = client_dip_y(lparam);
    // After a scroll the content moved under a still pointer; the official client keeps
    // the toolbar hidden until the pointer itself moves.
    if (!message_hover_scroll_gate_pointer(&g_message_hover_gate, x, y))
        return false;
    set_message_avatar_hover(message_avatar_at(x, y));
    const MessageActionBarPointerResult tracked = message_action_bar_track_pointer(
        &g_app.message_action_bar, &g_app.message_scene, x, y,
        g_app.account_ui ? &g_app.account_ui->reaction_preferences : nullptr,
        +[](void*, u64 id) noexcept { return application_message_action_bar_capabilities(id); });
    if (!tracked.ok()) {
        return false;
    }
    if (tracked.changed) {
        // The action-bar panel is paint geometry, but its quick reactions are
        // glyph instances in the retained content-text stream.  Rebuild both
        // sides of the overlay on every open/move/close transition so a new
        // panel can never be paired with glyphs retained at the previous row.
        if (!refresh_retained_projection()) {
            return false;
        }
    }
    return tracked.action_bar.inside || tracked.hovered_message_id != 0;
}

bool handle_member_click(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked()) return false;
    if (!g_app.member_list_scene.active || g_app.login.visible) {
        return false;
    }
    const float x = client_dip_x(lparam);
    const float y = client_dip_y(lparam);
    const MemberListHit hit = member_list_scene_hit_test(g_app.member_list_scene, x, y);
    if (hit.user_id == 0) {
        return false;
    }
    u64 badge_user = 0;
    const auto badge = member_list_plugin_hit(g_app.member_list_scene, x, y, &badge_user);
    if (badge.action_id) {
        application_plugins_slot_activate(badge.owner_id, {kUiPluginSlotMemberBadge, 0, 0, badge_user, 0}, badge.action_id,
                                          badge.badge.rectangle);
        return true;
    }
    member_list_scene_focus_user(&g_app.member_list_scene, hit.user_id);
    const auto* row = member_list_scene_layout_node(g_app.member_list_scene, hit.widget);
    return application_profile_open(hit.user_id, row ? row->rectangle : g_app.member_list_scene.viewport,
                                    ProfileAnchor::member);
}

} // namespace maicord::app_detail
