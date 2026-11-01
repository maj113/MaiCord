#include "interaction_internal.hpp"
#include "../../guild/community.hpp"
#include "components.hpp"
#include "../../guild/lifecycle.hpp"
#include "../../guild/menu.hpp"
#include "../../voice/stream.hpp"
#include "../../voice/soundboard.hpp"
#include "../../account/friends.hpp"
#include "../../guild/member_moderation.hpp"
#include "../application_private_conversation.hpp"
#include "../../account/account_menu.hpp"
#include "../../shell/scrolling.hpp"
#include "../../guild/channel_overview.hpp"
#include "../../guild/settings/settings.hpp"
#include "maicord/ui/guild/context_policy.hpp"

namespace maicord::app_detail {

static bool activate_popup_action(u32 action, bool shift_pressed) noexcept {
    if (application_plugins_popover_action(action))
        return application_plugins_popover_activate(action);
    if (application_profile_menu_action(action))
        return application_profile_menu_activate(action);
    if (application_voice_menu_action(action))
        return application_voice_menu_activate(action);
    if (application_stream_menu_action(action))
        return application_stream_menu_activate(action);
    if (application_soundboard_menu_action(action))
        return application_soundboard_menu_activate(action, shift_pressed);
    if (application_notification_menu_action(action))
        return application_notification_menu_activate(action);
    if (application_guild_menu_action(action))
        return application_guild_menu_activate(action);
    if (application_scoped_read_menu_action(action))
        return application_scoped_read_menu_activate(action);
    // A right-clicked attachment's media and link rows (message menu owner).
    if (received_attachment_menu_action(action))
        return application_downloads_context_action(action);
    if (message_link_menu_action(action))
        return activate_message_link_action(action);
    if (user_context_action(action))
        return application_user_menu_activate(action);
    if (application_member_moderation_action(action))
        return application_member_moderation_activate(action);
    if (g_app.popup_owner_kind == PopupOwnerKind::account_menu)
        return application_account_menu_action(action);
    if (application_plugins_menu_action_id(action))
        return application_plugins_menu_action(action);
    if (g_app.popup_owner_kind == PopupOwnerKind::friends)
        return application_friends_popup_action(action);
    if (!g_app.popup_scene.active || action == 0) {
        return false;
    }
    if (thread_menu_action(action)) {
        if (!thread_menu_scope.active || g_app.popup_owner_kind != PopupOwnerKind::channel)
            return true;
        return application_thread_menu_activate(thread_menu_activation_source(), action).handled;
    }
    if (!g_app.account_ui)
        return false;
    if (g_app.popup_owner_kind == PopupOwnerKind::commands)
        return application_commands_activate(action);
    if (g_app.popup_owner_kind == PopupOwnerKind::message_components)
        return application_message_components_popup_action(action);
    const u64 owner_id = g_app.popup_scene.owner_id;
    if ((action == static_cast<u32>(MessageContextAction::mark_unread) &&
         g_app.popup_owner_kind == PopupOwnerKind::message) ||
        ((action == popup_action_mark_channel_unread || action == popup_action_retry_channel_unread) &&
         (g_app.popup_owner_kind == PopupOwnerKind::channel ||
          g_app.popup_owner_kind == PopupOwnerKind::private_channel))) {
        const bool message = action == static_cast<u32>(MessageContextAction::mark_unread);
        const auto kind = message ? UnreadContextKind::message : UnreadContextKind::channel;
        const bool owner = message ? g_app.popup_owner_kind == PopupOwnerKind::message
                                   : g_app.popup_owner_kind == PopupOwnerKind::channel ||
                                         g_app.popup_owner_kind == PopupOwnerKind::private_channel;
        const bool retry = action == popup_action_retry_channel_unread;
        if (!owner || unread_popup.kind != kind || unread_popup.id != owner_id ||
            unread_popup.action != action ||
            !unread_context_current(unread_popup, unread_source(kind, owner_id, retry)))
            return dismiss_popup();
        if (message)
            application_read_state_mark_message_unread(owner_id);
        else if (retry)
            application_read_state_retry_channel_unread(owner_id);
        else
            application_read_state_mark_channel_unread(owner_id);
        return dismiss_popup();
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::composer_edit)
        return application_composer_edit_menu_action(action);
    if (g_app.popup_owner_kind == PopupOwnerKind::attachments)
        return application_attachment_action(action);
    if (g_app.popup_owner_kind == PopupOwnerKind::message &&
        action == static_cast<u32>(MessageContextAction::add_reaction)) {
        return open_reaction_picker(owner_id);
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::message &&
        application_user_actions_message_activate(owner_id, action))
        return true;
    if (g_app.popup_owner_kind == PopupOwnerKind::message &&
        action == static_cast<u32>(MessageContextAction::view_reactions)) {
        return open_reaction_viewer(owner_id);
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::message &&
        action == static_cast<u32>(MessageContextAction::create_thread)) {
        // Retire all borrowed popup/plugin descriptors, then revalidate the
        // current source and creation permissions through the host authority.
        const bool current =
            creation_menu_scope.message == owner_id && creation_menu_current(creation_menu_scope);
        if (!dismiss_popup())
            return false;
        return !current || !application_thread_creation_available(owner_id) ||
               application_thread_creation_open(owner_id);
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::message &&
        (action == static_cast<u32>(MessageContextAction::pin_message) ||
         action == static_cast<u32>(MessageContextAction::unpin_message))) {
        const bool pin = action == static_cast<u32>(MessageContextAction::pin_message);
        // Release popup/plugin borrows before opening confirmation. Permissions,
        // message lifetime and pending mutations may have changed since capture.
        if (!dismiss_popup())
            return false;
        if (!application_pinned_messages_can_pin(owner_id))
            return true;
        return application_pinned_messages_action(owner_id, pin, !shift_pressed);
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::autocomplete &&
        action >= popup_action_autocomplete_first) {
        const u32 suggestion_index = action - popup_action_autocomplete_first;
        if (suggestion_index >= g_app.popup_scene.item_count || suggestion_index >= kPopupItemCapacity) {
            return false;
        }
        const ComposerAutocompleteSuggestion& suggestion =
            g_app.composer_autocomplete.suggestions[suggestion_index];
        const u32 begin = g_app.composer_autocomplete.token_begin;
        const u32 end = g_app.composer_autocomplete.token_end;
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
        g_app.composer_autocomplete.count = 0;
        g_app.composer_autocomplete.kind = ComposerAutocompleteKind::none;
        return composer_apply_result(
            message_composer_replace_range(&g_app.message_composer, begin, end,
                                           Utf8View{suggestion.insertion, suggestion.insertion_bytes}),
            true);
    }
    bool applied = false;
    if (application_private_channel_menu_action(action, owner_id, shift_pressed, &applied)) {
        if (!applied)
            log_u64(LogLevel::warning, "ui", "popup_action_not_applied", action);
        return dismiss_popup();
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::message) {
        MessageContextQuickReaction reaction{};
        if (message_context_controller_resolve_quick_reaction(g_app.message_context, action, &reaction)) {
            applied = queue_message_reaction_emoji(owner_id, 0, reaction.glyph, false);
        }
    }
    if (g_app.popup_owner_kind == PopupOwnerKind::message && !applied &&
        action == static_cast<u32>(MessageContextAction::edit_message)) {
        applied = begin_message_edit(owner_id);
    } else if (action == static_cast<u32>(MessageContextAction::reply) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        applied = begin_message_reply(owner_id) && (!shift_pressed || application_reply_draft_set(owner_id, false));
    } else if (action == static_cast<u32>(MessageContextAction::forward_message) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        // Retire the popup and its capture before binding the forwarding modal.
        return dismiss_popup() && application_message_forward_open(owner_id);
    } else if (action == static_cast<u32>(MessageContextAction::copy_text) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        const MessageStoreResult found = message_store_find(g_app.message_view.store, owner_id);
        applied = found.ok() &&
                  application_copy_text(message_store_content(g_app.message_view.store, found.handle),
                                        kToastTextCopied, kToastCopyFailed);
    } else if (action == static_cast<u32>(MessageContextAction::copy_message_link) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        applied = copy_message_link(owner_id);
    } else if (action == static_cast<u32>(MessageContextAction::delete_message) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        // The confirmation replaces the popup; Shift deletes immediately.
        if (!dismiss_popup())
            return false;
        return application_message_delete_request(owner_id, shift_pressed) || refresh_account_frame(true);
    } else if (action == static_cast<u32>(MessageContextAction::retry_send) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        applied = retry_failed_message(owner_id);
    } else if (action == static_cast<u32>(MessageContextAction::discard_failed) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        applied = discard_failed_message(owner_id);
    } else if (action == static_cast<u32>(MessageContextAction::message_author) &&
               g_app.popup_owner_kind == PopupOwnerKind::message) {
        const auto* record = application_message_record(owner_id);
        applied =
            record && queue_private_conversation(PrivateConversationOperation::open, record->author_id);
    } else if (action == popup_action_message_user && g_app.popup_owner_kind == PopupOwnerKind::member) {
        applied = queue_private_conversation(PrivateConversationOperation::open, owner_id);
    } else if (action == popup_action_mark_channel_read &&
               (g_app.popup_owner_kind == PopupOwnerKind::channel ||
                g_app.popup_owner_kind == PopupOwnerKind::private_channel)) {
        // Resolve again because permissions or channel lifetime may change
        // while the native context menu is open.
        PopupItem current_items[2]{};
        const u32 count = channel_context_policy_build(g_app.account_ui->chat_state, owner_id,
                                                       popup_action_mark_channel_read,
                                                       popup_action_close_dm, {current_items, 2});
        applied = count && current_items[0].action == popup_action_mark_channel_read &&
                  application_read_state_mark(owner_id, true);
    } else if (action == popup_action_browse_threads && g_app.popup_owner_kind == PopupOwnerKind::channel) {
        const auto* parent = chat_state_store_find_channel(g_app.account_ui->chat_state, owner_id);
        const bool current = parent && parent->generation == browser_menu_scope.generation &&
                             owner_id == browser_menu_scope.parent &&
                             browser_menu_scope.account == g_app.plugin_account_scope_serial &&
                             browser_menu_scope.profile == g_app.account.status.profile_id &&
                             browser_menu_scope.self == g_app.account_ui->chat_state.self_user_id;
        if (!dismiss_popup())
            return false;
        return !current || !application_thread_browser_available(owner_id) ||
               application_thread_browser_open(owner_id);
    } else if (action == popup_action_edit_channel && g_app.popup_owner_kind == PopupOwnerKind::channel) {
        dismiss_popup();
        return application_channel_overview_open(owner_id);
    } else if (action == popup_action_invite_people && g_app.popup_owner_kind == PopupOwnerKind::guild) {
        dismiss_popup();
        return application_guild_lifecycle_open_invite(owner_id) || true;
    } else if (action == popup_action_guild_onboarding && g_app.popup_owner_kind == PopupOwnerKind::guild) {
        dismiss_popup();
        return application_guild_community_open_onboarding(owner_id, true) || true;
    } else if (action == popup_action_create_event && g_app.popup_owner_kind == PopupOwnerKind::guild) {
        dismiss_popup();
        return application_guild_community_open_create(owner_id) || true;
    } else if (action == popup_action_leave_guild && g_app.popup_owner_kind == PopupOwnerKind::guild) {
        dismiss_popup();
        return application_guild_lifecycle_leave(owner_id) || true;
    } else if (action == popup_action_open_voice_chat && g_app.popup_owner_kind == PopupOwnerKind::channel) {
        dismiss_popup();
        return application_navigate_known_message(owner_id, 0) || true;
    } else if (action == popup_action_folder_settings && g_app.popup_owner_kind == PopupOwnerKind::guild_folder) {
        dismiss_popup();
        return application_guild_lifecycle_open_folder(owner_id) || true;
    } else if ((action == popup_action_create_text_channel || action == popup_action_create_voice_channel) &&
               (g_app.popup_owner_kind == PopupOwnerKind::guild ||
                g_app.popup_owner_kind == PopupOwnerKind::channel)) {
        const bool guild = g_app.popup_owner_kind == PopupOwnerKind::guild;
        dismiss_popup();
        return application_guild_settings_create_channel(guild ? owner_id : 0, guild ? 0 : owner_id,
                                                         action == popup_action_create_voice_channel ? 2 : 0);
    } else if (action == popup_action_copy_guild_id && g_app.popup_owner_kind == PopupOwnerKind::guild &&
               g_app.preferences.developer_mode) {
        PopupItem current{};
        char id[20]{};
        const u32 bytes = guild_context_policy_build(g_app.account_ui->chat_state, owner_id,
                                                     popup_action_copy_guild_id, {&current, 1})
                              ? message_context_format_snowflake(owner_id, {id, 20})
                              : 0;
        applied = application_copy_text({id, bytes}, kToastIdCopied, kToastIdCopyFailed);
    } else if (action == popup_action_close_dm &&
               g_app.popup_owner_kind == PopupOwnerKind::private_channel) {
        const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, owner_id);
        applied = channel && channel->type == 1 &&
                  queue_private_conversation(PrivateConversationOperation::close, owner_id);
    } else if (action == popup_action_private_notice_retry &&
               g_app.popup_owner_kind == PopupOwnerKind::private_notice) {
        const auto& request = g_app.account_ui->private_conversation;
        applied = queue_private_conversation(request.operation, request.target_id);
    } else if (action == popup_action_private_notice_close &&
               g_app.popup_owner_kind == PopupOwnerKind::private_notice) {
        reset_value(&g_app.account_ui->private_conversation);
        applied = true;
    } else if (action == popup_action_copy_member_name &&
               g_app.popup_owner_kind == PopupOwnerKind::member) {
        const UserRecord* user = user_store_find(g_app.account_ui->user_store, owner_id);
        applied = user && application_copy_text(user_store_name(g_app.account_ui->user_store, *user),
                                                kToastUsernameCopied, kToastCopyFailed);
    } else if (action == static_cast<u32>(MessageContextAction::copy_id) &&
               g_app.popup_owner_kind == PopupOwnerKind::message && g_app.preferences.developer_mode) {
        applied = copy_message_id(owner_id, shift_pressed);
    } else if (action == popup_action_copy_member_id && g_app.popup_owner_kind == PopupOwnerKind::member &&
               g_app.preferences.developer_mode) {
        char id[20]{};
        const u32 bytes = message_context_format_snowflake(owner_id, Slice<char>{id, 20});
        applied = application_copy_text({id, bytes}, kToastIdCopied, kToastIdCopyFailed);
    }
    if (!applied) {
        log_u64(LogLevel::warning, "ui", "popup_action_not_applied", action);
    }
    const bool dismissed = dismiss_popup();
    return dismissed && ((action != static_cast<u32>(MessageContextAction::reply) &&
                          action != static_cast<u32>(MessageContextAction::edit_message)) ||
                         !applied || refresh_message_composer_ui());
}

bool refresh_reaction_viewer_presentation() noexcept {
    return refresh_account_frame(true);
}

static bool dismiss_reaction_viewer() noexcept {
    ui_scroll_host_cancel(&g_app.scroll_host);
    if (!g_app.reaction_viewer.active) {
        return true;
    }
    const ReactionViewerControllerResult closed =
        reaction_viewer_controller_close(&g_app.reaction_viewer_controller);
    if (!closed.ok()) {
        log_u64(LogLevel::warning, "reaction_viewer", "close_error", static_cast<u64>(closed.error));
        return false;
    }
    return !closed.changed || refresh_reaction_viewer_presentation();
}

bool handle_reaction_viewer_hover(float pointer_x, float pointer_y) noexcept {
    if (!g_app.reaction_viewer.active) {
        return false;
    }
    bool changed = false;
    if (!reaction_viewer_set_hover(&g_app.reaction_viewer, pointer_x, pointer_y, &changed)) {
        return true;
    }
    if (changed && !refresh_reaction_viewer_presentation()) {
        log_write(LogLevel::warning, "reaction_viewer", "hover_refresh_failed");
    }
    return true;
}

bool handle_reaction_viewer_click(float pointer_x, float pointer_y) noexcept {
    if (!g_app.reaction_viewer.active) {
        return false;
    }
    const ReactionViewerHit hit = reaction_viewer_hit_test(g_app.reaction_viewer, pointer_x, pointer_y);
    if (hit.kind == ReactionViewerHitKind::outside || hit.kind == ReactionViewerHitKind::close) {
        return dismiss_reaction_viewer();
    }
    if (hit.kind == ReactionViewerHitKind::aggregate) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const ReactionViewerControllerResult selected =
            reaction_viewer_controller_select(&g_app.reaction_viewer_controller, hit.index);
        if (!selected.ok()) {
            log_u64(LogLevel::warning, "reaction_viewer", "select_error", static_cast<u64>(selected.error));
            return true;
        }
        if (selected.changed) {
            refresh_reaction_viewer_presentation();
            pump_message_history();
        }
    }
    return true;
}

bool handle_reaction_viewer_key(WPARAM key) noexcept {
    ReactionViewer& viewer = g_app.reaction_viewer;
    if (!viewer.active) {
        return false;
    }
    if (key == VK_ESCAPE) {
        return dismiss_reaction_viewer();
    }
    bool changed = false;
    if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const i32 rows = key == VK_UP      ? -1
                         : key == VK_DOWN  ? 1
                         : key == VK_PRIOR ? -static_cast<i32>(reaction_viewer_visible_user_count(viewer))
                                           : static_cast<i32>(reaction_viewer_visible_user_count(viewer));
        reaction_viewer_scroll_users(&viewer, rows, &changed);
        const ReactionViewerControllerResult more =
            reaction_viewer_controller_request_more(&g_app.reaction_viewer_controller);
        changed = changed || (more.ok() && more.changed);
        if (more.ok() && more.changed) {
            pump_message_history();
        }
    } else if ((key == VK_LEFT || key == VK_RIGHT) && viewer.aggregate_count != 0) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const u32 selected = key == VK_LEFT
                                 ? (viewer.selected_aggregate == 0 ? viewer.aggregate_count - 1
                                                                   : viewer.selected_aggregate - 1)
                                 : (viewer.selected_aggregate + 1) % viewer.aggregate_count;
        const ReactionViewerControllerResult result =
            reaction_viewer_controller_select(&g_app.reaction_viewer_controller, selected);
        changed = result.ok() && result.changed;
        if (changed) {
            pump_message_history();
        }
    }
    if (changed) {
        refresh_reaction_viewer_presentation();
    }
    return true;
}

bool handle_popup_hover(LPARAM lparam) noexcept {
    if (!g_app.popup_scene.active) {
        return false;
    }
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const float x = static_cast<short>(LOWORD(lparam)) * scale;
    const float y = static_cast<short>(HIWORD(lparam)) * scale;
    if (application_voice_menu_pointer(x, y, false) || application_stream_menu_pointer(x, y, false))
        return true;
    // The editor owns its captured drag, even when it crosses the suggestions.
    if (g_app.popup_scene.kind == PopupKind::autocomplete && g_app.message_composer_pointer_selection)
        return false;
    bool changed = false;
    if (!popup_scene_set_hover(&g_app.popup_scene, x, y, &changed)) {
        return false;
    }
    if (changed && g_app.popup_owner_kind == PopupOwnerKind::commands)
        application_commands_hover();

    if (changed && !refresh_popup_text())
        log_write(LogLevel::warning, "ui", "popup_hover_refresh_failed");
    return g_app.popup_scene.kind != PopupKind::autocomplete ||
           popup_scene_hit_test(g_app.popup_scene, x, y).inside;
}

bool handle_popup_click(WPARAM wparam, LPARAM lparam) noexcept {
    if (!g_app.popup_scene.active) {
        return false;
    }
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const float x = static_cast<short>(LOWORD(lparam)) * scale;
    const float y = static_cast<short>(HIWORD(lparam)) * scale;
    if (application_voice_menu_pointer(x, y, true) || application_stream_menu_pointer(x, y, true))
        return true;
    // A scrollable menu's bar drags like every other list's.
    if (ui_rect_contains(g_app.popup_scene.rectangle, x, y) && handle_scrollbar_begin(x, y))
        return true;
    const PopupHit hit = popup_scene_hit_test(g_app.popup_scene, x, y);
    if (hit.enabled) {
        if (!activate_popup_action(hit.action, (wparam & MK_SHIFT) != 0))
            log_write(LogLevel::warning, "ui", "popup_action_failed");
        return true;
    }
    if (hit.inside) {
        return true;
    }
    // Suggestions belong to the editor; clicking/selecting its text should
    // neither dismiss the picker nor transfer focus to its menu widgets.
    const bool profile = g_app.profile && g_app.profile->scene.active;
    const bool autocomplete = g_app.popup_scene.kind == PopupKind::autocomplete;
    if (autocomplete && !profile &&
        message_composer_scene_hit_test(g_app.message_composer_scene, x, y) == MessageComposerControl::edit)
        return false;
    dismiss_popup();
    return profile || !autocomplete;
}

bool handle_popup_key(WPARAM key) noexcept {
    if (!g_app.popup_scene.active) {
        // A selected command without a list (only its option bar shows) still
        // hands its built-in's inline results their keys; Escape dismisses them.
        if (!g_app.message_composer.focused || !application_commands_bar_visible())
            return false;
        if (key == VK_ESCAPE)
            return application_commands_escape();
        bool handled = false;
        return application_media_picker_inline_key(static_cast<u32>(key), &handled) && handled &&
               refresh_popup_text();
    }
    if (key == VK_ESCAPE) {
        // The command picker's rail hands Escape back to its list first.
        if (g_app.popup_owner_kind == PopupOwnerKind::commands && application_commands_key(VK_ESCAPE))
            return refresh_popup_text();
        return dismiss_popup();
    }
    // The soundboard picker filters by typed letters, digits and spaces.
    if (g_app.popup_owner_kind == PopupOwnerKind::soundboard && application_soundboard_menu_key(static_cast<u32>(key)))
        return true;
    if (g_app.popup_owner_kind == PopupOwnerKind::commands) {
        const bool shift = application_shift_down();
        if (application_control_down() || application_alt_down())
            return false;
        // Shift+Tab selects the previous option chip.
        if (shift) {
            if (key != VK_TAB || !application_commands_option_tab(true))
                return false;
            if (!g_app.popup_scene.active)
                g_app.composer_menu_held_key = static_cast<u32>(key);
            return refresh_popup_text();
        }
        if (application_commands_key(static_cast<u32>(key))) {
            if (!g_app.popup_scene.active && (key == VK_RETURN || key == VK_TAB))
                g_app.composer_menu_held_key = static_cast<u32>(key);
            return refresh_popup_text();
        }
        return false;
    }
    if (key == VK_RETURN || (key == VK_TAB && g_app.popup_scene.kind == PopupKind::autocomplete) ||
        (key == VK_SPACE && g_app.popup_scene.kind != PopupKind::autocomplete)) {
        const u32 action = popup_scene_selected_action(g_app.popup_scene);
        if (action && (g_app.popup_owner_kind == PopupOwnerKind::composer_edit ||
                       g_app.popup_owner_kind == PopupOwnerKind::account_menu))
            g_app.composer_menu_held_key = static_cast<u32>(key);
        return action == 0 || activate_popup_action(action, application_shift_down());
    }
    if ((key == VK_PRIOR || key == VK_NEXT) && (g_app.popup_scene.kind == PopupKind::context_menu ||
                                                g_app.popup_scene.kind == PopupKind::autocomplete ||
                                                g_app.popup_scene.kind == PopupKind::popover)) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const float before = g_app.popup_scene.scroll_offset;
        bool changed = false;
        if (!popup_scene_page_selection(&g_app.popup_scene, key == VK_PRIOR, &changed))
            return true;
        return animate_popup_scroll_from(before) && (!changed || refresh_popup_text());
    }
    if ((key == VK_HOME || key == VK_END) && (g_app.popup_scene.kind == PopupKind::context_menu ||
                                              g_app.popup_scene.kind == PopupKind::popover)) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const float before = g_app.popup_scene.scroll_offset;
        const u32 count = g_app.popup_scene.item_count;
        for (u32 i = 0; i < count; ++i) {
            const u32 index = key == VK_HOME ? i : count - i - 1;
            if (!(g_app.popup_scene.items[index].flags & popup_item_enabled))
                continue;
            bool changed = false;
            return popup_scene_select_index(&g_app.popup_scene, index, &changed) &&
                   animate_popup_scroll_from(before) && (!changed || refresh_popup_text());
        }
        return true;
    }
    if (key == VK_UP || key == VK_DOWN) {
        ui_scroll_host_cancel(&g_app.scroll_host);
        const float before = g_app.popup_scene.scroll_offset;
        bool changed = false;
        if (!popup_scene_move_selection(&g_app.popup_scene, key == VK_DOWN ? 1 : -1, &changed)) {
            return false;
        }
        return animate_popup_scroll_from(before) && (!changed || refresh_popup_text());
    }
    return g_app.popup_scene.kind != PopupKind::autocomplete;
}

} // namespace maicord::app_detail
