#include "maicord/app/message_send_response.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/rest_client.hpp"

namespace maicord {
MessageSendResponseResult message_send_accept_response(MessageComposerController* controller,
    const RestClientResponse& response, Utf8View body, const MessageHistoryDecodeScratch& scratch,
    u64 active_channel_id, u64 current_user_id, u64 now_ms) noexcept {
    MessageSendResponseResult result{};
    const u64 nonce = controller ? controller->inflight_nonce : 0;
    MessageSendHttpDisposition disposition = MessageSendHttpDisposition::terminal_failure;
    if (response.transport_ok()) {
        if (response.status_code == 429 && response.rate_limited && response.retry_at_ms &&
            !api_rejection_requires_action(response.rejection))
            disposition = MessageSendHttpDisposition::retry_wait;
        else if (response.status_code >= 200 && response.status_code < 300)
            disposition = MessageSendHttpDisposition::success;
    }
    result.http = message_composer_controller_observe_http(controller, response.status_code, disposition,
        now_ms, response.retry_at_ms);
    if (!result.http.ok() || disposition != MessageSendHttpDisposition::success) return result;
    result.body_expected = true;
    if (!body.data || !body.bytes || body.bytes != response.body_bytes || !scratch.messages.data || !scratch.messages.count) {
        result.decode.error = GatewayEventDecodeError::invalid_input;
        return result;
    }
    auto& message = scratch.messages[0];
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds];
    result.decode = gateway_decode_message_create({body, 0, JsonValueKind::object}, scratch.message_structurals,
        scratch.maximum_depth, scratch.content, {mention_roles, kGatewayMessageMentionRoleMaximumIds},
        scratch.rich.member_roles, scratch.mention_users,
        {scratch.rich.reactions, scratch.rich.attachments, scratch.rich.embeds, scratch.rich.embed_fields,
            scratch.rich.stickers, scratch.rich.components, scratch.rich.snapshot}, current_user_id, &message);
    if (result.decode.ok()) {
        result.body_decoded = true;
        result.message = message_composer_controller_observe_response(controller, nonce, message, active_channel_id);
    }
    // Role IDs used during this transaction must not escape as stack views.
    message.mention_roles = {};
    return result;
}
Utf8View message_send_rejection_system_notice(ApiRejection rejection) noexcept {
    if (!rejection.has_code)
        return {};
    switch (rejection.code) {
    case 200000:
        return utf8_literal("Your message was blocked by AutoMod. It may contain content this server doesn't allow.");
    case 200001:
        return utf8_literal("Your message was blocked by AutoMod because of its title.");
    case 50007:
        return utf8_literal("Your message couldn't be delivered. You may not share a server with this person, or "
                            "they only accept direct messages from friends.");
    default:
        return {};
    }
}

MessageInput message_send_rejection_notice_input(u64 channel_id, Utf8View text, u64 now_unix_ms,
                                                 u32 sequence) noexcept {
    MessageInput input{};
    const u64 since_epoch =
        now_unix_ms > kSnowflakeEpochMilliseconds ? now_unix_ms - kSnowflakeEpochMilliseconds : 0u;
    input.id = (since_epoch << 22) | (static_cast<u64>(sequence) & 0x3FFFFFu) | 1u;
    input.channel_id = channel_id;
    input.timestamp_ms = now_unix_ms;
    input.content = text;
    input.flags = kMessageFlagEphemeral;
    input.type = kMessageTypeLocalNotice;
    // Local inserts are never authoritative; the notice is not a send.
    input.delivery = MessageDeliveryState::pending;
    return input;
}
} // namespace maicord
