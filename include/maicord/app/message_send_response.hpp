#pragma once

#include "maicord/app/message_composer_controller.hpp"

namespace maicord {
struct RestClientResponse;
struct MessageHistoryDecodeScratch;

struct MessageSendResponseResult {
    MessageComposerControllerResult http;
    MessageComposerControllerResult message;
    GatewayEventDecodeResult decode;
    bool body_expected;
    bool body_decoded;
};

// The caller has matched the transport request ID. Uses the outstanding nonce
// before finishing that response, and borrows the history decode scratch.
// A malformed success body keeps Gateway/timeout recovery; it is not a
// transport failure and must not trigger an automatic duplicate send.
MessageSendResponseResult message_send_accept_response(MessageComposerController* controller,
    const RestClientResponse& response, Utf8View body, const MessageHistoryDecodeScratch& scratch,
    u64 active_channel_id, u64 current_user_id, u64 now_ms) noexcept;
// Local message type of client-generated system notices (never sent by the
// server); rendered as a compact system row with the ephemeral footer.
inline constexpr u16 kMessageTypeLocalNotice = 0xFFF0;
inline constexpr u32 kMessageFlagEphemeral = 1u << 6;

// The official client answers some send rejections with an in-channel system notice that
// only the sender sees: AutoMod blocks (200000/200001) and undeliverable
// direct messages (50007). Empty for other rejections.
Utf8View message_send_rejection_system_notice(ApiRejection rejection) noexcept;
// The local notice record: an ephemeral system row timestamped now, with a
// snowflake-ordered local id (unique per sequence).
MessageInput message_send_rejection_notice_input(u64 channel_id, Utf8View text, u64 now_unix_ms,
                                                 u32 sequence) noexcept;
} // namespace maicord
