#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/protocol/json_ids.hpp"

#include "gateway_dispatch_common.hpp"
#include "gateway_payload_writer.hpp"
#include "gateway_member_list_validation.hpp"

namespace maicord {
namespace {

inline constexpr u32 kMemberListMaximumRoles = 256;
inline constexpr u32 kMemberListMaximumRecords = 2048;

static_assert(sizeof(GatewayClientMemberListMutation) == 40);
static_assert(sizeof(GatewayClientMemberListUpdate) == 48);

struct PayloadWriter : GatewayPayloadWriter {
    u32 records;
    u32 members;
    u32 decode_detail;
};

// Categorical diagnostic only: marker | stage | field ordinal | JSON kind.
// No field contents or identity are retained in a skipped-dispatch record.
enum class MemberDecodeStage : u32 {
    item = 1,
    member,
    user,
    presence,
    roles,
    text,
    user_appearance,
    member_appearance,
    activities,
    serialization,
    operation,
    groups,
};
u32 decode_detail(MemberDecodeStage stage, u32 field = 0, u32 kind = 0xFFu) noexcept {
    return 0x4D000000u | (static_cast<u32>(stage) << 16) | (field << 8) | kind;
}

struct RootContext {
    u64 guild_id;
    u64 list_id;
    u32 member_count;
    u32 online_count;
    JsonValue ops;
    JsonValue groups;
    JsonValue member_array;
    GatewayEventDecodeError error;
    bool guild_seen;
    bool list_seen;
    bool members_seen;
    bool online_seen;
    bool ops_seen;
    bool groups_seen;
    bool member_array_seen;
};

struct OpObject {
    JsonString operation;
    JsonValue range;
    JsonValue items;
    JsonValue item;
    u32 index;
    GatewayEventDecodeError error;
    bool operation_seen;
    bool range_seen;
    bool items_seen;
    bool item_seen;
    bool index_seen;
};

struct ItemObject {
    JsonValue member;
    JsonValue group;
    GatewayEventDecodeError error;
    u32 field_detail;
    bool member_seen;
    bool group_seen;
};

struct MemberObject {
    JsonValue user;
    JsonValue roles;
    JsonValue presence;
    JsonString nickname;
    GatewayEventDecodeError error;
    u32 field_detail;
    bool user_seen;
    bool roles_seen;
    bool presence_seen;
    bool nickname_seen;
    bool nickname_null;
    bool timeout_seen;
    u32 timeout_until;
    bool flags_seen;
    u32 flags;
};

using UserObject = GatewayMemberUser;

struct PresenceObject {
    JsonString status;
    GatewayEventDecodeError error;
    u32 field_detail;
    bool status_seen;
    bool client_status_seen;
    u8 client_status;
};

struct U32PairContext {
    u32 values[2];
    u32 count;
    GatewayEventDecodeError error;
};

struct RoleContext {
    u64* roles;
    u32 capacity;
    u32 count;
    GatewayEventDecodeError error;
};

struct ItemArrayContext {
    PayloadWriter* writer;
    const JsonIndexedDocument* document;
    u32 maximum_depth;
    u32 range_first;
    u32 range_last;
    GatewayEventDecodeError error;
};

struct OpArrayContext {
    PayloadWriter* writer;
    const JsonIndexedDocument* document;
    u32 maximum_depth;
    GatewayEventDecodeError error;
};

GatewayClientDispatchRouteResult result(GatewayClientDispatchRouteError route,
                                        GatewayEventDecodeError event = GatewayEventDecodeError::none,
                                        JsonScanError scan = JsonScanError::none,
                                        JsonReadError read = JsonReadError::none, u32 offset = 0) noexcept {
    GatewayClientDispatchRouteResult value{};
    value.error = route;
    value.event_error = event;
    value.scan_error = scan;
    value.read_error = read;
    value.error_offset = offset;
    return value;
}

bool reject(GatewayEventDecodeError* error,
            GatewayEventDecodeError value = GatewayEventDecodeError::invalid_data) noexcept {
    if (error && *error == GatewayEventDecodeError::none) {
        *error = value;
    }
    return false;
}

bool u32_value(JsonValue value, u32* output) noexcept {
    u64 decoded = 0;
    if (!output || !json_value_to_u64(value, &decoded) || decoded > 0xFFFFFFFFull) {
        return false;
    }
    *output = static_cast<u32>(decoded);
    return true;
}

bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RootContext*>(opaque);
    const bool array = value.kind == JsonValueKind::array;
    if (json_name_is(name, utf8_literal("guild_id"))) {
        if (context->guild_seen || !json_value_to_snowflake(value, &context->guild_id))
            return reject(&context->error, gateway_field_error(context->guild_seen));
        context->guild_seen = true;
    } else if (json_name_is(name, utf8_literal("id"))) {
        if (context->list_seen || !json_value_to_member_list_id(value, &context->list_id))
            return reject(&context->error, gateway_field_error(context->list_seen));
        context->list_seen = true;
    } else if (json_name_is(name, utf8_literal("member_count"))) {
        if (context->members_seen || !u32_value(value, &context->member_count))
            return reject(&context->error, gateway_field_error(context->members_seen));
        context->members_seen = true;
    } else if (json_name_is(name, utf8_literal("online_count"))) {
        if (context->online_seen || !u32_value(value, &context->online_count))
            return reject(&context->error, gateway_field_error(context->online_seen));
        context->online_seen = true;
    } else if (json_name_is(name, utf8_literal("groups"))) {
        return gateway_keep_member(&context->error, &context->groups_seen, &context->groups, value, array);
    } else if (json_name_is(name, utf8_literal("members"))) {
        return gateway_keep_member(&context->error, &context->member_array_seen, &context->member_array,
                                   value, array);
    } else if (json_name_is(name, utf8_literal("ops"))) {
        return gateway_keep_member(&context->error, &context->ops_seen, &context->ops, value, array);
    }
    return true;
}

bool visit_op(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<OpObject*>(opaque);
    if (json_name_is(name, utf8_literal("op"))) {
        if (context->operation_seen || !json_value_as_string(value, &context->operation))
            return reject(&context->error, gateway_field_error(context->operation_seen));
        context->operation_seen = true;
    } else if (json_name_is(name, utf8_literal("range"))) {
        return gateway_keep_member(&context->error, &context->range_seen, &context->range, value,
                                   value.kind == JsonValueKind::array);
    } else if (json_name_is(name, utf8_literal("items"))) {
        return gateway_keep_member(&context->error, &context->items_seen, &context->items, value,
                                   value.kind == JsonValueKind::array);
    } else if (json_name_is(name, utf8_literal("item"))) {
        return gateway_keep_member(&context->error, &context->item_seen, &context->item, value,
                                   value.kind == JsonValueKind::object);
    } else if (json_name_is(name, utf8_literal("index"))) {
        if (context->index_seen || !u32_value(value, &context->index))
            return reject(&context->error, gateway_field_error(context->index_seen));
        context->index_seen = true;
    }
    return true;
}

bool visit_item(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ItemObject*>(opaque);
    const bool object_or_null =
        value.kind == JsonValueKind::object || value.kind == JsonValueKind::null_value;
    if (json_name_is(name, utf8_literal("member"))) {
        context->field_detail = (1u << 8) | static_cast<u32>(value.kind);
        return gateway_keep_member(&context->error, &context->member_seen, &context->member, value,
                                   object_or_null);
    }
    if (json_name_is(name, utf8_literal("group"))) {
        context->field_detail = (2u << 8) | static_cast<u32>(value.kind);
        return gateway_keep_member(&context->error, &context->group_seen, &context->group, value,
                                   object_or_null);
    }
    return true;
}

bool visit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<MemberObject*>(opaque);
    if (json_name_is(name, utf8_literal("user"))) {
        context->field_detail = (1u << 8) | static_cast<u32>(value.kind);
        return gateway_keep_member(&context->error, &context->user_seen, &context->user, value,
                                   value.kind == JsonValueKind::object);
    }
    if (json_name_is(name, utf8_literal("roles"))) {
        context->field_detail = (2u << 8) | static_cast<u32>(value.kind);
        return gateway_keep_member(&context->error, &context->roles_seen, &context->roles, value,
                                   value.kind == JsonValueKind::array);
    }
    if (json_name_is(name, utf8_literal("presence"))) {
        context->field_detail = (3u << 8) | static_cast<u32>(value.kind);
        return gateway_keep_member(
            &context->error, &context->presence_seen, &context->presence, value,
            value.kind == JsonValueKind::object || value.kind == JsonValueKind::null_value);
    }
    if (json_name_is(name, utf8_literal("nick"))) {
        context->field_detail = (4u << 8) | static_cast<u32>(value.kind);
        if (context->nickname_seen) {
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        }
        context->nickname_seen = true;
        context->nickname_null = value.kind == JsonValueKind::null_value;
        if (!context->nickname_null && !json_value_as_string(value, &context->nickname)) {
            return reject(&context->error);
        }
    } else if (json_name_is(name, utf8_literal("flags"))) {
        context->field_detail = (6u << 8) | static_cast<u32>(value.kind);
        if (context->flags_seen)
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        context->flags_seen = true;
        if (!u32_value(value, &context->flags))
            return reject(&context->error);
    } else if (json_name_is(name, utf8_literal("communication_disabled_until"))) {
        context->field_detail = (5u << 8) | static_cast<u32>(value.kind);
        if (context->timeout_seen)
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        context->timeout_seen = true;
        if (value.kind != JsonValueKind::null_value) {
            JsonString stamp{};
            u64 milliseconds = 0;
            if (!json_value_as_string(value, &stamp) || !parse_iso_timestamp(stamp, &milliseconds) ||
                milliseconds / 1000 > 0xFFFFFFFFull)
                return reject(&context->error);
            context->timeout_until = static_cast<u32>(milliseconds / 1000);
        }
    }
    return true;
}

bool visit_presence(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<PresenceObject*>(opaque);
    if (json_name_is(name, utf8_literal("status"))) {
        context->field_detail = (1u << 8) | static_cast<u32>(value.kind);
        if (context->status_seen || !json_value_as_string(value, &context->status))
            return reject(&context->error, gateway_field_error(context->status_seen));
        context->status_seen = true;
    } else if (json_name_is(name, utf8_literal("client_status"))) {
        if (context->client_status_seen)
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        context->client_status_seen = true;
        // Advisory per-platform detail: a malformed object reads as offline.
        if (!client_status_decode(value, &context->client_status))
            context->client_status = 0;
    }
    return true;
}

bool visit_u32_pair(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<U32PairContext*>(opaque);
    if (index >= 2 || !u32_value(value, &context->values[index])) {
        return reject(&context->error);
    }
    context->count = index + 1;
    return true;
}

bool visit_role(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<RoleContext*>(opaque);
    u64 role = 0;
    if (index >= context->capacity || !json_value_to_snowflake(value, &role)) {
        return reject(&context->error, index >= context->capacity
                                           ? GatewayEventDecodeError::mention_role_capacity
                                           : GatewayEventDecodeError::invalid_data);
    }
    for (u32 previous = 0; previous < index; ++previous) {
        if (context->roles[previous] == role) {
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        }
    }
    context->roles[index] = role;
    context->count = index + 1;
    return true;
}

bool visit_object(JsonValue value, const JsonIndexedDocument& document, u32 maximum_depth,
                  JsonMemberVisitor visitor, void* context, GatewayEventDecodeError* error) noexcept {
    const JsonReadResult read = json_visit_indexed_object(document, value, maximum_depth, visitor, context);
    if (!read.ok()) {
        return reject(error, GatewayEventDecodeError::invalid_json);
    }
    return true;
}

bool append_simple(PayloadWriter* writer, GatewayClientMemberListOperation operation, u32 first,
                   u32 last) noexcept {
    if (!writer || writer->records == kMemberListMaximumRecords) {
        return false;
    }
    GatewayClientMemberListMutation mutation{};
    mutation.index = first;
    mutation.range_end = last;
    mutation.operation = operation;
    mutation.status = GatewayClientMemberListStatus::unknown;
    if (!gateway_payload_append(writer, &mutation, sizeof(mutation))) {
        return false;
    }
    ++writer->records;
    return true;
}

struct GroupObject {
    u64 id;
    u32 count;
    bool id_seen;
    bool count_seen;
    GatewayEventDecodeError error;
    u32 field_detail;
};
bool visit_group(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* group = static_cast<GroupObject*>(opaque);
    if (json_name_is(name, utf8_literal("id"))) {
        group->field_detail = (3u << 8) | static_cast<u32>(value.kind);
        if (group->id_seen)
            return reject(&group->error, GatewayEventDecodeError::duplicate_field);
        JsonString text{};
        if (json_value_as_string(value, &text) && json_name_is(text, utf8_literal("online")))
            group->id = 0;
        else if (json_value_as_string(value, &text) &&
                 json_name_is(text, utf8_literal("offline")))
            group->id = ~0ull;
        else if (!json_value_to_snowflake(value, &group->id))
            return reject(&group->error);
        group->id_seen = true;
    } else if (json_name_is(name, utf8_literal("count"))) {
        group->field_detail = (4u << 8) | static_cast<u32>(value.kind);
        if (group->count_seen)
            return reject(&group->error, GatewayEventDecodeError::duplicate_field);
        if (!u32_value(value, &group->count))
            return reject(&group->error);
        group->count_seen = true;
    }
    return true;
}
bool append_group(PayloadWriter* writer, JsonValue value, u32 index,
                  GatewayClientMemberListOperation operation, GatewayClientMemberListItemKind kind,
                  const JsonIndexedDocument& document, u32 maximum_depth, GatewayEventDecodeError* error,
                  u32* count = nullptr) noexcept {
    GroupObject group{};
    if (value.kind != JsonValueKind::object ||
        !visit_object(value, document, maximum_depth, visit_group, &group, &group.error) ||
        !group.id_seen || (kind == GatewayClientMemberListItemKind::summary && !group.count_seen)) {
        const u32 field = group.error != GatewayEventDecodeError::none
                              ? group.field_detail
                              : (((!group.id_seen ? 3u : 4u) << 8) | 0xFFu);
        writer->decode_detail =
            (writer->decode_detail & 0xFFFF0000u) |
            (value.kind == JsonValueKind::object ? field : static_cast<u32>(value.kind));
        return reject(error, group.error == GatewayEventDecodeError::none
                                 ? GatewayEventDecodeError::invalid_data
                                 : group.error);
    }
    if (group.count == ~0u) {
        writer->decode_detail =
            (writer->decode_detail & 0xFFFF0000u) | (4u << 8) | static_cast<u32>(JsonValueKind::number);
        return reject(error);
    }
    if (writer->records >= kMemberListMaximumRecords)
        return reject(error, GatewayEventDecodeError::message_content_capacity);
    GatewayClientMemberListMutation mutation{};
    mutation.user_id = group.id;
    mutation.index = index;
    mutation.range_end = group.count;
    mutation.operation = operation;
    mutation.kind = kind;
    if (kind == GatewayClientMemberListItemKind::group && !group.count_seen)
        mutation.hydration_flags = gateway_member_group_count_omitted;
    if (!gateway_payload_append(writer, &mutation, sizeof(mutation)))
        return reject(error, GatewayEventDecodeError::message_content_capacity);
    ++writer->records;
    if (count)
        *count = group.count;
    return true;
}
struct GroupArrayContext {
    PayloadWriter* writer;
    const JsonIndexedDocument* document;
    u32 maximum_depth;
    u32 logical_count;
    u32 group_count;
    GatewayEventDecodeError error;
};
bool visit_summary_group(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<GroupArrayContext*>(opaque);
    u32 count = 0;
    if (!append_group(context->writer, value, context->logical_count,
                      GatewayClientMemberListOperation::group_summary,
                      GatewayClientMemberListItemKind::summary, *context->document, context->maximum_depth,
                      &context->error, &count))
        return false;
    if (count == ~0u || context->logical_count > ~0u - count - 1)
        return reject(&context->error);
    context->logical_count += count + 1;
    ++context->group_count;
    return true;
}

bool append_member_item(PayloadWriter* writer, JsonValue value, u32 list_index,
                        GatewayClientMemberListOperation operation, const JsonIndexedDocument& document,
                        u32 maximum_depth, GatewayEventDecodeError* error, bool direct = false,
                        bool full_roles = false) noexcept {
    writer->decode_detail = decode_detail(MemberDecodeStage::item, 0, static_cast<u32>(value.kind));
    if (list_index == ~0u)
        return reject(error);
    ItemObject item{};
    if (direct) {
        item.member = value;
        item.member_seen = value.kind == JsonValueKind::object;
    } else if (!visit_object(value, document, maximum_depth, visit_item, &item, &item.error)) {
        writer->decode_detail = decode_detail(MemberDecodeStage::item, 0, 0) | item.field_detail;
        return reject(error, item.error);
    }
    const bool has_member = item.member_seen && item.member.kind == JsonValueKind::object;
    const bool has_group = item.group_seen && item.group.kind == JsonValueKind::object;
    if (has_group) {
        if (has_member) {
            writer->decode_detail = decode_detail(MemberDecodeStage::item, 5, 0);
            return reject(error);
        }
        return append_group(writer, item.group, list_index, operation,
                            GatewayClientMemberListItemKind::group, document, maximum_depth, error);
    }
    if (!has_member) {
        writer->decode_detail = decode_detail(MemberDecodeStage::item, 6);
        return reject(error, GatewayEventDecodeError::missing_required_field);
    }
    MemberObject member{};
    writer->decode_detail = decode_detail(MemberDecodeStage::member, 0, 0);
    if (!visit_object(item.member, document, maximum_depth, visit_member, &member, &member.error) ||
        !member.user_seen || (full_roles && !member.roles_seen)) {
        writer->decode_detail |=
            member.error != GatewayEventDecodeError::none ? member.field_detail : (1u << 8) | 0xFFu;
        return reject(error, member.error != GatewayEventDecodeError::none
                                 ? member.error
                                 : GatewayEventDecodeError::missing_required_field);
    }
    UserObject user{};
    writer->decode_detail = decode_detail(MemberDecodeStage::user, 0, 0);
    if (!visit_object(member.user, document, maximum_depth, gateway_visit_member_user, &user, &user.error) ||
        !user.id_seen || !user.username_seen) {
        writer->decode_detail |= user.error != GatewayEventDecodeError::none
                                     ? user.field_detail
                                     : ((!user.id_seen ? 1u : 2u) << 8) | 0xFFu;
        return reject(error, user.error != GatewayEventDecodeError::none
                                 ? user.error
                                 : GatewayEventDecodeError::missing_required_field);
    }
    PresenceObject presence{};
    // A member profile is complete without a presence. Null, like an omitted
    // field, must not discard its identity or clear an existing status/activity.
    const bool has_presence = member.presence_seen && member.presence.kind == JsonValueKind::object;
    writer->decode_detail = decode_detail(MemberDecodeStage::presence, 0, 0);
    if (has_presence && !visit_object(member.presence, document, maximum_depth, visit_presence, &presence,
                                      &presence.error)) {
        writer->decode_detail |= presence.field_detail;
        return reject(error, presence.error);
    }
    u64 roles[kMemberListMaximumRoles]{};
    RoleContext role_context{roles, kMemberListMaximumRoles, 0, GatewayEventDecodeError::none};
    writer->decode_detail = decode_detail(MemberDecodeStage::roles);
    if (member.roles_seen) {
        const JsonReadResult read =
            json_visit_indexed_array(document, member.roles, maximum_depth, visit_role, &role_context);
        if (!read.ok()) {
            return reject(error, role_context.error != GatewayEventDecodeError::none
                                     ? role_context.error
                                     : GatewayEventDecodeError::invalid_json);
        }
    }
    JsonString display = user.username;
    if (user.global_name_seen && !user.global_name_null) {
        display = user.global_name;
    } else if (user.display_name_seen && !user.display_name_null) {
        display = user.display_name;
    }
    u16 nickname_bytes = 0;
    u16 display_bytes = 0;
    u16 avatar_hash_bytes = 0;
    u16 username_bytes = 0;
    writer->decode_detail = decode_detail(MemberDecodeStage::text);
    if ((!member.nickname_null && member.nickname_seen &&
         !gateway_member_string_size(member.nickname, &nickname_bytes)) ||
        !gateway_member_string_size(display, &display_bytes) || display_bytes == 0 ||
        !gateway_member_string_size(user.username, &username_bytes) || username_bytes > 128 ||
        (user.avatar_seen && !user.avatar_null &&
         !gateway_member_string_size(user.avatar_hash, &avatar_hash_bytes)) ||
        avatar_hash_bytes > kUserAvatarHashMaximumBytes ||
        writer->records == kMemberListMaximumRecords) {
        return reject(error);
    }
    UserAppearance global_appearance{}, guild_appearance{};
    writer->decode_detail = decode_detail(MemberDecodeStage::user_appearance);
    if (!user_appearance_decode(document, member.user, user.id, 0, &global_appearance))
        return reject(error);
    writer->decode_detail = decode_detail(MemberDecodeStage::member_appearance);
    if (!user_appearance_decode(document, item.member, user.id, 1, &guild_appearance))
        return reject(error);
    writer->decode_detail = decode_detail(MemberDecodeStage::activities);
    if (has_presence && !activity_decode(document, member.presence, &guild_appearance))
        return reject(error);
    writer->decode_detail = decode_detail(MemberDecodeStage::serialization);
    u8 user_extra[1024]{}, member_extra[1024 + kUserActivityRecordMaximumBytes]{};
    const u32 user_extra_bytes =
        user_appearance_write(global_appearance, {user_extra, sizeof(user_extra)});
    u32 member_extra_bytes =
        user_appearance_write(guild_appearance, {member_extra, 1024});
    if ((global_appearance.fields && !user_extra_bytes) || (guild_appearance.fields && !member_extra_bytes))
        return reject(error);
    // Rich presence rides behind the member appearance while the payload keeps
    // room for the rest of the list; without the flag the activities are unknown.
    bool activities_known = false;
    if (has_presence && (guild_appearance.fields & appearance_activity)) {
        UserActivitySet activities{};
        bool listed = false;
        if (user_activities_decode(document, member.presence, user.id, &activities, &listed)) {
            const u32 tail = activities.count ? user_activities_write(activities, {member_extra + member_extra_bytes,
                                                                                   kUserActivityRecordMaximumBytes})
                                              : 0;
            // A quarter of the payload stays free so rich presence never
            // crowds out the rest of a list update.
            const u32 room = static_cast<u32>(writer->output.count) - writer->used;
            activities_known = !activities.count ||
                               (tail && room > member_extra_bytes + tail + static_cast<u32>(writer->output.count) / 4);
            if (activities_known)
                member_extra_bytes += tail;
        }
    }
    GatewayClientMemberListMutation mutation{};
    mutation.user_id = user.id;
    mutation.user_appearance_bytes = static_cast<u16>(user_extra_bytes);
    mutation.member_appearance_bytes = static_cast<u16>(member_extra_bytes);
    mutation.index = list_index;
    mutation.role_count = static_cast<u16>(role_context.count);
    if (!member.roles_seen)
        mutation.hydration_flags = gateway_member_roles_omitted;
    if (direct && !full_roles) {
        if (!member.nickname_seen)
            mutation.hydration_flags |= gateway_member_nickname_omitted;
        if (!user.avatar_seen)
            mutation.hydration_flags |= gateway_member_avatar_omitted;
    }
    if (activities_known)
        mutation.hydration_flags |= gateway_member_activities_known;
    if (member.timeout_seen) {
        mutation.hydration_flags |= gateway_member_timeout_known;
        mutation.communication_disabled_until = member.timeout_until;
    }
    if (member.flags_seen) {
        mutation.hydration_flags |= gateway_member_quarantine_known;
        if (guild_member_flags_quarantined(member.flags))
            mutation.hydration_flags |= gateway_member_quarantined;
    }
    mutation.nickname_bytes = nickname_bytes;
    mutation.display_name_bytes = display_bytes;
    mutation.avatar_hash_bytes = avatar_hash_bytes;
    mutation.username_bytes = username_bytes;
    mutation.operation = operation;
    mutation.status = gateway_member_status(presence.status_seen, presence.status);
    mutation.client_status = presence.client_status;
    if (!gateway_payload_append(writer, &mutation, sizeof(mutation))) {
        return reject(error, GatewayEventDecodeError::message_content_capacity);
    }
    for (u32 index = 0; index < role_context.count; ++index) {
        if (!gateway_payload_append_u64(writer, roles[index])) {
            return reject(error, GatewayEventDecodeError::message_content_capacity);
        }
    }
    if ((nickname_bytes != 0 && !gateway_append_json_string(writer, member.nickname, nickname_bytes)) ||
        !gateway_append_json_string(writer, display, display_bytes) ||
        (avatar_hash_bytes != 0 && !gateway_append_json_string(writer, user.avatar_hash, avatar_hash_bytes)) ||
        !gateway_append_json_string(writer, user.username, username_bytes) ||
        !gateway_payload_append(writer, user_extra, user_extra_bytes) ||
        !gateway_payload_append(writer, member_extra, member_extra_bytes)) {
        return reject(error, GatewayEventDecodeError::message_content_capacity);
    }
    ++writer->records;
    ++writer->members;
    return true;
}

bool visit_sync_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<ItemArrayContext*>(opaque);
    if (context->range_first > ~0u - index || index > context->range_last - context->range_first ||
        !append_member_item(context->writer, value, context->range_first + index,
                            GatewayClientMemberListOperation::sync, *context->document,
                            context->maximum_depth, &context->error)) {
        return false;
    }
    return true;
}

bool visit_chunk_member(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<ItemArrayContext*>(opaque);
    return index < kGatewayGuildMemberRequestMaximum &&
           append_member_item(context->writer, value, index, GatewayClientMemberListOperation::sync,
                              *context->document, context->maximum_depth, &context->error, true, true);
}

bool parse_range(JsonValue value, const JsonIndexedDocument& document, u32 maximum_depth, u32* first,
                 u32* last, GatewayEventDecodeError* error) noexcept {
    U32PairContext range{};
    const JsonReadResult read =
        json_visit_indexed_array(document, value, maximum_depth, visit_u32_pair, &range);
    if (!read.ok() || range.count != 2 || range.values[1] < range.values[0]) {
        return reject(error, range.error != GatewayEventDecodeError::none
                                 ? range.error
                                 : GatewayEventDecodeError::invalid_data);
    }
    *first = range.values[0];
    *last = range.values[1];
    return true;
}

bool visit_operation(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<OpArrayContext*>(opaque);
    context->writer->decode_detail = decode_detail(MemberDecodeStage::operation);
    if (value.kind != JsonValueKind::object) {
        return reject(&context->error);
    }
    OpObject operation{};
    if (!visit_object(value, *context->document, context->maximum_depth, visit_op, &operation,
                      &operation.error) ||
        !operation.operation_seen) {
        return reject(&context->error, operation.error != GatewayEventDecodeError::none
                                           ? operation.error
                                           : GatewayEventDecodeError::missing_required_field);
    }
    if (json_name_is(operation.operation, utf8_literal("SYNC"))) {
        u32 first = 0;
        u32 last = 0;
        if (!operation.range_seen || !operation.items_seen ||
            !parse_range(operation.range, *context->document, context->maximum_depth, &first, &last,
                         &context->error) ||
            !append_simple(context->writer, GatewayClientMemberListOperation::invalidate, first, last)) {
            return reject(&context->error);
        }
        ItemArrayContext items{context->writer,
                               context->document,
                               context->maximum_depth,
                               first,
                               last,
                               GatewayEventDecodeError::none};
        const JsonReadResult read = json_visit_indexed_array(
            *context->document, operation.items, context->maximum_depth, visit_sync_item, &items);
        if (!read.ok()) {
            return reject(&context->error, items.error != GatewayEventDecodeError::none
                                               ? items.error
                                               : GatewayEventDecodeError::invalid_json);
        }
        return true;
    }
    if (json_name_is(operation.operation, utf8_literal("INVALIDATE"))) {
        u32 first = 0;
        u32 last = 0;
        return operation.range_seen &&
               parse_range(operation.range, *context->document, context->maximum_depth, &first, &last,
                           &context->error) &&
               append_simple(context->writer, GatewayClientMemberListOperation::invalidate, first, last);
    }
    if (json_name_is(operation.operation, utf8_literal("DELETE"))) {
        return operation.index_seen &&
               append_simple(context->writer, GatewayClientMemberListOperation::remove, operation.index,
                             operation.index);
    }
    const GatewayClientMemberListOperation kind =
        json_name_is(operation.operation, utf8_literal("INSERT"))
            ? GatewayClientMemberListOperation::insert
            : GatewayClientMemberListOperation::update;
    if ((!json_name_is(operation.operation, utf8_literal("INSERT")) &&
         !json_name_is(operation.operation, utf8_literal("UPDATE"))) ||
        !operation.index_seen || !operation.item_seen) {
        return reject(&context->error);
    }
    return append_member_item(context->writer, operation.item, operation.index, kind, *context->document,
                              context->maximum_depth, &context->error);
}

template <typename Value> bool read_packed(ConstSlice<u8> payload, u32* cursor, Value* output) noexcept {
    if (!cursor || !output || *cursor > payload.count || sizeof(Value) > payload.count - *cursor) {
        return false;
    }
    copy_bytes(output, payload.data + *cursor, static_cast<u32>(sizeof(Value)));
    *cursor += static_cast<u32>(sizeof(Value));
    return true;
}

} // namespace

bool gateway_visit_member_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<GatewayMemberUser*>(opaque);
    if (json_name_is(name, utf8_literal("id"))) {
        context->field_detail = (1u << 8) | static_cast<u32>(value.kind);
        if (context->id_seen || !json_value_to_snowflake(value, &context->id))
            return reject(&context->error, gateway_field_error(context->id_seen));
        context->id_seen = true;
    } else if (json_name_is(name, utf8_literal("username"))) {
        context->field_detail = (2u << 8) | static_cast<u32>(value.kind);
        if (context->username_seen || !json_value_as_string(value, &context->username))
            return reject(&context->error, gateway_field_error(context->username_seen));
        context->username_seen = true;
    } else if (json_name_is(name, utf8_literal("global_name")) ||
               json_name_is(name, utf8_literal("display_name"))) {
        const bool global = json_name_is(name, utf8_literal("global_name"));
        context->field_detail = ((global ? 3u : 4u) << 8) | static_cast<u32>(value.kind);
        bool& seen = global ? context->global_name_seen : context->display_name_seen;
        bool& null_value = global ? context->global_name_null : context->display_name_null;
        JsonString& destination = global ? context->global_name : context->display_name;
        if (seen) {
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        }
        seen = true;
        null_value = value.kind == JsonValueKind::null_value;
        if (!null_value && !json_value_as_string(value, &destination)) {
            return reject(&context->error);
        }
    } else if (json_name_is(name, utf8_literal("avatar"))) {
        context->field_detail = (5u << 8) | static_cast<u32>(value.kind);
        if (context->avatar_seen) {
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        }
        context->avatar_seen = true;
        context->avatar_null = value.kind == JsonValueKind::null_value;
        if (!context->avatar_null && !json_value_as_string(value, &context->avatar_hash)) {
            return reject(&context->error);
        }
    }
    return true;
}

bool gateway_append_json_string(GatewayPayloadWriter* writer, JsonString string, u16 expected) noexcept {
    if (!writer || writer->used > writer->output.count || expected > writer->output.count - writer->used) {
        return false;
    }
    if (expected == 0) {
        return true;
    }
    const JsonStringDecodeResult decoded = json_string_decode_utf8(
        string, Slice<char>{writer->output.data + writer->used, writer->output.count - writer->used});
    if (!decoded.ok() || decoded.bytes != expected) {
        return false;
    }
    writer->used += decoded.bytes;
    return true;
}

bool gateway_member_string_size(JsonString value, u16* bytes) noexcept {
    if (!bytes) {
        return false;
    }
    const JsonStringDecodeResult sized = json_string_decode_utf8(value, Slice<char>{nullptr, 0});
    if ((sized.error != JsonStringDecodeError::output_too_small && !sized.ok()) || sized.bytes > 128u ||
        sized.bytes > 0xFFFFu) {
        return false;
    }
    *bytes = static_cast<u16>(sized.bytes);
    return true;
}

GatewayClientMemberListStatus gateway_member_status(bool seen, JsonString status) noexcept {
    if (!seen) {
        return GatewayClientMemberListStatus::unknown;
    }
    if (json_name_is(status, utf8_literal("online"))) {
        return GatewayClientMemberListStatus::online;
    }
    if (json_name_is(status, utf8_literal("idle"))) {
        return GatewayClientMemberListStatus::idle;
    }
    if (json_name_is(status, utf8_literal("dnd"))) {
        return GatewayClientMemberListStatus::dnd;
    }
    if (json_name_is(status, utf8_literal("offline")) ||
        json_name_is(status, utf8_literal("invisible"))) {
        return GatewayClientMemberListStatus::offline;
    }
    return GatewayClientMemberListStatus::unknown;
}

GatewayClientDispatchRouteResult gateway_client_route_member_list_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept {
    if (!event || !payload || !handled || !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0 || !payload_scratch.data || payload_scratch.count == 0) {
        return result(GatewayClientDispatchRouteError::invalid_input);
    }
    const bool chunk = json_name_is(envelope.event_name, utf8_literal("GUILD_MEMBERS_CHUNK"));
    // A joined member is hydrated exactly like a member update.
    const bool member_update =
        json_name_is(envelope.event_name, utf8_literal("GUILD_MEMBER_UPDATE")) ||
        json_name_is(envelope.event_name, utf8_literal("GUILD_MEMBER_ADD"));
    *handled = chunk || member_update ||
               json_name_is(envelope.event_name, utf8_literal("GUILD_MEMBER_LIST_UPDATE"));
    if (!*handled) {
        return result(GatewayClientDispatchRouteError::none);
    }
    const JsonScanResult root_scan = json_scan_structurals_avx1(envelope.data.source, structural_scratch);
    if (!root_scan.ok()) {
        return result(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed,
                      root_scan.error, JsonReadError::none, root_scan.error_offset);
    }
    RootContext root{};
    const JsonReadResult root_read = json_visit_value_object(
        envelope.data, ConstSlice<u32>{structural_scratch.data, root_scan.count}, maximum_depth,
        visit_root, &root);
    if (!root_read.ok() || !root.guild_seen ||
        (chunk ? !root.member_array_seen : !member_update && !root.ops_seen)) {
        return result(GatewayClientDispatchRouteError::decode,
                      root.error != GatewayEventDecodeError::none ? root.error
                      : !root.guild_seen || !root.ops_seen ? GatewayEventDecodeError::missing_required_field
                                                           : GatewayEventDecodeError::invalid_json,
                      JsonScanError::none, root_read.error, root_read.error_offset);
    }
    const JsonIndexedDocument document{envelope.data.source, {structural_scratch.data, root_scan.count}};
    PayloadWriter writer{{payload_scratch, 0}, 0, 0, 0};
    if (chunk || member_update) {
        ItemArrayContext members{&writer, &document, maximum_depth, 0, 0, GatewayEventDecodeError::none};
        bool decoded = false;
        if (chunk)
            decoded = json_visit_indexed_array(document, root.member_array, maximum_depth,
                                               visit_chunk_member, &members)
                          .ok();
        else
            decoded = append_member_item(&writer, envelope.data, 0, GatewayClientMemberListOperation::sync,
                                         document, maximum_depth, &members.error, true);
        if (!decoded)
            return result(GatewayClientDispatchRouteError::decode,
                          members.error == GatewayEventDecodeError::none
                              ? GatewayEventDecodeError::invalid_data
                              : members.error);
        event->dispatch_kind = chunk ? GatewayClientDispatchKind::guild_members_chunk
                                     : GatewayClientDispatchKind::guild_member_profile_update;
        reset_value(&event->member_list);
        event->member_list.guild_id = root.guild_id;
        event->member_list.record_count = writer.records;
        event->member_list.member_record_count = writer.members;
        event->member_list.member_added =
            !chunk && json_name_is(envelope.event_name, utf8_literal("GUILD_MEMBER_ADD"));
        *payload = {reinterpret_cast<const u8*>(payload_scratch.data), writer.used};
        return {};
    }
    OpArrayContext operations{&writer, &document, maximum_depth, GatewayEventDecodeError::none};
    const JsonReadResult op_read =
        json_visit_indexed_array(document, root.ops, maximum_depth, visit_operation, &operations);
    if (!op_read.ok()) {
        const GatewayEventDecodeError event_error = operations.error != GatewayEventDecodeError::none
                                                        ? operations.error
                                                        : GatewayEventDecodeError::invalid_json;
        auto failed = result(event_error == GatewayEventDecodeError::message_content_capacity
                                 ? GatewayClientDispatchRouteError::payload_capacity
                                 : GatewayClientDispatchRouteError::decode,
                             event_error, JsonScanError::none, op_read.error, op_read.error_offset);
        failed.detail = writer.decode_detail;
        return failed;
    }
    GroupArrayContext groups{};
    groups.writer = &writer;
    groups.document = &document;
    groups.maximum_depth = maximum_depth;
    if (root.groups_seen) {
        writer.decode_detail = decode_detail(MemberDecodeStage::groups);
        const auto group_read =
            json_visit_indexed_array(document, root.groups, maximum_depth, visit_summary_group, &groups);
        if (!group_read.ok()) {
            auto failed =
                result(groups.error == GatewayEventDecodeError::message_content_capacity
                           ? GatewayClientDispatchRouteError::payload_capacity
                           : GatewayClientDispatchRouteError::decode,
                       groups.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json
                                                                     : groups.error,
                       JsonScanError::none, group_read.error, group_read.error_offset);
            failed.detail = writer.decode_detail;
            return failed;
        }
    }
    const ConstSlice<u8> packed{reinterpret_cast<const u8*>(payload_scratch.data), writer.used};
    if (!gateway_member_list_summary_ids_unique(packed, groups.group_count))
        return result(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::duplicate_field);
    event->dispatch_kind = GatewayClientDispatchKind::guild_member_list_update;
    event->member_list =
        GatewayClientMemberListUpdate{root.guild_id,        root.list_seen ? root.list_id : 0,
                                      root.member_count,    root.online_count,
                                      writer.records,       writer.members,
                                      groups.logical_count, groups.group_count,
                                      root.members_seen,    root.online_seen,
                                      root.groups_seen};
    *payload = packed;
    return result(GatewayClientDispatchRouteError::none);
}

bool gateway_client_event_member_list_record(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             u32* cursor, u32* index,
                                             GatewayClientMemberListMutation* mutation,
                                             ConstSlice<u8>* role_payload, Utf8View* nickname,
                                             Utf8View* display_name, Utf8View* avatar_hash,
                                             Utf8View* username, ConstSlice<u8>* user_appearance,
                                             ConstSlice<u8>* member_appearance) noexcept {
    if (!cursor || !index || !mutation || !role_payload || !nickname || !display_name || !avatar_hash ||
        event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_member_list_update &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_members_chunk &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_member_profile_update) ||
        event.dispatch_payload_bytes != payload.count || *index >= event.member_list.record_count ||
        *cursor > payload.count || !read_packed(payload, cursor, mutation) ||
        mutation->operation > GatewayClientMemberListOperation::group_summary ||
        mutation->status > GatewayClientMemberListStatus::offline ||
        mutation->role_count > kMemberListMaximumRoles) {
        return false;
    }
    const bool positional = mutation->operation == GatewayClientMemberListOperation::sync ||
                            mutation->operation == GatewayClientMemberListOperation::update ||
                            mutation->operation == GatewayClientMemberListOperation::insert;
    const bool member = positional && mutation->kind == GatewayClientMemberListItemKind::member;
    const bool group = positional && mutation->kind == GatewayClientMemberListItemKind::group;
    const bool summary = mutation->operation == GatewayClientMemberListOperation::group_summary &&
                         mutation->kind == GatewayClientMemberListItemKind::summary;
    // Presence markers are specific to ordinary member/group records; thread
    // flags and contradictory omitted-but-populated role payloads fail closed.
    const u8 member_flags =
        static_cast<u8>(gateway_member_roles_omitted | gateway_member_timeout_known | gateway_member_activities_known |
                        gateway_member_quarantine_known | gateway_member_quarantined |
                        (event.dispatch_kind == GatewayClientDispatchKind::guild_member_profile_update
                             ? gateway_member_nickname_omitted | gateway_member_avatar_omitted
                             : 0));
    if (group ? ((mutation->hydration_flags & ~gateway_member_group_count_omitted) != 0 ||
                 ((mutation->hydration_flags & gateway_member_group_count_omitted) && mutation->range_end))
        : member ? ((mutation->hydration_flags & ~member_flags) != 0 ||
                    ((mutation->hydration_flags & gateway_member_roles_omitted) && mutation->role_count))
                 : mutation->hydration_flags != 0)
        return false;
    if (mutation->communication_disabled_until &&
        (!member || !(mutation->hydration_flags & gateway_member_timeout_known)))
        return false;
    if ((mutation->hydration_flags & gateway_member_quarantined) &&
        !(mutation->hydration_flags & gateway_member_quarantine_known))
        return false;
    if (((mutation->hydration_flags & gateway_member_nickname_omitted) && mutation->nickname_bytes) ||
        ((mutation->hydration_flags & gateway_member_avatar_omitted) && mutation->avatar_hash_bytes))
        return false;
    if (mutation->index == ~0u || ((group || summary) && mutation->range_end == ~0u) ||
        ((!member) && mutation->status != GatewayClientMemberListStatus::unknown) ||
        (member && mutation->range_end != 0) ||
        (mutation->operation == GatewayClientMemberListOperation::remove &&
         mutation->range_end != mutation->index))
        return false;
    if ((!positional && !summary && mutation->kind != GatewayClientMemberListItemKind::member) ||
        (positional && !member && !group) ||
        (mutation->operation == GatewayClientMemberListOperation::group_summary && !summary) ||
        ((positional || summary) && mutation->index == ~0u))
        return false;
    const u32 role_bytes = static_cast<u32>(mutation->role_count) * 8u;
    const u32 text_bytes = static_cast<u32>(mutation->nickname_bytes) + mutation->display_name_bytes +
                           mutation->avatar_hash_bytes + mutation->username_bytes +
                           mutation->user_appearance_bytes + mutation->member_appearance_bytes;
    if ((member && (mutation->user_id == 0 || mutation->display_name_bytes == 0)) ||
        mutation->nickname_bytes > 128 || mutation->display_name_bytes > 128 ||
        mutation->avatar_hash_bytes > kUserAvatarHashMaximumBytes ||
        mutation->username_bytes > 128 ||
        (!member && ((!group && !summary && mutation->user_id != 0) || mutation->role_count != 0 ||
                     text_bytes != 0)) ||
        (mutation->operation == GatewayClientMemberListOperation::invalidate &&
         mutation->range_end < mutation->index) ||
        *cursor > payload.count || role_bytes > payload.count - *cursor ||
        text_bytes > payload.count - *cursor - role_bytes) {
        return false;
    }
    role_payload->data = payload.data + *cursor;
    role_payload->count = role_bytes;
    *cursor += role_bytes;
    nickname->data = reinterpret_cast<const char*>(payload.data + *cursor);
    nickname->bytes = mutation->nickname_bytes;
    *cursor += mutation->nickname_bytes;
    display_name->data = reinterpret_cast<const char*>(payload.data + *cursor);
    display_name->bytes = mutation->display_name_bytes;
    *cursor += mutation->display_name_bytes;
    avatar_hash->data = reinterpret_cast<const char*>(payload.data + *cursor);
    avatar_hash->bytes = mutation->avatar_hash_bytes;
    *cursor += mutation->avatar_hash_bytes;
    const Utf8View login_name{reinterpret_cast<const char*>(payload.data + *cursor),
                              mutation->username_bytes};
    *cursor += mutation->username_bytes;
    if (username)
        *username = login_name;
    const ConstSlice<u8> global_extra{payload.data + *cursor, mutation->user_appearance_bytes};
    *cursor += mutation->user_appearance_bytes;
    const ConstSlice<u8> guild_extra{payload.data + *cursor, mutation->member_appearance_bytes};
    *cursor += mutation->member_appearance_bytes;
    UserAppearance decoded{};
    u32 appearance_bytes = 0;
    UserActivitySet activities{};
    if (member &&
        (!user_appearance_read(global_extra, mutation->user_id, 0, &decoded) ||
         !user_appearance_read_prefix(guild_extra, mutation->user_id, event.member_list.guild_id, &decoded,
                                      &appearance_bytes) ||
         (appearance_bytes < guild_extra.count &&
          (!(mutation->hydration_flags & gateway_member_activities_known) ||
           !user_activities_read({guild_extra.data + appearance_bytes, guild_extra.count - appearance_bytes},
                                 mutation->user_id, &activities)))))
        return false;
    if (user_appearance)
        *user_appearance = global_extra;
    if (member_appearance)
        *member_appearance = guild_extra;
    if (!utf8_validate(*nickname).valid || !utf8_validate(*display_name).valid ||
        !utf8_validate(*avatar_hash).valid || !utf8_validate(login_name).valid) {
        return false;
    }
    if (!gateway_avatar_hash_valid(*avatar_hash))
        return false;
    for (u32 role = 0; role < mutation->role_count; ++role) {
        u64 id = 0;
        for (u32 byte = 0; byte < 8; ++byte)
            id |= static_cast<u64>((*role_payload)[role * 8 + byte]) << (byte * 8);
        if (!id)
            return false;
        for (u32 previous = 0; previous < role; ++previous) {
            bool equal = true;
            for (u32 byte = 0; byte < 8; ++byte)
                equal &= (*role_payload)[role * 8 + byte] == (*role_payload)[previous * 8 + byte];
            if (equal)
                return false;
        }
    }
    ++*index;
    if (*index == event.member_list.record_count && *cursor != payload.count) {
        return false;
    }
    return true;
}

} // namespace maicord
