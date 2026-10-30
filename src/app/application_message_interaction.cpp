#include "application_guild_community.hpp"
#include "maicord/core/memory.hpp"
#include "application_toast.hpp"
#include "application_content_input.hpp"
#include "application_user_actions.hpp"
#include "maicord/ui/user_context_menu.hpp"
#include "application_message_components.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_guild_menu.hpp"
#include "application_guild_folders.hpp"
#include "application_click_count.hpp"
#include "application_action_captcha.hpp"
#include "application_reply_draft.hpp"
#include "application_voice.hpp"
#include "application_stream.hpp"
#include "application_soundboard.hpp"
#include "application_commands.hpp"
#include "application_friends.hpp"
#include "application_profile.hpp"
#include "application_message_interaction.hpp"
#include "application_message_forward.hpp"
#include "application_downloads.hpp"
#include "application_member_moderation.hpp"
#include "application_composer_edit_menu.hpp"
#include "application_attachments.hpp"
#include "application_plugins.hpp"

#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_feedback.hpp"
#include "application_message_delete.hpp"
#include "application_message_edit.hpp"
#include "application_message_history.hpp"
#include "application_message_reaction.hpp"
#include "application_thread_header.hpp"
#include "application_pinned_messages.hpp"
#include "application_private_conversation.hpp"
#include "application_private_channel.hpp"
#include "application_state.hpp"
#include "application_read_state.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_notification_controls.hpp"
#include "application_account_menu.hpp"
#include "application_media_picker.hpp"
#include "application_retained_projection.hpp"
#include "application_scrolling.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/navigation_context.hpp"
#include "application_channel_overview.hpp"
#include "application_guild_settings.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/unread_context.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/received_attachment_menu.hpp"
#include "maicord/ui/message_author.hpp"

namespace maicord::app_detail {

namespace {
UnreadContextTarget unread_popup;
struct ThreadMenuScope {
    ApplicationThreadActionTicket ticket;
    bool active, from_header;
};
ThreadMenuScope thread_menu_scope;
struct ThreadBrowserMenuScope {
    u64 account, profile, self, parent;
    u16 generation;
};
ThreadBrowserMenuScope browser_menu_scope;
struct MessageThreadCreationMenuScope {
    u64 account, profile, self, parent, message;
    u32 message_revision;
    u16 channel_generation, message_generation;
    bool available;
};
MessageThreadCreationMenuScope creation_menu_scope;
bool creation_menu_current(const MessageThreadCreationMenuScope& source) noexcept {
    const auto* ui = g_app.account_ui;
    const auto* parent = ui ? chat_state_store_find_channel(ui->chat_state, source.parent) : nullptr;
    const auto* message = application_message_record(source.message);
    return source.available && ui && source.account == g_app.plugin_account_scope_serial &&
           source.profile == g_app.account.status.profile_id &&
           source.self == ui->chat_state.self_user_id && source.parent == ui->active_channel_id &&
           parent && parent->generation == source.channel_generation && message &&
           message->channel_id == source.parent && message->generation == source.message_generation &&
           message->revision == source.message_revision &&
           message->delivery == MessageDeliveryState::authoritative;
}
bool thread_menu_current(const ApplicationThreadActionTicket& source) noexcept {
    if (!thread_menu_scope.active || source.thread_id != thread_menu_scope.ticket.thread_id ||
        !application_thread_actions_current(source) || !g_app.account_ui)
        return false;
    return thread_menu_scope.from_header
               ? g_app.account_ui->active_channel_id == source.thread_id
               : channel_context_row_current(g_app.account_ui->navigation_view,
                                             g_app.shell_navigation_scene, source.thread_id) &&
                     channel_context_target_current(g_app.account_ui->chat_state, source.thread_id,
                                                    source.channel_generation, source.channel_revision);
}
bool thread_menu_action(u32 action) noexcept {
    const auto ids = kApplicationThreadMenuActions;
    return action == ids.check_status || action == ids.join || action == ids.leave ||
           action == ids.unarchive || action == ids.retry || action == ids.cancel;
}
ApplicationThreadHeaderActivationSource thread_menu_activation_source() noexcept {
    auto source = application_thread_header_activation_source(nullptr, thread_menu_scope.ticket);
    source.popup = &g_app.popup_scene;
    source.menu_actions = kApplicationThreadMenuActions;
    source.current = [](void*, const ApplicationThreadActionTicket& ticket) noexcept {
        return thread_menu_current(ticket);
    };
    return source;
}
UnreadContextSource unread_source(UnreadContextKind kind, u64 id, bool retry = false) noexcept {
    auto* ui = g_app.account_ui;
    const bool available = kind == UnreadContextKind::message
                               ? application_read_state_message_unread_available(id)
                               : kind == UnreadContextKind::channel &&
                                     (retry ? application_read_state_channel_unread_retry_available(id)
                                            : application_read_state_channel_unread_available(id));
    return {g_app.plugin_account_scope_serial,
            g_app.account.status.profile_id,
            ui ? ui->chat_state.self_user_id : 0,
            g_app.account.instance,
            ui ? &ui->chat_state : nullptr,
            &g_app.message_view.store,
            &g_app.message_scene,
            ui ? &ui->navigation_view : nullptr,
            &g_app.shell_navigation_scene,
            &g_app.popup_scene,
            available};
}
bool unread_popup_capture(UnreadContextKind kind, u64 id, bool enabled, u32 action) noexcept {
    UnreadContextTarget next{};
    if (enabled &&
        !unread_context_capture(unread_source(kind, id, action == popup_action_retry_channel_unread), kind,
                                id, &next, action)) {
        reset_value(&unread_popup);
        dismiss_popup();
        return false;
    }
    unread_popup = next;
    return true;
}
} // namespace

static const MessageTextCommand* selected_message_text_command() noexcept {
    if (!g_app.account_ui || !g_app.message_text_selection.valid) {
        return nullptr;
    }
    const MessageTextPresentation& presentation = g_app.account_ui->message_text;
    if (!presentation.active || g_app.message_text_selection.command_index >= presentation.command_count ||
        presentation.command_count > presentation.commands.count) {
        return nullptr;
    }
    const MessageTextCommand& command = presentation.commands[g_app.message_text_selection.command_index];
    return command.message_id == g_app.message_text_selection.message_id &&
                   command.measurement_identity == g_app.message_text_selection.command_identity &&
                   command.message_revision == g_app.message_text_selection.message_revision &&
                   command.text.bytes == g_app.message_text_selection.text_bytes
               ? &command
               : nullptr;
}

bool clear_message_text_selection(bool rebuild) noexcept {
    const bool had_selection = g_app.message_text_selection.valid;
    const bool held = g_app.message_text_pointer_selection;
    g_app.message_text_pointer_selection = false;
    reset_value<RendererMessageTextPositionHit>(&g_app.message_text_selection);
    g_app.message_text_selection_anchor = 0;
    g_app.message_text_selection_caret = 0;
    renderer_clear_message_text_selection(&g_app.renderer);
    if (held && GetCapture() == g_app.window) {
        ReleaseCapture();
    }
    if (!rebuild || !had_selection || !g_app.account_ui) {
        return true;
    }
    const bool refreshed = refresh_account_text(true);
    const bool invalidated = !g_app.window || application_request_frame() != FALSE;
    return refreshed && invalidated;
}

bool begin_message_text_selection(LPARAM lparam, bool double_click) noexcept {
    if (!g_app.account_ui || g_app.login.visible) {
        return false;
    }
    const RendererMessageTextPositionHit hit = renderer_message_text_position_hit_test(
        g_app.renderer, static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)));
    if (!hit.valid) {
        return false;
    }
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const u8 clicks =
        application_click_count(&g_app.message_scene, static_cast<float>(static_cast<short>(LOWORD(lparam))) * scale,
                                static_cast<float>(static_cast<short>(HIWORD(lparam))) * scale, double_click);
    g_app.message_text_selection = hit;
    u32 anchor = hit.byte_offset, caret = hit.byte_offset;
    g_app.message_text_pointer_unit = {anchor, anchor, UiTextSelectionUnit::character};
    // Double click selects the word, triple click the paragraph (line).
    if (clicks > 1) {
        const MessageTextCommand* command = selected_message_text_command();
        if (!command || !ui_text_pointer_press(command->text, hit.byte_offset, clicks, false, false, anchor, false,
                                               &g_app.message_text_pointer_unit, &anchor, &caret)) {
            anchor = caret = hit.byte_offset;
            g_app.message_text_pointer_unit = {anchor, anchor, UiTextSelectionUnit::character};
        }
    }
    g_app.message_text_selection_anchor = anchor;
    g_app.message_text_selection_caret = caret;
    g_app.message_text_pointer_selection = true;
    renderer_set_message_text_selection(&g_app.renderer, hit, anchor, caret);
    SetCapture(g_app.window);
    if (anchor != caret) {
        if (!refresh_account_text(true))
            log_write(LogLevel::error, "ui", "message_text_selection_rebuild_failed");
        application_request_frame();
    }
    return true;
}

void end_message_text_selection() noexcept {
    const bool held = g_app.message_text_pointer_selection;
    g_app.message_text_pointer_selection = false;
    if (held && GetCapture() == g_app.window) {
        ReleaseCapture();
    }
}

bool handle_message_text_selection_drag(WPARAM buttons, LPARAM lparam) noexcept {
    if (!g_app.message_text_pointer_selection) {
        return false;
    }
    if ((buttons & MK_LBUTTON) == 0) {
        end_message_text_selection();
        return true;
    }

    const RendererMessageTextPositionHit hit = renderer_message_text_position_hit_test(
        g_app.renderer, static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)));
    if (!hit.valid || hit.message_id != g_app.message_text_selection.message_id ||
        hit.command_identity != g_app.message_text_selection.command_identity ||
        hit.message_revision != g_app.message_text_selection.message_revision ||
        hit.command_index != g_app.message_text_selection.command_index) {
        return true;
    }
    // Word/paragraph selections grow by whole units from the clicked one.
    u32 anchor = g_app.message_text_selection_anchor, caret = hit.byte_offset;
    if (g_app.message_text_pointer_unit.unit != UiTextSelectionUnit::character) {
        const MessageTextCommand* command = selected_message_text_command();
        if (!command || !ui_text_pointer_drag(command->text, g_app.message_text_pointer_unit, hit.byte_offset,
                                              false, false, &anchor, &caret))
            return true;
    }
    if (anchor == g_app.message_text_selection_anchor && caret == g_app.message_text_selection_caret)
        return true;
    g_app.message_text_selection_anchor = anchor;
    g_app.message_text_selection_caret = caret;
    renderer_set_message_text_selection(&g_app.renderer, g_app.message_text_selection,
                                        g_app.message_text_selection_anchor,
                                        g_app.message_text_selection_caret);
    if (!refresh_account_text(true)) {
        log_write(LogLevel::error, "ui", "message_text_selection_rebuild_failed");
        end_message_text_selection();
        return true;
    }
    application_request_frame();
    return true;
}

bool copy_message_text_selection() noexcept {
    const MessageTextCommand* command = selected_message_text_command();
    const u32 first = minimum(g_app.message_text_selection_anchor, g_app.message_text_selection_caret);
    const u32 last = maximum(g_app.message_text_selection_anchor, g_app.message_text_selection_caret);
    if (!command || first >= last || last > command->text.bytes) {
        return false;
    }
    Utf8View copied{command->text.data + first, last - first};
    char projected[kMaximumMessageTextUtf8Bytes]{};
    const MessageStore* store = g_app.message_view.timeline.store;
    const Utf8View source = store ? message_text_presentation_command_source(
                                        *store, g_app.message_view.timeline.rich_store, *command)
                                  : Utf8View{};
    if (command->source_id != 0 && source.bytes == 0)
        return false;
    if (source.bytes != 0) {
        const MessageTextCopyProjectionResult projection = message_text_project_selection_for_copy(
            *command, source, first, last, Slice<char>{projected, sizeof(projected)});
        if (projection.ok) {
            copied = Utf8View{projected, projection.bytes};
        }
    }
    if (!clipboard_write_utf8(g_app.window, copied)) {
        log_write(LogLevel::warning, "ui", "message_text_selection_copy_failed");
    }
    return true;
}

bool refresh_popup_text() noexcept {
    // Popup hover and opening change a retained overlay, not the timeline.
    // Reconcile actual account changes without forcing message-row convergence
    // and then refresh only the renderer's retained interaction suffix.
    return refresh_account_text(false) &&
           (!g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme())) &&
           (!g_app.window || application_request_frame() != FALSE);
}

bool dismiss_popup() noexcept {
    const bool changed = popup_scene_close(&g_app.popup_scene);
    application_scoped_read_menu_clear();
    application_plugins_menu_dismissed();
    set_popup_owner(PopupOwnerKind::none);
    reset_value(&thread_menu_scope);
    reset_value(&creation_menu_scope);
    g_app.composer_autocomplete.count = 0;
    g_app.composer_autocomplete.kind = ComposerAutocompleteKind::none;
    // A dismissed '@' list stays closed until the draft changes again.
    g_app.composer_autocomplete.member_query_guild_id = 0;
    return !changed || refresh_popup_text();
}

bool open_reaction_picker(u64 message_id) noexcept {
    if (!g_app.popup_scene.active)
        return false;
    return open_reaction_picker(message_id, g_app.popup_scene.anchor);
}

// Add Reaction opens the shared emoji picker as a popout; it replaces the
// menu it may have been chosen from.
bool open_reaction_picker(u64 message_id, UiRect anchor) noexcept {
    if (!g_app.account_ui || message_id == 0 || (g_app.popup_scene.active && !dismiss_popup()))
        return false;
    return application_media_picker_open_reaction(message_id, anchor);
}

static bool retry_failed_message(u64 nonce) noexcept {
    if (!g_app.account_ui || nonce == 0) {
        return false;
    }
    const MessageRecord* record = application_message_record(nonce);
    if (!record || record->delivery != MessageDeliveryState::failed) {
        return false;
    }
    // A failed local command message re-executes its command instead.
    if (application_commands_retry(nonce))
        return refresh_account_frame(true);
    const auto captcha = application_action_captcha_phase(ApplicationActionCaptchaKind::message_send, nonce);
    if (captcha == ApplicationActionCaptchaPhase::captured || captcha == ApplicationActionCaptchaPhase::failed)
        return application_action_captcha_verify(ApplicationActionCaptchaKind::message_send, nonce) &&
               refresh_account_text(true);
    if (captcha == ApplicationActionCaptchaPhase::waiting)
        return true;
    const MessageComposerControllerResult retried = message_composer_controller_retry(
        &g_app.account_ui->message_composer_controller, nonce, clock_monotonic_milliseconds());
    if (!retried.ok()) {
        if (retried.error == MessageComposerControllerError::reply_unavailable) {
            application_reply_menu_deleted(record->channel_id);
            return refresh_popup_text();
        }
        log_u64(LogLevel::error, "message", "message_retry_controller_error",
                static_cast<u64>(retried.error));
        return false;
    }
    const bool refreshed = refresh_account_text(true);
    const bool invalidated = !g_app.window || application_request_frame() != FALSE;
    // Presentation rebuilding is not allowed to suppress an already accepted
    // user retry. The retained request must reach the transport even if this
    // frame's text snapshot could not be refreshed.
    const bool pumped = pump_message_history();
    return refreshed && invalidated && pumped;
}

static bool discard_failed_message(u64 nonce) noexcept {
    if (!g_app.account_ui || nonce == 0) {
        return false;
    }
    u64 armed_channel = 0;
    for (const auto& request : g_app.account_ui->message_composer_controller.requests)
        if (request.occupied && request.nonce == nonce && request.attachment_bytes)
            armed_channel = request.channel_id;
    const MessageComposerControllerResult discarded =
        message_composer_controller_discard_failed(&g_app.account_ui->message_composer_controller, nonce);
    if (!discarded.ok()) {
        log_u64(LogLevel::error, "message", "message_discard_controller_error",
                static_cast<u64>(discarded.error));
        return false;
    }
    if (armed_channel)
        application_composer_slowmode_clear(armed_channel);
    application_attachment_complete(nonce);
    return refresh_account_frame(true);
}

bool open_message_context_popup(u64 message_id, UiRect anchor) noexcept {
    return open_message_media_context_popup(message_id, anchor, {});
}

bool open_message_media_context_popup(u64 message_id, UiRect anchor, ConstSlice<PopupItem> media,
                                      const ApplicationPluginMenuExtra* plugin) noexcept {
    if (!g_app.account_ui || g_app.login.visible || message_id == 0) {
        return false;
    }
    const MessageRecord* record = application_message_record(message_id);
    if (!record) {
        return false;
    }
    const MessageRichRecord* rich =
        message_rich_store_find(g_app.account_ui->message_rich_store, message_id);
    const bool can_unread = unread_context_available(unread_source(UnreadContextKind::message, message_id),
                                                     UnreadContextKind::message, message_id);
    PopupItem plugin_items[kApplicationPluginMenuRows]{};
    const u32 plugin_count = application_plugins_menu_items(ApplicationPluginMenuSurface::message, record->id,
                                                            {plugin_items, kApplicationPluginMenuRows}, 0, plugin);
    const auto open = [&](ConstSlice<PopupItem> extra) noexcept {
        return message_context_controller_open(
            &g_app.message_context, &g_app.popup_scene, *record,
            application_message_edit_available(record->id), message_can_delete(*record),
            rich && rich->reaction_count != 0, &g_app.account_ui->reaction_preferences, anchor,
            application_viewport_dips(),
            record->author_id != 0 && record->author_id != g_app.account_ui->chat_state.self_user_id, extra,
            application_pinned_messages_can_pin(record->id),
            application_message_forward_available(record->id), can_unread,
            application_reply_request_deleted(record->channel_id, record->id),
            application_thread_creation_available(record->id), g_app.preferences.developer_mode,
            application_user_actions_message_extras(*record), media);
    };
    auto menu = open({plugin_items, plugin_count});
    if (!menu.ok() && plugin_count) {
        log_u64(LogLevel::warning, "plugins", "message_menu_open_failed_using_builtins",
                static_cast<u64>(menu.error));
        application_plugins_menu_dismissed();
        menu = open({});
    }
    if (!menu.ok()) {
        return false;
    }
    const auto* parent =
        chat_state_store_find_channel(g_app.account_ui->chat_state, record->channel_id);
    creation_menu_scope = {g_app.plugin_account_scope_serial,
                           g_app.account.status.profile_id,
                           g_app.account_ui->chat_state.self_user_id,
                           record->channel_id,
                           record->id,
                           record->revision,
                           static_cast<u16>(parent ? parent->generation : 0),
                           record->generation,
                           application_thread_creation_available(record->id)};
    set_popup_owner(PopupOwnerKind::message);
    if (!application_plugins_menu_bind())
        return false;
    if (!unread_popup_capture(UnreadContextKind::message, message_id, can_unread,
                              static_cast<u32>(MessageContextAction::mark_unread)))
        return false;
    message_scene_focus_message(&g_app.message_scene, message_id);
    g_app.context_focus = &g_app.message_scene.widgets;
    return refresh_popup_text();
}

namespace {
// The link a message menu was opened on; its rows act on this semantic only
// while that same menu is showing.
struct LinkMenu {
    MessageTextHit hit;
    u32 opens;
    bool valid;
    PopupItem items[kMessageLinkMenuItems];
} link_menu;
} // namespace

// The official client merges a right-clicked link's Copy Link / Open Link group into the
// message menu (after the message sections, before Copy Message ID).
static bool open_message_link_context_popup(const MessageTextHit& hit, UiRect anchor) noexcept {
    if (!g_app.account_ui || !hit.valid || !hit.message_id ||
        (hit.kind != MessageMarkdownSemanticKind::link && hit.kind != MessageMarkdownSemanticKind::message_link))
        return false;
    const Utf8View url = message_text_presentation_hit_source(g_app.message_view.store,
                                                              &g_app.account_ui->message_rich_store, hit);
    link_menu.valid = false;
    // Generated rows (system messages, "Dismiss message") carry actions, not URLs.
    if (!message_markdown_url_valid(url))
        return false;
    // An attachment chip offers Copy Media Link / Copy Text (its file name).
    Utf8View name{};
    const bool chip = (hit.reserved & kMessageMarkdownAttachmentChip) && message_markdown_attachment_link(url, &name);
    const u32 count = chip ? attachment_link_menu({link_menu.items, kMessageLinkMenuItems})
                           : message_link_menu(url, {link_menu.items, kMessageLinkMenuItems});
    // Plugins get the URL and the link's visible text (an autolink's is the URL).
    const ApplicationPluginMenuExtra plugin{MC_MENU_LINK, 0, 0, false, url, chip ? name : url};
    if (!count || !open_message_media_context_popup(hit.message_id, anchor, {link_menu.items, count}, &plugin))
        return false;
    link_menu.hit = hit;
    link_menu.opens = g_app.message_context.opens;
    link_menu.valid = true;
    return true;
}

static bool activate_message_link_action(u32 action) noexcept {
    if (!message_link_menu_action(action) || !link_menu.valid || !g_app.account_ui ||
        g_app.popup_owner_kind != PopupOwnerKind::message || !g_app.popup_scene.active ||
        !g_app.message_context.active || g_app.message_context.opens != link_menu.opens ||
        g_app.message_context.message_id != link_menu.hit.message_id)
        return false;
    const MessageTextHit hit = link_menu.hit;
    link_menu.valid = false;
    if (!dismiss_popup())
        return false;
    // The source is re-read for the menu's message revision: an edited or
    // deleted message yields nothing.
    const Utf8View url = message_text_presentation_hit_source(g_app.message_view.store,
                                                              &g_app.account_ui->message_rich_store, hit);
    if (!url.bytes)
        return true;
    if (action == kMessageLinkActionCopy) {
        // An attachment chip copies its refreshed media link.
        if (application_copy_content_attachment_link(hit))
            return true;
        if (!application_copy_text(url, kToastLinkCopied, kToastLinkCopyFailed))
            log_write(LogLevel::warning, "message", "copy_link_failed");
        return true;
    }
    if (action == kMessageLinkActionCopyText) {
        Utf8View name{};
        if (message_markdown_attachment_link(url, &name) &&
            !application_copy_text(name, kToastTextCopied, kToastCopyFailed))
            log_write(LogLevel::warning, "message", "copy_text_failed");
        return true;
    }
    return application_open_content_link(hit);
}

namespace {
bool copy_message_link(u64 message_id) noexcept {
    const MessageRecord* record = g_app.account_ui ? application_message_record(message_id) : nullptr;
    const ChannelRecord* channel =
        record ? chat_state_store_find_channel(g_app.account_ui->chat_state, record->channel_id) : nullptr;
    char link[kMessageContextMaximumLinkBytes]{};
    const u32 bytes = record ? message_context_format_link(channel ? channel->guild_id : 0, record->channel_id,
                                                           record->id,
                                                           Slice<char>{link, kMessageContextMaximumLinkBytes})
                             : 0;
    return application_copy_text({link, bytes}, kToastLinkCopied, kToastLinkCopyFailed);
}

// Shift copies the "channel-message" form, as the official client does.
bool copy_message_id(u64 message_id, bool with_channel) noexcept {
    const MessageRecord* record = g_app.account_ui ? application_message_record(message_id) : nullptr;
    char id[41]{};
    u32 bytes = 0;
    if (record && with_channel) {
        bytes = message_context_format_snowflake(record->channel_id, Slice<char>{id, 20});
        if (bytes)
            id[bytes++] = '-';
    }
    const u32 message_bytes = message_context_format_snowflake(message_id, Slice<char>{id + bytes, 20});
    return application_copy_text({id, message_bytes ? bytes + message_bytes : 0}, kToastMessageIdCopied,
                                 kToastIdCopyFailed);
}

bool begin_reply_from_action(u64 message_id, bool without_mention) noexcept {
    return begin_message_reply(message_id) &&
           (!without_mention || application_reply_draft_set(message_id, false)) && refresh_message_composer_ui();
}
} // namespace

u16 application_message_action_bar_capabilities(u64 message_id) noexcept {
    const MessageRecord* record = g_app.account_ui ? application_message_record(message_id) : nullptr;
    if (!record || record->delivery != MessageDeliveryState::authoritative)
        return 0;
    u16 capabilities = 0;
    const auto add = [&](bool condition, MessageActionBarCapability capability) noexcept {
        if (condition)
            capabilities = static_cast<u16>(capabilities | capability);
    };
    add(application_message_forward_available(message_id), message_action_bar_forward);
    add(application_message_edit_available(message_id), message_action_bar_edit);
    add((GetKeyState(VK_SHIFT) & 0x8000) != 0 && !g_app.popup_scene.active, message_action_bar_expanded);
    add(application_pinned_messages_can_pin(message_id), message_action_bar_pin);
    add(message_can_delete(*record), message_action_bar_delete);
    add(g_app.preferences.developer_mode, message_action_bar_copy_id);
    add(unread_context_available(unread_source(UnreadContextKind::message, message_id), UnreadContextKind::message,
                                 message_id),
        message_action_bar_mark_unread);
    return capabilities;
}

bool application_message_action_bar_modifiers_changed() noexcept {
    auto& bar = g_app.message_action_bar;
    if (!g_app.account_ui || !bar.active || g_app.login.visible)
        return false;
    bool changed = false;
    if (!message_action_bar_reconfigure(&bar, g_app.message_scene, &g_app.account_ui->reaction_preferences,
                                        {application_message_action_bar_capabilities(bar.message_id)}, &changed))
        return false;
    return !changed || refresh_retained_projection();
}

bool application_message_action_bar_activate(const MessageActionBarHit& hit, bool shift) noexcept {
    const u64 id = hit.message_id;
    switch (hit.action) {
    case MessageActionBarAction::quick_reaction_0:
    case MessageActionBarAction::quick_reaction_1:
    case MessageActionBarAction::quick_reaction_2:
        if (queue_message_reaction_emoji(id, 0, hit.glyph, false) && !pump_message_history())
            log_write(LogLevel::error, "rest", "message_action_bar_reaction_pump_failed");
        return true;
    case MessageActionBarAction::copy_link:
        copy_message_link(id);
        return true;
    case MessageActionBarAction::copy_id:
        copy_message_id(id, shift);
        return true;
    case MessageActionBarAction::mark_unread:
        message_action_bar_close(&g_app.message_action_bar);
        if (unread_context_available(unread_source(UnreadContextKind::message, id), UnreadContextKind::message, id))
            application_read_state_mark_message_unread(id);
        return refresh_retained_projection();
    case MessageActionBarAction::retry:
        message_action_bar_close(&g_app.message_action_bar);
        return retry_failed_message(id);
    case MessageActionBarAction::plugin_0:
    case MessageActionBarAction::plugin_1:
        // The click carries the hovered message; a popover anchors to the cell.
        application_plugins_hover_activate(hit.catalog_index, id, hit.rectangle);
        return true;
    case MessageActionBarAction::none:
        return true;
    default:
        break;
    }
    message_action_bar_close(&g_app.message_action_bar);
    switch (hit.action) {
    case MessageActionBarAction::add_reaction:
        return open_reaction_picker(id, hit.rectangle);
    case MessageActionBarAction::reply:
        return begin_reply_from_action(id, shift);
    case MessageActionBarAction::edit:
        return begin_message_edit(id) && refresh_message_composer_ui();
    case MessageActionBarAction::forward:
        return application_message_forward_open(id);
    case MessageActionBarAction::more:
        return open_message_context_popup(id, hit.rectangle);
    case MessageActionBarAction::pin:
    case MessageActionBarAction::unpin:
        return !application_pinned_messages_can_pin(id) ||
               application_pinned_messages_action(id, hit.action == MessageActionBarAction::pin, !shift);
    case MessageActionBarAction::delete_message: {
        const MessageRecord* record = g_app.account_ui ? application_message_record(id) : nullptr;
        if (record && record->delivery == MessageDeliveryState::failed)
            return discard_failed_message(id);
        return application_message_delete_request(id, shift) || refresh_retained_projection();
    }
    default:
        return refresh_retained_projection();
    }
}

static bool open_reaction_viewer(u64 message_id) noexcept {
    if (!g_app.account_ui || g_app.login.visible || message_id == 0) {
        return false;
    }
    const MessageRecord* record = application_message_record(message_id);
    const MessageRichRecord* rich =
        record ? message_rich_store_find(g_app.account_ui->message_rich_store, message_id) : nullptr;
    const ChannelRecord* channel =
        record ? chat_state_store_find_channel(g_app.account_ui->chat_state, record->channel_id)
               : nullptr;
    if (!record || !rich || rich->reaction_count == 0) {
        return false;
    }
    if (!reaction_viewer_storage_materialize(&g_app.reaction_viewer_storage, &g_app.reaction_viewer,
                                             &g_app.reaction_viewer_controller)) {
        log_write(LogLevel::error, "reaction_viewer", "storage_failed");
        return false;
    }
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    g_app.composer_autocomplete.count = 0;
    g_app.composer_autocomplete.kind = ComposerAutocompleteKind::none;
    message_action_bar_close(&g_app.message_action_bar);
    const ReactionViewerControllerResult opened = reaction_viewer_controller_open(
        &g_app.reaction_viewer_controller, channel ? channel->guild_id : 0, record->channel_id, record->id,
        g_app.account_ui->message_rich_store, *rich, application_viewport_dips());
    if (!opened.ok()) {
        log_u64(LogLevel::error, "reaction_viewer", "open_error", static_cast<u64>(opened.error));
        return false;
    }
    message_scene_focus_message(&g_app.message_scene, message_id);
    const bool refreshed = refresh_account_frame(true);
    const bool pumped = pump_message_history();
    return refreshed && pumped;
}

namespace {

// The profile card offers the same user menu.
bool open_profile_context_popup(UiRect anchor) noexcept {
    const auto& profile = *g_app.profile;
    if (!profile.data.user_id || g_app.popup_scene.active || g_app.text_form)
        return true;
    return application_user_menu_open(profile.data.user_id, anchor, profile.data.guild_id, UserMenuSource::profile);
}

// A reaction chip opens the message menu with the reaction's emoji rows for
// plugins (the official client has no separate reaction menu). False: not on a reaction.
bool open_reaction_context_popup(const MessageSceneHit& hit, UiRect anchor) noexcept {
    u32 index = 0;
    if (!g_app.account_ui || !message_scene_reaction_index(g_app.message_scene, hit, &index))
        return false;
    const auto* record = application_message_record(hit.message_id);
    const auto* rich = record ? message_rich_store_find(g_app.account_ui->message_rich_store, hit.message_id) : nullptr;
    if (!rich || rich->message_revision != record->revision || index >= rich->reaction_count)
        return false;
    const auto& reaction = rich->reactions[index];
    Utf8View name = message_rich_store_string(g_app.account_ui->message_rich_store, reaction.name);
    if (!name.bytes && reaction.emoji_id)
        if (const auto* emoji = chat_state_store_find_emoji(g_app.account_ui->chat_state, reaction.emoji_id))
            name = chat_state_store_emoji_name(*emoji);
    const ApplicationPluginMenuExtra plugin{MC_MENU_EMOJI, 0, reaction.emoji_id, (reaction.flags & 4) != 0, {}, name};
    return open_message_media_context_popup(hit.message_id, anchor, {}, &plugin);
}

bool open_channel_context_popup(u64 channel_id, UiRect anchor) noexcept {
    if (!g_app.account_ui)
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    if (!channel || !channel_context_target_valid(g_app.account_ui->chat_state, channel_id))
        return dismiss_popup();
    if (channel->type == 4)
        return application_scoped_read_menu_open(ReadScopeKind::category, channel_id, anchor);
    const bool ordinary_unread = unread_context_available(
        unread_source(UnreadContextKind::channel, channel_id), UnreadContextKind::channel, channel_id);
    const bool can_retry_unread =
        unread_context_available(unread_source(UnreadContextKind::channel, channel_id, true),
                                 UnreadContextKind::channel, channel_id);
    const bool can_unread = ordinary_unread && !can_retry_unread;
    constexpr u32 native_capacity = 4 + 1 + 6 + 1 + 3 + kNotificationMenuItemCount + kUserContextMaximumItems;
    PopupItem items[native_capacity + kApplicationPluginMenuRows]{};
    const auto thread_view = application_thread_actions_view(channel_id);
    const auto thread_header = application_thread_header_projection(thread_view);
    const u32 ordinary = channel_context_policy_build(
        g_app.account_ui->chat_state, channel_id, popup_action_mark_channel_read, popup_action_close_dm,
        {items, 4}, popup_action_edit_channel, can_unread ? popup_action_mark_channel_unread : 0,
        can_retry_unread ? popup_action_retry_channel_unread : 0);
    const u32 thread_count = ui_thread_menu_build(thread_header, kApplicationThreadMenuActions,
                                                  {items + ordinary, native_capacity - ordinary});
    if (ordinary && thread_count)
        items[ordinary].flags |= popup_item_separator_before;
    u32 builtins = ordinary + thread_count;
    // Text in voice: a voice channel's chat opens without joining the call.
    constexpr u32 chat_read = channel_action_view | channel_action_read_history;
    if (channel_is_voice_chat(channel->type) && channel->guild_id && (channel->actions & chat_read) == chat_read) {
        items[builtins] = {utf8_literal("Open chat"), popup_action_open_voice_chat, popup_item_enabled};
        items[builtins++].presentation = ui_menu_row_presentation(UiIcon::direct_message);
    }
    const bool browse = application_thread_browser_available(channel_id);
    browser_menu_scope =
        browse ? ThreadBrowserMenuScope{g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
                                        g_app.account_ui->chat_state.self_user_id, channel_id,
                                        channel->generation}
               : ThreadBrowserMenuScope{};
    if (browse)
        items[builtins++] = {utf8_literal("Browse threads"), popup_action_browse_threads,
                             popup_item_enabled};
    builtins += application_private_channel_menu_items(channel_id, {items + builtins, native_capacity - builtins});
    builtins += application_notification_menu_items(0, channel_id, {items + builtins, native_capacity - builtins});
    if (channel->type == 1)
        builtins += application_user_actions_private_channel_items(channel_id, {items + builtins, native_capacity - builtins});
    const u32 count =
        builtins + application_plugins_menu_items(ApplicationPluginMenuSurface::channel, channel_id,
                                                  {items + builtins, kApplicationPluginMenuRows});
    if (!count)
        return dismiss_popup();
    if (!builtins)
        items[0].flags &= ~popup_item_separator_before;
    const UiPopupFeedback feedback{thread_header.status, ui_popup_feedback_style(),
                                   thread_header.status_error};
    const auto* footer = thread_count && feedback.text.bytes ? &feedback : nullptr;
    auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, channel_id, {items, count},
                                   anchor, application_viewport_dips(),
                                   thread_count || count > builtins ? 240.0f : 184.0f, false, 0, 0,
                                   thread_count ? thread_view.ticket.publication_revision : 0, footer);
    if (!opened.ok() && count > builtins) {
        application_plugins_menu_dismissed();
        if (!builtins)
            return dismiss_popup();
        opened =
            popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, channel_id, {items, builtins},
                             anchor, application_viewport_dips(), thread_count ? 240.0f : 184.0f, false, 0,
                             0, thread_count ? thread_view.ticket.publication_revision : 0, footer);
    }
    if (!opened.ok())
        return false;
    thread_menu_scope = {thread_view.ticket, thread_count != 0, false};
    set_popup_owner(channel->type == 1 ? PopupOwnerKind::private_channel : PopupOwnerKind::channel);
    shell_navigation_scene_focus_channel(&g_app.shell_navigation_scene, channel_id);
    g_app.context_focus = &g_app.shell_navigation_scene.widgets;
    if (!application_plugins_menu_bind())
        return false;
    if (!unread_popup_capture(UnreadContextKind::channel, channel_id, can_unread || can_retry_unread,
                              can_retry_unread ? popup_action_retry_channel_unread
                                               : popup_action_mark_channel_unread))
        return false;
    return refresh_popup_text();
}

} // namespace

bool application_thread_actions_menu_open(const ApplicationThreadActionTicket& ticket, UiRect anchor) noexcept {
    if (!g_app.account_ui || g_app.account_ui->active_channel_id != ticket.thread_id ||
        !application_thread_actions_current(ticket))
        return false;
    const auto host = application_thread_actions_view(ticket.thread_id);
    const auto view = application_thread_header_projection(host);
    PopupItem items[6]{};
    const u32 count = ui_thread_menu_build(view, kApplicationThreadMenuActions, {items, 6});
    if (!count)
        return false;
    const UiPopupFeedback feedback{view.status, ui_popup_feedback_style(), view.status_error};
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, ticket.thread_id, {items, count},
                          anchor, application_viewport_dips(), 240.f, false, 0, 0,
                          ticket.publication_revision, feedback.text.bytes ? &feedback : nullptr)
             .ok())
        return false;
    application_plugins_menu_dismissed();
    reset_value(&unread_popup);
    thread_menu_scope = {host.ticket, true, true};
    set_popup_owner(PopupOwnerKind::channel);
    g_app.context_focus = &g_app.shell_scene.widgets;
    return refresh_popup_text();
}

bool application_refresh_thread_actions_menu() noexcept {
    if (!thread_menu_scope.active || !g_app.popup_scene.active ||
        g_app.popup_owner_kind != PopupOwnerKind::channel ||
        g_app.popup_scene.owner_id != thread_menu_scope.ticket.thread_id)
        return true;
    if (!g_app.account_ui ||
        (thread_menu_scope.from_header
             ? g_app.account_ui->active_channel_id != thread_menu_scope.ticket.thread_id
             : !channel_context_row_current(g_app.account_ui->navigation_view, g_app.shell_navigation_scene,
                                            thread_menu_scope.ticket.thread_id)))
        return dismiss_popup();
    const auto host = application_thread_actions_view(thread_menu_scope.ticket.thread_id);
    if (!host.ticket.thread_id || host.ticket.account_scope != thread_menu_scope.ticket.account_scope ||
        host.ticket.account_instance != thread_menu_scope.ticket.account_instance ||
        host.ticket.profile_id != thread_menu_scope.ticket.profile_id ||
        host.ticket.self_user_id != thread_menu_scope.ticket.self_user_id ||
        host.ticket.channel_generation != thread_menu_scope.ticket.channel_generation)
        return dismiss_popup();
    if (host.ticket.channel_revision == thread_menu_scope.ticket.channel_revision &&
        host.ticket.publication_revision == thread_menu_scope.ticket.publication_revision &&
        host.ticket.serial == thread_menu_scope.ticket.serial)
        return true;
    const auto anchor = g_app.popup_scene.anchor;
    const bool from_header = thread_menu_scope.from_header;
    const auto selected_action = popup_scene_selected_action(g_app.popup_scene);
    if (!(from_header ? application_thread_actions_menu_open(host.ticket, anchor)
                      : open_channel_context_popup(host.ticket.thread_id, anchor)))
        return false;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        if (g_app.popup_scene.items[i].action == selected_action &&
            (g_app.popup_scene.items[i].flags & popup_item_enabled)) {
            bool changed = false;
            popup_scene_select_index(&g_app.popup_scene, i, &changed);
            break;
        }
    return refresh_popup_text();
}

namespace {

bool open_guild_context_popup(u64 guild_id, UiRect anchor) noexcept {
    return application_scoped_read_menu_open(ReadScopeKind::guild, guild_id, anchor);
}

// Folder header rows: the folder's own settings (name and colour).
bool open_folder_context_popup(u64 row_id, UiRect anchor) noexcept {
    const auto* row = g_app.account_ui ? navigation_view_find_guild(g_app.account_ui->navigation_view, row_id) : nullptr;
    const u64 folder_id = row ? application_guild_folders_folder_id(*row) : 0;
    if (!folder_id)
        return dismiss_popup();
    PopupItem items[1]{};
    items[0] = {utf8_literal("Folder settings"), popup_action_folder_settings, popup_item_enabled};
    items[0].presentation = ui_menu_row_presentation(UiIcon::settings);
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, folder_id, {items, 1}, anchor,
                          application_viewport_dips(), 6 * kUiMenuRowHeight + 2 * kUiMenuPadding)
             .ok())
        return false;
    set_popup_owner(PopupOwnerKind::guild_folder);
    guild_rail_scene_focus_guild(&g_app.guild_rail_scene, row_id);
    g_app.context_focus = &g_app.guild_rail_scene.widgets;
    return refresh_popup_text();
}

} // namespace

void focus_context_at(float x, float y) noexcept {
    g_app.context_focus = nullptr;
    if (!g_app.account_ui || g_app.login.visible)
        return;
    const auto guild = guild_rail_scene_hit_test(g_app.guild_rail_scene, x, y);
    if (guild.id) {
        if (guild.selectable && guild_rail_scene_focus_guild(&g_app.guild_rail_scene, guild.id))
            g_app.context_focus = &g_app.guild_rail_scene.widgets;
        return;
    }
    const auto message = message_scene_hit_test(g_app.message_scene, x, y);
    if (message.message_id && message_scene_focus_message(&g_app.message_scene, message.message_id)) {
        g_app.context_focus = &g_app.message_scene.widgets;
        return;
    }
    const auto member = member_list_scene_hit_test(g_app.member_list_scene, x, y);
    if (member.user_id && member_list_scene_focus_user(&g_app.member_list_scene, member.user_id)) {
        g_app.context_focus = &g_app.member_list_scene.widgets;
        return;
    }
    const auto channel = shell_navigation_scene_hit_test(g_app.shell_navigation_scene, x, y);
    if (channel.selectable &&
        shell_navigation_scene_focus_channel(&g_app.shell_navigation_scene, channel.id))
        g_app.context_focus = &g_app.shell_navigation_scene.widgets;
}

bool open_focused_context_popup() noexcept {
    if (g_app.profile && g_app.profile->scene.active) {
        const UiRect card = g_app.profile->scene.rectangle;
        return !g_app.account_ui || g_app.login.visible ||
               open_profile_context_popup({card.right - 1.0f, card.top, card.right, card.top + 1.0f});
    }
    // A keyboard request must not escape an overlay or fall back to an older
    // focus retained by another content surface.
    if (!g_app.account_ui || g_app.login.visible || g_app.popup_scene.active ||
        g_app.reaction_viewer.active || !g_app.context_focus)
        return true;
    UiRect anchor{};
    if (!ui_widget_manager_context_anchor(*g_app.context_focus, &anchor)) {
        bool revealed = false;
        if (g_app.context_focus == &g_app.message_scene.widgets)
            revealed =
                message_scene_reveal_message(&g_app.message_scene, g_app.message_scene.focused_message_id);
        else if (g_app.context_focus == &g_app.member_list_scene.widgets)
            revealed = member_list_scene_reveal_user(&g_app.member_list_scene,
                                                     g_app.member_list_scene.focused_user_id);
        else if (g_app.context_focus == &g_app.shell_navigation_scene.widgets)
            revealed = shell_navigation_scene_reveal_channel(
                &g_app.shell_navigation_scene, g_app.account_ui->navigation_view,
                g_app.shell_navigation_scene.focused_channel_id);
        else if (g_app.context_focus == &g_app.guild_rail_scene.widgets)
            revealed =
                guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, g_app.account_ui->navigation_view,
                                              g_app.guild_rail_scene.focused_guild_id);
        if (!revealed)
            return true;
        ui_scroll_host_cancel(&g_app.scroll_host);
        if (!refresh_account_frame(true))
            return false;
        if (!g_app.context_focus || !ui_widget_manager_context_anchor(*g_app.context_focus, &anchor))
            return true;
    }
    const auto focused = g_app.context_focus->focused;
    if (g_app.context_focus == &g_app.guild_rail_scene.widgets && g_app.guild_rail_scene.active) {
        const auto* row = guild_rail_scene_resolve(g_app.guild_rail_scene, focused);
        if (row && row->view.folder_role == GuildFolderRole::header)
            return open_folder_context_popup(row->view.id, anchor);
        if (row && row->view.folder_role == GuildFolderRole::conversation)
            return open_channel_context_popup(row->view.id, anchor);
        return !row || open_guild_context_popup(row->view.id, anchor);
    }
    if (g_app.context_focus == &g_app.message_composer_scene.widgets &&
        message_composer_scene_focused_control(g_app.message_composer_scene) ==
            MessageComposerControl::edit)
        return application_composer_edit_menu_open(anchor);
    if (g_app.context_focus == &g_app.message_scene.widgets && g_app.message_scene.active) {
        if (application_downloads_context_focused())
            return true;
        const auto* row = message_scene_resolve(g_app.message_scene, focused);
        return !row || open_message_context_popup(row->message_id, anchor);
    }
    if (g_app.context_focus == &g_app.member_list_scene.widgets && g_app.member_list_scene.active) {
        const auto* row = member_list_scene_resolve(g_app.member_list_scene, focused);
        return !row || !row->user_id ||
               application_user_menu_open(row->user_id, anchor, 0, UserMenuSource::member_list);
    }
    if (g_app.context_focus == &g_app.shell_navigation_scene.widgets &&
        g_app.shell_navigation_scene.active) {
        const auto* row = shell_navigation_scene_resolve(g_app.shell_navigation_scene, focused);
        return !row || open_channel_context_popup(row->view.id, anchor);
    }
    return true;
}

bool open_context_popup(i32 client_x, i32 client_y) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return false;
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const float x = static_cast<float>(client_x) * scale;
    const float y = static_cast<float>(client_y) * scale;
    const UiRect anchor{x, y, x + 1.0f, y + 1.0f};
    if (g_app.profile && g_app.profile->scene.active)
        return !ui_rect_contains(g_app.profile->scene.rectangle, x, y) || open_profile_context_popup(anchor);
    if (message_composer_scene_hit_test(g_app.message_composer_scene, x, y) == MessageComposerControl::edit) {
        u32 offset = 0; // the word under the pointer selects spelling suggestions
        return application_composer_edit_menu_open(
            anchor, renderer_message_composer_edit_hit_test(g_app.renderer, client_x, client_y, &offset)
                        ? offset
                        : kComposerEditMenuAtCaret);
    }
    if (application_downloads_context_at(x, y))
        return true;
    const auto author = application_message_profile_hit_test(x, y);
    if (author.user_id)
        return application_user_menu_open(author.user_id, anchor, 0, UserMenuSource::message);
    if (open_message_link_context_popup(renderer_content_hit_test(g_app.renderer, client_x, client_y), anchor))
        return true;
    const auto message = message_scene_hit_test(g_app.message_scene, x, y);
    if (message.message_id)
        return open_reaction_context_popup(message, anchor) || open_message_context_popup(message.message_id, anchor);
    const auto member = member_list_scene_hit_test(g_app.member_list_scene, x, y);
    if (member.user_id)
        return application_user_menu_open(member.user_id, anchor, 0, UserMenuSource::member_list);
    if (application_voice_context_at(x, y, anchor))
        return true;
    // The server header takes the server's context menu, as in the official client.
    if (const auto* header = ui_widget_manager_resolve(
            g_app.shell_scene.widgets, ui_widget_manager_hit_test(g_app.shell_scene.widgets, x, y));
        header && header->id == static_cast<u32>(UiSemantic::guild_menu) + 1u) {
        const auto& state = g_app.account_ui->chat_state;
        return state.home_selected || !state.selected_guild_id ||
               open_guild_context_popup(state.selected_guild_id, anchor);
    }
    const auto navigation = renderer_navigation_hit_test(g_app.renderer, client_x, client_y);
    if (navigation.kind == RendererNavigationHitKind::guild && guild_folder_row_id(navigation.id))
        return open_folder_context_popup(navigation.id, anchor);
    if (navigation.kind == RendererNavigationHitKind::guild)
        return open_guild_context_popup(navigation.id, anchor);
    const auto* channel =
        navigation.kind == RendererNavigationHitKind::channel
            ? chat_state_store_find_channel(g_app.account_ui->chat_state, navigation.id)
            : nullptr;
    if (channel)
        return open_channel_context_popup(channel->id, anchor);
    return dismiss_popup();
}

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
    if (application_user_menu_action(action))
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
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
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
        return action == 0 || activate_popup_action(action, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
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
