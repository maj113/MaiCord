#pragma once

#include "maicord/app/gateway_client.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/protocol/gateway_reaction_events.hpp"

namespace maicord {

struct GatewayPayloadWriter;

// Appends the shared notification metadata to an already normalized state
// record, using the same final storage as its typed payload.
void gateway_client_append_notification_sidecar(const GatewayEnvelope& envelope,
                                                Slice<u32> structural_scratch, Slice<char> storage,
                                                u64 self_user_id, GatewayClientEvent* event,
                                                ConstSlice<u8>* payload) noexcept;

// Worker-owned, session-scoped progress. Keep through RESUME and clear when
// invalidating the saved session or committing a fresh READY.
struct GatewayReactionBatchCursor {
    u64 sequence;
    u32 published;
    bool active;
};
// Each accepted batch entry becomes the same pointer-free event as a single
// reaction add. The publisher must copy its borrowed name before returning.
// A cursor suppresses an already-published prefix of this exact replayed
// sequence. visited includes that prefix; cancellation retains total progress.
GatewayReactionBatchVisitResult gateway_client_publish_reaction_batch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> name_scratch, u64 now_ms,
    bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept, void* context,
    GatewayReactionBatchCursor* cursor = nullptr) noexcept;
// MESSAGE_POLL_VOTE_ADD_MANY under the same contract: each vote becomes a
// batched message_poll_vote_add with the envelope's sequence.
GatewayReactionBatchVisitResult gateway_client_publish_poll_vote_batch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth, u64 now_ms,
    bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept, void* context,
    GatewayReactionBatchCursor* cursor = nullptr) noexcept;

// A member's user object as member-list and thread-member records carry it.
// field_detail is the last visited field (ordinal << 8 | JSON kind), kept for
// skipped-dispatch diagnostics.
struct GatewayMemberUser {
    u64 id;
    JsonString username;
    JsonString global_name;
    JsonString display_name;
    JsonString avatar_hash;
    GatewayEventDecodeError error;
    u32 field_detail;
    bool id_seen;
    bool username_seen;
    bool global_name_seen;
    bool display_name_seen;
    bool avatar_seen;
    bool global_name_null;
    bool display_name_null;
    bool avatar_null;
};
// JsonMemberVisitor filling a GatewayMemberUser.
bool gateway_visit_member_user(void* opaque, JsonString name, JsonValue value) noexcept;
// Decodes string into the writer; false unless it decodes to exactly expected bytes that fit.
bool gateway_append_json_string(GatewayPayloadWriter* writer, JsonString string, u16 expected) noexcept;
// Decoded byte count of a member text field (at most 128 bytes).
bool gateway_member_string_size(JsonString value, u16* bytes) noexcept;
// A presence "status" value; unknown when absent or unrecognized.
GatewayClientMemberListStatus gateway_member_status(bool seen, JsonString status) noexcept;

inline GatewayClientDispatchRouteResult route_error(
    GatewayClientDispatchRouteError error,
    GatewayEventDecodeError event_error = GatewayEventDecodeError::none) noexcept {
    return GatewayClientDispatchRouteResult{event_error, error};
}

// A rejected object member: a repeated key is a duplicate, any other failure invalid data.
constexpr GatewayEventDecodeError gateway_field_error(bool repeated) noexcept {
    return repeated ? GatewayEventDecodeError::duplicate_field : GatewayEventDecodeError::invalid_data;
}

// Fails a visit on a repeated key.
inline bool gateway_duplicate_member(GatewayEventDecodeError* error) noexcept {
    *error = GatewayEventDecodeError::duplicate_field;
    return false;
}

// Keeps the first occurrence of a member whose value has the expected shape. A repeat or another
// shape fails the visit and sets *error unless an earlier error is already recorded.
inline bool gateway_keep_member(GatewayEventDecodeError* error, bool* seen, JsonValue* slot,
                                JsonValue value, bool shape_ok) noexcept {
    if (*seen || !shape_ok) {
        if (*error == GatewayEventDecodeError::none)
            *error = gateway_field_error(*seen);
        return false;
    }
    *seen = true;
    *slot = value;
    return true;
}

inline GatewayClientEvent gateway_dispatch_event(GatewayClientDispatchKind kind, u64 sequence,
                                                 u64 now_ms) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = kind;
    event.sequence = sequence;
    event.has_sequence = true;
    event.timestamp_ms = now_ms;
    return event;
}

// An empty avatar hash, or 32 hexadecimal digits optionally prefixed "a_" for an animated one.
inline bool gateway_avatar_hash_valid(Utf8View hash) noexcept {
    if (!hash.bytes)
        return true;
    const u32 first = hash.bytes == 34 && hash.data[0] == 'a' && hash.data[1] == '_' ? 2u : 0u;
    if (hash.bytes - first != 32)
        return false;
    for (u32 i = first; i < hash.bytes; ++i) {
        const char digit = hash.data[i];
        if (!ascii_is_hex_digit(digit))
            return false;
    }
    return true;
}

inline constexpr u32 kGatewayMessageRichFieldMask =
    gateway_message_field_message_reference | gateway_message_field_referenced_message |
    gateway_message_field_reactions | gateway_message_field_attachments | gateway_message_field_embeds |
    gateway_message_field_sticker_items | gateway_message_field_components |
    gateway_message_field_interaction | gateway_message_field_message_snapshots |
    gateway_message_field_poll | gateway_message_field_call;

} // namespace maicord
