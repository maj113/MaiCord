#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "gateway_decode.hpp"

namespace maicord {
namespace {

using protocol_detail::accept_member;
using protocol_detail::decode_failure;
using protocol_detail::mark_seen;
using protocol_detail::read_failure;

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
        JsonString text{};
        const bool null = value.kind == JsonValueKind::null_value;
        if (!accept_member(&context->seen, 1,
                           null || (json_value_as_string(value, &text) &&
                                    parse_iso_timestamp(text, &context->ended_timestamp_ms)),
                           &context->error))
            return false;
        context->ended = !null;
    } else if (json_string_equals_ascii(name, utf8_literal("participants"))) {
        if (!accept_member(&context->seen, 2, value.kind == JsonValueKind::array, &context->error))
            return false;
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
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    CallContext context{};
    context.current_user_id = current_user_id;
    JsonReadResult read = json_visit_value_object(call, {structural_scratch.data, scan.count},
                                                 maximum_depth, visit_call_member, &context);
    if (read.ok() && (context.seen & 2u)) {
        const JsonScanResult inner = json_scan_structurals_avx1(context.participants.source, structural_scratch);
        if (!inner.ok())
            return decode_failure(GatewayEventDecodeError::scan_failed, inner.error_offset, inner.error);
        read = json_visit_value_array(context.participants, {structural_scratch.data, inner.count},
                                     maximum_depth, visit_call_participant, &context);
    }
    if (!read.ok())
        return read_failure(context.error, read);
    *output = GatewayMessageCall{context.ended_timestamp_ms, true, context.ended, context.participated};
    return GatewayEventDecodeResult{};
}

struct AuthorContext {
    u64 id;
    JsonString username;
    JsonString global_name;
    JsonString avatar;
    GatewayEventDecodeError error;
    u32 seen;
    bool global_name_null;
    bool avatar_null;
    u32 public_flags;
    bool bot;
    u32 private_flags;
};
constexpr u32 author_seen_id = 1, author_seen_username = 2, author_seen_global_name = 4,
              author_seen_avatar = 8, author_seen_public_flags = 16, author_seen_bot = 32,
              author_seen_private_flags = 64;

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
    JsonString nick;
    GatewayEventDecodeError error;
    bool roles_seen;
    bool nick_seen;
};

bool json_value_to_message_nonce(JsonValue value, u64* output) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *output = 0;
        return true;
    }
    if (value.kind == JsonValueKind::string) {
        JsonString string{};
        if (!json_value_as_string(value, &string))
            return false;
        // Other clients use opaque string nonces. They cannot match the outbox
        // and must not reject an otherwise valid history page.
        if (!json_string_to_u64(string, output))
            *output = 0;
        return true;
    }
    return json_value_to_u64(value, output);
}

bool visit_message_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<MessageContext*>(opaque);
    auto& event = context->event;
    auto& error = context->error;
    u64 number = 0;
    if (json_string_equals_ascii(name, utf8_literal("type"))) {
        if (!accept_member(&event.fields, gateway_message_field_type,
                           json_value_to_u64(value, &number) && number <= 65535, &error))
            return false;
        event.type = static_cast<u16>(number);
    } else if (json_string_equals_ascii(name, utf8_literal("activity_instance")) ||
               json_string_equals_ascii(name, utf8_literal("application_id")) ||
               json_string_equals_ascii(name, utf8_literal("changelog_id"))) {
        const bool activity = json_string_equals_ascii(name, utf8_literal("activity_instance"));
        const bool application = json_string_equals_ascii(name, utf8_literal("application_id"));
        const u32 field = activity      ? gateway_message_field_activity_instance
                          : application ? gateway_message_field_application_id
                                        : gateway_message_field_changelog_id;
        if (!mark_seen(&event.fields, field, &error))
            return false;
        if (activity)
            event.activity_instance_present = value.kind != JsonValueKind::null_value;
        else if (application)
            event.application_present = value.kind != JsonValueKind::null_value;
        else
            event.changelog_present = value.kind != JsonValueKind::null_value;
    } else if (json_string_equals_ascii(name, utf8_literal("interaction_metadata"))) {
        if (!mark_seen(&event.fields, gateway_message_field_interaction_metadata, &error))
            return false;
        event.interaction_metadata_present = value.kind != JsonValueKind::null_value;
    } else if (json_string_equals_ascii(name, utf8_literal("poll")) ||
               json_string_equals_ascii(name, utf8_literal("call")) ||
               json_string_equals_ascii(name, utf8_literal("activity"))) {
        const bool poll = json_string_equals_ascii(name, utf8_literal("poll"));
        const bool call = json_string_equals_ascii(name, utf8_literal("call"));
        const u32 field = poll   ? gateway_message_field_poll
                          : call ? gateway_message_field_call
                                 : gateway_message_field_activity;
        const bool object = value.kind == JsonValueKind::object;
        if (!accept_member(&event.fields, field, object || value.kind == JsonValueKind::null_value, &error))
            return false;
        if (poll)
            event.poll.present = object;
        if (call && object)
            context->call = value;
        if (object)
            event.forwarding_features |= poll   ? gateway_message_forwarding_poll
                                         : call ? gateway_message_forwarding_call
                                                : gateway_message_forwarding_activity;
    } else if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!accept_member(&event.fields, gateway_message_field_id,
                           json_value_to_snowflake(value, &event.id), &error,
                           GatewayEventDecodeError::invalid_message_id))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (!accept_member(&event.fields, gateway_message_field_channel_id,
                           json_value_to_snowflake(value, &event.channel_id), &error,
                           GatewayEventDecodeError::invalid_channel_id))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("author"))) {
        if (!accept_member(&event.fields, gateway_message_field_author_id,
                           value.kind == JsonValueKind::object, &error,
                           GatewayEventDecodeError::invalid_author))
            return false;
        context->author = value;
    } else if (json_string_equals_ascii(name, utf8_literal("content"))) {
        if (!accept_member(&event.fields, gateway_message_field_content,
                           json_value_as_string(value, &context->content), &error,
                           GatewayEventDecodeError::invalid_message_content))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("timestamp"))) {
        JsonString timestamp{};
        if (!accept_member(&event.fields, gateway_message_field_timestamp,
                           json_value_as_string(value, &timestamp) &&
                               parse_iso_timestamp(timestamp, &event.timestamp_ms),
                           &error, GatewayEventDecodeError::invalid_message_timestamp))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("edited_timestamp"))) {
        JsonString timestamp{};
        const bool null = value.kind == JsonValueKind::null_value;
        if (!accept_member(&event.fields, gateway_message_field_edited_timestamp,
                           null || (json_value_as_string(value, &timestamp) &&
                                    parse_iso_timestamp(timestamp, &event.edited_timestamp_ms)),
                           &error, GatewayEventDecodeError::invalid_edited_timestamp))
            return false;
        if (null)
            event.edited_timestamp_ms = 0;
    } else if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        if (!accept_member(&event.fields, gateway_message_field_flags,
                           json_value_to_u64(value, &number) && number <= 0xFFFFFFFFull, &error,
                           GatewayEventDecodeError::invalid_message_flags))
            return false;
        event.flags = static_cast<u32>(number);
    } else if (json_string_equals_ascii(name, utf8_literal("nonce"))) {
        if (!accept_member(&event.fields, gateway_message_field_nonce,
                           json_value_to_message_nonce(value, &event.nonce), &error,
                           GatewayEventDecodeError::invalid_message_nonce))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("pinned"))) {
        if (!accept_member(&event.fields, gateway_message_field_pinned,
                           json_value_to_bool(value, &event.pinned), &error))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("mention_everyone"))) {
        if (!accept_member(&event.fields, gateway_message_field_mention_everyone,
                           json_value_to_bool(value, &event.mention_everyone), &error))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("mentions"))) {
        if (!accept_member(&event.fields, gateway_message_field_mentions,
                           value.kind == JsonValueKind::array, &error))
            return false;
        context->mentions = value;
    } else if (json_string_equals_ascii(name, utf8_literal("mention_roles"))) {
        if (!accept_member(&event.fields, gateway_message_field_mention_roles,
                           value.kind == JsonValueKind::array, &error))
            return false;
        context->mention_roles = value;
    } else if (json_string_equals_ascii(name, utf8_literal("member"))) {
        if (!mark_seen(&event.fields, gateway_message_field_member, &error) ||
            value.kind != JsonValueKind::object) {
            error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->member = value;
    }
    return true;
}

bool visit_author_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<AuthorContext*>(opaque);
    const bool null = value.kind == JsonValueKind::null_value;
    u64 flags = 0;
    // Public and private flags combine; keep the low policy bits (including
    // SPAMMER) and accept unknown high flags.
    if (json_string_equals_ascii(name, utf8_literal("public_flags"))) {
        if (!accept_member(&context->seen, author_seen_public_flags, json_value_to_u64(value, &flags),
                           &context->error))
            return false;
        context->public_flags = static_cast<u32>(flags);
    } else if (json_string_equals_ascii(name, utf8_literal("flags"))) {
        if (!accept_member(&context->seen, author_seen_private_flags, json_value_to_u64(value, &flags),
                           &context->error))
            return false;
        context->private_flags = static_cast<u32>(flags);
    } else if (json_string_equals_ascii(name, utf8_literal("bot"))) {
        if (!accept_member(&context->seen, author_seen_bot, json_value_to_bool(value, &context->bot),
                           &context->error))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!accept_member(&context->seen, author_seen_id, json_value_to_snowflake(value, &context->id),
                           &context->error, GatewayEventDecodeError::invalid_author_id))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("username"))) {
        if (!accept_member(&context->seen, author_seen_username,
                           json_value_as_string(value, &context->username), &context->error,
                           GatewayEventDecodeError::invalid_user_name))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("global_name"))) {
        context->global_name_null = null;
        if (!accept_member(&context->seen, author_seen_global_name,
                           null || json_value_as_string(value, &context->global_name), &context->error,
                           GatewayEventDecodeError::invalid_user_name))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("avatar"))) {
        context->avatar_null = null;
        if (!accept_member(&context->seen, author_seen_avatar,
                           null || json_value_as_string(value, &context->avatar), &context->error))
            return false;
    }
    return true;
}

// Decodes `text` into output after the first *used bytes, at most `maximum`
// bytes long. Empty text leaves *view unset, and is invalid when required.
GatewayEventDecodeResult decode_text(JsonString text, Slice<char> output, u32* used, u32 maximum,
                                     bool require_nonempty, GatewayEventDecodeError invalid, u32 offset,
                                     Utf8View* view) noexcept {
    const JsonStringDecodeResult size = json_string_decode_utf8(text, Slice<char>{nullptr, 0});
    const bool capacity_error = *used > output.count || size.bytes > output.count - *used;
    if ((size.error != JsonStringDecodeError::output_too_small && !(size.ok() && size.bytes == 0)) ||
        (require_nonempty && size.bytes == 0) || size.bytes > maximum || capacity_error) {
        return decode_failure(capacity_error ? GatewayEventDecodeError::message_content_capacity : invalid,
                              offset);
    }
    if (size.bytes != 0) {
        const JsonStringDecodeResult decoded =
            json_string_decode_utf8(text, Slice<char>{output.data + *used, output.count - *used});
        if (!decoded.ok() || decoded.bytes != size.bytes) {
            return decode_failure(invalid, offset);
        }
        *view = Utf8View{output.data + *used, decoded.bytes};
        *used += decoded.bytes;
    }
    return decode_failure(GatewayEventDecodeError::none);
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
            return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        read = json_visit_value_object(author, {structural_scratch.data, scan.count}, maximum_depth,
                                      visit_author_member, &context);
    }
    if (!read.ok()) {
        return read_failure(context.error, read);
    }
    if (!(context.seen & author_seen_id)) {
        return decode_failure(GatewayEventDecodeError::invalid_author_id, author.source_offset);
    }
    *author_id = context.id;
    if (public_flags)
        *public_flags = context.public_flags | context.private_flags;
    if (bot)
        *bot = context.bot;
    if (policy_explicit)
        *policy_explicit = (context.seen & (author_seen_public_flags | author_seen_bot)) ==
                           (author_seen_public_flags | author_seen_bot);
    if (capture_name) {
        const JsonString selected = (context.seen & author_seen_global_name) && !context.global_name_null
                                        ? context.global_name
                                        : context.username;
        if (selected.encoded.data) {
            const GatewayEventDecodeResult name =
                decode_text(selected, name_output, used, kGatewayMessageUserNameMaximumBytes, true,
                            GatewayEventDecodeError::invalid_user_name, author.source_offset, author_name);
            if (!name.ok())
                return name;
        }
    }
    if (avatar_hash && (context.seen & author_seen_avatar) && !context.avatar_null) {
        return decode_text(context.avatar, name_output, used, kGatewayMessageAvatarHashMaximumBytes, false,
                           GatewayEventDecodeError::invalid_data, author.source_offset, avatar_hash);
    }
    return decode_failure(GatewayEventDecodeError::none);
}

bool visit_mention_user(void* opaque, u32, JsonValue value) noexcept {
    auto* context = static_cast<MentionUserContext*>(opaque);
    if (value.kind != JsonValueKind::object) {
        context->error = GatewayEventDecodeError::invalid_author;
        return false;
    }
    u64 id = 0;
    Utf8View name{};
    // Mentions past the capacity still validate and count toward the current
    // user's mention, but keep no name.
    const bool keep = context->capture_names && context->count < kGatewayMessageMentionUserMaximum &&
                      context->count < context->users.count;
    const GatewayEventDecodeResult decoded = decode_author(
        value, context->structural_scratch, context->maximum_depth, context->name_output, context->used,
        keep, &id, &name, nullptr, nullptr, nullptr, nullptr, &context->document);
    if (!decoded.ok()) {
        context->error = decoded.error;
        return false;
    }
    context->found = context->found || (context->current_user_id != 0 && id == context->current_user_id);
    if (keep) {
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

// Role IDs past the capacity are validated but not kept.
bool visit_mention_role(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<MentionRoleContext*>(opaque);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    if (index >= minimum(kGatewayMessageMentionRoleMaximumIds, context->ids.count))
        return true;
    for (u32 previous = 0; previous < index; ++previous) {
        if (context->ids[previous] == id) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
    }
    context->ids[index] = id;
    context->count = index + 1;
    return true;
}

// Member roles are kept whole or not at all: past the scratch (a shared
// history pool) they are validated and reported partial.
struct MemberRoleContext {
    Slice<u64> ids;
    GatewayEventDecodeError error;
    u32 count;
    bool partial;
};

bool visit_member_role(void* opaque, u32 index, JsonValue value) noexcept {
    auto* context = static_cast<MemberRoleContext*>(opaque);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id) || index >= kGatewayMessageMemberRoleMaximumIds) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    if (index >= context->ids.count) {
        context->partial = true;
        return true;
    }
    for (u32 previous = 0; previous < index; ++previous) {
        if (context->ids[previous] == id) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
    }
    context->ids[index] = id;
    context->count = index + 1;
    return true;
}

bool visit_member_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<MemberContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("nick"))) {
        if (context->nick_seen) {
            context->error = GatewayEventDecodeError::duplicate_field;
            return false;
        }
        context->nick_seen = true;
        if (value.kind != JsonValueKind::null_value && !json_value_as_string(value, &context->nick)) {
            context->error = GatewayEventDecodeError::invalid_user_name;
            return false;
        }
        return true;
    }
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

// Decodes member.roles, and member.nick into name_output right after the
// author's name and avatar hash.
GatewayEventDecodeResult decode_member(MessageContext* context, Slice<u32> structural_scratch,
                                       u32 maximum_depth, Slice<u64> member_role_scratch,
                                       Slice<char> name_output, u32* used) noexcept {
    if ((context->event.fields & gateway_message_field_member) == 0) {
        return decode_failure(GatewayEventDecodeError::none);
    }
    const JsonScanResult member_scan =
        json_scan_structurals_avx1(context->member.source, structural_scratch);
    if (!member_scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, member_scan.error_offset,
                              member_scan.error);
    }
    MemberContext member{};
    const JsonReadResult member_read = json_visit_value_object(
        context->member, ConstSlice<u32>{structural_scratch.data, member_scan.count}, maximum_depth,
        visit_member_field, &member);
    if (!member_read.ok()) {
        return read_failure(member.error, member_read);
    }
    if (!member.roles_seen) {
        return decode_failure(GatewayEventDecodeError::missing_required_field,
                              context->member.source_offset);
    }
    if (member.nick.encoded.data) {
        const GatewayEventDecodeResult nick =
            decode_text(member.nick, name_output, used, kGatewayMessageUserNameMaximumBytes, false,
                        GatewayEventDecodeError::invalid_user_name, context->member.source_offset,
                        &context->event.member_nick);
        if (!nick.ok())
            return nick;
    }
    const JsonScanResult role_scan = json_scan_structurals_avx1(member.roles.source, structural_scratch);
    if (!role_scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, role_scan.error_offset,
                              role_scan.error);
    }
    MemberRoleContext roles{member_role_scratch, GatewayEventDecodeError::none, 0, false};
    const JsonReadResult role_read = json_visit_value_array(
        member.roles, ConstSlice<u32>{structural_scratch.data, role_scan.count}, maximum_depth,
        visit_member_role, &roles);
    if (!role_read.ok()) {
        return read_failure(roles.error, role_read);
    }
    context->event.member_roles_partial = roles.partial;
    context->event.member_roles = ConstSlice<u64>{member_role_scratch.data, roles.partial ? 0u : roles.count};
    return decode_failure(GatewayEventDecodeError::none);
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
            return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
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
            return read_failure(users.error, read);
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
            return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        }
        MentionRoleContext roles{mention_role_scratch, GatewayEventDecodeError::none, 0};
        const JsonReadResult read = json_visit_value_array(
            context->mention_roles, ConstSlice<u32>{structural_scratch.data, scan.count},
            maximum_depth, visit_mention_role, &roles);
        if (!read.ok()) {
            return read_failure(roles.error, read);
        }
        if (roles.count != minimum(read.member_count, minimum(kGatewayMessageMentionRoleMaximumIds,
                                                              mention_role_scratch.count))) {
            return decode_failure(GatewayEventDecodeError::invalid_data,
                                  context->mention_roles.source_offset);
        }
        context->event.mention_roles = ConstSlice<u64>{mention_role_scratch.data, roles.count};
    }
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult decode_message(JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth,
                                        Slice<char> content_scratch, Slice<u64> mention_role_scratch,
                                        Slice<u64> member_role_scratch,
                                        Slice<GatewayMessageNamedUser> mention_user_scratch,
                                        const GatewayMessageRichScratch& rich_scratch, u64 current_user_id,
                                        bool capture_names, bool capture_rich, bool create,
                                        GatewayMessageEvent* output, bool snapshot_core = false) noexcept {
    reset_value<GatewayMessageEvent>(output);
    if (data.kind != JsonValueKind::object) {
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    MessageContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_message_member, &context);
    if (!read.ok()) {
        return read_failure(context.error, read);
    }

    constexpr u32 update_required = gateway_message_field_id | gateway_message_field_channel_id;
    constexpr u32 create_required = update_required | gateway_message_field_author_id |
                                    gateway_message_field_content | gateway_message_field_timestamp;
    const u32 required = snapshot_core ? 0u : (create ? create_required : update_required);
    if ((context.event.fields & required) != required) {
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    u32 transient_used = 0;
    if ((context.event.fields & gateway_message_field_content) != 0) {
        const JsonStringDecodeResult decoded = json_string_decode_utf8(
            context.content,
            Slice<char>{content_scratch.data,
                        minimum(content_scratch.count, kGatewayMessageContentMaximumBytes)});
        if (decoded.error != JsonStringDecodeError::output_too_small && !decoded.ok()) {
            return decode_failure(GatewayEventDecodeError::invalid_message_content,
                                  context.content.source_offset);
        }
        if (decoded.bytes > kGatewayMessageContentMaximumBytes) {
            return decode_failure(GatewayEventDecodeError::message_content_too_large,
                                  context.content.source_offset);
        }
        if (decoded.bytes > content_scratch.count) {
            return decode_failure(GatewayEventDecodeError::message_content_capacity,
                                  context.content.source_offset);
        }
        if (!decoded.ok()) {
            return decode_failure(GatewayEventDecodeError::invalid_message_content,
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
    // The nickname follows the author's name and avatar hash in the transient text.
    const GatewayEventDecodeResult member = decode_member(
        &context, structural_scratch, maximum_depth, member_role_scratch, content_scratch, &transient_used);
    if (!member.ok()) {
        return member;
    }
    const GatewayEventDecodeResult mentions = decode_mentions(
        &context, structural_scratch, maximum_depth, mention_role_scratch, mention_user_scratch,
        content_scratch, &transient_used, current_user_id, capture_names);
    if (!mentions.ok()) {
        return mentions;
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
            return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    context.event.transient_bytes = transient_used;
    copy_value(output, context.event);
    return decode_failure(GatewayEventDecodeError::none);
}

bool visit_delete_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<DeleteContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        return accept_member(&context->fields, 1, json_value_to_snowflake(value, &context->event.id),
                             &context->error, GatewayEventDecodeError::invalid_message_id);
    }
    if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        return accept_member(&context->fields, 2,
                             json_value_to_snowflake(value, &context->event.channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
    }
    return true;
}

bool visit_bulk_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<BulkContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("ids"))) {
        if (!accept_member(&context->fields, 1, value.kind == JsonValueKind::array, &context->error,
                           GatewayEventDecodeError::invalid_message_ids))
            return false;
        context->ids = value;
    } else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        return accept_member(&context->fields, 2,
                             json_value_to_snowflake(value, &context->event.channel_id), &context->error,
                             GatewayEventDecodeError::invalid_channel_id);
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
        reset_value(output);
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    reset_value(&output->mention_roles);
    reset_value(&output->member_roles);
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
    reset_value<GatewayMessageDeleteEvent>(output);
    if (data.kind != JsonValueKind::object) {
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    DeleteContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_delete_member, &context);
    if (!read.ok()) {
        return read_failure(context.error, read);
    }
    if (context.fields != 3u) {
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    *output = context.event;
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_message_bulk_delete(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<u64> ids,
    GatewayMessageBulkDeleteEvent* output) noexcept {
    reset_value<GatewayMessageBulkDeleteEvent>(output);
    if (data.kind != JsonValueKind::object) {
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    BulkContext context{};
    JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_bulk_member, &context);
    if (!read.ok()) {
        return read_failure(context.error, read);
    }
    if (context.fields != 3u) {
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }

    scan = json_scan_structurals_avx1(context.ids.source, structural_scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    BulkIdContext id_context{ids, GatewayEventDecodeError::none};
    read = json_visit_value_array(context.ids, ConstSlice<u32>{structural_scratch.data, scan.count},
                                 maximum_depth, visit_bulk_id, &id_context);
    if (!read.ok()) {
        return read_failure(id_context.error, read);
    }
    if (read.member_count == 0 || read.member_count > kGatewayBulkDeleteMaximumIds) {
        return decode_failure(GatewayEventDecodeError::invalid_message_ids, context.ids.source_offset);
    }
    context.event.id_count = read.member_count;
    *output = context.event;
    return decode_failure(GatewayEventDecodeError::none);
}

} // namespace maicord
