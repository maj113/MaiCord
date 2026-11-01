#pragma once

#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/gateway_message_data.hpp"

namespace maicord {

// Poll strings borrow caller-owned decoded text; no view borrows JSON input.
// Omitted message fields are untouched; top-level null sets present=false.
// A supplied poll object may be partial. Missing results do not mean zero votes.
// Explicit results:null is rejected because the API schema permits only objects.
GatewayEventDecodeResult gateway_decode_message_poll(JsonValue data, const JsonIndexedDocument& document,
                                                     u32 maximum_depth, Slice<char> text_output,
                                                     u32* text_used,
                                                     const GatewayMessagePollScratch& scratch,
                                                     GatewayMessagePoll* output,
                                                     bool require_complete = false) noexcept;
GatewayEventDecodeResult gateway_decode_message_poll_vote(JsonValue data, Slice<u32> structural_scratch,
                                                          u32 maximum_depth, bool add,
                                                          GatewayMessagePollVoteEvent* output) noexcept;

// Decodes the API's RFC 3339/ISO 8601 timestamp representation to Unix
// milliseconds. Fractional precision beyond milliseconds is validated and
// truncated, matching the model's storage precision.
bool parse_iso_timestamp(const JsonString& value, u64* timestamp_ms) noexcept;

// Content is unescaped into caller storage; the returned view is valid only
// while that storage is. The fields mask distinguishes an absent MESSAGE_UPDATE
// field from a present empty or null value. Snapshot core fields are partial
// and carry no message/channel/author identity.
//
// Overloads taking a JsonIndexedDocument decode a value of an already indexed document
// (a page of messages): nothing is scanned and the index stays intact.
GatewayEventDecodeResult gateway_decode_message_snapshot_core(const JsonIndexedDocument& document, JsonValue data,
                                                              u32 maximum_depth,
                                                              Slice<char> content_scratch,
                                                              Slice<GatewayMessageNamedUser> mention_users,
                                                              GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_extract_message_snapshot(const JsonIndexedDocument& document, JsonValue array,
                                                          u32 maximum_depth, JsonValue* body,
                                                          bool* present) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(const JsonIndexedDocument& document, JsonValue data,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(const JsonIndexedDocument& document, JsonValue data,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(
    const JsonIndexedDocument& document, JsonValue data, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_update(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_update(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_update(
    const JsonIndexedDocument& document, JsonValue data, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;

GatewayEventDecodeResult gateway_decode_message_rich_fields(JsonValue data, Slice<u32> structural_scratch,
                                                            u32 maximum_depth, Slice<char> text_output,
                                                            u32* text_used,
                                                            const GatewayMessageRichScratch& scratch,
                                                            GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_rich_fields(const JsonIndexedDocument& document, JsonValue data,
                                                            u32 maximum_depth, Slice<char> text_output,
                                                            u32* text_used,
                                                            const GatewayMessageRichScratch& scratch,
                                                            GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_delete(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth,
                                                       GatewayMessageDeleteEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_bulk_delete(JsonValue data, Slice<u32> structural_scratch,
                                                            u32 maximum_depth, Slice<u64> ids,
                                                            GatewayMessageBulkDeleteEvent* output) noexcept;

} // namespace maicord
