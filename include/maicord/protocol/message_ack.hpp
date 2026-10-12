#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {
inline constexpr u32 kMessageAckMaximumTargetBytes = 128;
inline constexpr u32 kMessageAckMaximumBodyBytes = 64 * 1024;

struct MessageAckRequest {
    // Decoded opaque UTF-8. An unknown token writes null; a known empty token
    // writes an empty JSON string. This builder does not retain or log tokens.
    Utf8View token;
    u32 last_viewed, flags;
    bool token_known, last_viewed_known, flags_known;
    // Manual unread ACKs omit ordinary token/date/flags entirely. A known
    // mention count writes even zero; an unknown count is omitted, not guessed.
    // Mixed manual/ordinary metadata, or an ordinary mention count, is invalid.
    bool manual;
    u32 mention_count;
    bool mention_count_known;
};
enum class MessageAckWriteError : u8 { none, invalid_input, capacity };
struct MessageAckWriteResult {
    u32 bytes;
    MessageAckWriteError error;
    constexpr bool ok() const noexcept { return error == MessageAckWriteError::none; }
};
// Failure leaves output bytes unchanged. Caller output must not overlap token.
// A zero message cursor is legal (the previous-millisecond boundary clamps to
// zero). Channel IDs must be nonzero; ordinary callers enforce their own cursor.
MessageAckWriteResult write_message_ack_target(
    u64 channel_id, u64 message_id, Slice<u8> output) noexcept;
MessageAckWriteResult write_message_ack_body(
    const MessageAckRequest& request, Slice<u8> output) noexcept;

struct MessageAckResponse {
    // Borrows the response JSON. Decode into controller-owned storage only
    // while that response buffer is alive. Missing and explicit null differ.
    JsonString token;
    bool token_known, token_null;
};
// Validates the entire JSON object, including unknown fields. Duplicate token
// members (including escaped spellings) fail. Failure leaves output unchanged.
bool decode_message_ack_response(Utf8View body, Slice<u32> structural_scratch,
    MessageAckResponse* output) noexcept;
} // namespace maicord
