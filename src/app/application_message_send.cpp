#include "application_action_captcha.hpp"
#include "application_commands.hpp"
#include "application_message_send.hpp"
#include "application_message_forward.hpp"
#include "application_attachments.hpp"

#include "application_account_presentation.hpp"
#include "application_message_feedback.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_edit.hpp"
#include "application_state.hpp"
#include "application_thread_actions.hpp"
#include "application_reply_draft.hpp"
#include "application_emoji_preferences.hpp"
#include "application_plugins.hpp"
#include "application_mention_confirmation.hpp"

#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_send_response.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/typing_store.hpp"

namespace maicord::app_detail {
namespace {

// The composer's channel is the active one and accepts a send.
bool composer_can_send_now() noexcept {
    const MessageComposer& composer = g_app.message_composer;
    return g_app.account_ui && g_app.account.instance && composer.channel_id != 0 &&
           composer.channel_id == g_app.account_ui->active_channel_id &&
           application_thread_actions_channel_sendable(composer.channel_id) &&
           message_composer_scene_can_send(g_app.message_composer_scene);
}

bool queue_composer_message(bool mention_confirmed) noexcept {
    if (!composer_can_send_now())
        return true;
    MessageComposer& composer = g_app.message_composer;
    UploadedAttachment attachments[kAttachmentUploadMaximumCount];
    u64 attachment_ids[kAttachmentUploadMaximumCount];
    u64 attachment_sizes[kAttachmentUploadMaximumCount];
    u32 attachment_count = 0;
    if (!application_attachment_prepare_send(composer.channel_id,
        {attachments, kAttachmentUploadMaximumCount}, {attachment_ids, kAttachmentUploadMaximumCount}, &attachment_count,
        {attachment_sizes, kAttachmentUploadMaximumCount})) return true;
    const auto reply = application_reply_draft();
    if (reply.message_id && !application_channel_readable(g_app, reply.channel_id)) return true;
    if (!mention_confirmed) {
        bool opened = false;
        if (!application_mention_confirmation_request(&opened)) return false;
        if (opened) return true;
    }
    const auto emoji_uses = application_emoji_preferences_scan(message_composer_text(composer));
    const MessageComposerControllerResult queued = message_composer_controller_queue_send(
        &g_app.account_ui->message_composer_controller, g_app.message_composer_scene.channel_allows_send,
        g_app.account_ui->chat_state.self_user_id, clock_unix_milliseconds(),
        clock_monotonic_milliseconds(), reply.message_id, {attachments, attachment_count},
        {attachment_sizes, attachment_count}, nullptr, {}, reply.mention_reply_author, &reply);
    if (!queued.ok()) {
        log_u64(LogLevel::error, "message", "message_send_queue_error", static_cast<u64>(queued.error));
    } else {
        application_attachment_did_queue({attachment_ids, attachment_count}, queued.nonce);
        // Uploads arm the cooldown as they start (the client's UPLOAD_START); a discarded failure retires it.
        if (attachment_count)
            application_composer_note_sent(composer.channel_id);
        application_emoji_preferences_track_message(emoji_uses);
        const TypingStoreResult typing_stopped =
            typing_store_note_message(&g_app.account_ui->typing_store, composer.channel_id,
                                              g_app.account_ui->chat_state.self_user_id, true);
        if (!typing_stopped.ok()) {
            log_u64(LogLevel::warning, "typing", "message_stop_error",
                    static_cast<u64>(typing_stopped.error));
        }
    }
    return refresh_message_composer_ui();
}

} // namespace

bool application_message_send_confirmed() noexcept {
    return queue_composer_message(true);
}

bool queue_message_send() noexcept {
    if (g_app.account.status.required_action != AccountRequirement::none) return true;
    // Before READY the text stays in the composer; READY confirms the right to send.
    if (g_app.startup_shell_provisional) return true;
    if (application_composer_text_input_composing()) return true;
    if (g_app.message_edit.editor_active) {
        return queue_message_edit();
    }
    if (!composer_can_send_now())
        return true;
    bool prepared_handled = false;
    if (!application_composer_prepare_send(&prepared_handled)) return false;
    if (prepared_handled) return refresh_message_composer_ui();
    bool plugin_handled = false;
    if (!application_plugins_compose(&plugin_handled)) return false;
    if (plugin_handled) return refresh_message_composer_ui();
    bool command_handled = false;
    if (!application_commands_send(&command_handled)) return false;
    if (command_handled) return refresh_message_composer_ui();
    return queue_composer_message(false);
}

bool accept_message_send_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!response.transport_ok()) {
        log_u64(LogLevel::error, "rest", "message_send_transport_error",
                static_cast<u64>(response.error));
    } else if (response.status_code == 429 && response.rate_limited && response.retry_at_ms != 0 &&
               !api_rejection_requires_action(response.rejection)) {
        log_u64(LogLevel::warning, "rest", "message_send_retry_at_ms", response.retry_at_ms);
    } else if (response.status_code < 200 || response.status_code >= 300) {
        log_u64(LogLevel::error, "rest", "message_send_http_status", response.status_code);
    }
    auto& ui = *g_app.account_ui;
    MessageHistoryDecodeScratch scratch{};
    if (response.transport_ok() && response.status_code >= 200 && response.status_code < 300 &&
        body.data && body.bytes && body.bytes == response.body_bytes &&
        !application_message_decode_scratch(ui, &scratch, body)) {
        log_write(LogLevel::error, "message", "history_decode_storage_admission_failed");
        return false;
    }
    u64 request_channel = 0;
    for (const auto& request : ui.message_composer_controller.requests) {
        if (request.occupied && request.nonce == ui.message_composer_controller.inflight_nonce) {
            request_channel = request.channel_id;
            g_app.message_rejection = response.rejection;
            g_app.message_rejection_channel_id = request.channel_id;
            // Retrying this failed message opens the browser challenge.
            application_action_captcha_capture(ApplicationActionCaptchaKind::message_send, request.nonce, response,
                                               body);
            break;
        }
    }
    const auto accepted = message_send_accept_response(&ui.message_composer_controller, response, body, scratch,
        ui.active_channel_id, ui.chat_state.self_user_id, clock_monotonic_milliseconds());
    // AutoMod blocks and undeliverable DMs also get the official client's in-channel notice.
    if (const auto notice = message_send_rejection_system_notice(response.rejection);
        notice.bytes && request_channel && request_channel == ui.active_channel_id &&
        response.transport_ok() && response.status_code >= 400) {
        static u32 notice_sequence = 0;
        const auto inserted = message_timeline_insert_local(
            &g_app.message_view.timeline,
            message_send_rejection_notice_input(request_channel, notice, clock_unix_milliseconds(),
                                                ++notice_sequence));
        if (!inserted.ok())
            log_write(LogLevel::warning, "message", "rejection_notice_insert_failed");
    }
    if (!accepted.http.ok()) {
        log_u64(LogLevel::error, "message", "message_send_http_controller_error",
                static_cast<u64>(accepted.http.error));
        return false;
    }
    // Delivery starts the prediction even when its body cannot be reconciled.
    if (request_channel && response.transport_ok() && response.status_code >= 200 && response.status_code < 300)
        application_composer_note_sent(request_channel);
    if (accepted.body_expected && !accepted.body_decoded) {
        log_u64(LogLevel::warning, "message", "message_send_response_decode_error", static_cast<u64>(accepted.decode.error));
    } else if (!accepted.message.ok()) {
        log_u64(LogLevel::warning, "message", "message_send_response_reconcile_error", static_cast<u64>(accepted.message.error));
        g_app.message_pump.resync_required = true;
    } else if (accepted.message.changed) {
        const auto& message = scratch.messages[0];
        if (!message_history_observe_page(&ui.user_store, &ui.guild_member_store, &ui.chat_state,
            {scratch.messages.data, 1})) g_app.message_pump.resync_required = true;
        const bool viewed = g_app.message_pump.view_active && g_app.message_view.timeline.follow_bottom &&
            ui.active_channel_id == message.channel_id;
        // Self-authored delivery advances the same read/activity state as a
        // Gateway create. The helper ignores already-observed older IDs.
        chat_state_store_note_message(&ui.chat_state, message.channel_id, message.id,
            message.author_id, message.mentions_current_user, viewed);
        if (!message_history_apply_mention_flags(&g_app.message_view.timeline, &ui.chat_state,
            {scratch.messages.data, 1})) g_app.message_pump.resync_required = true;
        if (!consume_message_send_completions()) return false;
    }
    return refresh_account_frame(true);
}

bool application_message_send_captcha_resume() noexcept {
    const u64 nonce = application_action_captcha_subject(ApplicationActionCaptchaKind::message_send);
    if (!nonce || !g_app.account_ui ||
        application_action_captcha_phase(ApplicationActionCaptchaKind::message_send, nonce) !=
            ApplicationActionCaptchaPhase::solved)
        return false;
    const auto* record = application_message_record(nonce);
    if (!record || record->delivery != MessageDeliveryState::failed ||
        !message_composer_controller_retry(&g_app.account_ui->message_composer_controller, nonce,
                                           clock_monotonic_milliseconds())
             .ok()) {
        application_action_captcha_cancel();
        return false;
    }
    return true;
}

bool consume_message_send_completions() noexcept {
    if (!g_app.account_ui) {
        return true;
    }
    bool changed = false;
    MessageSendCompletion completion{};
    while (message_composer_controller_take_completion(&g_app.account_ui->message_composer_controller,
                                                       &completion)) {
        application_message_forward_completion(completion);
        if (completion.kind == MessageSendCompletionKind::accepted) application_attachment_complete(completion.nonce);
        if (completion.http_observed) {
            log_u64(LogLevel::info, "message", "message_send_api_elapsed_ms", completion.api_elapsed_ms);
        }
        if (completion.gateway_observed) {
            log_u64(LogLevel::info, "message", "message_send_gateway_elapsed_ms",
                    completion.gateway_elapsed_ms);
        }
        if (completion.history_observed) {
            log_write(LogLevel::info, "message", "message_send_history_confirmed");
        }
        if (completion.response_observed) {
            log_write(LogLevel::info, "message", "message_send_response_confirmed");
        }
        changed = true;
    }
    return !changed || refresh_account_text(true);
}

} // namespace maicord::app_detail
