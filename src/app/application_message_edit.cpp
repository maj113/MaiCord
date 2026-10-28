
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_message_mutation_source.hpp"
#include "application_state.hpp"
#include "application_thread_actions.hpp"
#include "application_reply_draft.hpp"

#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/message_reconcile.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
MessageMutationSource edit_source;

const MessageRecord* editable_record(u64 message_id) noexcept {
    if (!g_app.account_ui || !message_id)
        return nullptr;
    const auto* record = application_message_record(message_id);
    // Voice messages cannot be edited; Components V2 use a separate component
    // editor and cannot carry a text caption. System messages are not text edits.
    constexpr u32 unsupported_flags = (1u << 13) | (1u << 15);
    return record && record->delivery == MessageDeliveryState::authoritative &&
                   record->content_availability == MessageContentAvailability::available &&
                   record->channel_id == g_app.message_composer.channel_id &&
                   record->channel_id == g_app.account_ui->active_channel_id && record->author_id != 0 &&
                   application_thread_actions_channel_sendable(record->channel_id) &&
                   record->author_id == g_app.account_ui->chat_state.self_user_id &&
                   (record->type == 0 || record->type == 19) && !(record->flags & unsupported_flags)
               ? record
               : nullptr;
}

bool retained_rich_content(const MessageRecord& record) noexcept {
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, record.id);
    // Reactions and a quoted reply header alone cannot keep an empty message.
    // A rich entry from an earlier content revision is not authoritative proof.
    return rich && rich->occupied && rich->message_id == record.id &&
           rich->message_revision == record.revision &&
           (rich->attachment_count || rich->embed_count || rich->sticker_count || rich->component_count);
}

bool submitted_content_equal(const MessageEditRuntime& edit, Utf8View content) noexcept {
    return utf8_equal(Utf8View{edit.content, edit.content_bytes}, content);
}

bool apply_response(u32 body_bytes) noexcept {
    MessageEditRuntime& edit = g_app.message_edit;
    GatewayMessageEvent message{};
    bool decoded_response = false;
    if (body_bytes != 0) {
        MessageHistoryDecodeScratch scratch{};
        const Utf8View body{
            reinterpret_cast<const char*>(g_app.account_ui->response_shape_scratch.response), body_bytes};
        if (!application_message_decode_scratch(*g_app.account_ui, &scratch, body)) {
            log_write(LogLevel::error, "message", "history_decode_storage_admission_failed");
            return false;
        }
        u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
        const GatewayEventDecodeResult decoded = gateway_decode_message_create(
            JsonValue{body, 0, JsonValueKind::object}, scratch.message_structurals, 64, scratch.content,
            Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds}, scratch.rich.member_roles,
            scratch.mention_users,
            {scratch.rich.reactions, scratch.rich.attachments, scratch.rich.embeds,
             scratch.rich.embed_fields, scratch.rich.stickers, scratch.rich.components,
             scratch.rich.snapshot},
            g_app.account_ui->chat_state.self_user_id, &message);
        decoded_response = decoded.ok() && message.id == edit.message_id &&
                           message.channel_id == edit.channel_id &&
                           message.author_id == g_app.account_ui->chat_state.self_user_id;
        // No stack-backed role-ID view may outlive this decode transaction.
        reset_value(&message.mention_roles);
        if (!decoded_response) {
            log_u64(LogLevel::warning, "message", "message_edit_response_decode_error",
                    static_cast<u64>(decoded.error));
        }
    }
    if (!decoded_response) {
        // The gateway update remains authoritative; do not fabricate ordering
        // metadata when the PATCH response body is absent or malformed.
        return true;
    }
    const MessageRecord* existing = application_message_record(edit.message_id);
    if (!existing || existing->channel_id != edit.channel_id)
        return true;
    if (message.edited_timestamp_ms != 0 &&
        existing->edited_timestamp_ms > message.edited_timestamp_ms) {
        return true;
    }
    const u32 previous_revision = existing->revision;
    const auto* previous_rich =
        message_rich_store_find(g_app.account_ui->message_rich_store, edit.message_id);
    const bool rich_owned = !previous_rich || previous_rich->message_revision == previous_revision;
    const MessageReconcileResult updated =
        message_reconcile_update(&g_app.message_view.store, message, MessageContentAvailability::available);
    if (!updated.ok()) {
        log_u64(LogLevel::warning, "message", "message_edit_local_reconcile_error",
                static_cast<u64>(updated.error));
        g_app.message_pump.resync_required = true;
    } else {
        const auto* resident = message_store_resolve(g_app.message_view.store, updated.handle);
        if (!resident || !rich_owned ||
            !message_rich_store_apply(&g_app.account_ui->message_rich_store, edit.message_id,
                                      resident->revision, message)
                 .ok())
            g_app.message_pump.resync_required = true;
    }
    return true;
}

// Queues content for the edit of record; the caller reserved its storage.
void submit_edit(const MessageRecord& record, Utf8View content) noexcept {
    MessageEditRuntime& edit = g_app.message_edit;
    copy_bytes(edit.content, content.data, content.bytes);
    edit.content_bytes = content.bytes;
    edit.channel_id = record.channel_id;
    edit.message_id = record.id;
    edit.retry_at_ms = 0;
    edit.phase = MessageEditRequestPhase::queued;
    edit_source = message_mutation_source_capture(message_store_find(g_app.message_view.store, record.id).handle, record);
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::edit, edit.channel_id, edit.message_id);
    if (g_app.window)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
}

} // namespace

void reset_message_edit() noexcept {
    message_edit_runtime_reset(&g_app.message_edit);
    reset_value(&edit_source);
}
bool application_message_edit_pending_available() noexcept {
    const auto* record = message_mutation_source_record(edit_source, g_app.message_edit.channel_id,
                                                        g_app.message_edit.message_id);
    return record && record->author_id == edit_source.self &&
           application_thread_actions_channel_sendable(record->channel_id);
}

bool cancel_message_edit_editor() noexcept {
    MessageEditRuntime& edit = g_app.message_edit;
    if (!edit.editor_active) {
        return true;
    }
    application_composer_text_input_suspend();
    if (edit.channel_id == 0 || g_app.message_composer.channel_id != edit.channel_id ||
        !message_composer_restore(&g_app.message_composer, edit.channel_id,
                                  Utf8View{edit.saved_draft, edit.saved_draft_bytes},
                                  edit.saved_draft_anchor, edit.saved_draft_caret)) {
        return false;
    }
    g_app.message_composer.focused = edit.saved_draft_focused;
    ++g_app.message_composer.revision;
    edit.editor_active = false;
    if (!application_reply_draft_set(edit.saved_reply_message_id, !edit.saved_reply_mention_suppressed)) return false;
    if (edit.phase == MessageEditRequestPhase::idle) {
        reset_message_edit();
    }
    reset_message_composer_caret_timing(g_app.message_composer.focused);
    return true;
}

bool application_message_edit_available(u64 message_id) noexcept {
    const auto* record = editable_record(message_id);
    return record && record->content_bytes <= kMessageComposerMaximumBytes &&
           (record->content_bytes != 0 || retained_rich_content(*record));
}

u64 application_message_edit_latest_own() noexcept {
    const MessageStore& store = g_app.message_view.store;
    for (u32 index = store.count; index-- > 0;) {
        const MessageRecord* record = message_store_resolve(store, message_store_ordered_handle(store, index));
        if (record && application_message_edit_available(record->id))
            return record->id;
    }
    return 0;
}

bool application_message_edit_retains_content() noexcept {
    const auto& edit = g_app.message_edit;
    const auto* record = edit.editor_active ? editable_record(edit.message_id) : nullptr;
    return record && record->channel_id == edit.channel_id && retained_rich_content(*record);
}

bool begin_message_edit(u64 message_id) noexcept {
    if (g_app.message_edit.message_id != 0) {
        return false;
    }
    const MessageRecord* record = editable_record(message_id);
    if (!record) {
        return false;
    }
    const MessageStoreResult found = message_store_find(g_app.message_view.store, message_id);
    const Utf8View content = message_store_content(g_app.message_view.store, found.handle);
    if ((content.bytes == 0 && !retained_rich_content(*record)) ||
        content.bytes > kMessageComposerMaximumBytes) {
        return false;
    }
    MessageEditRuntime& edit = g_app.message_edit;
    if (!message_edit_runtime_reserve(&edit, content.bytes, g_app.message_composer.bytes)) {
        return false;
    }
    application_composer_text_input_suspend();
    const MessageComposerControllerResult committed =
        message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller);
    if (!committed.ok()) {
        log_u64(LogLevel::error, "message", "message_edit_draft_commit_error",
                static_cast<u64>(committed.error));
        return false;
    }
    edit.channel_id = record->channel_id;
    edit.message_id = record->id;
    const auto reply = application_reply_draft();
    edit.saved_reply_message_id = reply.message_id;
    edit.saved_reply_mention_suppressed = !reply.mention_reply_author;
    edit.saved_draft_bytes = g_app.message_composer.bytes;
    edit.saved_draft_anchor = g_app.message_composer.anchor;
    edit.saved_draft_caret = g_app.message_composer.caret;
    edit.saved_draft_focused = g_app.message_composer.focused;
    copy_bytes(edit.saved_draft, g_app.message_composer.text, edit.saved_draft_bytes);
    if (!message_composer_restore(&g_app.message_composer, record->channel_id, content, content.bytes,
                                  content.bytes)) {
        reset_message_edit();
        return false;
    }
    stop_local_typing(record->channel_id);
    edit.editor_active = true;
    edit.phase = MessageEditRequestPhase::idle;
    g_app.message_composer.focused = true;
    ++g_app.message_composer.revision;
    message_composer_scene_focus_edit(&g_app.message_composer_scene);
    g_app.context_focus = &g_app.message_composer_scene.widgets;
    reset_message_composer_caret_timing(true);
    return true;
}

bool queue_message_edit() noexcept {
    if (g_app.account.status.required_action != AccountRequirement::none)
        return true;
    MessageEditRuntime& edit = g_app.message_edit;
    if (!g_app.account_ui || !edit.editor_active || edit.message_id == 0 || edit.channel_id == 0) {
        return false;
    }
    if (edit.phase != MessageEditRequestPhase::idle) {
        return true;
    }
    const auto* record = editable_record(edit.message_id);
    if (!record || record->channel_id != edit.channel_id)
        return true;
    const Utf8View content = message_composer_text(g_app.message_composer);
    if (!message_composer_can_send(g_app.message_composer, g_app.message_composer_scene.channel_allows_send,
                                   message_composer_maximum_codepoints(
                                       g_app.account_ui->chat_state.self_premium_type),
                                   retained_rich_content(*record)) ||
        content.bytes > kMessageComposerMaximumBytes ||
        !message_edit_runtime_reserve(&edit, content.bytes, edit.saved_draft_bytes)) {
        return true;
    }
    submit_edit(*record, content);
    return refresh_message_composer_ui();
}

bool queue_message_edit_content(u64 message_id, Utf8View content) noexcept {
    MessageEditRuntime& edit = g_app.message_edit;
    if (g_app.account.status.required_action != AccountRequirement::none || edit.message_id != 0 ||
        edit.editor_active || !content.bytes || content.bytes > kMessageComposerMaximumBytes ||
        !utf8_validate(content).valid)
        return false;
    const auto* record = editable_record(message_id);
    if (!record || !message_edit_runtime_reserve(&edit, content.bytes, 0))
        return false;
    submit_edit(*record, content);
    return true;
}

bool accept_message_edit_response(const RestClientResponse& response) noexcept {
    MessageEditRuntime& edit = g_app.message_edit;
    if (!g_app.account_ui || edit.message_id == 0 || edit.phase != MessageEditRequestPhase::submitting) {
        return false;
    }
    const auto failed = [&]() noexcept {
        application_message_mutation_notice_failed(ApplicationMessageMutationKind::edit, edit.channel_id,
                                                   edit.message_id, response);
        if (edit.editor_active) {
            edit.phase = MessageEditRequestPhase::idle;
        } else {
            reset_message_edit();
        }
        return refresh_message_composer_ui();
    };
    if (!response.transport_ok()) {
        log_u64(LogLevel::error, "rest", "message_edit_transport_error", static_cast<u64>(response.error));
        return failed();
    }
    if (response.status_code == 429 && response.rate_limited && response.retry_at_ms != 0 &&
        !api_rejection_requires_action(response.rejection)) {
        edit.retry_at_ms = response.retry_at_ms;
        edit.phase = MessageEditRequestPhase::retry_wait;
        log_u64(LogLevel::warning, "rest", "message_edit_retry_at_ms", response.retry_at_ms);
        return true;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        log_u64(LogLevel::error, "rest", "message_edit_http_status", response.status_code);
        return failed();
    }
    if (!apply_response(response.body_bytes)) {
        return false;
    }
    application_message_mutation_notice_succeeded(ApplicationMessageMutationKind::edit, edit.channel_id,
                                                  edit.message_id);
    const bool submitted_content_still_open =
        edit.editor_active && submitted_content_equal(edit, message_composer_text(g_app.message_composer));
    if (submitted_content_still_open) {
        if (!cancel_message_edit_editor()) {
            log_write(LogLevel::error, "message", "message_edit_draft_restore_failed");
            edit.phase = MessageEditRequestPhase::idle;
            return false;
        }
        reset_message_edit();
    } else if (edit.editor_active) {
        edit.phase = MessageEditRequestPhase::idle;
        edit.retry_at_ms = 0;
    } else {
        reset_message_edit();
    }
    return refresh_message_composer_ui();
}

} // namespace maicord::app_detail
