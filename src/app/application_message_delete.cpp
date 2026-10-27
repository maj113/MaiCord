#include "application_message_delete.hpp"
#include "maicord/core/memory.hpp"

#include "application_account_presentation.hpp"
#include "application_message_mutation_source.hpp"
#include "application_state.hpp"
#include "application_thread_actions.hpp"
#include "application_reply_draft.hpp"
#include "application_text_form.hpp"

#include "maicord/app/message_interaction_rules.hpp"
#include "maicord/core/log.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/ui/navigation_view.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
MessageMutationSource delete_source;
TextFormOwnerKey delete_confirmation;

const MessageRecord* deletable_record(u64 message_id) noexcept {
    if (!g_app.account_ui || !message_id || g_app.message_delete.message_id != 0)
        return nullptr;
    const MessageRecord* record = application_message_record(message_id);
    return record && record->channel_id == g_app.account_ui->active_channel_id && message_can_delete(*record)
               ? record
               : nullptr;
}

bool confirmation_valid(TextFormOwnerKey key) noexcept {
    const MessageRecord* record = deletable_record(key.item);
    return record && key.scope == delete_confirmation.scope && key.collection == delete_confirmation.collection &&
           key.item == delete_confirmation.item && key.scope == g_app.plugin_account_scope_serial &&
           record->channel_id == key.collection;
}

bool confirmation_save(TextFormOwnerKey key, Utf8View) noexcept {
    // Closing is correct when the message vanished while the dialog was open.
    if (!confirmation_valid(key))
        return true;
    reset_value(&delete_confirmation);
    return queue_message_delete(key.item);
}
} // namespace

bool application_message_delete_request(u64 message_id, bool skip_confirmation) noexcept {
    const MessageRecord* record = deletable_record(message_id);
    if (!record)
        return false;
    if (skip_confirmation)
        return queue_message_delete(message_id);
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* author = user_store_find(g_app.account_ui->user_store, record->author_id);
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, message_id);
    constexpr Utf8View lead = utf8_literal("This can\xE2\x80\x99t be undone.\n\n");
    constexpr Utf8View tip = utf8_literal("\n\nHold Shift when deleting to skip this confirmation.");
    char context[lead.bytes + 512 + tip.bytes];
    copy_bytes(context, lead.data, lead.bytes);
    const u32 preview = message_delete_preview_format(
        author ? user_store_name(g_app.account_ui->user_store, *author) : Utf8View{},
        message_store_content(g_app.message_view.store, found.handle),
        rich && rich->message_id == message_id && rich->attachment_count, {context + lead.bytes, 512});
    copy_bytes(context + lead.bytes + preview, tip.data, tip.bytes);
    delete_confirmation = {g_app.plugin_account_scope_serial, record->channel_id, record->id};
    ApplicationTextFormRequest request{utf8_literal("Delete message?"),
                                             {context, lead.bytes + preview + tip.bytes},
                                             {},
                                             utf8_literal("Delete"),
                                             {},
                                             delete_confirmation,
                                             confirmation_valid,
                                             confirmation_save,
                                             0,
                                             true};
    request.danger = true;
    if (!application_text_form_open(request)) {
        reset_value(&delete_confirmation);
        return false;
    }
    return true;
}

void reset_message_delete() noexcept {
    reset_value<MessageDeleteRequest>(&g_app.message_delete);
    reset_value(&delete_source);
}

bool message_can_delete(const MessageRecord& record) noexcept {
    if (!g_app.account_ui || record.id == 0 || record.channel_id == 0 ||
        record.delivery != MessageDeliveryState::authoritative ||
        !application_thread_actions_channel_readable(record.channel_id)) {
        return false;
    }
    // These service messages cannot be deleted, even by their author or a moderator.
    switch (record.type) {
    case 1: // Recipient added
    case 2: // Recipient removed
    case 3: // Call
    case 4: // Channel name changed
    case 5: // Channel icon changed
    case 21: // Thread starter reference
    case 35: // Premium referral
    case 56: // Chat wallpaper set
    case 57: // Chat wallpaper removed
    case 64: // Premium group invite
    case 68: // Media mention
        return false;
    }
    // Auto moderation actions require Manage Messages even when authored by self.
    if (record.type != 24 && record.author_id != 0 &&
        record.author_id == g_app.account_ui->chat_state.self_user_id) {
        return true;
    }
    const ChannelViewRow* channel =
        navigation_view_find_channel(g_app.account_ui->navigation_view, record.channel_id);
    return channel && (channel->actions & channel_action_manage_messages) != 0;
}

bool queue_message_delete(u64 message_id) noexcept {
    if (!g_app.account_ui || message_id == 0 || g_app.message_delete.message_id != 0) {
        return false;
    }
    const MessageStoreResult found = message_store_find(g_app.message_view.store, message_id);
    const MessageRecord* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record || !message_can_delete(*record)) {
        return false;
    }
    g_app.message_delete.channel_id = record->channel_id;
    g_app.message_delete.message_id = record->id;
    g_app.message_delete.retry_at_ms = 0;
    delete_source = message_mutation_source_capture(found.handle, *record);
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::erase, record->channel_id,
                                              record->id);
    if (g_app.window) {
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    }
    return true;
}
bool application_message_delete_pending_available() noexcept {
    const auto* record = message_mutation_source_record(delete_source, g_app.message_delete.channel_id,
                                                        g_app.message_delete.message_id);
    return record && message_can_delete(*record);
}

bool accept_message_delete_response(const RestClientResponse& response) noexcept {
    if (!g_app.account_ui || g_app.message_delete.message_id == 0) {
        return false;
    }
    if (!response.transport_ok()) {
        log_u64(LogLevel::error, "rest", "message_delete_transport_error",
                static_cast<u64>(response.error));
        application_message_mutation_notice_failed(ApplicationMessageMutationKind::erase,
                                                   g_app.message_delete.channel_id,
                                                   g_app.message_delete.message_id, response);
        reset_message_delete();
        return refresh_account_text(true);
    }
    if (response.status_code == 429 && response.rate_limited && response.retry_at_ms != 0 &&
        !api_rejection_requires_action(response.rejection)) {
        g_app.message_delete.retry_at_ms = response.retry_at_ms;
        log_u64(LogLevel::warning, "rest", "message_delete_retry_at_ms", response.retry_at_ms);
        return true;
    }
    const u64 message_id = g_app.message_delete.message_id;
    if (response.status_code != 204 && response.status_code != 404) {
        log_u64(LogLevel::error, "rest", "message_delete_http_status", response.status_code);
        application_message_mutation_notice_failed(ApplicationMessageMutationKind::erase,
                                                   g_app.message_delete.channel_id, message_id, response);
        reset_message_delete();
        return refresh_account_text(true);
    }
    const MessageTimelineApplyResult removed =
        message_timeline_remove_local(&g_app.message_view.timeline, message_id);
    if (!removed.ok()) {
        log_u64(LogLevel::error, "message", "message_delete_timeline_error",
                static_cast<u64>(removed.error));
        reset_message_delete();
        return false;
    }
    application_reply_draft_deleted(g_app.message_delete.channel_id, message_id);
    application_message_mutation_notice_succeeded(ApplicationMessageMutationKind::erase,
                                                  g_app.message_delete.channel_id, message_id);
    reset_message_delete();
    return refresh_account_frame(true);
}

} // namespace maicord::app_detail
