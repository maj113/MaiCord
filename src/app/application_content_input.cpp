#include "application_guild_community.hpp"
#include "application_guild_menu.hpp"
#include "application_private_channel.hpp"
#include "application_guild_invite.hpp"
#include "application_guild_folders.hpp"
#include "application_downloads.hpp"
#include "application_video.hpp"
#include "application_poll.hpp"
#include "application_image_viewer.hpp"
#include "application_gif_favorite_star.hpp"
#include "application_profile.hpp"
#include "application_content_input.hpp"
#include "application_media_policy.hpp"
#include "application_guild_card.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_navigation_drag.hpp"
#include "application_friends.hpp"
#include "application_thread_browser.hpp"

#include "application_account_module.hpp"
#include "application_account_presentation.hpp"
#include "application_channel_session.hpp"
#include "application_composer.hpp"
#include "application_message_history.hpp"
#include "application_message_interaction.hpp"
#include "application_message_components.hpp"
#include "application_invite_cards.hpp"
#include "application_pinned_messages.hpp"
#include "application_message_reaction.hpp"
#include "application_scrolling.hpp"
#include "application_shell_input.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "application_channel_authority.hpp"
#include "application_read_state.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_message_navigation.hpp"
#include "application_plugins.hpp"
#include "application_link_trust.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/app/received_attachment_open.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_author.hpp"

#include <Windows.h>

namespace maicord::app_detail {
// Defined below beside ordinary message-link routing; retains its channel,
// permission and authenticated history machinery for OS activation.
bool application_navigate_notification(u64 channel_id, u64 message_id) noexcept;
namespace {

ReceivedAttachmentOpen received_attachment_open{};
ReceivedAttachmentOpenResult received_attachment_status{};
u64 received_attachment_account_scope() noexcept {
    return g_app.plugin_account_scope_serial ? g_app.plugin_account_scope_serial
                                             : g_app.account.status.profile_id;
}
bool received_attachment_source(const ReceivedAttachmentOpenIdentity& id, Utf8View* source) noexcept {
    if (!source || !g_app.account_ui || !g_app.account.instance || g_app.account.stop_failed ||
        g_app.login.visible || received_attachment_account_scope() != id.account_scope ||
        g_app.account_ui->active_channel_id != id.channel_id ||
        !application_channel_readable(g_app, id.channel_id))
        return false;
    const auto found = message_store_find(g_app.message_view.store, id.message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* outer = message_rich_store_find(store, id.message_id);
    if (id.source_channel_id &&
        (!outer || outer->reference.type != 1 || outer->reference.channel_id != id.source_channel_id ||
         !outer->reference.message_id))
        return false;
    const auto* rich = id.source_channel_id ? message_rich_store_snapshot(store, id.message_id) : outer;
    if (!message || message->channel_id != id.channel_id || message->revision != id.message_revision ||
        !rich || rich->message_revision != id.message_revision)
        return false;
    for (u32 i = 0; i < rich->attachment_count; ++i) {
        const auto& attachment = rich->attachments[i];
        if (attachment.id == id.attachment_id &&
            message_rich_attachment_format(store, attachment) == MediaAttachmentFormat::unknown) {
            if (message_scene_media_obscure(
                    g_app.message_scene, message_media_owner(*message, outer), message->id,
                    message_media_reveal_key(message->id, MessageMediaSource::attachment, i,
                                             id.source_channel_id != 0),
                    message_rich_attachment_spoiler(store, attachment),
                    message_rich_attachment_explicit(attachment)) != MessageMediaObscure::none)
                return false;
            *source = message_rich_store_string(store, attachment.url);
            return true;
        }
    }
    return false;
}
void publish_received_attachment_notice(ReceivedAttachmentOpenResult result,
                                        const ReceivedAttachmentOpenIdentity& id, bool beginning) noexcept {
    constexpr auto kind = ApplicationMessageMutationKind::attachment_open;
    if (beginning)
        application_message_mutation_notice_begin(kind, id.channel_id, id.message_id);
    if (result == ReceivedAttachmentOpenResult::pending)
        application_message_mutation_notice_pending(kind, id.channel_id, id.message_id);
    else if (result == ReceivedAttachmentOpenResult::failed ||
             result == ReceivedAttachmentOpenResult::invalid)
        application_message_mutation_notice_failed(kind, id.channel_id, id.message_id, {});
    else if (result == ReceivedAttachmentOpenResult::ready || result == ReceivedAttachmentOpenResult::stale)
        application_message_mutation_notice_succeeded(kind, id.channel_id, id.message_id);
    // The accessor drops an obsolete account/channel owner. Rebuild only on an
    // intent transition, never on each unresolved URL observation.
    application_message_mutation_notice_text();
    if (!refresh_message_composer_ui())
        log_write(LogLevel::warning, "ui", "attachment_open_notice_refresh_failed");
    application_request_frame();
}

bool select_content_channel(u64 channel_id) noexcept {
    if (!g_app.account_ui || channel_id == 0) {
        return false;
    }
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    if (channel && (channel->type == 15 || channel->type == 16))
        return application_thread_browser_open(channel_id);
    ui_scroll_host_cancel(&g_app.scroll_host);
    const bool was_friends = application_friends_active();
    const bool was_browser = application_thread_browser_active();
    const ChatStateSelectionResult selected =
        chat_state_store_select_channel(&g_app.account_ui->chat_state, channel_id);
    if (!selected.ok()) {
        return false;
    }
    application_thread_browser_close();
    application_friends_hide();
    application_read_state_activity();
    if ((selected.changed || was_friends || was_browser) &&
        (!synchronize_navigation_view() || !synchronize_selected_channel() ||
         ((was_friends || was_browser) && !synchronize_shell_layout()) || !refresh_account_text())) {
        application_stop_account();
        return false;
    }
    if (selected.changed || was_friends || was_browser) {
        if (!pump_message_history())
            return false;
        application_request_frame();
    }
    return true;
}

bool navigate_content_message(u64 channel_id, u64 message_id) noexcept {
    if (!g_app.account_ui || channel_id == 0) {
        return false;
    }
    const ChannelRecord* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    if (!channel) {
        log_write(LogLevel::info, "ui", "message_link_channel_unavailable");
        return false;
    }
    constexpr u32 read_actions = channel_action_view | channel_action_read_history;
    if (!channel->occupied || !channel_is_message_capable(channel->type) ||
        (channel->flags & kChannelObfuscatedFlag) ||
        ((channel->type == 10 || channel->type == 11 || channel->type == 12) &&
         !chat_state_store_thread_accessible(g_app.account_ui->chat_state, *channel)) ||
        (!channel_is_private(channel->type) && !channel->permissions_resolved) ||
        (channel->actions & read_actions) != read_actions)
        return false;
    if (!message_id) {
        g_app.pending_message_jump_channel_id = 0;
        g_app.pending_message_jump_message_id = 0;
        return select_content_channel(channel_id) && g_app.account_ui &&
               g_app.account_ui->active_channel_id == channel_id &&
               g_app.account_ui->chat_state.selected_channel_id == channel_id;
    }
    g_app.pending_message_jump_channel_id = channel_id;
    g_app.pending_message_jump_message_id = message_id;
    if (!select_content_channel(channel_id)) {
        g_app.pending_message_jump_channel_id = 0;
        g_app.pending_message_jump_message_id = 0;
        return false;
    }
    if (!g_app.account_ui || g_app.account_ui->active_channel_id != channel_id) {
        g_app.pending_message_jump_channel_id = 0;
        g_app.pending_message_jump_message_id = 0;
        return false;
    }
    if (scroll_to_resident_message(message_id)) {
        g_app.pending_message_jump_channel_id = 0;
        g_app.pending_message_jump_message_id = 0;
        if (!refresh_account_text(true)) {
            return false;
        }
        if (g_app.window) {
            application_request_frame();
        }
        return true;
    }
    if (!pump_message_history()) {
        g_app.pending_message_jump_channel_id = 0;
        g_app.pending_message_jump_message_id = 0;
        return false;
    }
    return true;
}

// Starter card: the thread itself, or the parent's thread list when the
// thread is not loaded (archived or not yet synced).
bool open_thread_starter(u64 parent_id, u64 thread_id) noexcept {
    return navigate_content_message(thread_id, 0) || application_thread_browser_open(parent_id);
}

bool navigate_content_message_link(Utf8View url) noexcept {
    MessageLink link{};
    if (!message_markdown_message_link(url, &link))
        return false;
    // Recognition owns consumption, even when this account cannot navigate.
    // Internal permission/history failures must never launch an external URL.
    if (!g_app.account_ui)
        return true;
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, link.channel_id);
    if (!channel || channel->guild_id != link.guild_id)
        return true;
    navigate_content_message(link.channel_id, link.message_id);
    return true;
}

bool open_content_url_impl(Utf8View url, const ReceivedAttachmentOpenIdentity* attachment = nullptr,
                           u64 attachment_source_fingerprint = 0, bool trusted_text = false) noexcept {
    if (navigate_content_message_link(url)) {
        return true;
    }
    // Invites open the native join dialog instead of the browser.
    if (!attachment && application_guild_invite_open_url(url))
        return true;
    if (!message_markdown_url_valid(url) || url.bytes > 2'047) {
        return true;
    }
    // A deferred attachment opens only while its message still carries the
    // source it was resolved from (no switched account, edit or delete).
    Utf8View source{};
    if (attachment &&
        (!received_attachment_source(*attachment, &source) ||
         hash64_bytes(kHash64Seed, source.data, source.bytes) != attachment_source_fingerprint))
        return true;
    // Every link leaves through the official trust rules and the themed
    // "Leaving Discord" warning; attachment links are never trusted text.
    return application_link_open(url, !attachment && trusted_text);
}
// Markdown link rule: a link is trusted when its text is its URL,
// unless it is in a DM with someone who is not a friend. Embed titles, authors
// and providers are named links, never their URL text.
bool content_link_trusted_text(const MessageTextHit& hit, Utf8View url) noexcept {
    if (!g_app.account_ui || !url.bytes)
        return false;
    const auto found = message_store_find(g_app.message_view.store, hit.message_id);
    const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!message || message->revision != hit.message_revision ||
        !application_link_channel_trusted(message->channel_id))
        return false;
    const auto& presentation = g_app.account_ui->message_text;
    for (u32 c = 0; c < presentation.command_count; ++c) {
        const auto& command = presentation.commands[c];
        if (command.message_id != hit.message_id || command.message_revision != hit.message_revision)
            continue;
        for (u32 s = 0; s < command.semantics.count; ++s) {
            const auto& semantic = command.semantics[s];
            if (semantic.kind != hit.kind || semantic.source_offset != hit.source_offset ||
                semantic.source_bytes != hit.source_bytes || semantic.byte_offset > command.text.bytes ||
                semantic.byte_count > command.text.bytes - semantic.byte_offset)
                continue;
            return semantic.byte_count == url.bytes &&
                   bytes_equal(command.text.data + semantic.byte_offset, url.data, url.bytes);
        }
    }
    return false;
}
bool open_content_link(const MessageTextHit& hit) noexcept {
    const auto url = message_text_presentation_hit_source(
        g_app.message_view.store, g_app.account_ui ? &g_app.account_ui->message_rich_store : nullptr, hit);
    MessageLink target{};
    if (!message_markdown_message_link(url, &target))
        return open_content_url_impl(url, nullptr, 0, content_link_trusted_text(hit, url));
    // Resolve the native semantic before capturing its timeline owner.
    // A parsed target never grants access to an unrelated source.
    const auto found = message_store_find(g_app.message_view.store, hit.message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    ApplicationMessageNavigationSource source{};
    if (message && message->revision == hit.message_revision &&
        application_message_navigation_source(message->channel_id, message->id, found.handle,
                                              hit.message_revision, &source))
        application_message_navigation_link(source, target);
    return true;
}

// A clicked command mention goes into the chat input: the tag
// </name sub:id> becomes "/name sub " in the composer.
bool insert_command_mention(const MessageTextHit& hit) noexcept {
    if (!g_app.account_ui)
        return true;
    const Utf8View source = message_text_presentation_hit_source(
        g_app.message_view.store, &g_app.account_ui->message_rich_store, hit);
    u32 separator = source.bytes;
    while (separator > 2 && source.data[separator - 1] != ':')
        --separator;
    if (!source.data || source.bytes < 6 || source.data[0] != '<' || source.data[1] != '/' || separator <= 3)
        return true;
    return application_composer_fill_command({source.data + 2, separator - 3});
}

bool select_navigation(bool guild, u64 id, bool reveal, bool home = false) noexcept {
    if (!g_app.account_ui)
        return false;
    if (home)
        return application_friends_show();
    if (!guild && navigation_events_row(id))
        return application_guild_community_open_events(g_app.account_ui->chat_state.selected_guild_id) || true;
    if (!guild && navigation_requests_row(id)) {
        application_private_channel_set_requests_mode(!application_private_channel_requests_mode());
        return true;
    }
    const bool was_friends = application_friends_active();
    auto& state = g_app.account_ui->chat_state;
    const auto* category = !home && !guild ? chat_state_store_find_channel(state, id) : nullptr;
    if (category && category->type == 4) {
        auto& mutable_category = state.channels[category - state.channels];
        mutable_category.collapsed = !mutable_category.collapsed;
        saturating_increment(mutable_category.revision);
        saturating_increment(state.channel_mutations);
        if (!synchronize_navigation_view() || !synchronize_shell_layout() || !refresh_account_text(true))
            return false;
        application_request_frame();
        return true;
    }
    if (category && (category->type == 15 || category->type == 16))
        return application_thread_browser_open(id);
    ui_scroll_host_cancel(&g_app.scroll_host);
    const bool was_browser = application_thread_browser_active();
    const auto selected = home ? chat_state_store_select_home(&g_app.account_ui->chat_state)
                          : guild
                              ? chat_state_store_select_guild(&g_app.account_ui->chat_state, id)
                              : chat_state_store_select_channel(&g_app.account_ui->chat_state, id);
    if (!selected.ok()) {
        log_u64(LogLevel::warning, "ui", "navigation_selection_error", static_cast<u64>(selected.error));
        return true;
    }
    application_thread_browser_close();
    application_friends_hide();
    const u64 selected_channel_id = g_app.account_ui->chat_state.selected_channel_id;
    application_read_state_activity();
    if (selected.changed || was_friends || was_browser) {
        if (!synchronize_navigation_view() || !synchronize_selected_channel() ||
            !synchronize_shell_layout()) {
            application_stop_account();
            return true;
        }
    }
    // Selection scrolls the rail into view with the wheel curve.
    const float rail_before = g_app.guild_rail_scene.scroll_dips;
    if (reveal) {
        const auto& view = g_app.account_ui->navigation_view;
        if (home)
            ui_widget_manager_focus(&g_app.shell_scene.widgets,
                                    g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::home_button)]);
        const bool guild_ready = !guild || !g_app.guild_rail_scene.active ||
                                 guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, view, id);
        const bool channel_ready =
            !selected_channel_id || !g_app.shell_navigation_scene.active ||
            shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, view, selected_channel_id);
        if (!guild_ready || !channel_ready)
            log_write(LogLevel::warning, "ui", "navigation_reveal_failed");
    }
    if (selected.changed || reveal || was_friends || was_browser) {
        if (!refresh_account_text(reveal)) {
            application_stop_account();
            return true;
        }
        if (reveal && guild && !animate_guild_rail_scroll_from(rail_before))
            log_write(LogLevel::warning, "ui", "guild_rail_scroll_animation_failed");
        if (home)
            ui_widget_manager_focus(&g_app.shell_scene.widgets,
                                    g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::home_button)]);
        pump_message_history();
        application_request_frame();
    }
    g_app.context_focus = home    ? &g_app.shell_scene.widgets
                          : guild ? &g_app.guild_rail_scene.widgets
                                  : &g_app.shell_navigation_scene.widgets;
    return true;
}

} // namespace
bool application_navigate_notification(u64 channel_id, u64 message_id) noexcept {
    return navigate_content_message(channel_id, message_id);
}
bool application_navigate_known_message(u64 channel_id, u64 message_id) noexcept {
    return navigate_content_message(channel_id, message_id);
}
bool application_open_content_url(Utf8View url) noexcept {
    return open_content_url_impl(url);
}
bool application_open_content_link(const MessageTextHit& hit) noexcept {
    return hit.valid &&
           (hit.kind == MessageMarkdownSemanticKind::link || hit.kind == MessageMarkdownSemanticKind::message_link) &&
           open_content_link(hit);
}
void application_received_attachment_pump() noexcept {
    if (!received_attachment_open.pending)
        return;
    const auto identity = received_attachment_open.identity;
    Utf8View source{}, ready{};
    if (!received_attachment_source(identity, &source)) {
        received_attachment_open = {};
        received_attachment_status = ReceivedAttachmentOpenResult::stale;
        publish_received_attachment_notice(received_attachment_status, identity, false);
        return;
    }
    const auto previous = received_attachment_status;
    received_attachment_status = received_attachment_open_resume(
        &received_attachment_open, identity, source, &g_app.media_urls, clock_monotonic_milliseconds(),
        clock_unix_milliseconds() / 1000, &ready);
    if (received_attachment_status != previous)
        publish_received_attachment_notice(received_attachment_status, identity, false);
    if (received_attachment_status == ReceivedAttachmentOpenResult::ready)
        open_content_url_impl(ready, &identity, hash64_bytes(kHash64Seed, source.data, source.bytes));
}

bool select_content_guild(u64 guild) noexcept {
    return select_navigation(true, guild, true);
}

// Guild voice and stage channels are joined, not selected. A stage is joined
// in the audience (suppressed by the server).
static bool join_voice_channel(u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    if (!channel || (channel->type != 2 && channel->type != 13) || !channel->guild_id)
        return false;
    return application_voice_join(channel->guild_id, channel->id);
}

bool handle_navigation_click(LPARAM lparam) noexcept {
    // Draggable rows act on release so a drag never also clicks them.
    if (application_navigation_drag_press(lparam))
        return true;
    return activate_navigation_press(lparam);
}

bool activate_navigation_press(LPARAM lparam) noexcept {
    const POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (renderer_guild_rail_hit_test(g_app.renderer, point.x, point.y)) {
        const auto rail = guild_rail_scene_hit_test(g_app.guild_rail_scene, client_dip_x(lparam), client_dip_y(lparam));
        if (rail.add_server)
            return application_guild_lifecycle_open_create() || true;
        if (rail.folder) {
            guild_rail_scene_focus_guild(&g_app.guild_rail_scene, rail.id);
            g_app.context_focus = &g_app.guild_rail_scene.widgets;
            application_guild_folders_toggle(rail.id);
            return true;
        }
    }
    const auto hit = renderer_navigation_hit_test(g_app.renderer, point.x, point.y);
    if (hit.kind == RendererNavigationHitKind::none)
        return false;
    if (g_app.account_ui && hit.kind == RendererNavigationHitKind::channel && navigation_events_row(hit.id)) {
        shell_navigation_scene_focus_channel(&g_app.shell_navigation_scene, hit.id);
        return application_guild_community_open_events(g_app.account_ui->chat_state.selected_guild_id) || true;
    }
    if (g_app.account_ui && hit.kind == RendererNavigationHitKind::channel && navigation_requests_row(hit.id)) {
        shell_navigation_scene_focus_channel(&g_app.shell_navigation_scene, hit.id);
        application_private_channel_set_requests_mode(!application_private_channel_requests_mode());
        return true;
    }
    if (g_app.account_ui && !hit.selectable && hit.kind == RendererNavigationHitKind::channel)
        join_voice_channel(hit.id);
    if (!hit.selectable || !g_app.account_ui)
        return true;
    if (hit.kind == RendererNavigationHitKind::channel &&
        application_private_channel_row_close(hit.id, client_dip_x(lparam), client_dip_y(lparam)))
        return true;
    if (hit.kind == RendererNavigationHitKind::home) {
        ui_widget_manager_focus(&g_app.shell_scene.widgets,
                                g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::home_button)]);
        return select_navigation(false, 0, false, true);
    }
    const bool guild = hit.kind == RendererNavigationHitKind::guild;
    if (guild ? !guild_rail_scene_focus_guild(&g_app.guild_rail_scene, hit.id)
              : !shell_navigation_scene_focus_channel(&g_app.shell_navigation_scene, hit.id))
        return false;
    return select_navigation(guild, hit.id, false);
}

bool activate_focused_navigation() noexcept {
    if (!g_app.account_ui)
        return false;
    if (g_app.context_focus == &g_app.shell_scene.widgets) {
        const auto* widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, g_app.shell_scene.widgets.focused);
        if (widget && widget->id == static_cast<u32>(UiSemantic::guild_menu) + 1u)
            return application_guild_menu_toggle(true);
        return select_navigation(false, 0, false, true);
    }
    const bool guild = g_app.context_focus == &g_app.guild_rail_scene.widgets;
    const u64 id =
        guild ? g_app.guild_rail_scene.focused_guild_id : g_app.shell_navigation_scene.focused_channel_id;
    if (guild && guild_folder_row_id(id))
        return application_guild_folders_toggle(id) || true;
    return !id || select_navigation(guild, id, false);
}

bool handle_navigation_shortcut(NavigationShortcut shortcut) noexcept {
    if (!g_app.account_ui || g_app.login.visible || shortcut.mode == NavigationMode::none)
        return false;
    const auto target = navigation_target(g_app.account_ui->navigation_view, shortcut);
    if (!target.id && !target.home)
        return true;
    if (g_app.popup_scene.active && g_app.popup_scene.kind == PopupKind::autocomplete && !dismiss_popup())
        return true;
    return select_navigation(target.guild, target.id, true, target.home);
}

bool handle_message_action_bar_click(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked())
        return false;
    if (!g_app.account_ui || !g_app.message_action_bar.active || g_app.login.visible) {
        return false;
    }
    const MessageActionBarHit hit =
        message_action_bar_hit_test(g_app.message_action_bar, client_dip_x(lparam), client_dip_y(lparam));
    if (!hit.inside) {
        return false;
    }
    if (hit.action == MessageActionBarAction::none) {
        return true;
    }
    SetFocus(g_app.window);
    if (message_scene_focus_message(&g_app.message_scene, hit.message_id))
        g_app.context_focus = &g_app.message_scene.widgets;
    return application_message_action_bar_activate(hit, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
}

// Refreshes an expired signed URL when needed, then opens it through the
// external-link confirmation. False when the source is no longer current.
static bool begin_received_attachment_open(const MessageRecord& record, u64 attachment_id,
                                           u64 snapshot_origin_channel) noexcept {
    const auto& rich_store = g_app.account_ui->message_rich_store;
    const auto* outer_rich = message_rich_store_find(rich_store, record.id);
    const auto* media_rich =
        snapshot_origin_channel ? message_rich_store_snapshot(rich_store, record.id) : outer_rich;
    if (!media_rich || media_rich->message_revision != record.revision ||
        (snapshot_origin_channel && (!outer_rich || outer_rich->reference.type != 1 ||
                                     outer_rich->reference.channel_id != snapshot_origin_channel)))
        return false;
    for (u32 i = 0; i < media_rich->attachment_count; ++i) {
        const auto& attachment = media_rich->attachments[i];
        if (attachment.id == attachment_id &&
            message_rich_attachment_format(rich_store, attachment) == MediaAttachmentFormat::unknown &&
            application_media_activation_consumed(
                g_app.message_scene, message_media_owner(record, outer_rich), record.id,
                message_media_reveal_key(record.id, MessageMediaSource::attachment, i,
                                         snapshot_origin_channel != 0),
                message_rich_attachment_spoiler(rich_store, attachment),
                message_rich_attachment_explicit(attachment)))
            return true;
    }
    const ReceivedAttachmentOpenIdentity identity{received_attachment_account_scope(),
                                                  record.channel_id,
                                                  record.id,
                                                  attachment_id,
                                                  record.revision,
                                                  snapshot_origin_channel};
    Utf8View source{}, ready{};
    if (!received_attachment_source(identity, &source))
        return false;
    received_attachment_status = received_attachment_open_begin(
        &received_attachment_open, identity, source, &g_app.media_urls, clock_monotonic_milliseconds(),
        clock_unix_milliseconds() / 1000, &ready);
    publish_received_attachment_notice(received_attachment_status, identity, true);
    if (received_attachment_status == ReceivedAttachmentOpenResult::ready)
        open_content_url_impl(ready, &identity, hash64_bytes(kHash64Seed, source.data, source.bytes));
    application_request_frame();
    return true;
}

bool application_received_attachment_open(u64 message_id, u64 attachment_id,
                                          u64 snapshot_origin_channel) noexcept {
    if (!g_app.account_ui || g_app.login.visible || !message_id || !attachment_id)
        return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    return record && begin_received_attachment_open(*record, attachment_id, snapshot_origin_channel);
}

bool handle_message_click(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked())
        return false;
    if (!g_app.account_ui || !g_app.message_scene.active || g_app.login.visible) {
        return false;
    }
    if (application_downloads_save_press(client_dip_x(lparam), client_dip_y(lparam)))
        return true;
    const auto reply =
        message_scene_reply_hit_test(g_app.message_scene, client_dip_x(lparam), client_dip_y(lparam));
    if (reply.message_id != 0) {
        SetFocus(g_app.window);
        return navigate_content_message(reply.channel_id, reply.message_id);
    }
    const MessageSceneHit hit =
        message_scene_hit_test(g_app.message_scene, client_dip_x(lparam), client_dip_y(lparam));
    // Generic received attachments use the same message access check as
    // inline media, while opening only their validated HTTPS download URL.
    const auto* row = message_scene_find_row(g_app.message_scene, hit.source_index);
    const auto found = hit.message_id ? message_store_find(g_app.message_view.store, hit.message_id)
                                      : MessageStoreResult{};
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto& rich_store = g_app.account_ui->message_rich_store;
    const auto* rich = record ? message_rich_store_find(rich_store, record->id) : nullptr;
    if (row && record && rich && rich->message_revision == record->revision && rich->thread.id &&
        hit.widget.slot == row->thread.slot && hit.widget.generation == row->thread.generation) {
        SetFocus(g_app.window);
        return open_thread_starter(record->channel_id, rich->thread.id);
    }
    if (row && record && record->channel_id == g_app.account_ui->active_channel_id && hit.widget.generation) {
        const auto* node = message_scene_layout_node(g_app.message_scene, hit.widget);
        const float x = client_dip_x(lparam), y = client_dip_y(lparam);
        if (application_invite_card_click(record->id, hit.widget, x, y)) {
            SetFocus(g_app.window);
            return true;
        }
        if (application_message_component_click(record->id, hit.widget,
                                                node ? node->rectangle : UiRect{x, y, x + 1, y + 1})) {
            SetFocus(g_app.window);
            return true;
        }
    }
    if (row && record && record->channel_id == g_app.account_ui->active_channel_id && rich &&
        rich->message_revision == record->revision) {
        const auto* outer_rich = rich;
        const auto* outer_row = row;
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
            if (content_scope) {
                rich = message_rich_store_snapshot(rich_store, record->id);
                row = message_scene_snapshot_row(g_app.message_scene, *outer_row);
                if (!rich || !row || outer_rich->reference.type != 1 || !outer_rich->reference.channel_id ||
                    !outer_rich->reference.message_id)
                    continue;
            }
            for (u32 i = 0; i < rich->attachment_count; ++i) {
                const auto& attachment = rich->attachments[i];
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, i);
                if (message_rich_attachment_audio_supported(rich_store, attachment) ||
                    message_rich_attachment_format(rich_store, attachment) !=
                        MediaAttachmentFormat::unknown ||
                    hit.widget.slot != widget.slot || hit.widget.generation != widget.generation)
                    continue;
                const auto* node = message_scene_layout_node(g_app.message_scene, hit.widget);
                const auto* row_node = message_scene_layout_node(g_app.message_scene, row->widget);
                if (!node || !row_node)
                    return true;
                const UiRect card{node->rectangle.left, node->rectangle.top,
                                  minimum(node->rectangle.right, node->rectangle.left + 420.0f),
                                  node->rectangle.top + 36.0f};
                const auto clip = ui_rect_intersection(
                    card, ui_rect_intersection(row_node->rectangle, g_app.message_scene.viewport));
                if (!ui_rect_contains(clip, client_dip_x(lparam), client_dip_y(lparam)))
                    return false;
                begin_received_attachment_open(*record, attachment.id,
                                               content_scope ? outer_rich->reference.channel_id : 0);
                return true;
            }
        }
    }
    u32 reaction_index = 0;
    if (!message_scene_reaction_index(g_app.message_scene, hit, &reaction_index)) {
        return false;
    }
    SetFocus(g_app.window);
    if (!queue_message_reaction(hit.message_id, reaction_index)) {
        return false;
    }
    if (!pump_message_history()) {
        log_write(LogLevel::error, "rest", "message_reaction_initial_pump_failed");
    }
    return true;
}

bool application_received_attachment_activate_focused() noexcept {
    if (application_video_open_focused())
        return true;
    const auto& scene = g_app.message_scene;
    if (g_app.context_focus != &scene.widgets || !scene.active || !g_app.account_ui || g_app.login.visible)
        return false;
    const auto handle = scene.widgets.focused;
    const auto* row = message_scene_resolve(scene, handle);
    const auto* message = row ? message_store_resolve(g_app.message_view.store, row->message) : nullptr;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = message ? message_rich_store_find(store, message->id) : nullptr;
    if (!rich || rich->message_revision != message->revision)
        return false;
    if (rich->thread.id && handle.slot == row->thread.slot && handle.generation == row->thread.generation)
        return open_thread_starter(message->channel_id, rich->thread.id);
    const auto* outer_row = row;
    for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
        if (content_scope) {
            row = message_scene_snapshot_row(scene, *outer_row);
            rich = message_rich_store_snapshot(store, message->id);
            if (!row || !rich)
                continue;
        }
        for (u32 i = 0; i < rich->attachment_count; ++i) {
            const auto widget =
                message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, i);
            if (widget.slot != handle.slot || widget.generation != handle.generation ||
                message_rich_attachment_audio_supported(store, rich->attachments[i]) ||
                message_rich_attachment_format(store, rich->attachments[i]) !=
                    MediaAttachmentFormat::unknown)
                continue;
            const auto* node = message_scene_layout_node(scene, handle);
            const auto* row_node = message_scene_layout_node(scene, row->widget);
            if (!node || !row_node)
                return false;
            const UiRect card{node->rectangle.left, node->rectangle.top,
                              minimum(node->rectangle.right, node->rectangle.left + 420.0f),
                              node->rectangle.top + 36.0f};
            const auto clip =
                ui_rect_intersection(card, ui_rect_intersection(row_node->rectangle, scene.viewport));
            if (!ui_rect_has_area(clip))
                return false;
            const float scale = static_cast<float>(maximum(g_app.renderer.dpi, 1u)) / 96.f;
            return handle_message_click(
                MAKELPARAM(static_cast<short>((clip.left + clip.right) * .5f * scale),
                           static_cast<short>((clip.top + clip.bottom) * .5f * scale)));
        }
    }
    return false;
}

MessageProfileHit application_message_profile_hit_test(float x, float y) noexcept {
    const auto& renderer = g_app.renderer;
    if (g_app.login.visible || !renderer.message_text || !renderer.chat_state || !renderer.user_store)
        return {};
    return message_author_hit_test(g_app.message_scene, *renderer.message_text, *renderer.chat_state,
                                   *renderer.user_store, renderer.message_measure_cache,
                                   application_theme(), x, y);
}

bool application_message_guild_tag_hit(float x, float y) noexcept {
    const auto& renderer = g_app.renderer;
    return !g_app.login.visible && renderer.message_text && renderer.chat_state && renderer.user_store &&
           message_author_guild_tag_hit_test(g_app.message_scene, *renderer.message_text, *renderer.chat_state,
                                             *renderer.user_store, renderer.message_measure_cache, application_theme(),
                                             x, y)
               .guild_id;
}

bool application_message_media_reveal_at(float x, float y) noexcept {
    if (application_thread_browser_read_blocked() || !g_app.account_ui || !g_app.message_scene.active ||
        g_app.login.visible || !ui_rect_contains(g_app.message_scene.viewport, x, y))
        return false;
    auto& scene = g_app.message_scene;
    const MessageSceneHit hit = message_scene_hit_test(scene, x, y);
    const auto* row = hit.message_id ? message_scene_find_row(scene, hit.source_index) : nullptr;
    const auto* record = row ? message_store_resolve(g_app.message_view.store, row->message) : nullptr;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* outer_rich = record ? message_rich_store_find(store, record->id) : nullptr;
    if (!outer_rich || outer_rich->message_revision != record->revision || !hit.widget.generation)
        return false;
    const auto* outer_row = row;
    for (u32 scope = 0; scope < 2; ++scope) {
        row = scope ? message_scene_snapshot_row(scene, *outer_row) : outer_row;
        const auto* rich = scope ? message_rich_store_snapshot(store, record->id) : outer_rich;
        if (!row || !rich || rich->message_revision != record->revision)
            continue;
        const auto same = [&](MessageSceneRichItemKind kind, u32 index) noexcept {
            const auto widget = message_scene_rich_item_widget(*row, kind, index);
            return widget.generation && widget.slot == hit.widget.slot &&
                   widget.generation == hit.widget.generation;
        };
        u64 key = 0;
        bool spoiler = false, explicit_media = false;
        u64 plugin_message_id = record->id;
        for (u32 i = 0; !key && i < rich->attachment_count && i < row->attachment_item_count; ++i)
            if (same(MessageSceneRichItemKind::attachment, i)) {
                key = message_media_reveal_key(record->id, MessageMediaSource::attachment, i, scope != 0);
                spoiler = message_rich_attachment_spoiler(store, rich->attachments[i]);
                explicit_media = message_rich_attachment_explicit(rich->attachments[i]);
            }
        for (u32 i = 0; !key && i < rich->embed_count && i < row->embed_item_count; ++i)
            if (same(MessageSceneRichItemKind::embed, i)) {
                u32 member = i;
                MessageRichEmbedGallery gallery{};
                if (message_rich_embed_gallery(store, *rich, i, &gallery)) {
                    const auto* node = message_scene_rich_item_layout_node(
                        scene, *row, MessageSceneRichItemKind::embed, i);
                    if (!node)
                        return false;
                    const auto block = message_rich_embed_media_rect(
                        row->rich_item_layout, i, node->content_rectangle,
                        message_rich_embed_media_only(rich->embeds[i]));
                    bool found = false;
                    for (u32 tile = 0; tile < gallery.count && tile < kMessageEmbedGalleryDrawn; ++tile)
                        if (ui_rect_contains(ui_rect_intersection(
                                                 message_rich_embed_gallery_tile(block, gallery.count, tile),
                                                 node->clip_rectangle),
                                             x, y)) {
                            member = gallery.members[tile];
                            found = true;
                            break;
                        }
                    if (!found)
                        return false;
                }
                key = message_media_reveal_key(record->id, MessageMediaSource::embed, member, scope != 0);
                explicit_media = message_rich_embed_explicit(rich->embeds[member]);
            }
        for (u32 i = 0; !key && i < rich->component_count && i < row->component_item_count; ++i)
            if (same(MessageSceneRichItemKind::component, i)) {
                key = message_media_reveal_key(record->id, MessageMediaSource::component, i, scope != 0);
                plugin_message_id = 0;
                spoiler = message_rich_component_spoiler(rich->components[i]);
            }
        if (key)
            return application_media_activation_consumed(scene, message_media_owner(*record, outer_rich),
                                                         plugin_message_id, key, spoiler, explicit_media);
    }
    return false;
}

bool handle_content_click(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked())
        return false;
    const POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    const float x = client_dip_x(lparam), y = client_dip_y(lparam);
    if (const auto& renderer = g_app.renderer;
        !g_app.login.visible && renderer.message_text && renderer.chat_state && renderer.user_store) {
        const auto slot = message_author_plugin_hit_test(g_app.message_scene, *renderer.message_text,
                                                         *renderer.chat_state, *renderer.user_store,
                                                         renderer.message_measure_cache, application_theme(), x, y);
        if (slot.action_id) {
            // The badge sits on the author line; anchor the popover under the click.
            application_plugins_slot_activate(slot.owner_id,
                {kUiPluginSlotMessageAccessory, slot.channel_id, slot.message_id, 0, 0}, slot.action_id,
                {x - 8, y - 10, x + 8, y + 10});
            return true;
        }
        // A server tag chip opens the server tag popout, not the profile.
        const auto tag = message_author_guild_tag_hit_test(g_app.message_scene, *renderer.message_text,
                                                           *renderer.chat_state, *renderer.user_store,
                                                           renderer.message_measure_cache, application_theme(), x, y);
        if (tag.guild_id)
            return application_guild_card_open(tag.guild_id, tag.chip) || true;
    }
    const auto profile = application_message_profile_hit_test(x, y);
    if (profile.user_id)
        return application_profile_open(profile.user_id, profile.anchor);
    const MessageTextHit hit = renderer_content_hit_test(g_app.renderer, point.x, point.y);
    if (!hit.valid) {
        return false;
    }
    // Plugin decoration text: the collapsed row's Show link and accessories.
    if (application_plugins_render_click(hit, {x, y, x + 1, y + 1}))
        return true;
    // Generated rows carry their actions in semantic targets: system rows
    // (referenced message, pinned messages, created thread) and the ephemeral
    // "Dismiss message" link, which targets its own message.
    if (g_app.account_ui && hit.target &&
        (hit.kind == MessageMarkdownSemanticKind::message_link || hit.kind == MessageMarkdownSemanticKind::link ||
         hit.kind == MessageMarkdownSemanticKind::channel_mention)) {
        const auto found = message_store_find(g_app.message_view.store, hit.message_id);
        const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (message && message->revision == hit.message_revision) {
            constexpr u32 ephemeral_flag = 1u << 6;
            if (hit.kind == MessageMarkdownSemanticKind::message_link && hit.target == message->id &&
                (message->flags & ephemeral_flag)) {
                SetFocus(g_app.window);
                if (g_app.message_scene.timeline)
                    message_timeline_remove_local(g_app.message_scene.timeline, message->id);
                return refresh_account_frame(true);
            }
            if (message_type_is_system(message->type)) {
                if (hit.kind == MessageMarkdownSemanticKind::message_link)
                    return navigate_content_message(message->channel_id, hit.target);
                if (hit.kind == MessageMarkdownSemanticKind::link && hit.target == 1)
                    return application_pinned_messages_toggle();
                if (hit.kind == MessageMarkdownSemanticKind::channel_mention && message->type == 18)
                    return open_thread_starter(message->channel_id, hit.target);
            }
        }
    }
    switch (hit.kind) {
    case MessageMarkdownSemanticKind::spoiler: {
        if (!g_app.account_ui) {
            return true;
        }
        bool changed = false;
        if (!message_text_presentation_toggle_spoiler(
                &g_app.account_ui->message_text, g_app.message_view.store,
                &g_app.account_ui->message_rich_store, hit, &changed)) {
            return false;
        }
        if (changed) {
            if (!refresh_account_text(true)) {
                return false;
            }
            application_request_frame();
        }
        return true;
    }
    case MessageMarkdownSemanticKind::user_mention:
        return application_profile_open(hit.target, {client_dip_x(lparam), client_dip_y(lparam),
                                                     client_dip_x(lparam) + 1, client_dip_y(lparam) + 1});
    case MessageMarkdownSemanticKind::channel_mention:
        return select_content_channel(hit.target);
    case MessageMarkdownSemanticKind::message_link:
        return open_content_link(hit);
    case MessageMarkdownSemanticKind::link:
        return open_content_link(hit);
    case MessageMarkdownSemanticKind::command_mention:
        return insert_command_mention(hit);
    default:
        return true;
    }
}

bool application_message_content_cursor(float x, float y, UiCursor* cursor) noexcept {
    const auto& scene = g_app.message_scene;
    if (!cursor || application_thread_browser_read_blocked() || !g_app.account_ui || !scene.active ||
        g_app.login.visible || !ui_rect_contains(scene.viewport, x, y))
        return false;
    const UiPoint point{x, y};
    // Media players and polls refine their card to the controls a click activates.
    if (application_video_cursor(point, cursor) || application_poll_cursor(point, cursor))
        return true;
    if (const auto& renderer = g_app.renderer;
        (renderer.message_text && renderer.chat_state && renderer.user_store &&
         message_author_plugin_hit_test(scene, *renderer.message_text, *renderer.chat_state,
                                        *renderer.user_store, renderer.message_measure_cache,
                                        application_theme(), x, y)
             .action_id) ||
        application_downloads_save_target(x, y) || application_gif_favorite_star_target(x, y) ||
        application_image_viewer_open_at(x, y, true) ||
        message_scene_reply_hit_test(scene, x, y).message_id) {
        *cursor = UiCursor::hand;
        return true;
    }
    // Declared rich buttons (reactions, stickers, components, thread cards,
    // generic file cards) present a hand. Media items and embeds are surfaces
    // whose previews and links carry their own targets.
    const auto hit = message_scene_hit_test(scene, x, y);
    const auto* row = hit.message_id ? message_scene_find_row(scene, hit.source_index) : nullptr;
    if (!row || ui_widget_cursor(scene.widgets, hit.widget) != UiCursor::hand)
        return false;
    const auto same = [&](UiWidgetHandle widget) noexcept {
        return widget.generation && widget.slot == hit.widget.slot && widget.generation == hit.widget.generation;
    };
    const auto* record = message_store_resolve(g_app.message_view.store, row->message);
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
        const auto* item_row = content_scope ? message_scene_snapshot_row(scene, *row) : row;
        if (!item_row)
            continue;
        if (same(item_row->poll))
            return false;
        for (u32 i = 0; i < item_row->embed_item_count; ++i)
            if (same(message_scene_rich_item_widget(*item_row, MessageSceneRichItemKind::embed, i)))
                return false;
        const auto* rich = record ? (content_scope ? message_rich_store_snapshot(store, record->id)
                                                   : message_rich_store_find(store, record->id))
                                  : nullptr;
        for (u32 i = 0; i < item_row->attachment_item_count; ++i) {
            if (!same(message_scene_rich_item_widget(*item_row, MessageSceneRichItemKind::attachment, i)))
                continue;
            const auto* node = message_scene_layout_node(scene, hit.widget);
            if (!rich || i >= rich->attachment_count || !node ||
                message_rich_attachment_audio_supported(store, rich->attachments[i]) ||
                message_rich_attachment_format(store, rich->attachments[i]) != MediaAttachmentFormat::unknown)
                return false;
            // The generic file card is the leading 420 x 36 DIP strip.
            const UiRect card{node->rectangle.left, node->rectangle.top,
                              minimum(node->rectangle.right, node->rectangle.left + 420.0f),
                              node->rectangle.top + 36.0f};
            if (!ui_rect_contains(card, x, y))
                return false;
            *cursor = UiCursor::hand;
            return true;
        }
    }
    *cursor = UiCursor::hand;
    return true;
}

bool handle_content_hover(LPARAM lparam) noexcept {
    if (application_thread_browser_read_blocked())
        return false;
    const POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    const MessageTextHit hit = renderer_content_hit_test(g_app.renderer, point.x, point.y);
    if (!g_app.account_ui) {
        return true;
    }
    bool changed = false;
    if (!message_text_presentation_set_hover(&g_app.account_ui->message_text, hit, &changed)) {
        return false;
    }
    if (changed) {
        if (!refresh_account_text(true)) {
            return false;
        }
        application_request_frame();
    }
    return true;
}

} // namespace maicord::app_detail
