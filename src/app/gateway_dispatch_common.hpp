#pragma once

#include "maicord/app/gateway_client.hpp"

namespace maicord {

struct GatewayPayloadWriter;

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

inline constexpr u32 kGatewayMessageRichFieldMask =
    gateway_message_field_message_reference | gateway_message_field_referenced_message |
    gateway_message_field_reactions | gateway_message_field_attachments | gateway_message_field_embeds |
    gateway_message_field_sticker_items | gateway_message_field_components |
    gateway_message_field_interaction | gateway_message_field_message_snapshots |
    gateway_message_field_poll | gateway_message_field_call;

} // namespace maicord
