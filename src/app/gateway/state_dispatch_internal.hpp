#pragma once
// Shared by state_dispatch.cpp and its parts (state_dispatch_*.cpp); private to them.
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/model/chat/chat_state.hpp"

#include "guild_details.hpp"
#include "payload_writer.hpp"
#include "user_payload.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"
#include "../../protocol/gateway/decode.hpp"

namespace maicord {
namespace state_dispatch_detail {}
using namespace state_dispatch_detail;
namespace state_dispatch_detail {

using protocol_detail::decode_failure;
using protocol_detail::member_bit;
using protocol_detail::member_index;
using protocol_detail::read_failure;
using protocol_detail::skim_member_index;

inline constexpr u32 kGatewayGuildRoleMaximum = 256;
inline constexpr u32 kGatewayGuildIconHashMaximumBytes = 34;

struct StateObjectContext {
    GatewayClientStateMutation mutation;
    JsonString name;
    JsonString icon;
    // Guild banner hash; banner_seen with an empty value is a null banner.
    JsonString banner;
    JsonValue channels;
    JsonValue threads;
    JsonValue roles;
    JsonValue members;
    JsonValue emojis;
    JsonValue permission_overwrites;
    JsonValue role;
    JsonValue user;
    JsonValue properties;
    JsonValue colors;
    JsonValue tags;
    JsonValue recipients;
    JsonValue recipient_ids;
    JsonValue nicks;
    JsonValue features;
    JsonValue thread_metadata;
    JsonValue thread_member;
    JsonValue safety_warnings;
    GatewayGuildDetailValues details;
    u64 allow;
    u64 deny;
    GatewayEventDecodeError error;
    bool channels_seen;
    bool name_null;
    bool threads_seen;
    bool roles_seen;
    bool members_seen;
    bool emojis_seen;
    bool permission_overwrites_seen;
    bool role_seen;
    bool user_seen;
    bool properties_seen;
    bool colors_seen;
    bool tags_seen;
    // Role tags.guild_connections is present: a linked role.
    bool role_linked;
    bool recipients_seen;
    bool recipient_ids_seen;
    bool nicks_seen;
    bool allow_seen;
    bool deny_seen;
    bool animated;
    bool available;
    bool require_colons;
    bool animated_seen;
    bool available_seen;
    bool require_colons_seen;
    bool features_seen;
    bool banner_seen;
    bool thread_metadata_seen;
    bool thread_member_seen;
    u8 guild_nsfw_level;
    bool guild_nsfw_level_seen;
    bool mentionable_seen;
    bool unicode_emoji_seen;
    bool unicode_emoji_present;
    bool message_request, message_request_seen, spam, spam_seen, safety_warnings_seen;
    // A guild's join time; the official client reads a guild without one as previewed.
    bool joined_at_seen, joined;
    bool newly_created_seen, newly_created;
};

using PayloadWriter = GatewayPayloadWriter;

GatewayClientDispatchRouteResult route_detail(GatewayClientDispatchRouteError error,
                                              GatewayEventDecodeError event_error, u32 detail) noexcept;
GatewayClientDispatchRouteResult append_failure(GatewayEventDecodeError error, u32 detail = 0) noexcept;
GatewayClientDispatchRouteResult decode_route(GatewayEventDecodeError error) noexcept;
GatewayEventDecodeResult visit_array(JsonValue array, Slice<u32> scratch, u32 maximum_depth,
                                     JsonArrayVisitor visit, void* context,
                                     const GatewayEventDecodeError& error,
                                     GatewayEventDecodeError fallback = GatewayEventDecodeError::invalid_json) noexcept;
void member_state_from_flags(GatewayClientStateMutation* m) noexcept;

// The first undismissed DM safety warning: {"id","type","dismiss_timestamp"}.
struct SafetyWarningContext {
    const JsonIndexedDocument* document;
    u64 id;
    u8 type;
};

bool safety_warning_item(void* opaque, u32, JsonValue value) noexcept;

struct RecipientContext {
    PayloadWriter* writer;
    Slice<u32> scratch;
    u32 depth;
    u16 count;
    bool ids;
    GatewayClientDispatchRouteResult result;
};
struct NicknameContext {
    PayloadWriter* writer;
    const JsonIndexedDocument* document;
    u32 depth;
    u32 count;
    GatewayClientDispatchRouteResult result;
    u64 seen[256];
    u32 seen_count;
};

bool append_nickname(void* opaque, u32, JsonValue value) noexcept;
bool append_recipient(void* opaque, u32, JsonValue value) noexcept;
GatewayEventDecodeResult decode_state_object(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                                             StateObjectContext* output, bool private_name_allowed = false,
                                             const JsonIndexedDocument* document = nullptr) noexcept;
bool merge_guild_properties(StateObjectContext* object, const StateObjectContext& properties) noexcept;
bool append_name(PayloadWriter* writer, JsonString source, u16* bytes) noexcept;
bool append_icon(PayloadWriter* writer, JsonString source, u8* bytes) noexcept;
u32 grown_record_capacity(u32 capacity) noexcept;
u8 role_flags(const StateObjectContext& object) noexcept;
GatewayEventDecodeResult append_role_array(JsonValue array, Slice<u32> array_scratch,
                                           Slice<u32> object_scratch, u32 maximum_depth,
                                           PayloadWriter* writer, u32* count) noexcept;
GatewayEventDecodeResult append_snowflake_array(JsonValue array, Slice<u32> scratch, u32 maximum_depth,
                                                PayloadWriter* writer, u32* count, u32 limit = kGatewayGuildRoleMaximum) noexcept;
GatewayEventDecodeResult append_emoji_array(JsonValue array, Slice<u32> array_scratch,
                                            Slice<u32> object_scratch, Slice<u32> role_scratch,
                                            u32 maximum_depth, PayloadWriter* writer, u32* count,
                                            u32* role_count) noexcept;
GatewayEventDecodeResult append_overwrite_array(JsonValue array, Slice<u32> array_scratch,
                                                Slice<u32> object_scratch, u32 maximum_depth,
                                                PayloadWriter* writer, u32* count,
                                                const JsonIndexedDocument* document = nullptr) noexcept;

struct MemberIdentity {
    u64 user_id, nested_id;
    bool user_id_seen, user_seen, nested_id_seen;
};

bool member_identity(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                     MemberIdentity* identity) noexcept;
GatewayEventDecodeResult append_self_member_roles(JsonValue members, Slice<u32> array_scratch,
                                                  Slice<u32> object_scratch, u32 maximum_depth,
                                                  u64 self_user_id, PayloadWriter* writer,
                                                  u32* count, GatewayClientStateMutation* guild,
                                                  bool* found) noexcept;
bool hydrate_self_member(StateObjectContext* object, u64 self_user_id) noexcept;
GatewayEventDecodeResult append_channel_array(JsonValue array, Slice<u32> array_scratch,
                                              Slice<u32> object_scratch, u32 maximum_depth, u64 guild_id,
                                              PayloadWriter* writer, u32* count,
                                              u32* overwrite_count, u64 self_user_id = 0,
                                              u32* skipped = nullptr) noexcept;

struct ReadyContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    u32 maximum_depth;
    u32 count;
    u32 error_offset;
    GatewayEventDecodeError error;
    JsonScanError scan_error;
    JsonReadError read_error;
};

bool visit_ready_guild(void* opaque, u32, JsonValue value) noexcept;
GatewayClientDispatchRouteResult route_thread_sync(const GatewayEnvelope& envelope, Slice<u32> scratch,
    u32 depth, Slice<char> storage, u64 self, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;

} // namespace state_dispatch_detail
} // namespace maicord
