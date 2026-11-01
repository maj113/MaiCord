#include "state_dispatch_internal.hpp"
#include "maicord/protocol/gateway/message_events.hpp"
#include "dispatch_common.hpp"

namespace maicord {
namespace {

using protocol_detail::wide_member_index;

static_assert(sizeof(GatewayClientStateMutation) == 168);
static_assert(sizeof(GatewayClientRoleMutation) == 40);
static_assert(sizeof(GatewayClientPermissionOverwriteMutation) == 32);
static_assert(sizeof(GatewayClientEmojiMutation) == 16);

} // namespace
namespace state_dispatch_detail {

GatewayClientDispatchRouteResult route_detail(GatewayClientDispatchRouteError error,
                                              GatewayEventDecodeError event_error, u32 detail) noexcept {
    GatewayClientDispatchRouteResult result = route_error(error, event_error);
    result.detail = detail;
    return result;
}

// A failed append reports invalid data as lack of payload room, anything else as a decode error.
GatewayClientDispatchRouteResult append_failure(GatewayEventDecodeError error, u32 detail) noexcept {
    return route_detail(error == GatewayEventDecodeError::invalid_data
                            ? GatewayClientDispatchRouteError::payload_capacity
                            : GatewayClientDispatchRouteError::decode,
                        error, detail);
}

GatewayEventDecodeResult decode_error(GatewayEventDecodeError error, u32 offset,
                                      JsonScanError scan_error,
                                      JsonReadError read_error) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

GatewayClientDispatchRouteResult decode_route(GatewayEventDecodeError error) noexcept {
    return route_error(GatewayClientDispatchRouteError::decode, error);
}

// A failed read reports the error its visitor recorded, else `fallback`.
GatewayEventDecodeResult read_failure(GatewayEventDecodeError error, const JsonReadResult& read,
                                      GatewayEventDecodeError fallback) noexcept {
    return decode_error(error != GatewayEventDecodeError::none ? error : fallback, read.error_offset,
                        JsonScanError::none, read.error);
}

// Scans `array` into `scratch` and visits its elements; `error` is the visitor's recorded error.
GatewayEventDecodeResult visit_array(JsonValue array, Slice<u32> scratch, u32 maximum_depth,
                                     JsonArrayVisitor visit, void* context,
                                     const GatewayEventDecodeError& error,
                                     GatewayEventDecodeError fallback) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(array.source, scratch);
    if (!scan.ok())
        return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    const JsonReadResult read =
        json_visit_value_array(array, ConstSlice<u32>{scratch.data, scan.count}, maximum_depth, visit, context);
    return read.ok() ? decode_error(GatewayEventDecodeError::none) : read_failure(error, read, fallback);
}

} // namespace state_dispatch_detail
namespace {

bool mark_field(StateObjectContext* context, u16 field) noexcept {
    if ((context->mutation.fields & field) != 0)
        return gateway_duplicate_member(&context->error);
    context->mutation.fields |= field;
    return true;
}

bool value_to_permission(JsonValue value, u64* output) noexcept {
    JsonString string{};
    return json_value_as_string(value, &string) && json_string_to_u64(string, output);
}

bool reject(StateObjectContext* context) noexcept {
    if (context->error == GatewayEventDecodeError::none) {
        context->error = GatewayEventDecodeError::invalid_data;
    }
    return false;
}

// A snowflake member recorded once; `nullable` accepts null as "none".
bool snowflake_member(StateObjectContext* context, u16 field, JsonValue value, u64* output,
                      bool nullable = false) noexcept {
    return (mark_field(context, field) &&
            ((nullable && value.kind == JsonValueKind::null_value) || json_value_to_snowflake(value, output))) ||
           reject(context);
}

bool flag_member(StateObjectContext* context, u16 field, JsonValue value, bool* output) noexcept {
    return (mark_field(context, field) && json_value_to_bool(value, output)) || reject(context);
}

// A number within `maximum`; any other value leaves the field absent for that record.
bool bounded_member(StateObjectContext* context, u16 field, JsonValue value, u64 maximum,
                    u32* output) noexcept {
    u64 decoded = 0;
    if (!mark_field(context, field))
        return false;
    if (!json_value_to_u64(value, &decoded) || decoded > maximum)
        context->mutation.fields = static_cast<u16>(context->mutation.fields & ~field);
    else
        *output = static_cast<u32>(decoded);
    return true;
}

} // namespace
namespace state_dispatch_detail {

// The current member's GuildSelfMember bits: "pending" is read with the
// settings keys below, the member flags here once the object is decoded.
void member_state_from_flags(GatewayClientStateMutation* m) noexcept {
    if ((m->fields & gateway_client_state_field_flags) == 0)
        return;
    m->guild_settings_known = static_cast<u16>(m->guild_settings_known | guild_settings_self_member |
                                               guild_settings_self_quarantine);
    if (m->flags & 2u)
        m->self_member |= guild_self_member_onboarding_completed;
    if (m->flags & 8u)
        m->self_member |= guild_self_member_onboarding_started;
    if (guild_member_flags_quarantined(m->flags))
        m->self_member |= guild_self_member_quarantined;
}

} // namespace state_dispatch_detail
namespace {

// Guild settings and member timeout keys. Null channel IDs and a null timeout
// are explicit observations of "none".
bool visit_guild_setting(StateObjectContext* context, JsonString name, JsonValue value, bool* handled) noexcept {
    const u32 index = wide_member_index<"pending", "mfa_level", "verification_level", "default_message_notifications",
                                   "explicit_content_filter", "system_channel_id", "afk_channel_id", "afk_timeout",
                                   "communication_disabled_until">(name);
    if (index == 0) {
        *handled = true;
        bool pending = false;
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (!json_value_to_bool(value, &pending))
            return reject(context);
        auto& m = context->mutation;
        m.guild_settings_known = static_cast<u16>(m.guild_settings_known | guild_settings_self_member);
        if (pending)
            m.self_member |= guild_self_member_pending;
        return true;
    }
    struct Key {
        u16 field;
        u8 maximum;
    };
    // Runs for every member of every visited state object.
    static constexpr Key keys[] = {
        {guild_settings_mfa_level, 1},
        {guild_settings_verification_level, 4},
        {guild_settings_default_notifications, 1},
        {guild_settings_explicit_content_filter, 2},
        {guild_settings_system_channel, 0},
        {guild_settings_afk_channel, 0},
        {guild_settings_afk_timeout, 0},
        {guild_settings_self_timeout, 0},
    };
    if (index < 9) {
        const Key& key = keys[index - 1];
        *handled = true;
        auto& m = context->mutation;
        if (m.guild_settings_known & key.field)
            return gateway_duplicate_member(&context->error);
        m.guild_settings_known = static_cast<u16>(m.guild_settings_known | key.field);
        u64 number = 0;
        switch (key.field) {
        case guild_settings_system_channel:
        case guild_settings_afk_channel: {
            u64& id = key.field == guild_settings_system_channel ? m.system_channel_id : m.afk_channel_id;
            return value.kind == JsonValueKind::null_value || json_value_to_snowflake(value, &id) ||
                   reject(context);
        }
        case guild_settings_afk_timeout:
            // Presentation-only setting: an unexpected value stays unknown.
            if (!json_value_to_u64(value, &number) || number > 86'400) {
                m.guild_settings_known = static_cast<u16>(m.guild_settings_known & ~key.field);
                return true;
            }
            m.afk_timeout = static_cast<u32>(number);
            return true;
        case guild_settings_self_timeout: {
            if (value.kind == JsonValueKind::null_value)
                return true;
            JsonString stamp{};
            u64 milliseconds = 0;
            if (!json_value_as_string(value, &stamp) || !parse_iso_timestamp(stamp, &milliseconds) ||
                milliseconds / 1000 > 0xFFFFFFFFull)
                return reject(context);
            m.self_communication_disabled_until = static_cast<u32>(milliseconds / 1000);
            return true;
        }
        default:
            // The API adds enum levels over time; an unknown level leaves the
            // setting unknown instead of rejecting the whole guild.
            if (!json_value_to_u64(value, &number) || number > key.maximum) {
                m.guild_settings_known = static_cast<u16>(m.guild_settings_known & ~key.field);
                return true;
            }
            (key.field == guild_settings_mfa_level            ? m.mfa_level
             : key.field == guild_settings_verification_level ? m.verification_level
             : key.field == guild_settings_default_notifications
                 ? m.default_message_notifications
                 : m.explicit_content_filter) = static_cast<u8>(number);
            return true;
        }
    }
    return true;
}

bool visit_state_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<StateObjectContext*>(opaque);
    bool setting = false;
    if (!visit_guild_setting(context, name, value, &setting))
        return false;
    if (setting || gateway_guild_detail_visit(&context->details, name, value))
        return true;
    // Ordering, flag and color values outside the native range (a negative
    // position, a future 64-bit flag) degrade to "absent" for that record.
    const bool array = value.kind == JsonValueKind::array;
    const bool object = value.kind == JsonValueKind::object;
    switch (wide_member_index<"id", "name", "type", "guild_id", "parent_id", "flags", "position", "permissions",
                         "last_message_id", "user", "roles", "channels", "members", "threads", "emojis",
                         "permission_overwrites", "features", "icon", "banner", "owner_id", "role_id", "color",
                         "colors", "tags", "hoist", "managed", "mentionable", "unicode_emoji", "allow", "deny",
                         "rate_limit_per_user", "nsfw", "nsfw_level", "member", "thread_metadata", "newly_created",
                         "member_list_id", "animated", "available", "require_colons", "joined_at", "unavailable",
                         "role", "nicks", "recipients", "recipient_ids", "properties", "is_message_request",
                         "is_spam", "safety_warnings", "default_auto_archive_duration">(name)) {
    case 0:
    case 20:
        return snowflake_member(context, gateway_client_state_field_id, value, &context->mutation.id);
    case 1:
        if (!mark_field(context, gateway_client_state_field_name))
            return false;
        if (value.kind == JsonValueKind::null_value) {
            context->name_null = true;
            return true;
        }
        return json_value_as_string(value, &context->name) ? true : reject(context);
    case 2: {
        u64 decoded = 0;
        if (!mark_field(context, gateway_client_state_field_type) || !json_value_to_u64(value, &decoded) ||
            decoded > 0xFFu) {
            return reject(context);
        }
        context->mutation.type = static_cast<u8>(decoded);
        return true;
    }
    case 3:
        return snowflake_member(context, gateway_client_state_field_guild_id, value, &context->mutation.guild_id);
    case 4:
        return snowflake_member(context, gateway_client_state_field_parent_id, value,
                                &context->mutation.parent_id, true);
    case 5:
        return bounded_member(context, gateway_client_state_field_flags, value, 0xFFFFFFFFull,
                              &context->mutation.flags);
    case 6:
        return bounded_member(context, gateway_client_state_field_position, value, 0xFFFFFFFFull,
                              &context->mutation.position);
    case 7:
        return mark_field(context, gateway_client_state_field_permissions) &&
                       value_to_permission(value, &context->mutation.permissions)
                   ? true
                   : reject(context);
    case 8:
        return snowflake_member(context, gateway_client_state_field_last_message_id, value,
                                &context->mutation.last_message_id, true);
    case 9:
        return gateway_keep_member(&context->error, &context->user_seen, &context->user, value, object);
    case 10:
        return gateway_keep_member(&context->error, &context->roles_seen, &context->roles, value, array);
    case 11:
        return gateway_keep_member(&context->error, &context->channels_seen, &context->channels, value, array);
    case 12:
        return gateway_keep_member(&context->error, &context->members_seen, &context->members, value, array);
    case 13:
        return gateway_keep_member(&context->error, &context->threads_seen, &context->threads, value, array);
    case 14:
        return gateway_keep_member(&context->error, &context->emojis_seen, &context->emojis, value,
                                   array || object);
    case 15:
        if (!mark_field(context, gateway_client_state_field_permission_overwrites) || !array)
            return reject(context);
        context->permission_overwrites_seen = true;
        context->permission_overwrites = value;
        return true;
    case 16:
        if (context->features_seen)
            return reject(context);
        if (!array)
            return true;
        context->features_seen = true;
        context->features = value;
        return true;
    case 17:
        if (!mark_field(context, gateway_client_state_field_icon)) {
            return false;
        }
        if (value.kind == JsonValueKind::null_value) {
            return true;
        }
        return json_value_as_string(value, &context->icon) ? true : reject(context);
    case 18:
        // Presentation only (guilds; users and members carry their own): a
        // repeated or non-string banner is ignored rather than rejecting.
        if (context->banner_seen)
            return true;
        context->banner_seen = value.kind == JsonValueKind::null_value ||
                               json_value_as_string(value, &context->banner);
        if (!context->banner_seen)
            reset_value(&context->banner);
        return true;
    case 19:
        return snowflake_member(context, gateway_client_state_field_owner_id, value, &context->mutation.owner_id);
    case 21:
        return bounded_member(context, gateway_client_state_field_color, value, 0xFFFFFFu,
                              &context->mutation.color);
    case 22:
        return gateway_keep_member(&context->error, &context->colors_seen, &context->colors, value, object);
    case 23:
        // Only an object carries role tags; other shapes are ignored.
        if (!object)
            return true;
        if (context->tags_seen)
            return gateway_duplicate_member(&context->error);
        context->tags_seen = true;
        context->tags = value;
        return true;
    case 24:
        return flag_member(context, gateway_client_state_field_hoist, value, &context->mutation.hoist);
    case 25:
        return flag_member(context, gateway_client_state_field_managed, value, &context->mutation.managed);
    case 26: {
        bool mentionable = false;
        if (context->mentionable_seen)
            return gateway_duplicate_member(&context->error);
        context->mentionable_seen = true;
        if (!json_value_to_bool(value, &mentionable))
            return reject(context);
        if (mentionable)
            context->mutation.role_flags |= gateway_client_role_mentionable;
        return true;
    }
    case 27: {
        JsonString emoji{};
        if (context->unicode_emoji_seen)
            return gateway_duplicate_member(&context->error);
        context->unicode_emoji_seen = true;
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (!json_value_as_string(value, &emoji))
            return reject(context);
        context->unicode_emoji_present = emoji.encoded.bytes != 0;
        return true;
    }
    case 28:
        return gateway_once_member(&context->error, &context->allow_seen, value_to_permission(value, &context->allow));
    case 29:
        return gateway_once_member(&context->error, &context->deny_seen, value_to_permission(value, &context->deny));
    case 30: {
        // Presentation-only: a malformed value must not reject the channel.
        u64 decoded = 0;
        if (json_value_to_u64(value, &decoded) && decoded <= 21600)
            context->mutation.rate_limit_per_user = static_cast<u16>(decoded + 1);
        return true;
    }
    case 31: {
        if (context->mutation.content_policy_known)
            return gateway_duplicate_member(&context->error);
        bool age_restricted = false;
        if (!json_value_to_bool(value, &age_restricted)) return reject(context);
        context->mutation.content_policy_known = true;
        context->mutation.content_policy = age_restricted ? 1u : 0u;
        return true;
    }
    case 32: {
        if (context->guild_nsfw_level_seen)
            return gateway_duplicate_member(&context->error);
        u64 level = 0;
        // An unknown future level leaves the guild classification unknown.
        if (!json_value_to_u64(value, &level) || level > 3) return true;
        context->guild_nsfw_level = static_cast<u8>(level);
        context->guild_nsfw_level_seen = true;
        return true;
    }
    case 33:
        if (context->thread_member_seen)
            return reject(context);
        // Only an object can describe the current thread member; other
        // shapes are advisory and ignored.
        if (!object && value.kind != JsonValueKind::null_value)
            return true;
        context->thread_member_seen = true;
        context->thread_member = value;
        return true;
    case 34:
        return gateway_keep_member(&context->error, &context->thread_metadata_seen, &context->thread_metadata,
                                   value, object);
    case 35:
        if (context->newly_created_seen)
            return gateway_duplicate_member(&context->error);
        context->newly_created_seen = true;
        return json_value_to_bool(value, &context->newly_created) || reject(context);
    case 36:
        if (context->mutation.member_list_id_known)
            return gateway_duplicate_member(&context->error);
        context->mutation.member_list_id_known = true;
        return value.kind == JsonValueKind::null_value ||
                       json_value_to_member_list_id(value, &context->mutation.member_list_id)
                   ? true
                   : reject(context);
    case 37:
        return gateway_once_member(&context->error, &context->animated_seen, json_value_to_bool(value, &context->animated));
    case 38:
        return gateway_once_member(&context->error, &context->available_seen, json_value_to_bool(value, &context->available));
    case 39:
        return gateway_once_member(&context->error, &context->require_colons_seen,
                                   json_value_to_bool(value, &context->require_colons));
    case 40: {
        if (context->joined_at_seen)
            return gateway_duplicate_member(&context->error);
        JsonString joined{};
        context->joined_at_seen = true;
        context->joined = json_value_as_string(value, &joined) && joined.encoded.bytes != 0;
        return true;
    }
    case 41:
        return flag_member(context, gateway_client_state_field_unavailable, value,
                           &context->mutation.unavailable);
    case 42:
        return gateway_keep_member(&context->error, &context->role_seen, &context->role, value, object);
    case 43:
        return gateway_keep_member(&context->error, &context->nicks_seen, &context->nicks, value,
                                   array || value.kind == JsonValueKind::null_value);
    case 44:
        return gateway_keep_member(&context->error, &context->recipients_seen, &context->recipients, value,
                                   array);
    case 45:
        return gateway_keep_member(&context->error, &context->recipient_ids_seen, &context->recipient_ids, value,
                                   array);
    case 46:
        return gateway_keep_member(&context->error, &context->properties_seen, &context->properties, value,
                                   object);
    // Message request and DM safety fields are advisory: malformed values are
    // ignored rather than rejecting the channel.
    case 47:
        context->message_request_seen = json_value_to_bool(value, &context->message_request) ||
                                        value.kind == JsonValueKind::null_value;
        return true;
    case 48:
        context->spam_seen = json_value_to_bool(value, &context->spam) || value.kind == JsonValueKind::null_value;
        return true;
    case 49:
        if (array || value.kind == JsonValueKind::null_value) {
            context->safety_warnings_seen = true;
            context->safety_warnings = value;
        }
        return true;
    case 50: {
        // Presentation-only like slowmode: an unexpected value keeps the default.
        u64 minutes = 0;
        if (json_value_to_u64(value, &minutes))
            context->mutation.default_archive_code = channel_archive_code(minutes);
        return true;
    }
    default: return true;
    }
}

struct SafetyWarningFields {
    u64 id;
    u64 type;
    bool dismissed, id_seen;
};
bool safety_warning_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<SafetyWarningFields*>(opaque);
    switch (member_index<"id", "type", "dismiss_timestamp">(name)) {
    case 0: f.id_seen = json_value_to_snowflake(value, &f.id); break;
    case 1: json_value_to_u64(value, &f.type); break;
    case 2: f.dismissed = value.kind != JsonValueKind::null_value; break;
    default: break;
    }
    return true;
}

} // namespace
namespace state_dispatch_detail {

bool safety_warning_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<SafetyWarningContext*>(opaque);
    if (c.id || value.kind != JsonValueKind::object)
        return true;
    SafetyWarningFields f{};
    if (!json_visit_indexed_object(*c.document, value, 8, safety_warning_member, &f).ok())
        return true;
    if (f.id_seen && f.id && !f.dismissed && f.type >= 1 && f.type <= 4) {
        c.id = f.id;
        c.type = static_cast<u8>(f.type);
    }
    return true;
}

} // namespace state_dispatch_detail
namespace {

struct NicknameFields {
    u64 id;
    JsonString name;
    GatewayEventDecodeError error;
    bool id_seen, name_seen;
};
bool nickname_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& fields = *static_cast<NicknameFields*>(opaque);
    switch (member_index<"id", "nick">(key)) {
    case 0:
        return gateway_once_member(&fields.error, &fields.id_seen,
                                   json_value_to_snowflake(value, &fields.id) && fields.id);
    case 1:
        return gateway_once_member(&fields.error, &fields.name_seen, json_value_as_string(value, &fields.name));
    default: break;
    }
    return true;
}

} // namespace
namespace state_dispatch_detail {

bool append_nickname(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<NicknameContext*>(opaque);
    if (value.kind != JsonValueKind::object) {
        context.result = decode_route(GatewayEventDecodeError::invalid_data);
        return false;
    }
    NicknameFields fields{};
    const auto read = json_visit_indexed_object(*context.document, value, context.depth,
                                               nickname_member, &fields);
    if (!read.ok() || !fields.id_seen || !fields.name_seen) {
        const auto error = fields.error != GatewayEventDecodeError::none ? fields.error
            : (!read.ok() ? GatewayEventDecodeError::invalid_json
                          : GatewayEventDecodeError::missing_required_field);
        context.result = decode_route(error);
        return false;
    }
    if (context.seen_count == 256) {
        context.result = route_error(GatewayClientDispatchRouteError::payload_capacity);
        return false;
    }
    for (u32 i = 0; i < context.seen_count; ++i)
        if (context.seen[i] == fields.id) {
            context.result = decode_route(GatewayEventDecodeError::duplicate_field);
            return false;
        }
    context.seen[context.seen_count++] = fields.id;
    char name[128];
    const auto decoded = json_string_decode_utf8(fields.name, {name, 128});
    if (!decoded.ok() || context.count == 256) {
        context.result = decode_route(GatewayEventDecodeError::invalid_data);
        return false;
    }
    if (!decoded.bytes)
        return true;
    const GatewayClientUserMutation item{fields.id, static_cast<u16>(decoded.bytes), 0, 1, 0};
    if (!gateway_payload_append(context.writer, &item, sizeof(item)) ||
        !gateway_payload_append(context.writer, name, decoded.bytes)) {
        context.result = route_error(GatewayClientDispatchRouteError::payload_capacity);
        return false;
    }
    ++context.count;
    return true;
}
bool append_recipient(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<RecipientContext*>(opaque);
    if (context.count == 256) {
        context.result = route_error(GatewayClientDispatchRouteError::payload_capacity);
        return false;
    }
    context.result =
        gateway_append_user(value, context.scratch, context.depth, context.writer, context.ids);
    if (!context.result.ok())
        return false;
    ++context.count;
    return true;
}

} // namespace state_dispatch_detail
namespace {

struct EmojiSetContext {
    JsonValue items;
    GatewayEventDecodeError error;
    bool items_seen;
    bool op_seen;
};

bool visit_emoji_set_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<EmojiSetContext*>(opaque);
    switch (member_index<"items", "op">(name)) {
    case 0:
        return gateway_keep_member(&context->error, &context->items_seen, &context->items, value,
                                   value.kind == JsonValueKind::array);
    case 1: {
        u64 op = 0;
        return gateway_once_member(&context->error, &context->op_seen, json_value_to_u64(value, &op) && op <= 2);
    }
    default: break;
    }
    return true;
}

struct RoleColorsContext {
    u32 primary;
    // RGB | kRoleColorPresent; null leaves them absent (a flat colour).
    u32 secondary;
    u32 tertiary;
    GatewayEventDecodeError error;
    bool primary_seen;
    bool secondary_seen;
    bool tertiary_seen;
};

// Enhanced role colours: primary_color always, secondary_color for a
// gradient and tertiary_color for the holographic style.
bool visit_role_color(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RoleColorsContext*>(opaque);
    const u32 index = member_index<"primary_color", "secondary_color", "tertiary_color">(name);
    if (index > 2) {
        return true;
    }
    const bool primary = index == 0;
    const bool secondary = index == 1;
    bool& seen = primary ? context->primary_seen : secondary ? context->secondary_seen : context->tertiary_seen;
    if (seen)
        return gateway_duplicate_member(&context->error);
    seen = true;
    if (!primary && value.kind == JsonValueKind::null_value) {
        return true;
    }
    u64 decoded = 0;
    if (!json_value_to_u64(value, &decoded) || decoded > 0xFFFFFFu) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    if (primary)
        context->primary = static_cast<u32>(decoded);
    else
        (secondary ? context->secondary : context->tertiary) = static_cast<u32>(decoded) | kRoleColorPresent;
    return true;
}

} // namespace
namespace state_dispatch_detail {

GatewayEventDecodeResult decode_state_object(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                                             StateObjectContext* output, bool private_name_allowed,
                                             const JsonIndexedDocument* document) noexcept {
    if (!output || value.kind != JsonValueKind::object || !value.source.data || value.source.bytes == 0 ||
        !scratch.data || scratch.count == 0 || maximum_depth == 0) {
        return decode_error(GatewayEventDecodeError::invalid_input, value.source_offset);
    }
    // Decoded in place: callers read the object only after success.
    StateObjectContext& context = *output;
    reset_value(&context);
    JsonReadResult read{};
    if (document) {
        // The array's immutable index already covers this complete child,
        // including large overwrite arrays. Auxiliary typed fields below use
        // separate writable scratch and cannot invalidate the parent reader.
        read = json_visit_indexed_object(*document, value, maximum_depth, visit_state_member, &context);
    } else {
        const JsonScanResult scan = json_scan_structurals_avx1(value.source, scratch);
        if (!scan.ok())
            return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
        read = json_visit_value_object(value, {scratch.data, scan.count}, maximum_depth,
                                      visit_state_member, &context);
    }
    if (!read.ok()) {
        // Preserve this helper's existing object-local error coordinate even
        // when its immutable index belongs to the containing channel array.
        if (document &&
            reinterpret_cast<usize>(value.source.data) >= reinterpret_cast<usize>(document->input.data)) {
            const usize begin =
                reinterpret_cast<usize>(value.source.data) - reinterpret_cast<usize>(document->input.data);
            if (begin <= read.error_offset)
                read.error_offset -= static_cast<u32>(begin);
        }
        return read_failure(context.error, read);
    }
    if (context.name_null &&
        (!private_name_allowed || (context.mutation.type != 1 && context.mutation.type != 3)))
        return decode_error(GatewayEventDecodeError::invalid_data, value.source_offset);
    // Some guild payloads also retain a legacy nsfw boolean. The documented
    // classification has precedence; distinct fields are not duplicates.
    if (context.guild_nsfw_level_seen) {
        context.mutation.content_policy = kGatewayStateContentGuildLevelMarker | context.guild_nsfw_level;
        context.mutation.content_policy_known = true;
    }
    if (context.thread_metadata_seen) {
        const auto metadata_scan = json_scan_structurals_avx1(context.thread_metadata.source, scratch);
        if (!metadata_scan.ok())
            return decode_error(GatewayEventDecodeError::scan_failed);
        struct ThreadMetadataContext {
            bool archived;
            bool seen;
            bool locked;
            bool locked_seen;
            GatewayEventDecodeError error;
        } metadata{};
        const auto metadata_read = json_visit_value_object(
            context.thread_metadata, {scratch.data, metadata_scan.count}, maximum_depth,
            +[](void* opaque, JsonString name, JsonValue item) noexcept {
                auto* target = static_cast<ThreadMetadataContext*>(opaque);
                const u32 index = member_index<"archived", "locked">(name);
                if (index > 1)
                    return true;
                const bool locked = index == 1;
                if (locked ? target->locked_seen : target->seen)
                    return gateway_duplicate_member(&target->error);
                if (locked) target->locked_seen = true;
                else target->seen = true;
                if (!json_value_to_bool(item, locked ? &target->locked : &target->archived)) {
                    target->error = GatewayEventDecodeError::invalid_data;
                    return false;
                }
                return true;
            },
            &metadata);
        if (!metadata_read.ok())
            return decode_error(metadata.error != GatewayEventDecodeError::none
                                    ? metadata.error
                                    : GatewayEventDecodeError::invalid_json,
                                metadata_read.error_offset);
        if (metadata.seen) {
            // The state apply path also verifies the resident channel type.
            if ((context.mutation.fields & gateway_client_state_field_type) != 0 &&
                !channel_is_thread(context.mutation.type))
                return decode_error(GatewayEventDecodeError::invalid_data, value.source_offset);
            context.mutation.thread_archived = metadata.archived;
            context.mutation.thread_archived_known = true;
        }
        if (metadata.locked_seen) {
            if ((context.mutation.fields & gateway_client_state_field_type) &&
                !channel_is_thread(context.mutation.type))
                return decode_error(GatewayEventDecodeError::invalid_data, value.source_offset);
            context.mutation.thread_locked = metadata.locked;
            context.mutation.thread_locked_known = true;
        }
    }
    if (context.emojis_seen && context.emojis.kind == JsonValueKind::object) {
        const JsonScanResult emoji_set_scan = json_scan_structurals_avx1(context.emojis.source, scratch);
        if (!emoji_set_scan.ok()) {
            return decode_error(GatewayEventDecodeError::scan_failed, emoji_set_scan.error_offset,
                                emoji_set_scan.error);
        }
        EmojiSetContext emoji_set{};
        const JsonReadResult emoji_set_read = json_visit_value_object(
            context.emojis, ConstSlice<u32>{scratch.data, emoji_set_scan.count}, maximum_depth,
            visit_emoji_set_member, &emoji_set);
        if (!emoji_set_read.ok()) {
            return read_failure(emoji_set.error, emoji_set_read);
        }
        if (emoji_set.items_seen) {
            context.emojis = emoji_set.items;
        } else {
            // Current user READY can advertise an out-of-band emoji data mode
            // with only an op marker. It is not an authoritative empty list;
            // a later GUILD_EMOJIS_UPDATE supplies the records.
            context.emojis_seen = false;
        }
    }
    if (context.colors_seen) {
        const JsonScanResult colors_scan = json_scan_structurals_avx1(context.colors.source, scratch);
        if (!colors_scan.ok()) {
            return decode_error(GatewayEventDecodeError::scan_failed, colors_scan.error_offset,
                                colors_scan.error);
        }
        RoleColorsContext colors{};
        const JsonReadResult colors_read =
            json_visit_value_object(context.colors, ConstSlice<u32>{scratch.data, colors_scan.count},
                                   maximum_depth, visit_role_color, &colors);
        if (!colors_read.ok()) {
            return read_failure(colors.error, colors_read);
        }
        if (colors.primary_seen) {
            context.mutation.color = colors.primary;
            context.mutation.fields |= gateway_client_state_field_color;
            // A tertiary colour only means something on top of a gradient.
            context.mutation.secondary_color = colors.secondary;
            context.mutation.tertiary_color = colors.secondary ? colors.tertiary : 0;
        }
    }
    if (context.tags_seen) {
        const auto tags_scan = json_scan_structurals_avx1(context.tags.source, scratch);
        if (!tags_scan.ok())
            return decode_error(GatewayEventDecodeError::scan_failed, tags_scan.error_offset, tags_scan.error);
        if (!json_visit_value_object(
                 context.tags, {scratch.data, tags_scan.count}, maximum_depth,
                 +[](void* opaque, JsonString name, JsonValue) noexcept {
                     if (json_name_is(name, utf8_literal("guild_connections")))
                         *static_cast<bool*>(opaque) = true;
                     return true;
                 },
                 &context.role_linked)
                 .ok())
            return decode_error(GatewayEventDecodeError::invalid_json);
    }
    if (context.features_seen) {
        const auto features_scan = json_scan_structurals_avx1(context.features.source, scratch);
        if (!features_scan.ok())
            return decode_error(GatewayEventDecodeError::scan_failed);
        if (!json_visit_value_array(
                 context.features, {scratch.data, features_scan.count}, maximum_depth,
                 +[](void* opaque, u32, JsonValue value) noexcept {
                     auto* flags = static_cast<u8*>(opaque);
                     JsonString name{};
                     // Unknown feature shapes are ignored like unknown names.
                     if (!json_value_as_string(value, &name))
                         return true;
                     static constexpr u8 bits[] = {1, 2, 4, guild_record_community, guild_record_onboarding,
                                                   guild_record_verification_gate, guild_record_onboarding_prompts,
                                                   guild_record_animated_banner};
                     const u32 index = member_index<"MAX_FILE_SIZE_50_MB", "MAX_FILE_SIZE_100_MB",
                                                    "MAX_FILE_SIZE_250_MB", "COMMUNITY", "GUILD_ONBOARDING",
                                                    "MEMBER_VERIFICATION_GATE_ENABLED",
                                                    "GUILD_ONBOARDING_HAS_PROMPTS", "ANIMATED_BANNER">(name);
                     if (index < 8)
                         *flags |= bits[index];
                     return true;
                 },
                 &context.mutation.upload_features)
                 .ok())
            return decode_error(GatewayEventDecodeError::invalid_data);
        context.mutation.upload_features_known = true;
    }
    return decode_error(GatewayEventDecodeError::none);
}

bool merge_guild_properties(StateObjectContext* object, const StateObjectContext& properties) noexcept {
    if (properties.mutation.content_policy_known) {
        if (object->mutation.content_policy_known) return false;
        object->mutation.content_policy = properties.mutation.content_policy;
        object->mutation.content_policy_known = true;
    }
    if (properties.mutation.upload_features_known) {
        if (object->mutation.upload_features_known)
            return false;
        object->mutation.upload_features = properties.mutation.upload_features;
        object->mutation.upload_features_known = true;
    }
    const u16 id = gateway_client_state_field_id;
    const u16 owner = gateway_client_state_field_owner_id;
    const u16 name = gateway_client_state_field_name;
    const u16 flags = gateway_client_state_field_flags;
    const u16 icon = gateway_client_state_field_icon;
    if ((properties.mutation.fields & id) != 0) {
        if ((object->mutation.fields & id) != 0 && object->mutation.id != properties.mutation.id) {
            return false;
        }
        object->mutation.id = properties.mutation.id;
        object->mutation.fields |= id;
    }
    if ((properties.mutation.fields & owner) != 0) {
        if ((object->mutation.fields & owner) != 0 &&
            object->mutation.owner_id != properties.mutation.owner_id) {
            return false;
        }
        object->mutation.owner_id = properties.mutation.owner_id;
        object->mutation.fields |= owner;
    }
    if ((properties.mutation.fields & name) != 0) {
        if ((object->mutation.fields & name) != 0) {
            return false;
        }
        object->name = properties.name;
        object->mutation.fields |= name;
    }
    if ((properties.mutation.fields & flags) != 0 && (object->mutation.fields & flags) == 0) {
        object->mutation.flags = properties.mutation.flags;
        object->mutation.fields |= flags;
    }
    if ((properties.mutation.fields & icon) != 0) {
        if ((object->mutation.fields & icon) != 0) {
            return false;
        }
        object->icon = properties.icon;
        object->mutation.fields |= icon;
    }
    if (properties.banner_seen && !object->banner_seen) {
        object->banner = properties.banner;
        object->banner_seen = true;
    }
    if (properties.features_seen && !object->features_seen) {
        object->features = properties.features;
        object->features_seen = true;
    }
    gateway_guild_detail_merge(&object->details, properties.details);
    const u16 settings = properties.mutation.guild_settings_known &
                         ~static_cast<u16>(guild_settings_self_timeout | guild_settings_self_member |
                                           guild_settings_self_quarantine);
    auto& to = object->mutation;
    const auto& from = properties.mutation;
    // The same setting at both levels must agree.
    const u16 both = to.guild_settings_known & settings;
    if (((both & guild_settings_mfa_level) && to.mfa_level != from.mfa_level) ||
        ((both & guild_settings_verification_level) && to.verification_level != from.verification_level) ||
        ((both & guild_settings_default_notifications) &&
         to.default_message_notifications != from.default_message_notifications) ||
        ((both & guild_settings_explicit_content_filter) &&
         to.explicit_content_filter != from.explicit_content_filter) ||
        ((both & guild_settings_system_channel) && to.system_channel_id != from.system_channel_id) ||
        ((both & guild_settings_afk_channel) && to.afk_channel_id != from.afk_channel_id) ||
        ((both & guild_settings_afk_timeout) && to.afk_timeout != from.afk_timeout))
        return false;
    if (settings & guild_settings_mfa_level)
        to.mfa_level = from.mfa_level;
    if (settings & guild_settings_verification_level)
        to.verification_level = from.verification_level;
    if (settings & guild_settings_default_notifications)
        to.default_message_notifications = from.default_message_notifications;
    if (settings & guild_settings_explicit_content_filter)
        to.explicit_content_filter = from.explicit_content_filter;
    if (settings & guild_settings_system_channel)
        to.system_channel_id = from.system_channel_id;
    if (settings & guild_settings_afk_channel)
        to.afk_channel_id = from.afk_channel_id;
    if (settings & guild_settings_afk_timeout)
        to.afk_timeout = from.afk_timeout;
    to.guild_settings_known = static_cast<u16>(to.guild_settings_known | settings);
    return true;
}

} // namespace state_dispatch_detail
} // namespace maicord
