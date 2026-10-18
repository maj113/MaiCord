#pragma once

#include "maicord/protocol/message_history.hpp"

namespace maicord {

inline constexpr u32 kMessagePinsMaximumItems = 50;
inline constexpr u32 kMessagePinsCursorMaximumBytes = 40;
inline constexpr u32 kMessagePinsMaximumTargetBytes = 256;
inline constexpr u64 kMessagePinsRouteId = 0x4d534750494e534cull;
inline constexpr u64 kMessagePinRouteId = 0x4d534750494e4d55ull;

// Preserve the server timestamp exactly: millisecond normalization would lose
// pin ordering/cursor precision. A zero-byte cursor requests the first page.
struct MessagePinsCursor {
    char timestamp[kMessagePinsCursorMaximumBytes];
    u8 bytes;
};

struct MessagePinsQuery {
    u64 channel_id;
    MessagePinsCursor before;
    u8 limit;
};

enum class MessagePinsOperation : u8 { fetch, pin, unpin };

struct MessagePinsRequest {
    MessagePinsQuery query;
    u64 message_id;
    MessagePinsOperation operation;
};

struct MessagePin {
    GatewayMessageEvent message;
    MessagePinsCursor pinned_at;
    u64 pinned_at_ms;
};

struct MessagePinsPage {
    u64 channel_id;
    MessagePinsCursor next_before;
    u32 count;
    u32 text_bytes;
    bool has_more;
};

enum class MessagePinsWriteError : u8 {
    none,
    invalid_input,
    output_capacity,
};

struct MessagePinsWriteResult {
    u32 bytes;
    MessagePinsWriteError error;
    constexpr bool ok() const noexcept { return error == MessagePinsWriteError::none; }
};

// Validates timestamp syntax through the shared API timestamp parser.
// Empty is valid only when allow_empty is true. No cursor text is logged.
bool message_pins_cursor_valid(const MessagePinsCursor& cursor,
                               bool allow_empty = true) noexcept;
MessagePinsWriteResult write_message_pins_target(
    const MessagePinsQuery& query, Slice<u8> output) noexcept;
// The target is shared by PUT pin and DELETE unpin. The HTTP method and rate
// limiting are handled by the authenticated REST service; neither operation has a body.
MessagePinsWriteResult write_message_pin_target(
    u64 channel_id, u64 message_id, Slice<u8> output) noexcept;

enum class MessagePinsDecodeError : u8 {
    none,
    invalid_input,
    scan_failed,
    invalid_json,
    duplicate_field,
    missing_field,
    invalid_field,
    item_capacity,
    message_decode,
    channel_mismatch,
    duplicate_message,
    invalid_order,
    invalid_cursor,
};

struct MessagePinsDecodeResult {
    MessagePinsPage page;
    u32 error_offset;
    MessagePinsDecodeError error;
    GatewayEventDecodeError message_error;
    JsonScanError scan_error;
    JsonReadError read_error;
    char schema_field[32];
    constexpr bool ok() const noexcept { return error == MessagePinsDecodeError::none; }
};

// Results remain separate from timeline events. On failure discard all output
// slices. On success all message text and rich views point into caller arenas;
// cursor values are owned copies. Pin order follows pinned_at, not message IDs.
MessagePinsDecodeResult decode_message_pins_page(
    Utf8View body, const MessagePinsQuery& query, Slice<u32> page_structurals,
    Slice<u32> message_structurals, u32 maximum_depth, Slice<char> text_output,
    Slice<MessagePin> items, Slice<GatewayMessageNamedUser> mention_users,
    const MessageHistoryRichScratch& rich_scratch) noexcept;

} // namespace maicord
