#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

struct MessageContext {
    GatewayMessageEvent event;
    JsonString content;
    JsonValue author;
    JsonValue mentions;
    JsonValue mention_roles;
    JsonValue member;
    JsonValue call;
    GatewayEventDecodeError error;
};

struct CallContext {
    u64 current_user_id;
    u64 ended_timestamp_ms;
    JsonValue participants;
    u32 seen;
    bool ended;
    bool participated;
    GatewayEventDecodeError error;
};

bool visit_call_participant(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<CallContext*>(opaque);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->participated = context->participated || (id && id == context->current_user_id);
    return true;
}

bool visit_call_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<CallContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("ended_timestamp"))) {
        if ((context->seen & 1u) != 0) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->seen |= 1u;
        if (value.kind == JsonValueKind::null_value)
            return true;
        JsonString text{};
        if (!json_value_as_string(value, &text) || !parse_iso_timestamp(text, &context->ended_timestamp_ms)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->ended = true;
    } else if (json_string_equals_ascii(name, utf8_literal("participants"))) {
        if ((context->seen & 2u) != 0 || value.kind != JsonValueKind::array) {
            context->error = (context->seen & 2u) ? GatewayEventDecodeError::duplicate_field
                                                  : GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->seen |= 2u;
        context->participants = value;
    }
    return true;
}

// CALL messages carry the participants and the end time; a missing end time
// means the call is still ongoing.
GatewayEventDecodeResult decode_call(JsonValue call, Slice<u32> structural_scratch, u32 maximum_depth,
                                     u64 current_user_id, GatewayMessageCall* output) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(call.source, structural_scratch);
    if (!scan.ok())
        return GatewayEventDecodeResult{scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error,
                                        JsonReadError::none};
    CallContext context{};
    context.current_user_id = current_user_id;
    JsonReadResult read = json_visit_value_object(call, {structural_scratch.data, scan.count},
                                                 maximum_depth, visit_call_member, &context);
    if (read.ok() && (context.seen & 2u)) {
        const JsonScanResult inner = json_scan_structurals_avx1(context.participants.source, structural_scratch);
        if (!inner.ok())
            return GatewayEventDecodeResult{inner.error_offset, GatewayEventDecodeError::scan_failed, inner.error,
                                            JsonReadError::none};
        read = json_visit_value_array(context.participants, {structural_scratch.data, inner.count},
                                     maximum_depth, visit_call_participant, &context);
    }
    if (!read.ok())
        return GatewayEventDecodeResult{read.error_offset,
                                        context.error != GatewayEventDecodeError::none
                                            ? context.error
                                            : GatewayEventDecodeError::invalid_json,
                                        JsonScanError::none, read.error};
    *output = GatewayMessageCall{context.ended_timestamp_ms, true, context.ended, context.participated};
    return GatewayEventDecodeResult{};
}

struct AuthorContext {
    u64 id;
    JsonString username;
    JsonString global_name;
    JsonString avatar;
    GatewayEventDecodeError error;
    bool id_seen;
    bool username_seen;
    bool global_name_seen;
    bool global_name_null;
    bool avatar_seen;
    bool avatar_null;
    u32 public_flags;
    bool public_flags_seen, bot_seen, bot;
    bool private_flags_seen;
    u32 private_flags;
};

struct DeleteContext {
    GatewayMessageDeleteEvent event;
    GatewayEventDecodeError error;
    u32 fields;
};

struct BulkContext {
    GatewayMessageBulkDeleteEvent event;
    JsonValue ids;
    GatewayEventDecodeError error;
    u32 fields;
};

struct BulkIdContext {
    Slice<u64> ids;
    GatewayEventDecodeError error;
};

struct MentionUserContext {
    JsonIndexedDocument document;
    Slice<u32> structural_scratch;
    Slice<char> name_output;
    Slice<GatewayMessageNamedUser> users;
    u32* used;
    u64 current_user_id;
    u32 maximum_depth;
    GatewayEventDecodeError error;
    u32 count;
    bool capture_names;
    bool found;
};

struct MentionRoleContext {
    Slice<u64> ids;
    GatewayEventDecodeError error;
    u32 count;
};

struct MemberContext {
    JsonValue roles;
    GatewayEventDecodeError error;
    bool roles_seen;
};

GatewayEventDecodeResult message_failure(GatewayEventDecodeError error, u32 offset = 0,
                                         JsonScanError scan_error = JsonScanError::none,
                                         JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

bool mark_message_field(MessageContext* context, u32 field) noexcept {
    if ((context->event.fields & field) != 0) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->event.fields |= field;
    return true;
}

bool json_value_to_message_nonce(JsonValue value, u64* output) noexcept {
    if (!output) {
        return false;
    }
    if (value.kind == JsonValueKind::null_value) {
        *output = 0;
        return true;
    }
    if (value.kind == JsonValueKind::string) {
        JsonString string{};
        if (!json_value_as_string(value, &string))
            return false;
        // Other clients may use opaque string nonces. Only our numeric
        // identity can match the outbox; an unrelated string is not a bad
        // message and must not reject an otherwise valid history page.
        if (!json_string_to_u64(string, output))
            *output = 0;
        return true;
    }
    return json_value_to_u64(value, output);
}

bool visit_message_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<MessageContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!mark_message_field(context, gateway_message_field_type))
            return false;
        u64 type = 0;
        if (!json_value_to_u64(value, &type) || type > 65535) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->event.type = static_cast<u16>(type);
    } else if (json_string_equals_ascii(name, utf8_literal("activity_instance")) ||
               json_string_equals_ascii(name, utf8_literal("application_id")) ||
               json_string_equals_ascii(name, utf8_literal("changelog_id"))) {
        const bool activity = json_string_equals_ascii(name, utf8_literal("activity_instance"));
        const bool application = json_string_equals_ascii(name, utf8_literal("application_id"));
        const u32 field = activity      ? gateway_message_field_activity_instance
                          : application ? gateway_message_field_application_id
                                        : gateway_message_field_changelog_id;
        if (!mark_message_field(context, field))
            return false;
        if (activity)
            context->event.activity_instance_present = value.kind != JsonValueKind::null_value;
        else if (application)
            context->event.application_present = value.kind != JsonValueKind::null_value;
        else
            context->event.changelog_present = value.kind != JsonValueKind::null_value;
    } else if (json_string_equals_ascii(name, utf8_literal("interaction_metadata"))) {
        if (!mark_message_field(context, gateway_message_field_interaction_metadata))
            return false;
        context->event.interaction_metadata_present = value.kind != JsonValueKind::null_value;
    } else if (json_string_equals_ascii(name, utf8_literal("poll")) ||
               json_string_equals_ascii(name, utf8_literal("call")) ||
               json_string_equals_ascii(name, utf8_literal("activity"))) {
        const bool poll = json_string_equals_ascii(name, utf8_literal("poll"));
        const bool call = json_string_equals_ascii(name, utf8_literal("call"));
        const u32 field = poll   ? gateway_message_field_poll
                          : call ? gateway_message_field_call
                                 : gateway_message_field_activity;
        if (!mark_message_field(context, field))
            return false;
        if (value.kind != JsonValueKind::object && value.kind != JsonValueKind::null_value) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        if (poll)
            context->event.poll.present = value.kind == JsonValueKind::object;
        if (call && value.kind == JsonValueKind::object)
            context->call = value;
        if (value.kind == JsonValueKind::object)
            context->event.forwarding_features |= poll   ? gateway_message_forwarding_poll
                                                  : call ? gateway_message_forwarding_call
                                                         : gateway_message_forwarding_activity;
    } else if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark_message_field(context, gateway_message_field_id)) {
            return false;
        }
        if (!json_value_to_snowflake(value, &context->event.id)) {
            context->error = GatewayEventDecodeError::invalid_message_id;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!mark_message_field(context, gateway_message_field_channel_id)) {
            return false;
        }
        if (!json_value_to_snowflake(value, &context->event.channel_id)) {
            context->error = GatewayEventDecodeError::invalid_channel_id;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("author"))) {
        if (!mark_message_field(context, gateway_message_field_author_id)) {
            return false;
        }
        if (value.kind != JsonValueKind::object) {
            context->error = GatewayEventDecodeError::invalid_author;
            return false;
        }
        context->author = value;
    } else if (json_string_equals_ascii(name, utf8_literal("content"))) {
        if (!mark_message_field(context, gateway_message_field_content)) {
            return false;
        }
        if (!json_value_as_string(value, &context->content)) {
            context->error = GatewayEventDecodeError::invalid_message_content;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("timestamp"))) {
        if (!mark_message_field(context, gateway_message_field_timestamp)) {
            return false;
        }
        JsonString timestamp{};
        if (!json_value_as_string(value, &timestamp) ||
            !parse_iso_timestamp(timestamp, &context->event.timestamp_ms)) {
            context->error = GatewayEventDecodeError::invalid_message_timestamp;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("edited_timestamp"))) {
        if (!mark_message_field(context, gateway_message_field_edited_timestamp)) {
            return false;
        }
        if (value.kind == JsonValueKind::null_value) {
            context->event.edited_timestamp_ms = 0;
        } else {
            JsonString timestamp{};
            if (!json_value_as_string(value, &timestamp) ||
                !parse_iso_timestamp(timestamp, &context->event.edited_timestamp_ms)) {
                context->error = GatewayEventDecodeError::invalid_edited_timestamp;
                return false;
            }
        }
    } else if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        if (!mark_message_field(context, gateway_message_field_flags)) {
            return false;
        }
        u64 flags = 0;
        if (!json_value_to_u64(value, &flags) || flags > 0xFFFFFFFFull) {
            context->error = GatewayEventDecodeError::invalid_message_flags;
            return false;
        }
        context->event.flags = static_cast<u32>(flags);
    } else if (json_string_equals_ascii(name, utf8_literal("nonce"))) {
        if (!mark_message_field(context, gateway_message_field_nonce)) {
            return false;
        }
        if (!json_value_to_message_nonce(value, &context->event.nonce)) {
            context->error = GatewayEventDecodeError::invalid_message_nonce;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("pinned"))) {
        if (!mark_message_field(context, gateway_message_field_pinned)) {
            return false;
        }
        if (!json_value_to_bool(value, &context->event.pinned)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("mention_everyone"))) {
        if (!mark_message_field(context, gateway_message_field_mention_everyone)) {
            return false;
        }
        if (!json_value_to_bool(value, &context->event.mention_everyone)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("mentions"))) {
        if (!mark_message_field(context, gateway_message_field_mentions)) {
            return false;
        }
        if (value.kind != JsonValueKind::array) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->mentions = value;
    } else if (json_string_equals_ascii(name, utf8_literal("mention_roles"))) {
        if (!mark_message_field(context, gateway_message_field_mention_roles)) {
            return false;
        }
        if (value.kind != JsonValueKind::array) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->mention_roles = value;
    } else if (json_string_equals_ascii(name, utf8_literal("member"))) {
        if (!mark_message_field(context, gateway_message_field_member) ||
            value.kind != JsonValueKind::object) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->member = value;
    }
    return true;
}

bool visit_author_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AuthorContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("public_flags")) ||
        json_string_equals_ascii(name, utf8_literal("flags"))) {
        // User.hasFlag combines public and private flags; retain the low
        // policy bits, including SPAMMER, without rejecting future high flags.
        const bool priv = json_string_equals_ascii(name, utf8_literal("flags"));
        const bool seen = priv ? context->private_flags_seen : context->public_flags_seen;
        u64 flags = 0;
        if (seen || !json_value_to_u64(value, &flags)) {
            context->error =
                seen ? GatewayEventDecodeError::duplicate_field : GatewayEventDecodeError::invalid_data;
            return false;
        }
        if (priv) {
            context->private_flags_seen = true;
            context->private_flags = static_cast<u32>(flags);
        } else {
            context->public_flags_seen = true;
            context->public_flags = static_cast<u32>(flags);
        }
    } else if (json_string_equals_ascii(name, utf8_literal("bot"))) {
        if (context->bot_seen || !json_value_to_bool(value, &context->bot)) {
            context->error = context->bot_seen ? GatewayEventDecodeError::duplicate_field
                                               : GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->bot_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (context->id_seen) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->id_seen = true;
        if (!json_value_to_snowflake(value, &context->id)) {
            context->error = GatewayEventDecodeError::invalid_author_id;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("username"))) {
        if (context->username_seen || !json_value_as_string(value, &context->username)) {
            context->error = context->username_seen ? GatewayEventDecodeError::duplicate_field
                                                    : GatewayEventDecodeError::invalid_user_name;
            return false;
        }
        context->username_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("global_name"))) {
        if (context->global_name_seen) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->global_name_seen = true;
        context->global_name_null = value.kind == JsonValueKind::null_value;
        if (!context->global_name_null && !json_value_as_string(value, &context->global_name)) {
            context->error = GatewayEventDecodeError::invalid_user_name;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("avatar"))) {
        if (context->avatar_seen) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->avatar_seen = true;
        context->avatar_null = value.kind == JsonValueKind::null_value;
        if (!context->avatar_null && !json_value_as_string(value, &context->avatar)) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
    }
    return true;
}

GatewayEventDecodeResult decode_author(JsonValue author, Slice<u32> structural_scratch, u32 maximum_depth,
                                       Slice<char> name_output, u32* used, bool capture_name,
                                       u64* author_id, Utf8View* author_name, Utf8View* avatar_hash,
                                       u32* public_flags = nullptr, bool* bot = nullptr,
                                       bool* policy_explicit = nullptr,
                                       const JsonIndexedDocument* indexed = nullptr) noexcept {
    AuthorContext context{};
    JsonReadResult read{};
    if (indexed) {
        read = json_visit_indexed_object(*indexed, author, maximum_depth, visit_author_member, &context);
    } else {
        const JsonScanResult scan = json_scan_structurals_avx1(author.source, structural_scratch);
        if (!scan.ok())
            return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        read = json_visit_value_object(author, {structural_scratch.data, scan.count}, maximum_depth,
                                      visit_author_member, &context);
    }
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? message_failure(context.error, read.error_offset, JsonScanError::none, read.error)
                   : message_failure(GatewayEventDecodeError::invalid_json, read.error_offset,
                                     JsonScanError::none, read.error);
    }
    if (!context.id_seen) {
        return message_failure(GatewayEventDecodeError::invalid_author_id, author.source_offset);
    }
    *author_id = context.id;
    if (public_flags)
        *public_flags = context.public_flags | context.private_flags;
    if (bot)
        *bot = context.bot;
    if (policy_explicit)
        *policy_explicit = context.public_flags_seen && context.bot_seen;
    if (author_name) {
        *author_name = Utf8View{};
    }
    if (avatar_hash) {
        *avatar_hash = Utf8View{};
    }
    if (capture_name) {
        const JsonString selected =
            context.global_name_seen && !context.global_name_null ? context.global_name : context.username;
        if (selected.encoded.data) {
            const JsonStringDecodeResult size = json_string_decode_utf8(selected, Slice<char>{nullptr, 0});
            const bool capacity_error = !used || *used > name_output.count ||
                                        size.bytes > name_output.count - minimum(*used, name_output.count);
            if ((size.error != JsonStringDecodeError::output_too_small &&
                 !(size.ok() && size.bytes == 0)) ||
                size.bytes == 0 || size.bytes > kGatewayMessageUserNameMaximumBytes || capacity_error) {
                return message_failure(capacity_error ? GatewayEventDecodeError::message_content_capacity
                                                      : GatewayEventDecodeError::invalid_user_name,
                                       author.source_offset);
            }
            const JsonStringDecodeResult decoded = json_string_decode_utf8(
                selected, Slice<char>{name_output.data + *used, name_output.count - *used});
            if (!decoded.ok() || decoded.bytes != size.bytes) {
                return message_failure(GatewayEventDecodeError::invalid_user_name, author.source_offset);
            }
            if (author_name) {
                *author_name = Utf8View{name_output.data + *used, decoded.bytes};
            }
            *used += decoded.bytes;
        }
    }
    if (avatar_hash && context.avatar_seen && !context.avatar_null) {
        const JsonStringDecodeResult size =
            json_string_decode_utf8(context.avatar, Slice<char>{nullptr, 0});
        const bool capacity_error = !used || *used > name_output.count ||
                                    size.bytes > name_output.count - minimum(*used, name_output.count);
        if ((size.error != JsonStringDecodeError::output_too_small && !(size.ok() && size.bytes == 0)) ||
            size.bytes > kGatewayMessageAvatarHashMaximumBytes || capacity_error) {
            return message_failure(capacity_error ? GatewayEventDecodeError::message_content_capacity
                                                  : GatewayEventDecodeError::invalid_data,
                                   author.source_offset);
        }
        if (size.bytes != 0) {
            const JsonStringDecodeResult decoded = json_string_decode_utf8(
                context.avatar, Slice<char>{name_output.data + *used, name_output.count - *used});
            if (!decoded.ok() || decoded.bytes != size.bytes) {
                return message_failure(GatewayEventDecodeError::invalid_data, author.source_offset);
            }
            *avatar_hash = Utf8View{name_output.data + *used, decoded.bytes};
            *used += decoded.bytes;
        }
    }
    return message_failure(GatewayEventDecodeError::none);
}

bool visit_mention_user(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<MentionUserContext*>(opaque);
    if (value.kind != JsonValueKind::object) {
        context->error = GatewayEventDecodeError::invalid_author;
        return false;
    }
    u64 id = 0;
    Utf8View name{};
    const GatewayEventDecodeResult decoded = decode_author(
        value, context->structural_scratch, context->maximum_depth, context->name_output, context->used,
        context->capture_names, &id, &name, nullptr, nullptr, nullptr, nullptr, &context->document);
    if (!decoded.ok()) {
        context->error = decoded.error;
        return false;
    }
    context->found = context->found || (context->current_user_id != 0 && id == context->current_user_id);
    if (context->capture_names) {
        if (context->count >= kGatewayMessageMentionUserMaximum || context->count >= context->users.count) {
            context->error = GatewayEventDecodeError::mention_user_capacity;
            return false;
        }
        for (u32 previous = 0; previous < context->count; ++previous) {
            if (context->users[previous].id == id) {
                context->error = GatewayEventDecodeError::duplicate_field;
                return false;
            }
        }
        context->users[context->count++] = GatewayMessageNamedUser{id, name};
    }
    return true;
}

bool visit_mention_role(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<MentionRoleContext*>(opaque);
    if (index >= kGatewayMessageMentionRoleMaximumIds || index >= context->ids.count ||
        !json_value_to_snowflake(value, &context->ids[index])) {
        context->error = index >= context->ids.count ? GatewayEventDecodeError::mention_role_capacity
                                                     : GatewayEventDecodeError::invalid_data;
        return false;
    }
    for (u32 previous = 0; previous < index; ++previous) {
        if (context->ids[previous] == context->ids[index]) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
    }
    context->count = index + 1;
    return true;
}

bool visit_member_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<MemberContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("roles"))) {
        return true;
    }
    if (context->roles_seen || value.kind != JsonValueKind::array) {
        context->error = context->roles_seen ? GatewayEventDecodeError::duplicate_field
                                             : GatewayEventDecodeError::invalid_data;
        return false;
    }
    context->roles_seen = true;
    context->roles = value;
    return true;
}

GatewayEventDecodeResult decode_member_roles(MessageContext* context, Slice<u32> structural_scratch,
                                             u32 maximum_depth, Slice<u64> member_role_scratch) noexcept {
    if ((context->event.fields & gateway_message_field_member) == 0) {
        return message_failure(GatewayEventDecodeError::none);
    }
    const JsonScanResult member_scan =
        json_scan_structurals_avx1(context->member.source, structural_scratch);
    if (!member_scan.ok()) {
        return message_failure(GatewayEventDecodeError::scan_failed, member_scan.error_offset,
                               member_scan.error);
    }
    MemberContext member{};
    const JsonReadResult member_read = json_visit_value_object(
        context->member, ConstSlice<u32>{structural_scratch.data, member_scan.count}, maximum_depth,
        visit_member_field, &member);
    if (!member_read.ok()) {
        return message_failure(member.error != GatewayEventDecodeError::none
                                   ? member.error
                                   : GatewayEventDecodeError::invalid_json,
                               member_read.error_offset, JsonScanError::none, member_read.error);
    }
    if (!member.roles_seen) {
        return message_failure(GatewayEventDecodeError::missing_required_field,
                               context->member.source_offset);
    }
    const JsonScanResult role_scan = json_scan_structurals_avx1(member.roles.source, structural_scratch);
    if (!role_scan.ok()) {
        return message_failure(GatewayEventDecodeError::scan_failed, role_scan.error_offset,
                               role_scan.error);
    }
    MentionRoleContext roles{member_role_scratch, GatewayEventDecodeError::none, 0};
    const JsonReadResult role_read = json_visit_value_array(
        member.roles, ConstSlice<u32>{structural_scratch.data, role_scan.count}, maximum_depth,
        visit_mention_role, &roles);
    if (!role_read.ok() || roles.count > kGatewayMessageMemberRoleMaximumIds) {
        return message_failure(roles.error != GatewayEventDecodeError::none
                                   ? roles.error
                                   : GatewayEventDecodeError::invalid_json,
                               role_read.error_offset, JsonScanError::none, role_read.error);
    }
    context->event.member_roles = ConstSlice<u64>{member_role_scratch.data, roles.count};
    return message_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult decode_mentions(MessageContext* context, Slice<u32> structural_scratch,
                                         u32 maximum_depth, Slice<u64> mention_role_scratch,
                                         Slice<GatewayMessageNamedUser> mention_user_scratch,
                                         Slice<char> name_output, u32* used, u64 current_user_id,
                                         bool capture_names) noexcept {
    if ((context->event.fields & gateway_message_field_mentions) != 0) {
        const JsonScanResult scan =
            json_scan_structurals_avx1(context->mentions.source, structural_scratch);
        if (!scan.ok()) {
            return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        }
        u32 object_scratch[512]{};
        MentionUserContext users{};
        users.document = {context->mentions.source, {structural_scratch.data, scan.count}};
        users.structural_scratch = Slice<u32>{object_scratch, 512};
        users.name_output = name_output;
        users.users = mention_user_scratch;
        users.used = used;
        users.current_user_id = current_user_id;
        users.maximum_depth = maximum_depth;
        users.capture_names = capture_names;
        const JsonReadResult read = json_visit_value_array(
            context->mentions, ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth,
            visit_mention_user, &users);
        if (!read.ok()) {
            return message_failure(users.error != GatewayEventDecodeError::none
                                       ? users.error
                                       : GatewayEventDecodeError::invalid_json,
                                   read.error_offset, JsonScanError::none, read.error);
        }
        context->event.mentions_current_user = users.found;
        if (capture_names) {
            context->event.mention_users =
                ConstSlice<GatewayMessageNamedUser>{mention_user_scratch.data, users.count};
        }
    }
    if ((context->event.fields & gateway_message_field_mention_roles) != 0) {
        const JsonScanResult scan =
            json_scan_structurals_avx1(context->mention_roles.source, structural_scratch);
        if (!scan.ok()) {
            return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        }
        MentionRoleContext roles{mention_role_scratch, GatewayEventDecodeError::none, 0};
        const JsonReadResult read = json_visit_value_array(
            context->mention_roles, ConstSlice<u32>{structural_scratch.data, scan.count},
            maximum_depth, visit_mention_role, &roles);
        if (!read.ok()) {
            return message_failure(roles.error != GatewayEventDecodeError::none
                                       ? roles.error
                                       : GatewayEventDecodeError::invalid_json,
                                   read.error_offset, JsonScanError::none, read.error);
        }
        if (read.member_count > kGatewayMessageMentionRoleMaximumIds || read.member_count != roles.count) {
            return message_failure(GatewayEventDecodeError::invalid_data,
                                   context->mention_roles.source_offset);
        }
        context->event.mention_roles = ConstSlice<u64>{mention_role_scratch.data, roles.count};
    }
    return message_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult decode_message(JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
                                        Slice<char> content_scratch, Slice<u64> mention_role_scratch,
                                        Slice<u64> member_role_scratch,
                                        Slice<GatewayMessageNamedUser> mention_user_scratch,
                                        const GatewayMessageRichScratch& rich_scratch, u64 current_user_id,
                                        bool capture_names, bool capture_rich, bool create,
                                        GatewayMessageEvent* output, bool snapshot_core = false) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0 ||
        (!content_scratch.data && content_scratch.count != 0) ||
        (!mention_role_scratch.data && mention_role_scratch.count != 0) ||
        (!member_role_scratch.data && member_role_scratch.count != 0) ||
        (!mention_user_scratch.data && mention_user_scratch.count != 0)) {
        return message_failure(GatewayEventDecodeError::invalid_input);
    }
    *output = GatewayMessageEvent{};
    if (data.kind != JsonValueKind::object) {
        return message_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    MessageContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_message_member, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? message_failure(context.error, read.error_offset, JsonScanError::none, read.error)
                   : message_failure(GatewayEventDecodeError::invalid_json, read.error_offset,
                                     JsonScanError::none, read.error);
    }

    constexpr u32 update_required = gateway_message_field_id | gateway_message_field_channel_id;
    constexpr u32 create_required = update_required | gateway_message_field_author_id |
                                    gateway_message_field_content | gateway_message_field_timestamp;
    const u32 required = snapshot_core ? 0u : (create ? create_required : update_required);
    if ((context.event.fields & required) != required) {
        return message_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    u32 transient_used = 0;
    if ((context.event.fields & gateway_message_field_content) != 0) {
        const JsonStringDecodeResult decoded = json_string_decode_utf8(
            context.content,
            Slice<char>{content_scratch.data,
                        minimum(content_scratch.count, kGatewayMessageContentMaximumBytes)});
        if (decoded.error != JsonStringDecodeError::output_too_small && !decoded.ok()) {
            return message_failure(GatewayEventDecodeError::invalid_message_content,
                                   context.content.source_offset);
        }
        if (decoded.bytes > kGatewayMessageContentMaximumBytes) {
            return message_failure(GatewayEventDecodeError::message_content_too_large,
                                   context.content.source_offset);
        }
        if (decoded.bytes > content_scratch.count) {
            return message_failure(GatewayEventDecodeError::message_content_capacity,
                                   context.content.source_offset);
        }
        if (!decoded.ok()) {
            return message_failure(GatewayEventDecodeError::invalid_message_content,
                                   context.content.source_offset);
        }
        context.event.content = Utf8View{content_scratch.data, decoded.bytes};
        transient_used = decoded.bytes;
    }
    if ((context.event.fields & gateway_message_field_author_id) != 0) {
        bool policy_explicit = false;
        const GatewayEventDecodeResult author =
            decode_author(context.author, structural_scratch, maximum_depth, content_scratch,
                          &transient_used, capture_names, &context.event.author_id,
                          &context.event.author_name, &context.event.author_avatar_hash,
                          &context.event.author_public_flags, &context.event.author_bot, &policy_explicit);
        if (!author.ok()) {
            return author;
        }
        if (create || policy_explicit)
            context.event.fields |= gateway_message_field_author_policy;
    }
    if (create)
        context.event.fields |= gateway_message_field_interaction_metadata;
    const GatewayEventDecodeResult mentions = decode_mentions(
        &context, structural_scratch, maximum_depth, mention_role_scratch, mention_user_scratch,
        content_scratch, &transient_used, current_user_id, capture_names);
    if (!mentions.ok()) {
        return mentions;
    }
    const GatewayEventDecodeResult member_roles =
        decode_member_roles(&context, structural_scratch, maximum_depth, member_role_scratch);
    if (!member_roles.ok()) {
        return member_roles;
    }
    if (context.call.source.data) {
        const GatewayEventDecodeResult call =
            decode_call(context.call, structural_scratch, maximum_depth, current_user_id, &context.event.call);
        if (!call.ok())
            return call;
    }
    if (capture_rich) {
        const GatewayEventDecodeResult rich =
            gateway_decode_message_rich_fields(data, structural_scratch, maximum_depth, content_scratch,
                                               &transient_used, rich_scratch, &context.event);
        if (!rich.ok()) {
            return rich;
        }
    }
    if (create && capture_rich && context.event.poll.present) {
        constexpr u32 required_poll =
            gateway_message_poll_field_question | gateway_message_poll_field_answers |
            gateway_message_poll_field_expiry | gateway_message_poll_field_multiselect |
            gateway_message_poll_field_layout;
        if ((context.event.poll.fields & required_poll) != required_poll)
            return message_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    context.event.transient_bytes = transient_used;
    *output = context.event;
    return message_failure(GatewayEventDecodeError::none);
}

bool mark_delete_field(DeleteContext* context, u32 field) noexcept {
    if ((context->fields & field) != 0) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->fields |= field;
    return true;
}

bool visit_delete_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<DeleteContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark_delete_field(context, 1u << 0)) {
            return false;
        }
        if (!json_value_to_snowflake(value, &context->event.id)) {
            context->error = GatewayEventDecodeError::invalid_message_id;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!mark_delete_field(context, 1u << 1)) {
            return false;
        }
        if (!json_value_to_snowflake(value, &context->event.channel_id)) {
            context->error = GatewayEventDecodeError::invalid_channel_id;
            return false;
        }
    }
    return true;
}

bool visit_bulk_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<BulkContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("ids"))) {
        if ((context->fields & (1u << 0)) != 0) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->fields |= 1u << 0;
        if (value.kind != JsonValueKind::array) {
            context->error = GatewayEventDecodeError::invalid_message_ids;
            return false;
        }
        context->ids = value;
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if ((context->fields & (1u << 1)) != 0) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->fields |= 1u << 1;
        if (!json_value_to_snowflake(value, &context->event.channel_id)) {
            context->error = GatewayEventDecodeError::invalid_channel_id;
            return false;
        }
    }
    return true;
}

bool visit_bulk_id(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<BulkIdContext*>(opaque);
    if (index >= kGatewayBulkDeleteMaximumIds) {
        context->error = GatewayEventDecodeError::invalid_message_ids;
        return false;
    }
    if (index >= context->ids.count) {
        context->error = GatewayEventDecodeError::message_id_capacity;
        return false;
    }
    if (!json_value_to_snowflake(value, &context->ids[index])) {
        context->error = GatewayEventDecodeError::invalid_message_id;
        return false;
    }
    return true;
}

} // namespace

GatewayEventDecodeResult gateway_decode_message_snapshot_core(JsonValue data, Slice<u32> structural_scratch,
                                                              u32 maximum_depth,
                                                              Slice<char> content_scratch,
                                                              Slice<GatewayMessageNamedUser> mention_users,
                                                              GatewayMessageEvent* output) noexcept {
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    auto result = decode_message(data, structural_scratch, maximum_depth, content_scratch,
                                 {mention_roles, kGatewayMessageMentionRoleMaximumIds},
                                 {member_roles, kGatewayMessageMemberRoleMaximumIds}, mention_users, {}, 0,
                                 mention_users.data != nullptr, false, false, output, true);
    if (!result.ok())
        return result;
    constexpr u32 allowed = gateway_message_field_content | gateway_message_field_timestamp |
                            gateway_message_field_edited_timestamp | gateway_message_field_flags |
                            gateway_message_field_mentions | gateway_message_field_mention_roles |
                            gateway_message_field_mention_everyone | gateway_message_field_type;
    if (output->fields & ~allowed) {
        *output = {};
        return message_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    output->mention_roles = {};
    output->member_roles = {};
    output->mentions_current_user = false;
    return result;
}

GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept {
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    const GatewayEventDecodeResult result = decode_message(
        data, structural_scratch, maximum_depth, content_scratch,
        Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds},
        Slice<u64>{member_roles, kGatewayMessageMemberRoleMaximumIds}, Slice<GatewayMessageNamedUser>{},
        GatewayMessageRichScratch{}, 0, false, false, true, output);
    if (result.ok()) {
        output->mention_roles = ConstSlice<u64>{nullptr, 0};
        output->member_roles = ConstSlice<u64>{nullptr, 0};
    }
    return result;
}

GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch, u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept {
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    const GatewayEventDecodeResult result = decode_message(
        data, structural_scratch, maximum_depth, content_scratch, mention_role_scratch,
        Slice<u64>{member_roles, kGatewayMessageMemberRoleMaximumIds}, Slice<GatewayMessageNamedUser>{},
        GatewayMessageRichScratch{}, current_user_id, false, false, true, output);
    if (result.ok()) {
        output->member_roles = ConstSlice<u64>{nullptr, 0};
    }
    return result;
}

GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept {
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    const GatewayEventDecodeResult result =
        decode_message(data, structural_scratch, maximum_depth, content_scratch, mention_role_scratch,
                       Slice<u64>{member_roles, kGatewayMessageMemberRoleMaximumIds}, mention_user_scratch,
                       GatewayMessageRichScratch{}, current_user_id, true, false, true, output);
    if (result.ok()) {
        output->member_roles = ConstSlice<u64>{nullptr, 0};
    }
    return result;
}

GatewayEventDecodeResult gateway_decode_message_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept {
    return decode_message(data, structural_scratch, maximum_depth, content_scratch, mention_role_scratch,
                          member_role_scratch, mention_user_scratch, rich_scratch, current_user_id, true,
                          true, true, output);
}

GatewayEventDecodeResult gateway_decode_message_update(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept {
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    const GatewayEventDecodeResult result = decode_message(
        data, structural_scratch, maximum_depth, content_scratch,
        Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds},
        Slice<u64>{member_roles, kGatewayMessageMemberRoleMaximumIds}, Slice<GatewayMessageNamedUser>{},
        GatewayMessageRichScratch{}, 0, false, false, false, output);
    if (result.ok()) {
        output->mention_roles = ConstSlice<u64>{nullptr, 0};
        output->member_roles = ConstSlice<u64>{nullptr, 0};
    }
    return result;
}

GatewayEventDecodeResult gateway_decode_message_update(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch, u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept {
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    const GatewayEventDecodeResult result = decode_message(
        data, structural_scratch, maximum_depth, content_scratch, mention_role_scratch,
        Slice<u64>{member_roles, kGatewayMessageMemberRoleMaximumIds}, Slice<GatewayMessageNamedUser>{},
        GatewayMessageRichScratch{}, current_user_id, false, false, false, output);
    if (result.ok()) {
        output->member_roles = ConstSlice<u64>{nullptr, 0};
    }
    return result;
}

GatewayEventDecodeResult gateway_decode_message_update(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept {
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    const GatewayEventDecodeResult result =
        decode_message(data, structural_scratch, maximum_depth, content_scratch, mention_role_scratch,
                       Slice<u64>{member_roles, kGatewayMessageMemberRoleMaximumIds}, mention_user_scratch,
                       GatewayMessageRichScratch{}, current_user_id, true, false, false, output);
    if (result.ok()) {
        output->member_roles = ConstSlice<u64>{nullptr, 0};
    }
    return result;
}

GatewayEventDecodeResult gateway_decode_message_update(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept {
    return decode_message(data, structural_scratch, maximum_depth, content_scratch, mention_role_scratch,
                          member_role_scratch, mention_user_scratch, rich_scratch, current_user_id, true,
                          true, false, output);
}

GatewayEventDecodeResult gateway_decode_message_delete(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth,
                                                       GatewayMessageDeleteEvent* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0) {
        return message_failure(GatewayEventDecodeError::invalid_input);
    }
    *output = GatewayMessageDeleteEvent{};
    if (data.kind != JsonValueKind::object) {
        return message_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    DeleteContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_delete_member, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? message_failure(context.error, read.error_offset, JsonScanError::none, read.error)
                   : message_failure(GatewayEventDecodeError::invalid_json, read.error_offset,
                                     JsonScanError::none, read.error);
    }
    if (context.fields != 3u) {
        return message_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    *output = context.event;
    return message_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_message_bulk_delete(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<u64> ids,
    GatewayMessageBulkDeleteEvent* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0 || (!ids.data && ids.count != 0)) {
        return message_failure(GatewayEventDecodeError::invalid_input);
    }
    *output = GatewayMessageBulkDeleteEvent{};
    if (data.kind != JsonValueKind::object) {
        return message_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    BulkContext context{};
    JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_bulk_member, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? message_failure(context.error, read.error_offset, JsonScanError::none, read.error)
                   : message_failure(GatewayEventDecodeError::invalid_json, read.error_offset,
                                     JsonScanError::none, read.error);
    }
    if (context.fields != 3u) {
        return message_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }

    scan = json_scan_structurals_avx1(context.ids.source, structural_scratch);
    if (!scan.ok()) {
        return message_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    BulkIdContext id_context{ids, GatewayEventDecodeError::none};
    read = json_visit_value_array(context.ids, ConstSlice<u32>{structural_scratch.data, scan.count},
                                 maximum_depth, visit_bulk_id, &id_context);
    if (!read.ok()) {
        return id_context.error != GatewayEventDecodeError::none
                   ? message_failure(id_context.error, read.error_offset, JsonScanError::none, read.error)
                   : message_failure(GatewayEventDecodeError::invalid_json, read.error_offset,
                                     JsonScanError::none, read.error);
    }
    if (read.member_count == 0 || read.member_count > kGatewayBulkDeleteMaximumIds) {
        return message_failure(GatewayEventDecodeError::invalid_message_ids, context.ids.source_offset);
    }
    context.event.id_count = read.member_count;
    *output = context.event;
    return message_failure(GatewayEventDecodeError::none);
}

} // namespace maicord
