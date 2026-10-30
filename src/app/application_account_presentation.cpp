#include "application_guild_community.hpp"
#include "application_guild_banner.hpp"
#include "application_call.hpp"
#include "application_thread_starter.hpp"
#include "application_stage.hpp"
#include "application_voice_channels.hpp"
#include "application_account_presentation.hpp"
#include "application_account_pump.hpp"
#include "application_account_text_retry.hpp"
#include "application_account_module.hpp"
#include "application_profile.hpp"
#include "application_dm_profile.hpp"
#include "application_composer_input.hpp"
#include "application_message_feedback.hpp"
#include "application_composer.hpp"
#include "application_commands.hpp"
#include "application_attachments.hpp"
#include "application_friends.hpp"
#include "application_settings.hpp"
#include "application_pinned_messages.hpp"
#include "application_thread_browser.hpp"
#include "application_message_edit.hpp"
#include "maicord/ui/message_typing_presentation.hpp"
#include "application_thread_header.hpp"
#include "application_channel_authority.hpp"
#include "application_reply_draft.hpp"
#include "application_poll.hpp"
#include "application_search.hpp"
#include "application_login_ui.hpp"
#include "application_text_form.hpp"
#include "application_forum_form.hpp"
#include "application_theme_editor.hpp"
#include "application_composer_text_input.hpp"
#include "application_voice.hpp"
#include "application_voice_message.hpp"
#include "application_plugins.hpp"
#include "application_private_channel.hpp"
#include "application_user_settings.hpp"
#include "application_message_intro.hpp"
#include "application_self_status.hpp"

#include "application_state.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "application_log_fields.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/theme_style_projection.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
namespace {
const UiNode* shell_node(UiSemantic semantic) noexcept {
    return ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, semantic));
}
} // namespace
#if !defined(MAICORD_ACCOUNT_TEXT_REFRESH_RUNTIME_TEST)
namespace {
void log_member_presentation() noexcept {
    if (!g_app.account_ui)
        return;
    const auto& ui = *g_app.account_ui;
    const auto& state = ui.chat_state;
    const auto* selected = chat_state_store_find_channel(state, state.selected_channel_id);
    if (!selected || !selected->guild_id)
        return;
    const auto& projection = ui.member_list;
    const auto& scene = g_app.member_list_scene;
    const u64 expected_list =
        projection.thread_mode ? selected->id : member_list_channel_id(state, selected->id);
    u32 visible_members = 0, visible_named = 0;
    for (u32 index = 0; index < scene.count; ++index) {
        if (!member_list_scene_row_resident(scene, index) ||
            scene.rows[index].kind != MemberListRowKind::member)
            continue;
        ++visible_members;
        const auto* profile = user_store_find(ui.user_store, scene.rows[index].user_id);
        if (profile && profile->name_bytes && profile->username_bytes)
            ++visible_named;
    }
    TraceRecord trace("member_presentation");
    const u64 fields[] = {projection.guild_id == selected->guild_id,
                          expected_list && projection.list_id == expected_list,
                          projection.thread_mode,
                          projection.authoritative,
                          projection.awaiting_sync,
                          projection.count,
                          projection.resident_count,
                          scene.active,
                          ui_rect_has_area(scene.viewport),
                          scene.count,
                          visible_members,
                          visible_named};
    trace_fields(trace, "same_guild\0same_list\0thread_mode\0authoritative\0awaiting_sync\0logical_count\0"
                        "resident_count\0scene_active\0pane_has_area\0scene_rows\0visible_members\0"
                        "visible_named_profiles",
                 fields);
    trace.write("member_hydration");
}
// The caller's own status for private member rows; unknown until the session
// reports it for this account.
GuildMemberStatus self_member_status(const AccountUiRuntime& ui) noexcept {
    const auto status = application_self_status_snapshot();
    return status.authoritative && status.profile_id == g_app.account.status.profile_id &&
                   status.self_user_id == ui.chat_state.self_user_id && status.status <= GatewayUserStatus::invisible
               ? static_cast<GuildMemberStatus>(static_cast<u8>(status.status) + 1)
               : GuildMemberStatus::unknown;
}
} // namespace

Utf8View application_account_notice() noexcept {
    const auto required = account_requirement_notice(g_app.account.status.required_action);
    if (required.bytes)
        return required;
    const auto mutation = application_message_mutation_notice_text();
    if (mutation.bytes)
        return mutation;
    if (g_app.account_ui && g_app.message_rejection_channel_id == g_app.account_ui->active_channel_id) {
        const auto rejection = api_rejection_notice(g_app.message_rejection);
        if (rejection.bytes)
            return rejection;
    }
    const auto sync = application_account_sync_notice();
    if (sync.bytes)
        return sync;
    const auto plugin = application_plugins_composer_notice();
    if (plugin.bytes)
        return plugin;
    return application_composer_status_notice();
}

UiRect application_viewport_dips() noexcept {
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    return UiRect{0.0f, 0.0f, static_cast<float>(g_app.renderer.width) * scale,
                  static_cast<float>(g_app.renderer.height) * scale};
}

bool synchronize_navigation_view() noexcept {
    if (!g_app.account_ui) {
        return true;
    }
    AccountUiRuntime& ui = *g_app.account_ui;
    ui.navigation_view.voice_states = &ui.voice_states;
    ui.navigation_view.private_rows = application_private_channel_rows();
    ui.navigation_view.stage_instances = &application_stage_instances();
    ui.navigation_view.voice_extras = &application_voice_channels_extras();
    application_guild_community_navigation(&ui.navigation_view);
    application_private_channel_navigation(&ui.navigation_view);
    // The official Home badge: incoming friend requests plus message requests.
    u32 home_badge = ui.chat_state.private_channels
                         ? private_channel_store_request_count(*ui.chat_state.private_channels, false)
                         : 0;
    if (const auto* relationships = application_friends_relationships())
        for (u32 i = 0; i < relationships->count; ++i)
            home_badge += relationships->records[i].type == RelationshipType::incoming;
    if (ui.navigation_view.home_badge != home_badge) {
        ui.navigation_view.home_badge = home_badge;
        ++ui.navigation_view.requests_revision;
    }
    const bool friends_mode = application_friends_active();
    if (ui.navigation_view.friends_mode != friends_mode) {
        ui.navigation_view.friends_mode = friends_mode;
        ++ui.navigation_view.requests_revision;
    }
    const bool voice_current = ui.navigation_view.source_voice_revision == ui.voice_states.revision &&
                               ui.navigation_view.source_stage_revision == application_stage_instances().revision &&
                               ui.navigation_view.source_extras_revision ==
                                   application_voice_channels_extras().revision;
    if (ui.navigation_view.source_guild_mutations == ui.chat_state.guild_mutations &&
        ui.navigation_view.source_channel_mutations == ui.chat_state.channel_mutations &&
        ui.navigation_view.source_selection_revision == ui.chat_state.selection_revision &&
        !ui.navigation_view.connected_voice_stale &&
        ui.navigation_view.source_folder_revision == ui.navigation_view.folder_revision &&
        ui.navigation_view.source_events_revision == ui.navigation_view.events_revision &&
        ui.navigation_view.source_requests_revision == ui.navigation_view.requests_revision) {
        return voice_current || !navigation_view_refresh_voice(&ui.navigation_view) ||
               synchronize_shell_widget_scenes();
    }
    const NavigationViewResult rebuilt =
        navigation_view_rebuild(&ui.navigation_view, ui.chat_state);
    if (!rebuilt.ok()) {
        log_u64(LogLevel::error, "ui", "navigation_view_error", static_cast<u64>(rebuilt.error));
        return false;
    }
    return synchronize_shell_widget_scenes();
}

bool synchronize_shell_widget_scenes() noexcept {
    const auto* styles = application_theme().styles;
    message_composer_scene_bind_theme_styles(&g_app.message_composer_scene, styles);
    message_scene_bind_theme_styles(&g_app.message_scene, styles);
    application_sync_attachment_drop_target();
    const bool friends = application_friends_active();
    if (g_app.account_ui && !friends) {
        AccountUiRuntime& ui = *g_app.account_ui;
        const auto* selected =
            chat_state_store_find_channel(ui.chat_state, ui.chat_state.selected_channel_id);
        const bool ordinary_thread = selected && (selected->type == 11 || selected->type == 12);
        const bool thread_source =
            ordinary_thread && selected->thread_archived_known && !selected->thread_archived &&
            thread_member_store_owns(ui.thread_member_store, selected->guild_id, selected->id);
        const bool awaiting_restoration = !selected && ui.chat_state.restore_channel_id &&
                                          !ui.member_list.thread_mode && ui.member_list.authoritative &&
                                          ui.member_list.guild_id && ui.member_list.count;
        const u64 list_id =
            awaiting_restoration
                ? ui.member_list.list_id
                : member_list_channel_cached_id(&g_app.member_list_identity, ui.chat_state,
                                                ui.chat_state.selected_channel_id);
        const u64 guild_id =
            awaiting_restoration ? ui.member_list.guild_id : ui.chat_state.selected_guild_id;
        const u32 prior_extent = ui.member_list.authoritative && ui.member_list.guild_id == guild_id &&
                                         ui.member_list.list_id == list_id
                                     ? member_list_view_count(ui.member_list)
                                     : 0;
        const MemberListViewResult members =
            thread_source
                ? member_list_view_thread(&ui.member_list, ui.thread_member_store,
                                                        ui.guild_member_store, ui.user_store,
                                                        ui.chat_state, selected->guild_id, selected->id)
            : selected && selected->type == 3
                ? member_list_view_private(&ui.member_list, ui.user_store, ui.chat_state, selected->id,
                                           {application_friends_relationships(), self_member_status(ui)})
                : member_list_view_rebuild(&ui.member_list, ui.guild_member_store,
                                                         ui.user_store, ui.chat_state,
                                                         ordinary_thread ? 0 : guild_id, list_id);
        if (!members.ok()) {
            log_u64(LogLevel::error, "ui", "member_list_projection_error", static_cast<u64>(members.error));
            return false;
        }
        if (members.changed && ui.member_list.thread_mode && ui.member_list.incomplete)
            log_u64(LogLevel::warning, "ui", "thread_member_list_incomplete_retained_ids",
                    ui.member_list.member_count);
        const auto* scope = guild_member_store_list(ui.guild_member_store, guild_id, list_id);
        if (!ordinary_thread && ((selected && selected->guild_id) || awaiting_restoration) &&
            prior_extent && (!scope || !scope->authoritative))
            member_list_view_await_sync(&ui.member_list, guild_id, list_id, prior_extent);
    }
    const NavigationView* navigation =
        g_app.account_ui ? &g_app.account_ui->navigation_view : nullptr;
    const UiNode* guild_rail = shell_node(UiSemantic::guild_navigation);
    const UiNode* channel_navigation = shell_node(UiSemantic::channel_navigation);
    const UiNode* message_view = shell_node(UiSemantic::message_view);
    const UiNode* composer = shell_node(UiSemantic::composer);
    const UiNode* members = shell_node(UiSemantic::member_sidebar);
    const UiRect guild_viewport = guild_rail ? guild_rail->rectangle : UiRect{};
    const UiRect channel_viewport = channel_navigation ? channel_navigation->rectangle : UiRect{};
    UiRect message_viewport = !friends && message_view ? message_view->rectangle : UiRect{};
    const bool selected_readable =
        !friends && g_app.account_ui &&
        g_app.account_ui->active_channel_id == g_app.account_ui->chat_state.selected_channel_id &&
        application_channel_readable(g_app, g_app.account_ui->active_channel_id);
    const u32 header_revision = g_app.thread_header.revision;
    const float thread_height = application_thread_header_project(
        &g_app.thread_header, &g_app.shell_scene,
        !friends && g_app.account_ui ? g_app.account_ui->chat_state.selected_channel_id : 0,
        message_viewport, message_viewport);
    message_viewport.top = minimum(message_viewport.bottom, message_viewport.top + thread_height);
    if (!selected_readable)
        reset_value(&message_viewport);
    if (header_revision != g_app.thread_header.revision)
        content_text_retention_invalidate(&g_app.renderer.content_text_retention);
    char reply_label_storage[256]{};
    Utf8View reply_label{};
    u64 composer_context_message_id = 0;
    if (!friends && g_app.message_edit.editor_active) {
        constexpr Utf8View edit_label = utf8_literal("Editing message");
        copy_bytes(reply_label_storage, edit_label.data, edit_label.bytes);
        reply_label = Utf8View{reply_label_storage, edit_label.bytes};
        composer_context_message_id = g_app.message_edit.message_id;
    } else if (!friends && g_app.account_ui) {
        reply_label = application_reply_draft_label({reply_label_storage, sizeof(reply_label_storage)});
        composer_context_message_id = application_reply_draft().message_id;
    }
    // A server waiting for its rules shows why the composer is held.
    const Utf8View community_notice = !friends ? application_guild_community_composer_notice() : Utf8View{};
    MessageComposerProjectionContext composer_projection{
        g_app.account_ui ? &g_app.account_ui->user_store : nullptr,
        g_app.account_ui ? &g_app.account_ui->chat_state : nullptr,
        g_app.account_ui ? g_app.account_ui->chat_state.selected_guild_id : 0, community_notice};
    application_commands_composer(&composer_projection);
    char typing_label_storage[kMessageTypingLabelMaximumBytes]{};
    Utf8View typing_label{};
    if (!friends && g_app.account_ui && g_app.account_ui->active_channel_id != 0) {
        const MessageTypingPresentationResult built = message_typing_presentation_build(
            g_app.account_ui->typing_store, g_app.account_ui->user_store,
            g_app.account_ui->active_channel_id, g_app.account_ui->chat_state.self_user_id,
            Slice<char>{typing_label_storage, kMessageTypingLabelMaximumBytes});
        if (!built.ok()) {
            log_u64(LogLevel::warning, "ui", "typing_label_error", static_cast<u64>(built.error));
        } else {
            typing_label = Utf8View{typing_label_storage, built.bytes};
        }
    }
    bool voice_notice_error = false;
    const auto voice_notice = !friends ? application_voice_message_notice(&voice_notice_error) : Utf8View{};
    if (voice_notice.bytes)
        typing_label = voice_notice;
    const auto account_notice = application_account_notice();
    if (account_notice.bytes)
        typing_label = account_notice;
    auto composer_attachments = application_attachment_input();
    composer_attachments.retained_content = application_message_edit_retains_content();
    const auto* read_channel =
        !friends && g_app.account_ui &&
                g_app.account_ui->active_channel_id == g_app.account_ui->chat_state.selected_channel_id
            ? chat_state_store_find_channel(g_app.account_ui->chat_state,
                                               g_app.account_ui->active_channel_id)
            : nullptr;
    const bool show_unread = read_channel && read_channel->read_state_known &&
                             (read_channel->actions & channel_action_read_history) &&
                             read_channel->last_read_message_id < read_channel->last_message_id;
    // Voice messages: the microphone, or the recording bar, for this channel.
    const auto* selected_row =
        navigation ? navigation_view_find_channel(*navigation, navigation->selected_channel_id) : nullptr;
    message_composer_scene_set_voice(
        &g_app.message_composer_scene,
        !friends && g_app.account_ui
            ? application_voice_message_view(selected_row ? selected_row->actions : 0,
                                             g_app.account.status.required_action != AccountRequirement::none ||
                                                 community_notice.bytes != 0)
            : nullptr);
    // The selected server's banner sets the channel list's top inset.
    application_guild_banner_project(clock_monotonic_milliseconds());
    u64 stage = 1;
    do {
#define MAICORD_SHELL_SCENE_STEP(expression)                                                               \
    if (!(expression)) {                                                                                   \
        break;                                                                                             \
    }                                                                                                      \
    ++stage

        MAICORD_SHELL_SCENE_STEP(guild_rail_scene_update(&g_app.guild_rail_scene, navigation,
                                                         guild_viewport, g_app.guild_rail_scene.scroll_dips)
                                     .ok());
        MAICORD_SHELL_SCENE_STEP(shell_navigation_scene_update(&g_app.shell_navigation_scene, navigation,
                                                               channel_viewport,
                                                               g_app.shell_navigation_scene.scroll_dips)
                                     .ok());
        g_app.message_scene.pixel_scale = static_cast<float>(maximum(g_app.renderer.dpi, 96u)) / 96.0f;
        MAICORD_SHELL_SCENE_STEP(message_scene_set_unread_cursor(
            &g_app.message_scene, show_unread ? read_channel->id : 0,
            show_unread ? read_channel->last_read_message_id : 0, show_unread));
        application_message_intro_synchronize();
        MAICORD_SHELL_SCENE_STEP(message_scene_update(&g_app.message_scene, message_viewport).ok());
        MAICORD_SHELL_SCENE_STEP(
            message_composer_scene_update(
                &g_app.message_composer_scene, &g_app.message_composer,
                composer ? composer->rectangle : UiRect{},
                selected_row && (selected_row->actions & channel_action_send_messages) != 0,
                reply_label.bytes != 0 ? composer_context_message_id : 0, reply_label,
                g_app.account_ui ? &composer_projection : nullptr, typing_label, composer_attachments,
                g_app.account.status.required_action != AccountRequirement::none ||
                    community_notice.bytes != 0,
                application_reply_draft().mention_reply_author, !g_app.message_edit.editor_active,
                application_message_mutation_notice_is_error() ||
                    application_composer_status_notice_is_error() || voice_notice_error,
                !application_reply_draft().message_id ||
                    application_channel_readable(g_app, application_reply_draft().channel_id),
                account_notice.bytes && account_notice.data == application_plugins_composer_notice().data
                    ? application_plugins_composer_notice_tone()
                    : u8{0})
                .ok());
        // Someone typing animates the dots; a notice in that row does not.
        g_app.message_composer_scene.typing_dots = typing_label.bytes && typing_label.data == typing_label_storage;
        if (g_app.message_composer_pointer_selection &&
            !g_app.message_composer_scene.widgets.captured.generation) {
            end_message_composer_pointer_selection();
        }
        g_app.member_list_scene.viewport_changed = [](void*) noexcept {
            synchronize_member_subscription(clock_monotonic_milliseconds());
        };
        g_app.member_list_scene.measure =
            g_app.account_ui ? &g_app.account_ui->text_measure_cache : nullptr;
        g_app.member_list_scene.media_cache =
            g_app.account_ui ? &g_app.account_ui->member_media_cache : nullptr;
        const u32 member_rebuilds = g_app.member_list_scene.rebuilds;
        const auto member_scene = member_list_scene_update(
            &g_app.member_list_scene, g_app.account_ui ? &g_app.account_ui->member_list : nullptr,
            g_app.account_ui ? &g_app.account_ui->guild_member_store : nullptr,
            g_app.account_ui ? &g_app.account_ui->user_store : nullptr,
            g_app.account_ui ? &g_app.account_ui->chat_state : nullptr,
            !friends && members && !g_app.shell_scene.side_panel_profile ? members->rectangle : UiRect{},
            g_app.member_list_scene.scroll_dips);
        MAICORD_SHELL_SCENE_STEP(member_scene.ok());
        if (g_app.member_list_scene.rebuilds != member_rebuilds)
            log_member_presentation();
        MAICORD_SHELL_SCENE_STEP(
            message_action_bar_synchronize(&g_app.message_action_bar, g_app.message_scene));

#undef MAICORD_SHELL_SCENE_STEP
        return true;
    } while (false);
    log_u64(LogLevel::error, "ui", "shell_widget_scene_stage_failed", stage);
    return false;
}

bool synchronize_shell_layout() noexcept {
    if (!g_app.renderer_ready) {
        return true;
    }
    const auto* styles = application_theme().styles;
    const auto& theme_metrics = ui_theme_metrics(styles);
    message_composer_scene_bind_theme_styles(&g_app.message_composer_scene, styles);
    message_scene_bind_theme_styles(&g_app.message_scene, styles);
    const float width_dip = static_cast<float>(g_app.renderer.width) * 96.0f /
                            static_cast<float>(maximum<u32>(g_app.renderer.dpi, 1));
    const float height_dip = static_cast<float>(g_app.renderer.height) * 96.0f /
                             static_cast<float>(maximum<u32>(g_app.renderer.dpi, 1));
    const bool friends = application_friends_active();
    UiShellLayoutMetrics metrics = friends ? kFriendsShellLayoutMetrics : kGuildChannelShellLayoutMetrics;
    metrics.guild_rail_width = theme_metrics.shell_rail_width;
    metrics.navigation_width = theme_metrics.shell_sidebar_width;
    metrics.member_sidebar_width = friends ? 0.f : theme_metrics.shell_member_width;
    metrics.title_bar_height = theme_metrics.shell_header_height;
    metrics.navigation_header_height = theme_metrics.shell_header_height;
    metrics.navigation_footer_reserve = application_voice_panel_reserve();
    if (metrics.member_sidebar_top != 0.f)
        metrics.member_sidebar_top = theme_metrics.shell_header_height;
    metrics.navigation_gutter_before = theme_metrics.shell_gutter;
    metrics.content_gutter_before = theme_metrics.shell_gutter;
    metrics.flags = static_cast<u16>(metrics.flags | shell_header_flags(friends));
    const UiShellView view = friends ? UiShellView::friends
                             : g_app.account_ui && g_app.account_ui->chat_state.home_selected
                                 ? UiShellView::private_channels
                                 : UiShellView::guild_channel;
    const bool home = g_app.account_ui && g_app.account_ui->chat_state.home_selected;
    const auto* selected =
        g_app.account_ui
            ? chat_state_store_find_channel(g_app.account_ui->chat_state,
                                               g_app.account_ui->chat_state.selected_channel_id)
            : nullptr;
    const bool group_dm = home && selected && selected->type == 3;
    const bool empty_home = home && !g_app.account_ui->chat_state.selected_channel_id;
    metrics.navigation_width =
        g_app.channel_pane_width > 0.0f ? g_app.channel_pane_width : metrics.navigation_width;
    metrics.member_sidebar_width =
        g_app.member_pane_width > 0.0f ? g_app.member_pane_width : metrics.member_sidebar_width;
    if (!g_app.member_pane_visible || (home && !group_dm)) {
        metrics.flags = static_cast<u16>(metrics.flags & ~static_cast<u16>(ui_shell_layout_members));
    }
    if (friends || (home && !group_dm))
        metrics.member_sidebar_width = 0.0f;
    // A one-to-one DM: the recipient's profile takes the slot (dm_profile_panel.hpp).
    const UiSidePanelInput side_input = application_side_panel_input();
    const UiSidePanelPlan side = ui_side_panel_plan(side_input);
    g_app.shell_scene.side_panel_profile = !friends && home && side_input.channel_type == 1;
    g_app.shell_scene.side_panel_unavailable = g_app.shell_scene.side_panel_profile && side.unavailable;
    if (g_app.shell_scene.side_panel_profile) {
        metrics.member_sidebar_width = side.width;
        if (side.panel == UiSidePanel::profile)
            metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_members);
    }
    if (empty_home)
        metrics.flags = static_cast<u16>(metrics.flags & ~static_cast<u16>(ui_shell_layout_composer));
    // Search results take the member-pane slot, so the chat column and its
    // composer stay whole beside the pane instead of running underneath it.
    if (!friends && application_search_active()) {
        const float content_left = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content).left;
        metrics.member_sidebar_width = ui_search_panel_width(width_dip - content_left);
        metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_members);
    }
    // Text in voice: the call view keeps the left of the content column and
    // the channel chat takes the rest; the member list makes way.
    if (!friends) {
        const bool navigation = (metrics.flags & ui_shell_layout_navigation) != 0;
        const float content_left = metrics.guild_rail_width + metrics.content_gutter_before +
                                   (navigation ? metrics.navigation_width + metrics.navigation_gutter_before : 0.f);
        if (const float split = application_call_chat_split(width_dip - content_left); split > 0) {
            metrics.content_gutter_before += split;
            metrics.flags = static_cast<u16>(metrics.flags & ~static_cast<u16>(ui_shell_layout_members));
        }
    }
    const bool typing_visible =
        !friends &&
        (application_account_notice().bytes != 0 ||
         (g_app.account_ui && typing_store_channel_count(
                                  g_app.account_ui->typing_store, g_app.account_ui->active_channel_id,
                                  g_app.account_ui->chat_state.self_user_id) != 0));
    const float composer_chrome =
        friends ? 0.f
                : message_composer_scene_chrome_height(
                      application_reply_draft().message_id != 0 || g_app.message_edit.editor_active,
                      typing_visible, message_composer_attachment_row(application_attachment_input()), styles);
    metrics.composer_height =
        friends ? 0.f
                : message_composer_scene_cached_height(
                      g_app.message_composer_scene,
                      application_reply_draft().message_id != 0 || g_app.message_edit.editor_active,
                      typing_visible, message_composer_attachment_row(application_attachment_input()),
                      ui_scene_composer_width(metrics, width_dip));
    const auto update_metrics_revision = [&]() noexcept {
        metrics.revision = 1'000u + ui_extent_units(metrics.composer_height) * 31u +
                           ui_extent_units(metrics.navigation_width) * 131u +
                           ui_extent_units(metrics.member_sidebar_width) * 521u +
                           ui_extent_units(metrics.guild_rail_width) * 1'031u +
                           ui_extent_units(metrics.title_bar_height) * 2'053u +
                           ui_extent_units(metrics.navigation_header_height) * 4'099u +
                           ui_extent_units(metrics.member_sidebar_top) * 8'209u +
                           ui_extent_units(metrics.navigation_gutter_before) * 16'411u +
                           ui_extent_units(metrics.content_gutter_before) * 32'771u +
                           (styles && styles->ready ? styles->geometry_revision : 0u) * 65'537u +
                           static_cast<u32>(metrics.flags) * 7u + (g_app.member_pane_visible ? 1u : 0u) +
                           ui_extent_units(metrics.navigation_footer_reserve) * 3u;
    };
    update_metrics_revision();
    u64 stage = 1;
    do {
#define MAICORD_SHELL_LAYOUT_STEP(expression)                                                              \
    if (!(expression)) {                                                                                   \
        break;                                                                                             \
    }                                                                                                      \
    ++stage

        MAICORD_SHELL_LAYOUT_STEP(
            ui_scene_layout_with_metrics(&g_app.shell_scene, view, metrics, width_dip, height_dip));
        MAICORD_SHELL_LAYOUT_STEP(synchronize_shell_widget_scenes());
        if (g_app.message_composer_scene.active) {
            MAICORD_SHELL_LAYOUT_STEP(
                renderer_bind_message_composer_scene(&g_app.renderer, &g_app.message_composer_scene));
            float measured = 0;
            MAICORD_SHELL_LAYOUT_STEP(renderer_measure_message_composer(&g_app.renderer, &measured));
            g_app.message_composer_scene.measured_row_height =
                measured - composer_chrome -
                message_composer_scene_toolbar_height(ui_rect_width(g_app.message_composer_scene.viewport),
                                                      styles);
            if (measured != metrics.composer_height) {
                metrics.composer_height = measured;
                update_metrics_revision();
                MAICORD_SHELL_LAYOUT_STEP(
                    ui_scene_layout_with_metrics(&g_app.shell_scene, view, metrics, width_dip, height_dip));
                MAICORD_SHELL_LAYOUT_STEP(synchronize_shell_widget_scenes());
                MAICORD_SHELL_LAYOUT_STEP(renderer_measure_message_composer(&g_app.renderer, &measured));
                g_app.message_composer_scene.measured_row_height =
                    measured - composer_chrome -
                    message_composer_scene_toolbar_height(
                        ui_rect_width(g_app.message_composer_scene.viewport), styles);
            }
        }
        MAICORD_SHELL_LAYOUT_STEP(
            !g_app.reaction_viewer.active ||
            reaction_viewer_layout(&g_app.reaction_viewer, application_viewport_dips()));
        application_profile_layout(application_viewport_dips());
        MAICORD_SHELL_LAYOUT_STEP(renderer_bind_ui_scene(&g_app.renderer, &g_app.shell_scene));
        MAICORD_SHELL_LAYOUT_STEP(renderer_bind_thread_header(&g_app.renderer, &g_app.thread_header));
        MAICORD_SHELL_LAYOUT_STEP(renderer_bind_guild_rail_scene(&g_app.renderer, &g_app.guild_rail_scene));
        MAICORD_SHELL_LAYOUT_STEP(
            renderer_bind_shell_navigation_scene(&g_app.renderer, &g_app.shell_navigation_scene));
        MAICORD_SHELL_LAYOUT_STEP(renderer_bind_message_scene(&g_app.renderer, &g_app.message_scene));
        MAICORD_SHELL_LAYOUT_STEP(
            renderer_bind_message_action_bar(&g_app.renderer, &g_app.message_action_bar));
        MAICORD_SHELL_LAYOUT_STEP(
            renderer_bind_message_composer_scene(&g_app.renderer, &g_app.message_composer_scene));
        MAICORD_SHELL_LAYOUT_STEP(
            renderer_bind_member_list_scene(&g_app.renderer, &g_app.member_list_scene));
        MAICORD_SHELL_LAYOUT_STEP(renderer_bind_popup_scene(&g_app.renderer, &g_app.popup_scene));
        MAICORD_SHELL_LAYOUT_STEP(renderer_bind_reaction_viewer(&g_app.renderer, &g_app.reaction_viewer));
        MAICORD_SHELL_LAYOUT_STEP(application_friends_synchronize());

#undef MAICORD_SHELL_LAYOUT_STEP
        return true;
    } while (false);
    log_u64(LogLevel::error, "ui", "shell_layout_stage_failed", stage);
    return false;
}

bool rebuild_account_text_cache(bool scroll_refresh, bool scroll_moving) noexcept {
    if (!g_app.account_ui) {
        return true;
    }
    // Beneath the Settings modal the shell keeps converging (new messages,
    // a resize's wrapping); the modal's own text is restored afterwards.
    if (application_settings_active() && !renderer_theme_editor_visible(g_app.renderer) &&
        renderer_settings_hides_shell(g_app.renderer))
        return application_settings_synchronize();
    if (application_friends_active())
        return application_friends_synchronize();
    AccountUiRuntime& ui = *g_app.account_ui;
    MessageTextCoordinatorTrace trace{};
    ui.message_poll_binding.now_ms = clock_unix_milliseconds();
    ui.message_poll_binding.can_vote =
        g_app.account.status.gateway_ready != 0 &&
        g_app.account.status.required_action == AccountRequirement::none;
    if (!application_poll_synchronize())
        return false;
    const bool reveal_spoilers = application_user_settings_reveal_spoilers(ui.active_channel_id);
    if (ui.message_text.reveal_spoilers != reveal_spoilers)
        ui.message_text.shift_layout_passes = 0;
    ui.message_text.reveal_spoilers = reveal_spoilers;
    if (!g_app.message_scene.active) {
        return message_action_bar_synchronize(&g_app.message_action_bar, g_app.message_scene) &&
               renderer_rebuild_content_text(&g_app.renderer, application_theme());
    }
    const MessageTextCoordinatorResult coordinated =
        message_text_coordinator_converge(MessageTextCoordinatorRequest{
            &g_app.message_scene, &ui.message_text, &ui.text_measure_cache, &ui.chat_state,
            &ui.user_store, &ui.navigation_view, &application_theme(),
            +[](void* opaque, const Theme& theme) noexcept {
                // Convergence may recycle message-row widget handles after
                // the shell scenes were synchronized.  Rebind (or close) the
                // hover action bar against that final scene before its emoji
                // commands enter the retained text stream. The selected poll
                // card follows its converged row.
                application_poll_synchronize();
                return message_action_bar_synchronize(&g_app.message_action_bar, g_app.message_scene) &&
                       renderer_rebuild_content_text(static_cast<D3D11Renderer*>(opaque), theme);
            },
            &g_app.renderer, clock_unix_milliseconds() / 1'000,
            renderer_text_measure_revision(g_app.renderer), &trace, scroll_refresh, scroll_moving});
    if (!coordinated.ok()) {
        const u64 failed[] = {static_cast<u64>(coordinated.error),
                              static_cast<u64>(coordinated.scene_error),
                              static_cast<u64>(coordinated.presentation_error),
                              coordinated.passes,
                              ui.message_text.failure_message_id,
                              ui.message_text.failure_source_index,
                              static_cast<u64>(ui.message_text.failure_stage),
                              static_cast<u64>(ui.message_text.failure_operation),
                              ui.message_text.failure_item_index,
                              static_cast<u64>(ui.message_text.failure_flow_error),
                              ui.message_text.rich.skipped_components};
        log_u64_fields(LogLevel::error, "ui",
                       "message_text_coordinator_error\0message_text_coordinator_scene_error\0"
                       "message_text_coordinator_presentation_error\0message_text_coordinator_passes\0"
                       "message_text_failure_message_id\0message_text_failure_source_index\0"
                       "message_text_failure_stage\0message_text_failure_operation\0"
                       "message_text_failure_item_index\0message_text_failure_flow_error\0"
                       "message_text_skipped_components",
                       failed);
        for (u32 pass = 0; pass < trace.count; ++pass) {
            const MessageTextCoordinatorPass& item = trace.passes[pass];
            const u64 values[] = {pass + 1u,      item.flags,         item.scroll_units, item.total_units,
                                  item.first,     item.count,         item.command_count, item.measurement_count};
            log_u64_fields(LogLevel::error, "ui",
                           "message_text_trace_pass\0message_text_trace_flags\0message_text_trace_scroll_units\0"
                           "message_text_trace_total_units\0message_text_trace_first\0message_text_trace_count\0"
                           "message_text_trace_commands\0message_text_trace_measurements",
                           values);
        }
        const u64 render[] = {static_cast<u64>(ui.renderer_text.last_error),
                              static_cast<u64>(ui.renderer_text.last_shape_error),
                              static_cast<u64>(static_cast<u32>(ui.renderer_text.native_error)),
                              ui.renderer_text.quad_count, ui.renderer_text.batch_count};
        log_u64_fields(LogLevel::error, "render",
                       "content_text_build_error\0content_text_shape_error\0content_text_native_error\0"
                       "content_text_quads\0content_text_batches",
                       render);
        const bool fallback_built = renderer_rebuild_shell_text(&g_app.renderer, application_theme());
        log_u64(fallback_built ? LogLevel::warning : LogLevel::error, "render",
                "content_text_shell_fallback", fallback_built ? 1u : 0u);
    }
    return coordinated.ok();
}

#endif
// The header's controls (pins, threads, DM actions) by what the selected
// channel allows now: its type and permissions may arrive after the layout.
u16 shell_header_flags(bool friends) noexcept {
    u16 flags = 0;
    if (application_pinned_messages_available())
        flags |= ui_shell_layout_pins;
    if (application_thread_browser_available())
        flags |= ui_shell_layout_threads;
    const auto* ui = g_app.account_ui;
    const auto* selected =
        ui ? chat_state_store_find_channel(ui->chat_state, ui->chat_state.selected_channel_id) : nullptr;
    if (!friends && ui && ui->chat_state.home_selected && selected && channel_is_private(selected->type))
        flags |= ui_shell_layout_private;
    return flags;
}

namespace {
bool account_text_sources_stale(const AccountUiRuntime& ui) noexcept {
    const MessageStore& messages = g_app.message_view.store;
    const u32 private_revision =
        ui.chat_state.private_channels ? ui.chat_state.private_channels->revision : 0;
    return ui.rendered_poll_revision != ui.message_poll_store.revision ||
           ui.rendered_navigation_revision != ui.navigation_view.revision ||
           ui.rendered_message_created != messages.created ||
           ui.rendered_message_updated != messages.updated ||
           ui.rendered_message_deleted != messages.deleted ||
           ui.rendered_user_revision != ui.user_store.revision ||
           ui.rendered_member_revision != ui.guild_member_store.revision ||
           ui.rendered_private_revision != private_revision ||
           ui.rendered_emoji_mutations != ui.chat_state.emoji_mutations ||
           ui.rendered_typing_revision != ui.typing_store.revision ||
           ui.rendered_permission_mutations != ui.chat_state.permission_mutations ||
           ui.rendered_composer_revision != g_app.message_composer.revision ||
           application_plugins_slots_stale() || application_plugins_render_stale() ||
           ui.rendered_channel_id != ui.active_channel_id;
}
bool account_polls_expired(const AccountUiRuntime& ui, u64 now_ms) noexcept {
    const auto& polls = ui.message_poll_store;
    for (u32 i = 0; i < polls.count; ++i) {
        const auto& poll = polls.records[i];
        if (!poll.expiry_null && !poll.is_finalized && poll.expiry_ms > ui.message_poll_binding.now_ms &&
            poll.expiry_ms <= now_ms)
            return true;
    }
    return false;
}
// The scheduler wakes at the earliest typing expiry. Expire before any
// presentation early-out (unchanged text, Friends, a full-window Settings
// layer): an overdue record left in the store turns every later wait into a
// zero timeout. The revision change then marks the typing row stale.
// Only the selected conversation's typing line, and Home's conversation rows,
// show typing: an expiry elsewhere (busy servers expire one every few hundred
// ms) presents nothing.
void expire_typing(AccountUiRuntime& ui) noexcept {
    const u32 visible_before =
        typing_store_channel_count(ui.typing_store, ui.active_channel_id, ui.chat_state.self_user_id);
    const TypingStoreResult typing_tick = typing_store_tick(&ui.typing_store, clock_monotonic_milliseconds());
    if (!typing_tick.ok())
        log_u64(LogLevel::warning, "typing", "tick_error", static_cast<u64>(typing_tick.error));
    else if (typing_tick.changed && g_app.window &&
             (ui.chat_state.home_selected ||
              typing_store_channel_count(ui.typing_store, ui.active_channel_id, ui.chat_state.self_user_id) !=
                  visible_before))
        application_request_frame();
}
bool refresh_shell_text(bool force) noexcept {
    AccountUiRuntime& ui = *g_app.account_ui;
    if (application_friends_active()) {
        const u32 private_revision =
            ui.chat_state.private_channels ? ui.chat_state.private_channels->revision : 0;
        const bool shell_stale = force || g_app.shell_scene.view != UiShellView::friends ||
                                 ui.rendered_navigation_revision != ui.navigation_view.revision ||
                                 ui.rendered_user_revision != ui.user_store.revision ||
                                 ui.rendered_private_revision != private_revision;
        if (!shell_stale)
            return application_friends_synchronize();
        if (!synchronize_shell_layout() ||
            !renderer_rebuild_content_text(&g_app.renderer, application_theme())) {
            log_write(LogLevel::warning, "friends", "shell_text_refresh_failed");
            return true;
        }
        ui.rendered_navigation_revision = ui.navigation_view.revision;
        ui.rendered_user_revision = ui.user_store.revision;
        ui.rendered_private_revision = private_revision;
        return true;
    }
    bool restored = false;
    if (ui.message_composer_controller.initialized &&
        !message_composer_controller_restore_local(&ui.message_composer_controller, ui.active_channel_id,
                                                   &restored)) {
        log_write(LogLevel::warning, "message", "local_send_restore_failed");
    }
    force = force || restored;
    const u64 poll_now = clock_unix_milliseconds();
    force = force || account_polls_expired(ui, poll_now);
    force = application_thread_starters_sync() || force;
    ui.message_poll_binding.now_ms = poll_now;
    const MessageStore& messages = g_app.message_view.store;
    const auto& polls = ui.message_poll_store;
    const u32 private_revision =
        ui.chat_state.private_channels ? ui.chat_state.private_channels->revision : 0;
    const bool retry_pending = ui.text_rebuild_retry_at_ms && application_account_text_retry_eligible();
    // Plugin decorators run here, bounded, before text reads their verdicts.
    application_plugins_render_prepare();
    const bool stale = force || retry_pending || account_text_sources_stale(ui);
    if (!stale) {
        return true;
    }
    const u64 now_ms = clock_monotonic_milliseconds();
    if (ui.text_rebuild_retry_at_ms != 0 && now_ms < ui.text_rebuild_retry_at_ms) {
        return true;
    }
    const bool typing_visible = application_account_notice().bytes != 0 ||
                                typing_store_channel_count(ui.typing_store, ui.active_channel_id,
                                                                   ui.chat_state.self_user_id) != 0;
    // A preceding child-scene refresh may already have removed the label while
    // retaining the old shell allocation. Compare the allocated height, not
    // that mutable label, so the editor cannot inherit the vacated typing row.
    const float required_composer_height = message_composer_scene_cached_height(
        g_app.message_composer_scene,
        application_reply_draft().message_id != 0 || g_app.message_edit.editor_active, typing_visible,
        message_composer_attachment_row(application_attachment_input()));
    const bool composer_height_changed =
        g_app.message_composer_scene.active &&
        ui_extent_units(ui_rect_height(g_app.message_composer_scene.viewport)) !=
            ui_extent_units(required_composer_height);
    const bool home = ui.chat_state.home_selected;
    const auto* selected =
        chat_state_store_find_channel(ui.chat_state, ui.chat_state.selected_channel_id);
    const bool group_dm = home && selected && selected->type == 3;
    const bool scope_changed =
        (g_app.shell_scene.view == UiShellView::private_channels) != home ||
        (home && (shell_node(UiSemantic::member_toggle) != nullptr) != group_dm) ||
        (home && (shell_node(UiSemantic::composer) != nullptr) != (ui.chat_state.selected_channel_id != 0));
    // A channel's permissions or type arriving after the first layout (launch
    // restores the channel before READY) changes its header controls.
    const bool header_stale = ui_scene_header_stale(g_app.shell_scene, shell_header_flags(false));
    const bool synchronized = composer_height_changed || scope_changed || header_stale
                                  ? synchronize_shell_layout()
                                  : synchronize_shell_widget_scenes();
    const bool autocomplete_stale = ui.rendered_user_revision != ui.user_store.revision ||
                                    ui.rendered_private_revision != private_revision ||
                                    ui.rendered_emoji_mutations != ui.chat_state.emoji_mutations ||
                                    ui.rendered_member_revision != ui.guild_member_store.revision ||
                                    ui.rendered_navigation_revision != ui.navigation_view.revision;
    // The DM profile panel's text goes with the shell's: settle the panel
    // first so this build already shows (or no longer shows) it.
    if (synchronized)
        application_dm_profile_synchronize(true);
    if (!synchronized || (autocomplete_stale && !refresh_active_autocomplete()) ||
        !rebuild_account_text_cache()) {
        log_write(LogLevel::error, "render", "account_text_cache_rebuild_failed");
        saturating_increment(ui.text_rebuild_failures);
        const u32 shift = minimum(ui.text_rebuild_failures > 0 ? ui.text_rebuild_failures - 1u : 0u, 4u);
        const u32 retry_ms =
            minimum(kUiRebuildRetryMaximumMilliseconds, kUiRebuildRetryMinimumMilliseconds << shift);
        ui.text_rebuild_retry_at_ms = deadline_after(now_ms, retry_ms);
        log_u64(LogLevel::warning, "render", "account_text_retry_ms", retry_ms);
        // Presentation is a derived cache. A rejected frame must never stall
        // the authoritative REST/Gateway loop; stale revisions schedule retry.
        return true;
    }
    ui.text_rebuild_retry_at_ms = 0;
    ui.text_rebuild_failures = 0;
    ui.rendered_navigation_revision = ui.navigation_view.revision;
    ui.rendered_message_created = messages.created;
    ui.rendered_message_updated = messages.updated;
    ui.rendered_message_deleted = messages.deleted;
    ui.rendered_poll_revision = polls.revision;
    ui.rendered_user_revision = ui.user_store.revision;
    ui.rendered_member_revision = ui.guild_member_store.revision;
    ui.rendered_private_revision = private_revision;
    ui.rendered_emoji_mutations = ui.chat_state.emoji_mutations;
    ui.rendered_typing_revision = ui.typing_store.revision;
    ui.rendered_permission_mutations = ui.chat_state.permission_mutations;
    ui.rendered_composer_revision = g_app.message_composer.revision;
    application_plugins_slots_rendered();
    application_plugins_render_rendered();
    ui.rendered_channel_id = ui.active_channel_id;
    if (retry_pending)
        application_request_frame();
    return true;
}

} // namespace

bool refresh_account_text(bool force) noexcept {
    if (!g_app.account_ui) {
        return true;
    }
    expire_typing(*g_app.account_ui);
    if (application_settings_active() && !renderer_theme_editor_visible(g_app.renderer)) {
        // A full-window layer hides the shell; the modal keeps it live
        // beneath (new messages, typing, presence, a resize's wrapping).
        if (renderer_settings_hides_shell(g_app.renderer))
            return application_settings_synchronize();
        return refresh_shell_text(force) && application_settings_synchronize();
    }
    return refresh_shell_text(force);
}

// A message-viewport move with every account source current needs only the
// message scene, its text projection and renderer text. Shell scenes, typing
// expiry, composer height and member projections keep unchanged inputs.
bool refresh_account_scroll(bool scroll_moving) noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active || application_settings_active() ||
        application_friends_active())
        return refresh_account_text(true);
    const AccountUiRuntime& ui = *g_app.account_ui;
    if (ui.text_rebuild_retry_at_ms || ui.message_composer_controller.request_count ||
        account_text_sources_stale(ui) || account_polls_expired(ui, clock_unix_milliseconds()))
        return refresh_account_text(true);
    // Only the message viewport moved: the renderer may keep the shell's
    // projection and glyphs while its scenes are unchanged. A rejected
    // projection takes the full path, which owns retry backoff.
    auto& shell = g_app.renderer.content_text_prefix.shell;
    shell.scroll_hint = true;
    const bool rebuilt = rebuild_account_text_cache(true, scroll_moving);
    shell.scroll_hint = false;
    return rebuilt || refresh_account_text(true);
}

bool refresh_account_frame(bool force) noexcept {
    return refresh_account_text(force) && (!g_app.window || application_request_frame() != FALSE);
}

#if !defined(MAICORD_ACCOUNT_TEXT_REFRESH_RUNTIME_TEST)
namespace {
bool theme_consumers_pending;
}
bool application_refresh_theme_consumers(bool typography_changed, bool geometry_changed) noexcept {
    const auto* styles = application_theme().styles;
    message_scene_bind_theme_styles(&g_app.message_scene, styles);
    message_composer_scene_bind_theme_styles(&g_app.message_composer_scene, styles);
    application_search_theme_changed();
    popup_scene_bind_theme_styles(&g_app.popup_scene, styles);
    if (typography_changed) {
        application_text_form_theme_changed();
        application_forum_form_theme_changed();
    }
    if (typography_changed && g_app.account_ui) {
        auto& ui = *g_app.account_ui;
        ui_text_measure_cache_invalidate(&ui.text_measure_cache);
        message_markdown_cache_clear(&ui.message_markdown_cache);
        ui.message_text.measurement_count = 0;
        ui.message_text.measurement_history_count = 0;
        ui.message_text.measurement_history_offset = 0;
        ui.message_poll_binding.backend_revision = renderer_text_measure_revision(g_app.renderer);
    }
    if (typography_changed || geometry_changed) {
        g_app.message_composer_scene.measured_row_height = 0;
        g_app.pane_layout_pending = true;
    }
    content_text_retention_invalidate(&g_app.renderer.content_text_retention);
    g_app.renderer.retained_presentation.valid = false;
    theme_consumers_pending = true;
    return application_request_frame();
}
bool application_synchronize_theme_consumers() noexcept {
    if (!theme_consumers_pending || !g_app.renderer_ready)
        return true;
    theme_consumers_pending = false;
    const bool ready =
        synchronize_shell_layout() &&
        (g_app.login.visible ? application_refresh_login_view() : refresh_account_text_for_layout()) &&
        application_settings_synchronize() && application_theme_editor_synchronize() &&
        application_text_form_synchronize() && application_forum_form_synchronize() &&
        application_search_synchronize() && application_composer_text_input_synchronize();
    if (!ready)
        theme_consumers_pending = true;
    return ready;
}
#endif

} // namespace maicord::app_detail
