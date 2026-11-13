// The account's per-frame text refresh: which sources changed since the last accepted build, the shell and composer
// layout they need, and the backoff after a rejected build. Alone in this file so the account pump's test compiles it
// against its own host adapters.
#include "account_presentation.hpp"
#include "account_text_retry.hpp"
#include "account_ui_runtime.hpp"
#include "dm_profile.hpp"
#include "friends.hpp"
#include "../chat/channel_authority.hpp"
#include "../chat/composer/attachments.hpp"
#include "../chat/composer/commands.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/reply_draft.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../chat/threads/browser.hpp"
#include "../chat/threads/starter.hpp"
#include "../plugins/surface_host.hpp"
#include "../settings/settings.hpp"
#include "../shell/state.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11/renderer_settings_visibility.hpp"
#include "maicord/ui/guild/context_policy.hpp"

namespace maicord::app_detail {
namespace {
const UiLayoutNode* shell_node(UiSemantic semantic) noexcept {
    return ui_scene_node(g_app.shell_scene, semantic);
}
} // namespace

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
    // Invite People heads a server's list while one of its channels takes invites.
    if (!friends && ui && !ui->chat_state.home_selected && ui->chat_state.selected_guild_id &&
        guild_context_invite_channel(ui->chat_state, ui->chat_state.selected_guild_id,
                                     ui->chat_state.selected_channel_id))
        flags |= ui_shell_layout_guild_invite;
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
           application_plugin_surfaces_stale() ||
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
    application_plugin_surfaces_prepare();
    const bool stale = force || retry_pending || account_text_sources_stale(ui);
    if (!stale) {
        return true;
    }
    const u64 now_ms = clock_monotonic_milliseconds();
    if (ui.text_rebuild_retry_at_ms != 0 && now_ms < ui.text_rebuild_retry_at_ms) {
        return true;
    }
    // The composer's height never depends on who is typing: the strip above it is always reserved.
    const float required_composer_height = message_composer_scene_cached_height(
        g_app.message_composer_scene,
        application_reply_draft().message_id != 0 || g_app.message_edit.editor_active,
        message_composer_attachment_row(application_attachment_input()), 0.f, application_commands_bar_visible());
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
    application_plugin_surfaces_rendered();
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

bool refresh_account_frame() noexcept {
    return refresh_account_text(true) && (!g_app.window || application_request_frame() != FALSE);
}

} // namespace maicord::app_detail
