#pragma once

#include "application_state.hpp"
#include "application_channel_authority.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {

bool application_reply_draft_activate(MessageComposerControl control) noexcept;

inline MessageComposerReplySnapshot application_reply_draft() noexcept {
    MessageComposerReplySnapshot reply{};
    if (g_app.account_ui)
        message_composer_controller_reply_snapshot(g_app.account_ui->message_composer_controller, &reply);
    return reply;
}

inline bool application_reply_draft_set(u64 message_id, bool mention_author = true) noexcept {
    if (!g_app.account_ui || g_app.message_edit.editor_active) return false;
    const auto result = message_composer_controller_set_reply(
        &g_app.account_ui->message_composer_controller, message_id, mention_author);
    if (!result.ok()) {
        log_u64(LogLevel::warning, "reply", "draft_update_error", static_cast<u64>(result.error));
        return false;
    }
    if (result.changed) ++g_app.message_composer.revision;
    return true;
}

inline bool application_reply_draft_cancel() noexcept {
    return application_reply_draft_set(0);
}

inline bool application_reply_request_deleted(u64 channel_id, u64 nonce) noexcept {
    if (!g_app.account_ui) return false;
    for (const auto& request : g_app.account_ui->message_composer_controller.requests)
        if (request.occupied && request.channel_id == channel_id && request.nonce == nonce)
            return request.reply_unavailable;
    return false;
}

inline void application_reply_menu_deleted(u64 channel_id) noexcept {
    auto& popup = g_app.popup_scene;
    if (g_app.popup_owner_kind != PopupOwnerKind::message || !popup.active ||
        !application_reply_request_deleted(channel_id, popup.owner_id)) return;
    for (u32 index = 0; index < popup.item_count; ++index) {
        auto& item = popup.items[index];
        if (item.action != static_cast<u32>(MessageContextAction::retry_send) ||
            !(item.flags & popup_item_enabled)) continue;
        item.label = utf8_literal("Original message was deleted");
        item.flags &= ~popup_item_enabled;
        if (index < g_app.message_context.menu.count)
            g_app.message_context.menu.items[index] = item;
        if (auto* widget = ui_widget_manager_resolve(&popup.widgets, popup.rows[index].widget))
            widget->flags &= ~ui_widget_enabled;
        ui_widget_manager_reconcile_input(&popup.widgets);
        bool changed = false;
        if (popup.selected_index == index) popup_scene_move_selection(&popup, 1, &changed);
        ++popup.revision;
    }
}

inline void application_reply_draft_deleted(u64 channel_id, u64 message_id) noexcept {
    if (!g_app.account_ui || !channel_id || !message_id) return;
    const auto reply = application_reply_draft();
    const auto cleared = message_composer_controller_clear_reply(
        &g_app.account_ui->message_composer_controller, channel_id, message_id);
    if (!cleared.ok())
        log_u64(LogLevel::warning, "reply", "draft_delete_error", static_cast<u64>(cleared.error));
    if (cleared.changed) {
        ++g_app.message_composer.revision;
        application_reply_menu_deleted(channel_id);
        if (g_app.window && (reply.channel_id == channel_id ||
                            g_app.account_ui->active_channel_id == channel_id))
            application_request_frame();
    }
    auto& edit = g_app.message_edit;
    if (edit.channel_id == channel_id && edit.saved_reply_message_id == message_id) {
        edit.saved_reply_message_id = 0;
        edit.saved_reply_mention_suppressed = false;
    }
}

inline Utf8View application_reply_draft_label(Slice<char> storage) noexcept {
    const auto reply = application_reply_draft();
    constexpr auto prefix = utf8_literal("Replying to ");
    if (!reply.message_id || !storage.data || storage.count < prefix.bytes) return {};
    Utf8View name = utf8_literal("a message");
    if (application_channel_readable(g_app, reply.channel_id)) {
        const auto found = message_store_find(g_app.message_view.store, reply.message_id);
        const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        const auto* user = message && message->channel_id == reply.channel_id
            ? user_store_find(g_app.account_ui->user_store, message->author_id) : nullptr;
        if (user) {
            const auto author = user_store_name(g_app.account_ui->user_store, *user);
            if (author.bytes) name = author;
        }
    }
    copy_bytes(storage.data, prefix.data, prefix.bytes);
    u32 copied = 0;
    while (copied < name.bytes) {
        const auto scalar = utf8_decode_one(name.data + copied, name.bytes - copied);
        if (!scalar.valid || scalar.width > storage.count - prefix.bytes - copied) break;
        copied += scalar.width;
    }
    copy_bytes(storage.data + prefix.bytes, name.data, copied);
    return {storage.data, prefix.bytes + copied};
}

} // namespace maicord::app_detail
