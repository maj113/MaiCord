#pragma once
// Shared by dispatch.cpp and its parts (dispatch_*.cpp); private to them.
#include "maicord/core/memory.hpp"
#include "maicord/app/gateway/client_event.hpp"

#include "relationship_dispatch.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"

namespace maicord {
namespace dispatch_detail {}
using namespace dispatch_detail;
namespace dispatch_detail {

bool append_message_rich_metadata(Slice<char> output, const GatewayMessageEvent& message, u32* cursor,
                                  u32* rich_payload_bytes) noexcept;
bool append_message_metadata(Slice<char> output, const GatewayMessageEvent& message,
                             ConstSlice<u64> role_ids, GatewayClientMessageMutation* mutation,
                             ConstSlice<u8>* payload) noexcept;

struct NotificationFields {
    JsonValue entries, proto, settings, threads, member, added, removed, members, schedule, rules, channels;
    u64 id, guild, user, version;
    u32 seen, flags, type;
    bool partial, version_known, bad, unavailable, default_mentions, request, spam, nsfw;
    u8 nsfw_level, owner_content_level;
    u32 known_gates, gates;
};

bool notification_fields(void* opaque, JsonString name, JsonValue v) noexcept;
bool notification_document(JsonValue source, Slice<u32> scratch, JsonIndexedDocument* doc,
                           NotificationFields* fields) noexcept;
bool restriction_inactive(const JsonIndexedDocument& doc, const NotificationFields& f, bool full) noexcept;
void notification_incomplete(GatewayClientEvent* e, ConstSlice<u8>* payload, u64 guild) noexcept;
bool notification_entry(JsonValue source, Slice<u32> scratch, Slice<char> storage, GatewayClientEvent* e,
                        ConstSlice<u8>* payload) noexcept;
bool notification_proto(JsonValue value, Slice<char> storage, MessageNotificationGlobal* g) noexcept;
bool notification_route(const GatewayEnvelope& env, Slice<u32> scratch, Slice<char> storage,
                        GatewayClientEvent* e, ConstSlice<u8>* payload, u32 kind) noexcept;

} // namespace dispatch_detail
} // namespace maicord
