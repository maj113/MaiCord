#include "state_dispatch_internal.hpp"
#include "maicord/core/byte_order.hpp"
#include "state_payload.hpp"
#include "maicord/protocol/messages/thread_actions.hpp"

namespace maicord {
namespace {

// Decodes `source` (at most `maximum` bytes) into the writer; a full writer sets its overflow flag.
template <typename Size>
bool append_text(PayloadWriter* writer, JsonString source, u32 maximum, Size* bytes) noexcept {
    const JsonStringDecodeResult sized = json_string_decode_utf8(source, Slice<char>{nullptr, 0});
    if ((sized.error != JsonStringDecodeError::output_too_small && !sized.ok()) || sized.bytes > maximum)
        return false;
    if (writer->used > writer->output.count || sized.bytes > writer->output.count - writer->used) {
        writer->overflow = true;
        return false;
    }
    const JsonStringDecodeResult decoded = json_string_decode_utf8(
        source, Slice<char>{writer->output.data + writer->used, writer->output.count - writer->used});
    if (!decoded.ok() || decoded.bytes != sized.bytes)
        return false;
    writer->used += decoded.bytes;
    *bytes = static_cast<Size>(decoded.bytes);
    return true;
}

} // namespace
namespace state_dispatch_detail {

bool append_name(PayloadWriter* writer, JsonString source, u16* bytes) noexcept {
    return append_text(writer, source, 400, bytes);
}

bool append_icon(PayloadWriter* writer, JsonString source, u8* bytes) noexcept {
    return append_text(writer, source, kGatewayGuildIconHashMaximumBytes, bytes);
}

// An oversized guild routes again into 256 KiB, then double that, up to the guild record capacity.
u32 grown_record_capacity(u32 capacity) noexcept {
    return capacity < 256 * 1024                              ? 256 * 1024
           : capacity > kGatewayClientGuildRecordCapacity / 2 ? kGatewayClientGuildRecordCapacity
                                                              : capacity * 2;
}

} // namespace state_dispatch_detail
namespace {

struct RoleArrayContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    u32 maximum_depth;
    u64 ids[kGatewayGuildRoleMaximum];
    u32 count;
    GatewayEventDecodeError error;
};

} // namespace
namespace state_dispatch_detail {

u8 role_flags(const StateObjectContext& object) noexcept {
    const bool icon = ((object.mutation.fields & gateway_client_state_field_icon) && object.icon.encoded.bytes) ||
                      object.unicode_emoji_present;
    const bool in_prompt = (object.mutation.fields & gateway_client_state_field_flags) && (object.mutation.flags & 1u);
    return static_cast<u8>((object.mutation.hoist ? gateway_client_role_hoist : 0u) |
                           (object.mutation.managed ? gateway_client_role_managed : 0u) |
                           (object.mutation.role_flags & gateway_client_role_mentionable) |
                           (icon ? gateway_client_role_has_icon : 0u) |
                           (in_prompt ? gateway_client_role_in_prompt : 0u) |
                           (object.role_linked ? gateway_client_role_linked : 0u));
}

} // namespace state_dispatch_detail
namespace {

bool visit_role(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<RoleArrayContext*>(opaque);
    StateObjectContext object;
    const GatewayEventDecodeResult decoded =
        decode_state_object(value, context->object_scratch, context->maximum_depth, &object);
    constexpr u16 required = gateway_client_state_field_id | gateway_client_state_field_permissions;
    if (!decoded.ok() || (object.mutation.fields & required) != required ||
        context->count == kGatewayGuildRoleMaximum) {
        context->error = decoded.ok() ? GatewayEventDecodeError::invalid_data : decoded.error;
        return false;
    }
    for (u32 previous = 0; previous < context->count; ++previous) {
        if (context->ids[previous] == object.mutation.id) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }
    JsonStringDecodeResult sized{};
    if ((object.mutation.fields & gateway_client_state_field_name) != 0) {
        sized = json_string_decode_utf8(object.name, Slice<char>{nullptr, 0});
        if ((sized.error != JsonStringDecodeError::output_too_small && !sized.ok()) || sized.bytes > 400 ||
            sized.bytes > 0xFFFFu) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }
    GatewayClientRoleMutation role{};
    role.id = object.mutation.id;
    role.permissions = object.mutation.permissions;
    role.position = object.mutation.position;
    role.color = object.mutation.color;
    role.secondary_color = object.mutation.secondary_color;
    role.tertiary_color = object.mutation.tertiary_color;
    role.name_bytes = static_cast<u16>(sized.bytes);
    role.flags = role_flags(object);
    if (!gateway_payload_append(context->writer, &role, sizeof(role)) ||
        (role.name_bytes != 0 && !append_name(context->writer, object.name, &role.name_bytes))) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->ids[context->count] = role.id;
    ++context->count;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayEventDecodeResult append_role_array(JsonValue array, Slice<u32> array_scratch,
                                           Slice<u32> object_scratch, u32 maximum_depth,
                                           PayloadWriter* writer, u32* count) noexcept {
    RoleArrayContext context{};
    context.writer = writer;
    context.object_scratch = object_scratch;
    context.maximum_depth = maximum_depth;
    const GatewayEventDecodeResult result =
        visit_array(array, array_scratch, maximum_depth, visit_role, &context, context.error);
    if (result.ok())
        *count = context.count;
    return result;
}

} // namespace state_dispatch_detail
namespace {

struct SnowflakeArrayContext {
    PayloadWriter* writer;
    u32 start;
    u32 count;
    GatewayEventDecodeError error;
    u32 limit;
};

bool visit_snowflake(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<SnowflakeArrayContext*>(opaque);
    u64 id = 0;
    if (context->count == context->limit || !json_value_to_snowflake(value, &id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    for (u32 previous = 0; previous < context->count; ++previous) {
        const u32 offset = context->start + previous * 8;
        const u64 resident =
            byte_load_le64(reinterpret_cast<const u8*>(context->writer->output.data + offset));
        if (resident == id) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }
    if (!gateway_payload_append_u64(context->writer, id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    ++context->count;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayEventDecodeResult append_snowflake_array(JsonValue array, Slice<u32> scratch, u32 maximum_depth,
                                                PayloadWriter* writer, u32* count, u32 limit) noexcept {
    SnowflakeArrayContext context{writer, writer->used, 0, GatewayEventDecodeError::none, limit};
    const GatewayEventDecodeResult result =
        visit_array(array, scratch, maximum_depth, visit_snowflake, &context, context.error);
    if (result.ok())
        *count = context.count;
    return result;
}

} // namespace state_dispatch_detail
namespace {

struct EmojiArrayContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    Slice<u32> role_scratch;
    u64 ids[kGuildEmojiMaximum];
    u32 maximum_depth;
    u32 count;
    u32 role_count;
    GatewayEventDecodeError error;
};

bool visit_emoji(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<EmojiArrayContext*>(opaque);
    StateObjectContext object;
    const GatewayEventDecodeResult decoded =
        decode_state_object(value, context->object_scratch, context->maximum_depth, &object);
    constexpr u16 required = gateway_client_state_field_id | gateway_client_state_field_name;
    if (!decoded.ok() || (object.mutation.fields & required) != required ||
        context->count == kGuildEmojiMaximum) {
        context->error = decoded.ok() ? GatewayEventDecodeError::invalid_data : decoded.error;
        return false;
    }
    for (u32 previous = 0; previous < context->count; ++previous) {
        if (context->ids[previous] == object.mutation.id) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }

    GatewayClientEmojiMutation emoji{};
    emoji.id = object.mutation.id;
    emoji.flags = static_cast<u8>(
        (object.animated ? gateway_client_emoji_animated : 0u) |
        ((!object.available_seen || object.available) ? gateway_client_emoji_available : 0u) |
        (object.mutation.managed ? gateway_client_emoji_managed : 0u) |
        ((!object.require_colons_seen || object.require_colons) ? gateway_client_emoji_require_colons
                                                                : 0u));
    const u32 header_offset = context->writer->used;
    if (!gateway_payload_append(context->writer, &emoji, sizeof(emoji))) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    u32 role_count = 0;
    if (object.roles_seen) {
        const GatewayEventDecodeResult roles = append_snowflake_array(
            object.roles, context->role_scratch, context->maximum_depth, context->writer, &role_count);
        if (!roles.ok() || role_count > 0xFFFFu) {
            context->error = roles.ok() ? GatewayEventDecodeError::invalid_data : roles.error;
            return false;
        }
    }
    u16 name_bytes = 0;
    const u32 name_offset = context->writer->used;
    if (!append_name(context->writer, object.name, &name_bytes) ||
        !gateway_state_emoji_name_valid(Utf8View{context->writer->output.data + name_offset, name_bytes})) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    emoji.name_bytes = name_bytes;
    emoji.role_count = static_cast<u16>(role_count);
    copy_bytes(context->writer->output.data + header_offset, &emoji, sizeof(emoji));
    context->ids[context->count++] = emoji.id;
    context->role_count += role_count;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayEventDecodeResult append_emoji_array(JsonValue array, Slice<u32> array_scratch,
                                            Slice<u32> object_scratch, Slice<u32> role_scratch,
                                            u32 maximum_depth, PayloadWriter* writer, u32* count,
                                            u32* role_count) noexcept {
    EmojiArrayContext context{};
    context.writer = writer;
    context.object_scratch = object_scratch;
    context.role_scratch = role_scratch;
    context.maximum_depth = maximum_depth;
    const GatewayEventDecodeResult result =
        visit_array(array, array_scratch, maximum_depth, visit_emoji, &context, context.error);
    if (result.ok()) {
        *count = context.count;
        *role_count = context.role_count;
    }
    return result;
}

} // namespace state_dispatch_detail
namespace {

struct OverwriteArrayContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    u32 maximum_depth;
    u32 start;
    u32 count;
    GatewayEventDecodeError error;
    const JsonIndexedDocument* document = nullptr;
    GatewayEventDecodeResult failure{};
};

bool visit_overwrite(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<OverwriteArrayContext*>(opaque);
    StateObjectContext object;
    const GatewayEventDecodeResult decoded = decode_state_object(
        value, context->object_scratch, context->maximum_depth, &object, false, context->document);
    constexpr u16 required = gateway_client_state_field_id | gateway_client_state_field_type;
    if (!decoded.ok() || (object.mutation.fields & required) != required || context->count == 0xFFFFu) {
        context->error = decoded.ok() ? GatewayEventDecodeError::invalid_data : decoded.error;
        if (!decoded.ok())
            context->failure = decoded;
        return false;
    }
    // The official client keys overwrites by target, reads absent bits as
    // zero and leaves unknown target types out of permission math. Mirror that
    // instead of rejecting the channel (and with it the whole guild).
    if (object.mutation.type > 1)
        return true;
    const GatewayClientPermissionOverwriteMutation overwrite{
        object.mutation.id, object.allow_seen ? object.allow : 0, object.deny_seen ? object.deny : 0,
        object.mutation.type, {}};
    for (u32 previous = 0; previous < context->count; ++previous) {
        const u32 offset = context->start + previous * sizeof(GatewayClientPermissionOverwriteMutation);
        const u64 id = byte_load_le64(reinterpret_cast<const u8*>(context->writer->output.data + offset));
        const u8 type = static_cast<u8>(context->writer->output[offset + 24]);
        if (id == object.mutation.id && type == object.mutation.type) {
            copy_bytes(context->writer->output.data + offset, &overwrite, sizeof(overwrite));
            return true;
        }
    }
    if (!gateway_payload_append(context->writer, &overwrite, sizeof(overwrite))) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    ++context->count;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayEventDecodeResult append_overwrite_array(JsonValue array, Slice<u32> array_scratch,
                                                Slice<u32> object_scratch, u32 maximum_depth,
                                                PayloadWriter* writer, u32* count,
                                                const JsonIndexedDocument* document) noexcept {
    OverwriteArrayContext context{
        writer, object_scratch, maximum_depth, writer->used, 0, GatewayEventDecodeError::none};
    context.document = document;
    JsonReadResult read{};
    if (document)
        read = json_visit_indexed_array(*document, array, maximum_depth, visit_overwrite, &context);
    else {
        const JsonScanResult scan = json_scan_structurals_avx1(array.source, array_scratch);
        if (!scan.ok())
            return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        read = json_visit_value_array(array, {array_scratch.data, scan.count}, maximum_depth,
                                     visit_overwrite, &context);
    }
    if (!read.ok()) {
        if (!context.failure.ok())
            return context.failure;
        if (document &&
            reinterpret_cast<usize>(array.source.data) >= reinterpret_cast<usize>(document->input.data)) {
            const usize begin =
                reinterpret_cast<usize>(array.source.data) - reinterpret_cast<usize>(document->input.data);
            if (begin <= read.error_offset)
                read.error_offset -= static_cast<u32>(begin);
        }
        return read_failure(context.error, read);
    }
    *count = context.count;
    return decode_failure(GatewayEventDecodeError::none);
}

} // namespace state_dispatch_detail
namespace {

struct SelfMemberContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    u64 self_user_id;
    u32 maximum_depth;
    u32 role_count;
    GatewayEventDecodeError error;
    bool found;
    bool roles_known;
    bool timeout_known;
    u32 timeout_until;
    u16 member_known;
    u8 member_state;
};

bool visit_member_identity_id(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& identity = *static_cast<MemberIdentity*>(opaque);
    if (!json_name_is(name, utf8_literal("id")))
        return true;
    if (identity.nested_id_seen)
        return false;
    identity.nested_id_seen = true;
    return json_value_to_snowflake(value, &identity.nested_id);
}
bool visit_member_identity(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& identity = *static_cast<MemberIdentity*>(opaque);
    const u32 index = member_index<"user_id", "user">(name);
    if (index == 0) {
        if (identity.user_id_seen)
            return false;
        identity.user_id_seen = true;
        return json_value_to_snowflake(value, &identity.user_id);
    }
    if (index != 1)
        return true;
    if (identity.user_seen || value.kind != JsonValueKind::object)
        return false;
    identity.user_seen = true;
    u32 scratch[512];
    const auto scan = json_scan_structurals_avx1(value.source, {scratch, 512});
    return scan.ok() &&
           json_visit_value_object(value, {scratch, scan.count}, 64, visit_member_identity_id, &identity)
               .ok() &&
           identity.nested_id_seen;
}

} // namespace
namespace state_dispatch_detail {

bool member_identity(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                     MemberIdentity* identity) noexcept {
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    return scan.ok() && value.kind == JsonValueKind::object &&
           json_visit_value_object(value, {scratch.data, scan.count}, maximum_depth, visit_member_identity,
                                   identity)
               .ok();
}

} // namespace state_dispatch_detail
namespace {

// Out of line: its StateObjectContext and role index (~5 KB) would otherwise cost every member of the array a
// stack probe, while only the current user's entry reaches it.
__declspec(noinline) bool visit_self_member(SelfMemberContext* context, JsonValue value) noexcept {
    StateObjectContext member;
    const GatewayEventDecodeResult decoded =
        decode_state_object(value, context->object_scratch, context->maximum_depth, &member);
    if (!decoded.ok()) {
        context->error = decoded.error;
        return false;
    }
    if (context->found) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->found = true;
    context->timeout_known = (member.mutation.guild_settings_known & guild_settings_self_timeout) != 0;
    context->timeout_until = member.mutation.self_communication_disabled_until;
    member_state_from_flags(&member.mutation);
    context->member_known = static_cast<u16>(member.mutation.guild_settings_known &
                                            (guild_settings_self_member | guild_settings_self_quarantine));
    context->member_state = member.mutation.self_member;
    if (!member.roles_seen)
        return true; // Pending or previewing members can arrive without roles.
    context->roles_known = true;
    u32 role_scratch[1'024];
    const GatewayEventDecodeResult roles =
        append_snowflake_array(member.roles, Slice<u32>{role_scratch, 1'024}, context->maximum_depth,
                               context->writer, &context->role_count);
    if (!roles.ok()) {
        context->error = roles.error;
        return false;
    }
    return true;
}

bool visit_member(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<SelfMemberContext*>(opaque);
    // Only the current member's roles and state are read here. Any other entry
    // (voice members, a member without roles) is skipped rather than costing
    // the guild, so a server joined while running always appears.
    MemberIdentity identity{};
    const bool identified =
        member_identity(value, context->object_scratch, context->maximum_depth, &identity);
    const bool self =
        identity.user_id == context->self_user_id || identity.nested_id == context->self_user_id;
    if (!self)
        return true;
    if (!identified ||
        (identity.user_id_seen && identity.user_seen && identity.user_id != identity.nested_id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    return visit_self_member(context, value);
}

} // namespace
namespace state_dispatch_detail {

// The current member is optional: a previewed guild has none, and a pending
// one may still lack roles. *found reports whether it was present.
GatewayEventDecodeResult append_self_member_roles(JsonValue members, Slice<u32> array_scratch,
                                                  Slice<u32> object_scratch, u32 maximum_depth,
                                                  u64 self_user_id, PayloadWriter* writer,
                                                  u32* count, GatewayClientStateMutation* guild,
                                                  bool* found) noexcept {
    SelfMemberContext context{writer, object_scratch, self_user_id, maximum_depth, 0};
    const GatewayEventDecodeResult result = visit_array(members, array_scratch, maximum_depth, visit_member,
                                                        &context, context.error, GatewayEventDecodeError::invalid_data);
    if (!result.ok())
        return result;
    *found = context.found;
    *count = context.role_count;
    if (context.roles_known)
        guild->member_role_flags |= gateway_client_member_roles_known;
    if (context.timeout_known) {
        guild->guild_settings_known = static_cast<u16>(guild->guild_settings_known | guild_settings_self_timeout);
        guild->self_communication_disabled_until = context.timeout_until;
    }
    if (context.member_known) {
        guild->guild_settings_known = static_cast<u16>(guild->guild_settings_known | context.member_known);
        guild->self_member = context.member_state;
    }
    return decode_failure(GatewayEventDecodeError::none);
}

bool hydrate_self_member(StateObjectContext* object, u64 self_user_id) noexcept {
    if (!object->thread_member_seen || object->thread_member.kind == JsonValueKind::null_value) return true;
    // The membership receipt is advisory: on a non-thread channel or in an
    // unexpected shape it leaves the joined state unknown, never the channel.
    if (!channel_is_thread(object->mutation.type) || !self_user_id) return true;
    ThreadMemberSnapshot member{};
    if (!thread_member_decode(object->thread_member.source, object->mutation.id, self_user_id, &member).ok()) return true;
    object->mutation.thread_self_joined = true;
    object->mutation.thread_self_joined_known = true;
    object->mutation.thread_join_seconds = thread_join_seconds_from_ms(member.join_timestamp_ms);
    return true;
}

} // namespace state_dispatch_detail
namespace {

GatewayEventDecodeResult append_channel_record(PayloadWriter* writer, StateObjectContext* object,
                                               u32 maximum_depth,
                                               const JsonIndexedDocument& document) noexcept {
    GatewayClientStateMutation mutation = object->mutation;
    mutation.name_bytes = 0;
    mutation.icon_bytes = 0;
    mutation.overwrite_count = 0;
    const u32 header_offset = writer->used;
    const bool has_name = (mutation.fields & gateway_client_state_field_name) != 0;
    if (!gateway_payload_append(writer, &mutation, sizeof(mutation)) ||
        (has_name && !append_name(writer, object->name, &mutation.name_bytes))) {
        return decode_failure(GatewayEventDecodeError::invalid_data);
    }
    if (object->permission_overwrites_seen) {
        u32 object_scratch[512];
        const GatewayEventDecodeResult appended =
            append_overwrite_array(object->permission_overwrites, {}, Slice<u32>{object_scratch, 512},
                                   maximum_depth, writer, &mutation.overwrite_count, &document);
        if (!appended.ok()) {
            return appended;
        }
    }
    copy_bytes(writer->output.data + header_offset, &mutation, sizeof(mutation));
    object->mutation.name_bytes = mutation.name_bytes;
    object->mutation.overwrite_count = mutation.overwrite_count;
    return decode_failure(GatewayEventDecodeError::none);
}

struct ChannelArrayContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    u64 guild_id;
    u32 maximum_depth;
    u32 count;
    u32 overwrite_count;
    GatewayEventDecodeError error;
    JsonIndexedDocument document{};
    GatewayEventDecodeResult failure{};
    u64 self_user_id{};
    u32 skipped{};
    // Guild snapshots skip one bad channel record; THREAD_LIST_SYNC keeps its
    // all-or-nothing contract and rejects the whole sync instead.
    bool tolerant{};
};

// In a guild snapshot one malformed or unsupported channel record is skipped
// (that channel is hidden) instead of failing its guild. Only a full payload
// buffer stops the array, because every later record would be lost the same way.
bool visit_channel(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ChannelArrayContext*>(opaque);
    const u32 mark = context->writer->used;
    const auto skip = [context, mark](GatewayEventDecodeError error, const GatewayEventDecodeResult* failure) noexcept {
        if (!context->tolerant || context->writer->overflow) {
            context->error = error;
            if (failure)
                context->failure = *failure;
            return false;
        }
        context->writer->used = mark;
        ++context->skipped;
        return true;
    };
    StateObjectContext object;
    const GatewayEventDecodeResult decoded = decode_state_object(
        value, context->object_scratch, context->maximum_depth, &object, false, &context->document);
    if (!decoded.ok())
        return skip(decoded.error, &decoded);
    constexpr u16 required = gateway_client_state_field_id | gateway_client_state_field_type;
    if ((object.mutation.fields & required) != required)
        return skip(GatewayEventDecodeError::missing_required_field, nullptr);
    if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0) {
        object.mutation.fields |= gateway_client_state_field_guild_id;
        object.mutation.guild_id = context->guild_id;
    }
    if (!hydrate_self_member(&object, context->self_user_id))
        return skip(GatewayEventDecodeError::invalid_data, nullptr);
    if (context->tolerant) {
        if (object.mutation.guild_id != context->guild_id || object.mutation.type == 1 || object.mutation.type == 3)
            return skip(GatewayEventDecodeError::invalid_data, nullptr);
        // A guild channel without an overwrite list has none (the official client
        // reads it as empty); threads inherit their parent's overwrites instead.
        if (!channel_is_thread(object.mutation.type))
            object.mutation.fields |= gateway_client_state_field_permission_overwrites;
    }
    const GatewayEventDecodeResult appended =
        append_channel_record(context->writer, &object, context->maximum_depth, context->document);
    if (!appended.ok())
        return skip(appended.error, &appended);
    ++context->count;
    context->overwrite_count += object.mutation.overwrite_count;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayEventDecodeResult append_channel_array(JsonValue array, Slice<u32> array_scratch,
                                              Slice<u32> object_scratch, u32 maximum_depth, u64 guild_id,
                                              PayloadWriter* writer, u32* count,
                                              u32* overwrite_count, u64 self_user_id,
                                              u32* skipped) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(array.source, array_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    ChannelArrayContext context{
        writer, object_scratch, guild_id, maximum_depth, 0, 0, GatewayEventDecodeError::none};
    context.document = {array.source, {array_scratch.data, scan.count}};
    context.self_user_id = self_user_id;
    context.tolerant = skipped != nullptr;
    const JsonReadResult read =
        json_visit_value_array(array, ConstSlice<u32>{array_scratch.data, scan.count}, maximum_depth,
                              visit_channel, &context);
    if (!read.ok()) {
        if (!context.failure.ok())
            return context.failure;
        return read_failure(context.error, read);
    }
    *count += context.count;
    *overwrite_count += context.overwrite_count;
    if (skipped)
        *skipped += context.skipped;
    return decode_failure(GatewayEventDecodeError::none);
}

bool visit_ready_guild(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<ReadyContext*>(opaque);
    StateObjectContext object;
    const GatewayEventDecodeResult decoded =
        decode_state_object(value, context->object_scratch, context->maximum_depth, &object);
    if (!decoded.ok() && decoded.error == GatewayEventDecodeError::scan_failed &&
        decoded.scan_error == JsonScanError::output_overflow) {
        // Index capacity, not data: the projection grows its index and retries.
        context->error = decoded.error;
        context->error_offset = decoded.error_offset;
        context->scan_error = decoded.scan_error;
        context->read_error = decoded.read_error;
        return false;
    }
    // An unreadable guild header is left out of READY instead of failing the
    // session; its full record is skipped by the same check later.
    if (!decoded.ok() || (object.mutation.fields & gateway_client_state_field_id) == 0)
        return true;
    if (!gateway_payload_append_u64(context->writer, object.mutation.id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    ++context->count;
    return true;
}

} // namespace state_dispatch_detail
namespace {

struct ThreadSyncRoot {
    u64 guild{};
    JsonValue parents{}, threads{}, members{};
    u8 seen{};
    GatewayEventDecodeError error{};
};
bool thread_sync_field(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& root = *static_cast<ThreadSyncRoot*>(opaque);
    const u32 bit = member_bit<"guild_id", "channel_ids", "threads", "members">(key);
    if (!bit) return true;
    if (root.seen & bit) { root.error = GatewayEventDecodeError::duplicate_field; return false; }
    root.seen |= bit;
    if (bit == 1) {
        if (json_value_to_snowflake(value, &root.guild)) return true;
    } else if (value.kind == JsonValueKind::array) {
        if (bit == 2) root.parents = value;
        if (bit == 4) root.threads = value;
        if (bit == 8) root.members = value;
        return true;
    }
    root.error = GatewayEventDecodeError::invalid_data;
    return false;
}
struct ThreadSyncMembers {
    PayloadWriter* writer;
    Slice<u32> scratch;
    u64 self;
    u32 depth, begin, count{};
    GatewayEventDecodeError error{};
};
bool thread_sync_member(void* opaque, u32, JsonValue value) noexcept {
    auto& members = *static_cast<ThreadSyncMembers*>(opaque);
    StateObjectContext object;
    const auto decoded = decode_state_object(value, members.scratch, members.depth, &object);
    ThreadMemberSnapshot snapshot{};
    if (!decoded.ok() || !(object.mutation.fields & gateway_client_state_field_id) ||
        !thread_member_decode(value.source, object.mutation.id, members.self, &snapshot).ok()) {
        members.error = GatewayEventDecodeError::invalid_data; return false;
    }
    for (u32 i = 0; i < members.count; ++i)
        if (byte_load_le64(reinterpret_cast<const u8*>(members.writer->output.data + members.begin +
                                                       i * kGatewayThreadSyncMemberBytes)) == snapshot.thread_id) {
            members.error = GatewayEventDecodeError::invalid_data; return false;
        }
    const u32 join_seconds = thread_join_seconds_from_ms(snapshot.join_timestamp_ms);
    if (!gateway_payload_append_u64(members.writer, snapshot.thread_id) ||
        !gateway_payload_append(members.writer, &join_seconds, sizeof(join_seconds))) {
        members.error = GatewayEventDecodeError::message_content_capacity; return false;
    }
    ++members.count;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayClientDispatchRouteResult route_thread_sync(const GatewayEnvelope& envelope, Slice<u32> scratch,
    u32 depth, Slice<char> storage, u64 self, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    const auto scan = json_scan_structurals_avx1(envelope.data.source, scratch);
    if (!scan.ok() || !self) return decode_route(GatewayEventDecodeError::invalid_data);
    ThreadSyncRoot root{};
    const auto read = json_visit_value_object(envelope.data, {scratch.data, scan.count}, depth, thread_sync_field, &root);
    // guild_id and threads are required; channel_ids and members are optional.
    if (!read.ok() || (root.seen & 5) != 5)
        return decode_route(root.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::missing_required_field : root.error);
    PayloadWriter writer{storage, 0};
    GatewayClientThreadListSync header{};
    header.guild_id = root.guild; header.self_user_id = self;
    header.scope = (root.seen & 2) ? GatewayClientThreadSyncScope::parents : GatewayClientThreadSyncScope::guild;
    if ((root.seen & 2) && !append_snowflake_array(root.parents, scratch, depth, &writer, &header.parent_count, 4096).ok())
        return decode_route(GatewayEventDecodeError::invalid_data);
    u32 objects[512];
    u32 overwrite_count{};
    const auto channels = append_channel_array(root.threads, scratch, {objects, 512}, depth, root.guild,
                                               &writer, &header.thread_count, &overwrite_count, self);
    if (!channels.ok()) return decode_route(channels.error);
    if (root.seen & 8) {
        const auto member_scan = json_scan_structurals_avx1(root.members.source, scratch);
        if (!member_scan.ok()) return decode_route(GatewayEventDecodeError::invalid_data);
        ThreadSyncMembers members{&writer, {objects, 512}, self, depth, writer.used};
        const auto member_read = json_visit_value_array(root.members, {scratch.data, member_scan.count}, depth, thread_sync_member, &members);
        if (!member_read.ok()) return decode_route(members.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : members.error);
        header.member_count = members.count;
        header.flags = gateway_client_thread_sync_members_known;
    }
    event->dispatch_kind = GatewayClientDispatchKind::thread_list_sync;
    event->thread_sync = header;
    event->dispatch_payload_bytes = writer.used;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    return {};
}

} // namespace state_dispatch_detail
} // namespace maicord
