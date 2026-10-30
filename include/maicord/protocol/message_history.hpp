#pragma once

#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {

inline constexpr u32 kMessageHistoryMaximumMessages = 100;

// Fields a history page message may lose when its rich (presentation) decode
// fails and the message falls back to its core text.
inline constexpr u32 kMessageHistoryRichFieldMask =
    gateway_message_field_message_reference | gateway_message_field_referenced_message |
    gateway_message_field_reactions | gateway_message_field_attachments | gateway_message_field_embeds |
    gateway_message_field_sticker_items | gateway_message_field_components | gateway_message_field_interaction |
    gateway_message_field_message_snapshots | gateway_message_field_poll | gateway_message_field_thread;

enum class MessageHistoryPosition : u8 {
    latest,
    before,
    after,
    around,
};

struct MessageHistoryQuery {
    u64 cursor;
    u32 limit;
    MessageHistoryPosition position;
};

enum class MessageHistoryTargetError : u8 {
    none,
    invalid_input,
    output_capacity,
};

struct MessageHistoryTargetResult {
    u32 bytes;
    MessageHistoryTargetError error;

    constexpr bool ok() const noexcept {
        return error == MessageHistoryTargetError::none;
    }
};

struct MessageHistoryPage {
    u64 channel_id;
    u64 newest_id;
    u64 oldest_id;
    u32 count;
    u32 content_bytes;
};

enum class MessageHistoryDecodeError : u8 {
    none,
    invalid_input,
    scan_failed,
    invalid_json,
    message_capacity,
    message_decode,
    invalid_order,
    channel_mismatch,
};

struct MessageHistoryDecodeResult {
    MessageHistoryPage page;
    u32 error_offset;
    MessageHistoryDecodeError error;
    GatewayEventDecodeError message_error;
    JsonScanError scan_error;
    JsonReadError read_error;
    char schema_field[32];
    // Messages whose rich fields failed to decode and were kept as core text.
    u32 degraded_messages;

    constexpr bool ok() const noexcept {
        return error == MessageHistoryDecodeError::none;
    }
};

// Aggregate caller storage for one decoded REST page. Each returned message
// owns a non-overlapping slice of these arrays; all text still points into the
// page's content_output arena.
struct MessageHistoryRichScratch {
    Slice<GatewayMessageReaction> reactions;
    Slice<GatewayMessageAttachment> attachments;
    Slice<GatewayMessageEmbed> embeds;
    Slice<GatewayMessageEmbedField> embed_fields;
    Slice<GatewayMessageSticker> stickers;
    Slice<GatewayMessageComponent> components;
    Slice<u64> member_roles;
    // Forwarded child descriptors are independent of the outer message pools.
    // Child strings share content_output and are included in transient_bytes.
    GatewayMessageSnapshotScratch snapshot;
    GatewayMessagePollScratch poll;
};

// Walks the scratch across one page: every decoded message consumes a prefix of
// each array and the next message decodes into what remains.
struct MessageHistoryRichCursor {
    MessageHistoryRichScratch scratch;
    u32 member_roles, reactions, attachments, embeds, embed_fields, stickers, components;
    u32 snapshot_attachments, snapshot_embeds, snapshot_embed_fields, snapshot_stickers;
    u32 snapshot_components, snapshot_mentions, poll_answers, poll_counts;

    // No array claims capacity without storage.
    bool valid() const noexcept;
    // Some array has storage: messages decode with rich capture.
    bool present() const noexcept;
    Slice<u64> remaining_member_roles() const noexcept;
    GatewayMessageRichScratch remaining() const noexcept;
    void advance(const GatewayMessageEvent& message) noexcept;
};

// Writes an origin-form API v10 request target. Rate-limit policy is not handled
// here; it comes from the HTTP response headers.
MessageHistoryTargetResult write_message_history_target(
    u64 channel_id, const MessageHistoryQuery& query, Slice<u8> output) noexcept;

// The API returns history arrays newest-to-oldest. The decoder validates that
// ordering and a single channel identity. Each message is decoded straight from
// the page index, so page_structural_scratch must stay intact for the whole call.
// Decoded content is packed into caller storage and every returned Utf8View
// points into content_output.
MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages) noexcept;
MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users) noexcept;
MessageHistoryDecodeResult decode_message_history_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users,
    const MessageHistoryRichScratch& rich_scratch) noexcept;
// GET /users/@me/mentions: the same message array, spanning channels.
MessageHistoryDecodeResult decode_message_mentions_page(
    Utf8View body, Slice<u32> page_structural_scratch, u32 maximum_depth, Slice<char> content_output,
    Slice<GatewayMessageEvent> messages, Slice<GatewayMessageNamedUser> mention_users) noexcept;

} // namespace maicord
