#include "maicord/app/gateway_client.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/model/chat_state.hpp"

#include "gateway_guild_details.hpp"
#include "gateway_dispatch_common.hpp"
#include "gateway_payload_writer.hpp"
#include "gateway_state_payload.hpp"
#include "gateway_user_payload.hpp"
#include "maicord/protocol/upload_allowance.hpp"
#include "maicord/protocol/thread_actions.hpp"
#include "../protocol/json_field_lists.hpp"

namespace maicord {
namespace {

using protocol_detail::member_bit;
using protocol_detail::member_index;
using protocol_detail::skim_member_index;
using protocol_detail::wide_member_index;

static_assert(sizeof(GatewayClientStateMutation) == 168);
static_assert(sizeof(GatewayClientRoleMutation) == 40);
static_assert(sizeof(GatewayClientPermissionOverwriteMutation) == 32);
static_assert(sizeof(GatewayClientEmojiMutation) == 16);
inline constexpr u32 kGatewayGuildRoleMaximum = 256;
inline constexpr u32 kGatewayGuildEmojiMaximum = kGuildEmojiMaximum;
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
                                              GatewayEventDecodeError event_error, u32 detail) noexcept {
    GatewayClientDispatchRouteResult result = route_error(error, event_error);
    result.detail = detail;
    return result;
}

// A failed append reports invalid data as lack of payload room, anything else as a decode error.
GatewayClientDispatchRouteResult append_failure(GatewayEventDecodeError error, u32 detail = 0) noexcept {
    return route_detail(error == GatewayEventDecodeError::invalid_data
                            ? GatewayClientDispatchRouteError::payload_capacity
                            : GatewayClientDispatchRouteError::decode,
                        error, detail);
}

GatewayEventDecodeResult decode_error(GatewayEventDecodeError error, u32 offset = 0,
                                      JsonScanError scan_error = JsonScanError::none,
                                      JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

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
                         "is_spam", "safety_warnings">(name)) {
    case 0:
        return mark_field(context, gateway_client_state_field_id) &&
                       json_value_to_snowflake(value, &context->mutation.id)
                   ? true
                   : reject(context);
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
        return mark_field(context, gateway_client_state_field_guild_id) &&
                       json_value_to_snowflake(value, &context->mutation.guild_id)
                   ? true
                   : reject(context);
    case 4:
        if (!mark_field(context, gateway_client_state_field_parent_id)) {
            return false;
        }
        return value.kind == JsonValueKind::null_value ||
                       json_value_to_snowflake(value, &context->mutation.parent_id)
                   ? true
                   : reject(context);
    case 5: {
        u64 decoded = 0;
        if (!mark_field(context, gateway_client_state_field_flags))
            return false;
        if (!json_value_to_u64(value, &decoded) || decoded > 0xFFFFFFFFull) {
            context->mutation.fields = static_cast<u16>(context->mutation.fields & ~gateway_client_state_field_flags);
            return true;
        }
        context->mutation.flags = static_cast<u32>(decoded);
        return true;
    }
    case 6: {
        u64 decoded = 0;
        if (!mark_field(context, gateway_client_state_field_position))
            return false;
        if (!json_value_to_u64(value, &decoded) || decoded > 0xFFFFFFFFull) {
            context->mutation.fields = static_cast<u16>(context->mutation.fields & ~gateway_client_state_field_position);
            return true;
        }
        context->mutation.position = static_cast<u32>(decoded);
        return true;
    }
    case 7:
        return mark_field(context, gateway_client_state_field_permissions) &&
                       value_to_permission(value, &context->mutation.permissions)
                   ? true
                   : reject(context);
    case 8:
        if (!mark_field(context, gateway_client_state_field_last_message_id)) {
            return false;
        }
        return value.kind == JsonValueKind::null_value ||
                       json_value_to_snowflake(value, &context->mutation.last_message_id)
                   ? true
                   : reject(context);
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
        if (!mark_field(context, gateway_client_state_field_permission_overwrites) || !array) {
            if (context->error == GatewayEventDecodeError::none) {
                context->error = GatewayEventDecodeError::invalid_data;
            }
            return false;
        }
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
        return mark_field(context, gateway_client_state_field_owner_id) &&
                       json_value_to_snowflake(value, &context->mutation.owner_id)
                   ? true
                   : reject(context);
    case 20:
        return mark_field(context, gateway_client_state_field_id) &&
                       json_value_to_snowflake(value, &context->mutation.id)
                   ? true
                   : reject(context);
    case 21: {
        u64 decoded = 0;
        if (!mark_field(context, gateway_client_state_field_color))
            return false;
        if (!json_value_to_u64(value, &decoded) || decoded > 0xFFFFFFu) {
            context->mutation.fields = static_cast<u16>(context->mutation.fields & ~gateway_client_state_field_color);
            return true;
        }
        context->mutation.color = static_cast<u32>(decoded);
        return true;
    }
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
        return mark_field(context, gateway_client_state_field_hoist) &&
                       json_value_to_bool(value, &context->mutation.hoist)
                   ? true
                   : reject(context);
    case 25:
        return mark_field(context, gateway_client_state_field_managed) &&
                       json_value_to_bool(value, &context->mutation.managed)
                   ? true
                   : reject(context);
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
        if (context->allow_seen || !value_to_permission(value, &context->allow)) {
            context->error = gateway_field_error(context->allow_seen);
            return false;
        }
        context->allow_seen = true;
        return true;
    case 29:
        if (context->deny_seen || !value_to_permission(value, &context->deny)) {
            context->error = gateway_field_error(context->deny_seen);
            return false;
        }
        context->deny_seen = true;
        return true;
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
        if (context->animated_seen || !json_value_to_bool(value, &context->animated)) {
            context->error = gateway_field_error(context->animated_seen);
            return false;
        }
        context->animated_seen = true;
        return true;
    case 38:
        if (context->available_seen || !json_value_to_bool(value, &context->available)) {
            context->error = gateway_field_error(context->available_seen);
            return false;
        }
        context->available_seen = true;
        return true;
    case 39:
        if (context->require_colons_seen || !json_value_to_bool(value, &context->require_colons)) {
            context->error = gateway_field_error(context->require_colons_seen);
            return false;
        }
        context->require_colons_seen = true;
        return true;
    case 40: {
        if (context->joined_at_seen)
            return gateway_duplicate_member(&context->error);
        JsonString joined{};
        context->joined_at_seen = true;
        context->joined = json_value_as_string(value, &joined) && joined.encoded.bytes != 0;
        return true;
    }
    case 41:
        return mark_field(context, gateway_client_state_field_unavailable) &&
                       json_value_to_bool(value, &context->mutation.unavailable)
                   ? true
                   : reject(context);
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
    default: return true;
    }
}

// The first undismissed DM safety warning: {"id","type","dismiss_timestamp"}.
struct SafetyWarningContext {
    const JsonIndexedDocument* document;
    u64 id;
    u8 type;
};
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
        if (fields.id_seen || !json_value_to_snowflake(value, &fields.id) || !fields.id) {
            fields.error = gateway_field_error(fields.id_seen);
            return false;
        }
        fields.id_seen = true;
        break;
    case 1:
        if (fields.name_seen || !json_value_as_string(value, &fields.name)) {
            fields.error = gateway_field_error(fields.name_seen);
            return false;
        }
        fields.name_seen = true;
        break;
    default: break;
    }
    return true;
}
bool append_nickname(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<NicknameContext*>(opaque);
    if (value.kind != JsonValueKind::object) {
        context.result =
            route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
        return false;
    }
    NicknameFields fields{};
    const auto read = json_visit_indexed_object(*context.document, value, context.depth,
                                               nickname_member, &fields);
    if (!read.ok() || !fields.id_seen || !fields.name_seen) {
        const auto error = fields.error != GatewayEventDecodeError::none ? fields.error
            : (!read.ok() ? GatewayEventDecodeError::invalid_json
                          : GatewayEventDecodeError::missing_required_field);
        context.result = route_error(GatewayClientDispatchRouteError::decode, error);
        return false;
    }
    if (context.seen_count == 256) {
        context.result = route_error(GatewayClientDispatchRouteError::payload_capacity);
        return false;
    }
    for (u32 i = 0; i < context.seen_count; ++i)
        if (context.seen[i] == fields.id) {
            context.result = route_error(GatewayClientDispatchRouteError::decode,
                                         GatewayEventDecodeError::duplicate_field);
            return false;
        }
    context.seen[context.seen_count++] = fields.id;
    char name[128];
    const auto decoded = json_string_decode_utf8(fields.name, {name, 128});
    if (!decoded.ok() || context.count == 256) {
        context.result =
            route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
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
        if (context->op_seen || !json_value_to_u64(value, &op) || op > 2) {
            context->error = gateway_field_error(context->op_seen);
            return false;
        }
        context->op_seen = true;
        break;
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

GatewayEventDecodeResult decode_state_object(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                                             StateObjectContext* output, bool private_name_allowed = false,
                                             const JsonIndexedDocument* document = nullptr) noexcept {
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
        return context.error != GatewayEventDecodeError::none
                   ? decode_error(context.error, read.error_offset, JsonScanError::none, read.error)
                   : decode_error(GatewayEventDecodeError::invalid_json, read.error_offset,
                                  JsonScanError::none, read.error);
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
            return decode_error(emoji_set.error != GatewayEventDecodeError::none
                                    ? emoji_set.error
                                    : GatewayEventDecodeError::invalid_json,
                                emoji_set_read.error_offset, JsonScanError::none, emoji_set_read.error);
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
            return decode_error(colors.error != GatewayEventDecodeError::none
                                    ? colors.error
                                    : GatewayEventDecodeError::invalid_json,
                                colors_read.error_offset, JsonScanError::none, colors_read.error);
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

struct RoleArrayContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    u32 maximum_depth;
    u64 ids[kGatewayGuildRoleMaximum];
    u32 count;
    GatewayEventDecodeError error;
};

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

GatewayEventDecodeResult append_role_array(JsonValue array, Slice<u32> array_scratch,
                                           Slice<u32> object_scratch, u32 maximum_depth,
                                           PayloadWriter* writer, u32* count) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(array.source, array_scratch);
    if (!scan.ok()) {
        return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    RoleArrayContext context{};
    context.writer = writer;
    context.object_scratch = object_scratch;
    context.maximum_depth = maximum_depth;
    const JsonReadResult read = json_visit_value_array(
        array, ConstSlice<u32>{array_scratch.data, scan.count}, maximum_depth, visit_role, &context);
    if (!read.ok()) {
        return decode_error(context.error != GatewayEventDecodeError::none
                                ? context.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    *count = context.count;
    return decode_error(GatewayEventDecodeError::none);
}

struct SnowflakeArrayContext {
    PayloadWriter* writer;
    u32 start;
    u32 count;
    GatewayEventDecodeError error;
    u32 limit = kGatewayGuildRoleMaximum;
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

GatewayEventDecodeResult append_snowflake_array(JsonValue array, Slice<u32> scratch, u32 maximum_depth,
                                                PayloadWriter* writer, u32* count, u32 limit = kGatewayGuildRoleMaximum) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(array.source, scratch);
    if (!scan.ok()) {
        return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    SnowflakeArrayContext context{writer, writer->used, 0, GatewayEventDecodeError::none};
    context.limit = limit;
    const JsonReadResult read = json_visit_value_array(
        array, ConstSlice<u32>{scratch.data, scan.count}, maximum_depth, visit_snowflake, &context);
    if (!read.ok()) {
        return decode_error(context.error != GatewayEventDecodeError::none
                                ? context.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    *count = context.count;
    return decode_error(GatewayEventDecodeError::none);
}

struct EmojiArrayContext {
    PayloadWriter* writer;
    Slice<u32> object_scratch;
    Slice<u32> role_scratch;
    u64 ids[kGatewayGuildEmojiMaximum];
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
        context->count == kGatewayGuildEmojiMaximum) {
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

GatewayEventDecodeResult append_emoji_array(JsonValue array, Slice<u32> array_scratch,
                                            Slice<u32> object_scratch, Slice<u32> role_scratch,
                                            u32 maximum_depth, PayloadWriter* writer, u32* count,
                                            u32* role_count) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(array.source, array_scratch);
    if (!scan.ok()) {
        return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    EmojiArrayContext context{};
    context.writer = writer;
    context.object_scratch = object_scratch;
    context.role_scratch = role_scratch;
    context.maximum_depth = maximum_depth;
    const JsonReadResult read =
        json_visit_value_array(array, ConstSlice<u32>{array_scratch.data, scan.count}, maximum_depth,
                              visit_emoji, &context);
    if (!read.ok()) {
        return decode_error(context.error != GatewayEventDecodeError::none
                                ? context.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    *count = context.count;
    *role_count = context.role_count;
    return decode_error(GatewayEventDecodeError::none);
}

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

GatewayEventDecodeResult append_overwrite_array(JsonValue array, Slice<u32> array_scratch,
                                                Slice<u32> object_scratch, u32 maximum_depth,
                                                PayloadWriter* writer, u32* count,
                                                const JsonIndexedDocument* document = nullptr) noexcept {
    OverwriteArrayContext context{
        writer, object_scratch, maximum_depth, writer->used, 0, GatewayEventDecodeError::none};
    context.document = document;
    JsonReadResult read{};
    if (document)
        read = json_visit_indexed_array(*document, array, maximum_depth, visit_overwrite, &context);
    else {
        const JsonScanResult scan = json_scan_structurals_avx1(array.source, array_scratch);
        if (!scan.ok())
            return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
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
        return decode_error(context.error != GatewayEventDecodeError::none
                                ? context.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    *count = context.count;
    return decode_error(GatewayEventDecodeError::none);
}

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

struct MemberIdentity {
    u64 user_id, nested_id;
    bool user_id_seen, user_seen, nested_id_seen;
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
    u32 scratch[512]{};
    const auto scan = json_scan_structurals_avx1(value.source, {scratch, 512});
    return scan.ok() &&
           json_visit_value_object(value, {scratch, scan.count}, 64, visit_member_identity_id, &identity)
               .ok() &&
           identity.nested_id_seen;
}
bool member_identity(JsonValue value, Slice<u32> scratch, u32 maximum_depth,
                     MemberIdentity* identity) noexcept {
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    return scan.ok() && value.kind == JsonValueKind::object &&
           json_visit_value_object(value, {scratch.data, scan.count}, maximum_depth, visit_member_identity,
                                   identity)
               .ok();
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
    u32 role_scratch[1'024]{};
    const GatewayEventDecodeResult roles =
        append_snowflake_array(member.roles, Slice<u32>{role_scratch, 1'024}, context->maximum_depth,
                               context->writer, &context->role_count);
    if (!roles.ok()) {
        context->error = roles.error;
        return false;
    }
    return true;
}

// The current member is optional: a previewed guild has none, and a pending
// one may still lack roles. *found reports whether it was present.
GatewayEventDecodeResult append_self_member_roles(JsonValue members, Slice<u32> array_scratch,
                                                  Slice<u32> object_scratch, u32 maximum_depth,
                                                  u64 self_user_id, PayloadWriter* writer,
                                                  u32* count, GatewayClientStateMutation* guild,
                                                  bool* found) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(members.source, array_scratch);
    if (!scan.ok()) {
        return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    SelfMemberContext context{
        writer, object_scratch, self_user_id, maximum_depth, 0, GatewayEventDecodeError::none, false};
    const JsonReadResult read =
        json_visit_value_array(members, ConstSlice<u32>{array_scratch.data, scan.count},
                              maximum_depth, visit_member, &context);
    if (!read.ok()) {
        return decode_error(context.error != GatewayEventDecodeError::none
                                ? context.error
                                : GatewayEventDecodeError::invalid_data,
                            read.error_offset, JsonScanError::none, read.error);
    }
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
    return decode_error(GatewayEventDecodeError::none);
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
        return decode_error(GatewayEventDecodeError::invalid_data);
    }
    if (object->permission_overwrites_seen) {
        u32 object_scratch[512]{};
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
    return decode_error(GatewayEventDecodeError::none);
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

GatewayEventDecodeResult append_channel_array(JsonValue array, Slice<u32> array_scratch,
                                              Slice<u32> object_scratch, u32 maximum_depth, u64 guild_id,
                                              PayloadWriter* writer, u32* count,
                                              u32* overwrite_count, u64 self_user_id = 0,
                                              u32* skipped = nullptr) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(array.source, array_scratch);
    if (!scan.ok()) {
        return decode_error(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
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
        return decode_error(context.error != GatewayEventDecodeError::none
                                ? context.error
                                : GatewayEventDecodeError::invalid_json,
                            read.error_offset, JsonScanError::none, read.error);
    }
    *count += context.count;
    *overwrite_count += context.overwrite_count;
    if (skipped)
        *skipped += context.skipped;
    return decode_error(GatewayEventDecodeError::none);
}

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
GatewayClientDispatchRouteResult route_thread_sync(const GatewayEnvelope& envelope, Slice<u32> scratch,
    u32 depth, Slice<char> storage, u64 self, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    const auto scan = json_scan_structurals_avx1(envelope.data.source, scratch);
    if (!scan.ok() || !self) return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    ThreadSyncRoot root{};
    const auto read = json_visit_value_object(envelope.data, {scratch.data, scan.count}, depth, thread_sync_field, &root);
    // guild_id and threads are required; channel_ids and members are optional.
    if (!read.ok() || (root.seen & 5) != 5) return route_error(GatewayClientDispatchRouteError::decode,
        root.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::missing_required_field : root.error);
    PayloadWriter writer{storage, 0};
    GatewayClientThreadListSync header{};
    header.guild_id = root.guild; header.self_user_id = self;
    header.scope = (root.seen & 2) ? GatewayClientThreadSyncScope::parents : GatewayClientThreadSyncScope::guild;
    if ((root.seen & 2) && !append_snowflake_array(root.parents, scratch, depth, &writer, &header.parent_count, 4096).ok())
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    u32 objects[512]{};
    u32 overwrite_count{};
    const auto channels = append_channel_array(root.threads, scratch, {objects, 512}, depth, root.guild,
                                               &writer, &header.thread_count, &overwrite_count, self);
    if (!channels.ok()) return route_error(GatewayClientDispatchRouteError::decode, channels.error);
    if (root.seen & 8) {
        const auto member_scan = json_scan_structurals_avx1(root.members.source, scratch);
        if (!member_scan.ok()) return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
        ThreadSyncMembers members{&writer, {objects, 512}, self, depth, writer.used};
        const auto member_read = json_visit_value_array(root.members, {scratch.data, member_scan.count}, depth, thread_sync_member, &members);
        if (!member_read.ok()) return route_error(GatewayClientDispatchRouteError::decode,
            members.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : members.error);
        header.member_count = members.count;
        header.flags = gateway_client_thread_sync_members_known;
    }
    event->dispatch_kind = GatewayClientDispatchKind::thread_list_sync;
    event->thread_sync = header;
    event->dispatch_payload_bytes = writer.used;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    return {};
}

GatewayClientDispatchRouteResult route_channel_object(const StateObjectContext& object,
                                                      Slice<u32> structural_scratch, u32 maximum_depth,
                                                      PayloadWriter& writer, GatewayClientEvent* event,
                                                      bool channel_create, bool channel_update,
                                                      bool channel_delete) noexcept {
    u32 object_scratch[512]{};
    const bool private_channel = object.mutation.type == 1 || object.mutation.type == 3;
    const u16 required = gateway_client_state_field_id | gateway_client_state_field_type |
                         (private_channel ? 0 : gateway_client_state_field_guild_id);
    if ((object.mutation.fields & required) != required) {
        return route_error(GatewayClientDispatchRouteError::decode,
                           GatewayEventDecodeError::missing_required_field);
    }
    if (private_channel && (object.mutation.guild_id != 0 || object.mutation.parent_id != 0 ||
                            object.permission_overwrites_seen))
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    event->dispatch_kind = channel_create ? GatewayClientDispatchKind::channel_create
                                          : (channel_update ? GatewayClientDispatchKind::channel_update
                                                            : GatewayClientDispatchKind::channel_delete);
    if ((object.mutation.fields & gateway_client_state_field_name) != 0 && !object.name_null &&
        !append_name(&writer, object.name, &event->state.name_bytes)) {
        return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    if (private_channel && !channel_delete) {
        if ((object.mutation.fields & gateway_client_state_field_icon) && object.icon.encoded.bytes) {
            if (!append_icon(&writer, object.icon, &event->state.icon_bytes))
                return route_error(GatewayClientDispatchRouteError::payload_capacity);
        }
        if (object.recipient_ids_seen || object.recipients_seen) {
            const JsonValue recipients =
                object.recipient_ids_seen ? object.recipient_ids : object.recipients;
            const auto scan = json_scan_structurals_avx1(recipients.source, structural_scratch);
            if (!scan.ok())
                return route_error(GatewayClientDispatchRouteError::decode,
                                   GatewayEventDecodeError::scan_failed);
            RecipientContext context{&writer, {object_scratch, 512},     maximum_depth,
                                     0,       object.recipient_ids_seen, {}};
            const auto read =
                json_visit_value_array(recipients, {structural_scratch.data, scan.count},
                                      maximum_depth, append_recipient, &context);
            if (!context.result.ok())
                return context.result;
            if (!read.ok())
                return route_error(GatewayClientDispatchRouteError::decode,
                                   GatewayEventDecodeError::invalid_json);
            event->state.recipient_count = context.count;
            event->state.recipients_known = true;
        }
        if (object.nicks_seen) {
            NicknameContext context{&writer, nullptr, maximum_depth, 0, {}};
            if (object.nicks.kind == JsonValueKind::array) {
                const auto scan = json_scan_structurals_avx1(object.nicks.source, structural_scratch);
                if (!scan.ok())
                    return route_error(GatewayClientDispatchRouteError::decode,
                                       GatewayEventDecodeError::scan_failed);
                const JsonIndexedDocument document{object.nicks.source,
                                                    {structural_scratch.data, scan.count}};
                context.document = &document;
                const auto read = json_visit_indexed_array(document, object.nicks, maximum_depth,
                                                           append_nickname, &context);
                if (!context.result.ok())
                    return context.result;
                if (!read.ok())
                    return route_error(GatewayClientDispatchRouteError::decode,
                                       GatewayEventDecodeError::invalid_json);
            }
            event->state.record_count = context.count;
            event->state.private_nicknames_known = true;
        }
        u8 flags = 0;
        if (object.message_request_seen || object.spam_seen)
            flags |= gateway_client_private_request_known |
                     (object.message_request ? gateway_client_private_request_pending : 0) |
                     (object.spam ? gateway_client_private_request_spam : 0);
        if (object.safety_warnings_seen) {
            flags |= gateway_client_private_safety_known;
            SafetyWarningContext warning{};
            if (object.safety_warnings.kind == JsonValueKind::array) {
                const auto scan = json_scan_structurals_avx1(object.safety_warnings.source, structural_scratch);
                const JsonIndexedDocument document{object.safety_warnings.source,
                                                   {structural_scratch.data, scan.ok() ? scan.count : 0}};
                warning.document = &document;
                if (scan.ok())
                    json_visit_indexed_array(document, {object.safety_warnings.source, 0, JsonValueKind::array},
                                             maximum_depth, safety_warning_item, &warning);
            }
            if (warning.id)
                flags |= static_cast<u8>(warning.type << kGatewayClientPrivateSafetyShift);
            if (flags && !gateway_payload_append(&writer, reinterpret_cast<const char*>(&flags), 1))
                return route_error(GatewayClientDispatchRouteError::payload_capacity);
            if (warning.id && !gateway_payload_append_u64(&writer, warning.id))
                return route_error(GatewayClientDispatchRouteError::payload_capacity);
        } else if (flags && !gateway_payload_append(&writer, reinterpret_cast<const char*>(&flags), 1))
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
    }
    if (!channel_delete && object.permission_overwrites_seen) {
        const GatewayEventDecodeResult appended = append_overwrite_array(
            object.permission_overwrites, structural_scratch, Slice<u32>{object_scratch, 512},
            maximum_depth, &writer, &event->state.overwrite_count);
        if (!appended.ok())
            return append_failure(appended.error);
    }
    return route_error(GatewayClientDispatchRouteError::none);
}

} // namespace

GatewayClientDispatchRouteResult gateway_client_route_channel_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (!event || !payload)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    StateObjectContext object;
    const auto decoded = decode_state_object(data, structural_scratch, maximum_depth, &object, true);
    if (!decoded.ok())
        return route_error(GatewayClientDispatchRouteError::decode, decoded.error);
    if (!hydrate_self_member(&object, self_user_id))
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    PayloadWriter writer{storage, 0};
    event->state = object.mutation;
    auto routed =
        route_channel_object(object, structural_scratch, maximum_depth, writer, event, true, false, false);
    routed.payload_overflow = !routed.ok() && writer.overflow;
    if (routed.ok())
        *payload = {reinterpret_cast<const u8*>(writer.output.data), writer.used};
    return routed;
}

GatewayClientDispatchRouteResult gateway_client_route_private_channel(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (!event || !payload)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    StateObjectContext object;
    const auto decoded = decode_state_object(data, structural_scratch, maximum_depth, &object, true);
    if (!decoded.ok())
        return route_error(GatewayClientDispatchRouteError::decode, decoded.error);
    if (object.mutation.type != 1 && object.mutation.type != 3)
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    reset_value(event);
    event->kind = GatewayClientEventKind::dispatch;
    event->state = object.mutation;
    PayloadWriter writer{storage, 0};
    const auto routed =
        route_channel_object(object, structural_scratch, maximum_depth, writer, event, true, false, false);
    if (!routed.ok())
        return routed;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    event->dispatch_payload_bytes = writer.used;
    return routed;
}

namespace {
GatewayClientDispatchRouteResult route_state_dispatch(const GatewayEnvelope& envelope,
                                                      Slice<u32> structural_scratch, u32 maximum_depth,
                                                      Slice<char> payload_scratch, u64 self_user_id,
                                                      GatewayClientEvent* event, ConstSlice<u8>* payload,
                                                      bool* handled, bool* overflowed,
                                                      JsonValue ready_members = {}) noexcept {
    if (!event || !payload || !handled) {
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    }
    const u32 event_index =
        skim_member_index<"READY_GUILD", "THREAD_LIST_SYNC", "THREAD_MEMBER_UPDATE", "GUILD_CREATE", "GUILD_UPDATE",
                          "GUILD_DELETE", "THREAD_CREATE", "CHANNEL_CREATE", "CHANNEL_UPDATE", "THREAD_UPDATE",
                          "CHANNEL_DELETE", "THREAD_DELETE", "GUILD_ROLE_CREATE", "GUILD_ROLE_UPDATE",
                          "GUILD_ROLE_DELETE", "GUILD_MEMBER_UPDATE", "GUILD_EMOJIS_UPDATE">(envelope.event_name);
    const bool ready_guild = event_index == 0;
    if (event_index == 1) {
        *handled = true;
        return route_thread_sync(envelope, structural_scratch, maximum_depth, payload_scratch, self_user_id, event, payload);
    }
    if (event_index == 2) {
        *handled = true;
        StateObjectContext member;
        const auto decoded = decode_state_object(envelope.data, structural_scratch, maximum_depth, &member);
        ThreadMemberSnapshot receipt{};
        if (!decoded.ok() || !member.mutation.id || !member.mutation.guild_id ||
            !thread_member_decode(envelope.data.source, member.mutation.id, self_user_id, &receipt).ok())
            return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
        event->dispatch_kind = GatewayClientDispatchKind::thread_self_update;
        event->thread_self = {member.mutation.guild_id, receipt.thread_id, self_user_id,
                             gateway_thread_self_known | gateway_thread_self_joined, {},
                             thread_join_seconds_from_ms(receipt.join_timestamp_ms)};
        reset_value(payload); event->dispatch_payload_bytes = 0;
        return {};
    }
    const bool guild_create = ready_guild || event_index == 3;
    const bool guild_update = event_index == 4;
    const bool guild_delete = event_index == 5;
    const bool thread_create = event_index == 6;
    const bool channel_create = event_index == 7 || thread_create;
    const bool channel_update = event_index == 8 || event_index == 9;
    const bool channel_delete = event_index == 10 || event_index == 11;
    const bool role_create = event_index == 12;
    const bool role_update = event_index == 13;
    const bool role_delete = event_index == 14;
    const bool member_update = event_index == 15;
    const bool guild_emojis_update = event_index == 16;
    *handled = guild_create || guild_update || guild_delete || channel_create || channel_update ||
               channel_delete || role_create || role_update || role_delete || member_update ||
               guild_emojis_update;
    if (!*handled) {
        return route_error(GatewayClientDispatchRouteError::none);
    }

    StateObjectContext object;
    const GatewayEventDecodeResult decoded =
        decode_state_object(envelope.data, structural_scratch, maximum_depth, &object,
                            channel_create || channel_update || channel_delete);
    if (!decoded.ok()) {
        auto result = route_error(GatewayClientDispatchRouteError::decode, decoded.error);
        // READY can grow its nested index and retry this record. Keep the
        // established error contract of the other state/receipt routes.
        if (ready_guild) {
            result.scan_error = decoded.scan_error;
            result.read_error = decoded.read_error;
            result.error_offset = decoded.error_offset;
        }
        return result;
    }
    if ((channel_create || channel_update) && !hydrate_self_member(&object, self_user_id))
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    if ((guild_create || guild_update) && object.properties_seen) {
        StateObjectContext properties;
        const GatewayEventDecodeResult property_decoded =
            decode_state_object(object.properties, structural_scratch, maximum_depth, &properties);
        if (!property_decoded.ok() || !merge_guild_properties(&object, properties)) {
            return route_detail(
                GatewayClientDispatchRouteError::decode,
                property_decoded.ok() ? GatewayEventDecodeError::invalid_data : property_decoded.error, 8);
        }
    }
    PayloadWriter writer{payload_scratch, 0};
    // Every return below reports whether the scratch filled up, so a caller
    // with larger storage can route the same record again.
    struct OverflowReport {
        const PayloadWriter& writer;
        bool* output;
        ~OverflowReport() { *output = writer.overflow; }
    } overflow_report{writer, overflowed};
    // Supplementary parts are dropped on overflow only at the largest record
    // capacity; below it the whole record is routed again with more room.
    const bool largest = payload_scratch.count >= kGatewayClientGuildRecordCapacity;
    // The envelope scan is complete, so its structural storage can be reused
    // by nested arrays. Only the object scan and emoji-role scan must remain
    // disjoint while an array visitor is active.
    u32 object_scratch[512]{};
    u32 role_scratch[1'024]{};
    event->state = object.mutation;

    if (channel_create || channel_update || channel_delete)
        event->state.channel_event_flags = thread_create && object.newly_created
            ? gateway_client_channel_new_thread : 0;

    if (guild_emojis_update) {
        if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0 || !object.emojis_seen) {
            return route_error(GatewayClientDispatchRouteError::decode,
                               GatewayEventDecodeError::missing_required_field);
        }
        const GatewayEventDecodeResult appended =
            append_emoji_array(object.emojis, structural_scratch, Slice<u32>{object_scratch, 512},
                               Slice<u32>{role_scratch, 1'024}, maximum_depth, &writer,
                               &event->state.emoji_count, &event->state.emoji_role_count);
        if (!appended.ok())
            return append_failure(appended.error);
        event->state.emojis_known = true;
        event->dispatch_kind = GatewayClientDispatchKind::guild_emojis_update;
    } else if (role_create || role_update || role_delete) {
        if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0) {
            return route_error(GatewayClientDispatchRouteError::decode,
                               GatewayEventDecodeError::missing_required_field);
        }
        if (role_delete) {
            if ((object.mutation.fields & gateway_client_state_field_id) == 0) {
                return route_error(GatewayClientDispatchRouteError::decode,
                                   GatewayEventDecodeError::missing_required_field);
            }
            event->dispatch_kind = GatewayClientDispatchKind::guild_role_delete;
        } else {
            if (!object.role_seen) {
                return route_error(GatewayClientDispatchRouteError::decode,
                                   GatewayEventDecodeError::missing_required_field);
            }
            StateObjectContext role;
            const GatewayEventDecodeResult role_decoded =
                decode_state_object(object.role, Slice<u32>{object_scratch, 512}, maximum_depth, &role);
            constexpr u16 required = gateway_client_state_field_id | gateway_client_state_field_permissions;
            if (!role_decoded.ok() || (role.mutation.fields & required) != required) {
                return route_error(GatewayClientDispatchRouteError::decode,
                                   role_decoded.ok() ? GatewayEventDecodeError::missing_required_field
                                                     : role_decoded.error);
            }
            event->state = role.mutation;
            event->state.role_flags = role_flags(role);
            event->state.guild_id = object.mutation.guild_id;
            event->state.fields |= gateway_client_state_field_guild_id;
            if ((role.mutation.fields & gateway_client_state_field_name) != 0 &&
                !append_name(&writer, role.name, &event->state.name_bytes)) {
                return route_error(GatewayClientDispatchRouteError::payload_capacity);
            }
            event->dispatch_kind = role_create ? GatewayClientDispatchKind::guild_role_create
                                               : GatewayClientDispatchKind::guild_role_update;
        }
    } else if (member_update) {
        if ((object.mutation.fields & gateway_client_state_field_guild_id) == 0 || self_user_id == 0) {
            return route_error(GatewayClientDispatchRouteError::decode,
                               GatewayEventDecodeError::missing_required_field);
        }
        MemberIdentity identity{};
        if (!member_identity(envelope.data, {object_scratch, 512}, maximum_depth, &identity) ||
            (!identity.user_id_seen && !identity.user_seen) ||
            (identity.user_id_seen && identity.user_seen && identity.user_id != identity.nested_id)) {
            return route_error(GatewayClientDispatchRouteError::decode,
                               GatewayEventDecodeError::invalid_data);
        }
        const u64 member_id = identity.user_id_seen ? identity.user_id : identity.nested_id;
        if (member_id == self_user_id) {
            event->state.member_role_flags = 0;
            member_state_from_flags(&event->state);
            event->state.guild_settings_known &=
                static_cast<u16>(guild_settings_self_timeout | guild_settings_self_member |
                                 guild_settings_self_quarantine);
            u32 role_count = 0;
            const GatewayEventDecodeResult roles =
                !object.roles_seen ? GatewayEventDecodeResult{}
                                   : append_snowflake_array(object.roles, structural_scratch, maximum_depth,
                                                            &writer, &role_count);
            if (!roles.ok())
                return append_failure(roles.error);
            if (object.roles_seen)
                event->state.member_role_flags = gateway_client_member_roles_known;
            event->state.id = member_id;
            event->state.fields |= gateway_client_state_field_id;
            event->state.member_role_count = role_count;
            event->state.record_count = role_count;
            event->dispatch_kind = GatewayClientDispatchKind::guild_member_update;
        } else {
            event->dispatch_kind = GatewayClientDispatchKind::unknown;
            // Non-self metadata belongs to the bounded member hydration route,
            // while the self route above retains its permission-role authority.
            *handled = false;
        }
    } else if (guild_create || guild_update || guild_delete) {
        event->state.member_role_flags = 0;
        if (ready_guild && ready_members.source.data) {
            if (ready_members.kind != JsonValueKind::array)
                return route_error(GatewayClientDispatchRouteError::decode,
                                   GatewayEventDecodeError::invalid_data);
            object.members = ready_members;
            object.members_seen = true;
        }
        if ((object.mutation.fields & gateway_client_state_field_id) == 0) {
            return route_detail(GatewayClientDispatchRouteError::decode,
                                GatewayEventDecodeError::missing_required_field, 1);
        }
        event->dispatch_kind = guild_create ? GatewayClientDispatchKind::guild_create
                                            : (guild_update ? GatewayClientDispatchKind::guild_update
                                                            : GatewayClientDispatchKind::guild_delete);
        const bool unavailable = object.mutation.unavailable &&
                                 (object.mutation.fields & gateway_client_state_field_unavailable) != 0;
        event->state.guild_settings_known &=
            static_cast<u16>(~(guild_settings_self_timeout | guild_settings_self_member |
                               guild_settings_self_quarantine));
        event->state.self_member = 0;
        constexpr u16 guild_required =
            gateway_client_state_field_name | gateway_client_state_field_owner_id;
        if (((guild_create && !unavailable) || guild_update) &&
            ((object.mutation.fields & guild_required) != guild_required || !object.roles_seen)) {
            return route_detail(
                GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::missing_required_field,
                2 | (static_cast<u32>(object.mutation.fields) << 8) | (object.roles_seen ? 1u << 24 : 0));
        }
        // A live GUILD_CREATE for a server joined elsewhere may carry no member
        // list, or one without the current member (a preview); both still apply.
        if (guild_create && !unavailable && (!object.channels_seen || self_user_id == 0)) {
            return route_detail(
                GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::missing_required_field,
                3 | (object.channels_seen ? 1u << 8 : 0) | (object.members_seen ? 1u << 9 : 0) |
                    (object.threads_seen ? 1u << 10 : 0));
        }
        if ((object.mutation.fields & gateway_client_state_field_name) != 0 &&
            !append_name(&writer, object.name, &event->state.name_bytes)) {
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
        }
        if ((object.mutation.fields & gateway_client_state_field_icon) != 0 &&
            object.icon.encoded.bytes != 0 &&
            !append_icon(&writer, object.icon, &event->state.icon_bytes)) {
            return route_error(GatewayClientDispatchRouteError::payload_capacity);
        }
        // The banner follows the icon: guild_banner is 1 + its bytes (1: null).
        if (object.banner_seen && (guild_create || guild_update)) {
            u8 bytes = 0;
            const u32 mark = writer.used;
            if (object.banner.encoded.bytes != 0 && !append_icon(&writer, object.banner, &bytes)) {
                if (writer.overflow)
                    return route_error(GatewayClientDispatchRouteError::payload_capacity);
                writer.used = mark; // An oversized or undecodable hash is dropped.
            } else {
                event->state.guild_banner = static_cast<u8>(1u + bytes);
            }
        }
        event->state.guild_details_bytes = 0;
        if ((guild_create && !unavailable) || guild_update) {
            if (!gateway_guild_details_append(object.details, object.features, object.features_seen,
                                              structural_scratch, maximum_depth, &writer,
                                              &event->state.guild_details_bytes))
                return route_error(GatewayClientDispatchRouteError::payload_capacity);
            GatewayEventDecodeResult appended =
                append_role_array(object.roles, structural_scratch, Slice<u32>{object_scratch, 512},
                                  maximum_depth, &writer, &event->state.role_count);
            if (!appended.ok())
                return append_failure(appended.error, 4);
            bool self_found = false;
            if (guild_create && object.members_seen) {
                appended = append_self_member_roles(object.members, structural_scratch,
                                                    Slice<u32>{object_scratch, 512}, maximum_depth,
                                                    self_user_id, &writer, &event->state.member_role_count,
                                                    &event->state, &self_found);
                if (!appended.ok())
                    return append_failure(appended.error, 5);
            }
            // A guild without a join time reads as previewed (lurked).
            if (guild_create && !ready_guild && !self_found && object.joined_at_seen && !object.joined) {
                event->state.guild_settings_known =
                    static_cast<u16>(event->state.guild_settings_known | guild_settings_self_member);
                event->state.self_member = static_cast<u8>(event->state.self_member | guild_self_member_lurking);
            }
        }
        if (guild_create && !unavailable) {
            u32 record_count = 0, skipped_channels = 0;
            u32 overwrite_count = 0;
            GatewayEventDecodeResult appended = append_channel_array(
                object.channels, structural_scratch, Slice<u32>{object_scratch, 512}, maximum_depth,
                object.mutation.id, &writer, &record_count, &overwrite_count, self_user_id, &skipped_channels);
            if (!appended.ok()) {
                auto failed = append_failure(appended.error, 6);
                failed.scan_error = appended.scan_error;
                failed.read_error = appended.read_error;
                failed.error_offset = appended.error_offset;
                return failed;
            }
            if (object.threads_seen) {
                // Active threads are supplementary (thread lists refetch them).
                // When they do not fit or cannot be read, keep the guild
                // without them rather than losing the whole guild.
                const u32 mark = writer.used, records_mark = record_count, overwrites_mark = overwrite_count;
                appended = append_channel_array(
                    object.threads, structural_scratch, Slice<u32>{object_scratch, 512}, maximum_depth,
                    object.mutation.id, &writer, &record_count, &overwrite_count, self_user_id, &skipped_channels);
                if (!appended.ok()) {
                    if (appended.error == GatewayEventDecodeError::scan_failed &&
                        appended.scan_error == JsonScanError::output_overflow) {
                        auto failed = route_detail(GatewayClientDispatchRouteError::decode, appended.error, 7);
                        failed.scan_error = appended.scan_error;
                        failed.error_offset = appended.error_offset;
                        return failed;
                    }
                    if (writer.overflow && !largest)
                        return route_detail(GatewayClientDispatchRouteError::payload_capacity, appended.error, 7);
                    writer.used = mark;
                    writer.overflow = false;
                    record_count = records_mark;
                    overwrite_count = overwrites_mark;
                }
            }
            event->state.record_count = record_count;
            event->state.overwrite_count = overwrite_count;
        }
        if (((guild_create && !unavailable) || guild_update) && object.emojis_seen) {
            const u32 mark = writer.used;
            const GatewayEventDecodeResult appended =
                append_emoji_array(object.emojis, structural_scratch, Slice<u32>{object_scratch, 512},
                                   Slice<u32>{role_scratch, 1'024}, maximum_depth, &writer,
                                   &event->state.emoji_count, &event->state.emoji_role_count);
            if (!appended.ok() && appended.error == GatewayEventDecodeError::scan_failed &&
                appended.scan_error == JsonScanError::output_overflow) {
                auto failed = route_detail(GatewayClientDispatchRouteError::decode, appended.error, 8);
                failed.scan_error = appended.scan_error;
                return failed;
            }
            if (!appended.ok() && writer.overflow && !largest)
                return route_detail(GatewayClientDispatchRouteError::payload_capacity, appended.error, 9);
            // Custom emoji are supplementary. A list beyond the native bound
            // or an unreadable entry leaves the emoji set unknown (a later
            // GUILD_EMOJIS_UPDATE supplies it) and keeps the guild.
            if (appended.ok()) {
                event->state.emojis_known = true;
            } else {
                writer.used = mark;
                writer.overflow = false;
                event->state.emoji_count = event->state.emoji_role_count = 0;
            }
        }
    } else {
        const auto routed = route_channel_object(object, structural_scratch, maximum_depth, writer, event,
                                                 channel_create, channel_update, channel_delete);
        if (!routed.ok())
            return routed;
    }
    *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(writer.output.data), writer.used};
    return route_error(GatewayClientDispatchRouteError::none);
}
} // namespace

GatewayClientDispatchRouteResult gateway_client_route_state_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept {
    bool overflowed = false;
    auto routed = route_state_dispatch(envelope, structural_scratch, maximum_depth, payload_scratch, self_user_id,
                                       event, payload, handled, &overflowed);
    routed.payload_overflow = !routed.ok() && overflowed;
    return routed;
}

GatewayClientDispatchRouteResult gateway_client_route_ready_guild(const GatewayEnvelope& envelope,
                                                                  JsonValue members, Slice<u32> scratch,
                                                                  Slice<char> storage, u64 self_user_id,
                                                                  GatewayClientLargeRecord large,
                                                                  GatewayClientEvent* event,
                                                                  ConstSlice<u8>* payload) noexcept {
    if (!event || !payload || !storage.data || !storage.count || !self_user_id)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    u32 capacity = storage.count;
    for (;;) {
        reset_value(event);
        reset_value(payload);
        event->kind = GatewayClientEventKind::dispatch;
        event->sequence = envelope.sequence;
        event->has_sequence = envelope.has_sequence;
        bool handled = false, overflowed = false;
        auto routed = route_state_dispatch(envelope, scratch, 64, storage, self_user_id, event, payload,
                                           &handled, &overflowed, members);
        routed.payload_overflow = !routed.ok() && overflowed;
        event->dispatch_payload_bytes = payload->count;
        if (routed.ok()) {
            gateway_client_append_notification_sidecar(envelope, scratch, storage, self_user_id, event,
                                                       payload);
            return routed;
        }
        if (!overflowed || !large.acquire || capacity >= kGatewayClientGuildRecordCapacity)
            return routed;
        capacity = grown_record_capacity(capacity);
        if (!large.acquire(large.context, capacity, &storage) || !storage.data || storage.count < capacity)
            return routed;
    }
}

GatewayClientDispatchRouteResult gateway_client_route_self_members(JsonValue members, u64 guild_id,
                                                                   u64 self_user_id, Slice<u32> scratch,
                                                                   Slice<char> storage,
                                                                   GatewayClientEvent* event,
                                                                   ConstSlice<u8>* payload) noexcept {
    if (!event || !payload || !guild_id || !self_user_id || members.kind != JsonValueKind::array)
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    reset_value(event);
    reset_value(payload);
    u32 object_scratch[512]{};
    PayloadWriter writer{storage, 0};
    bool found = false;
    const auto decoded =
        append_self_member_roles(members, scratch, {object_scratch, 512}, 64, self_user_id, &writer,
                                 &event->state.member_role_count, &event->state, &found);
    if (!decoded.ok() || event->state.member_role_count > kGatewayGuildRoleMaximum)
        return route_error(GatewayClientDispatchRouteError::decode,
                           decoded.ok() ? GatewayEventDecodeError::invalid_data : decoded.error);
    if (!found)
        return {};
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_kind = GatewayClientDispatchKind::guild_member_update;
    event->state.id = self_user_id;
    event->state.guild_id = guild_id;
    event->state.fields = gateway_client_state_field_id | gateway_client_state_field_guild_id;
    event->state.record_count = event->state.member_role_count;
    event->dispatch_payload_bytes = writer.used;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    return {};
}

GatewayClientDispatchRouteResult gateway_client_route_dispatch_large(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, GatewayClientLargeRecord large,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    auto routed = gateway_client_route_dispatch(envelope, structural_scratch, maximum_depth, content_scratch,
                                                bulk_ids, self_user_id, event, payload);
    u32 capacity = content_scratch.count;
    while (!routed.ok() && routed.payload_overflow && large.acquire &&
           capacity < kGatewayClientGuildRecordCapacity) {
        // Grow geometrically: an oversized guild routes at most four more
        // times, and only the pages its record touches are ever committed.
        capacity = grown_record_capacity(capacity);
        Slice<char> storage{};
        if (!large.acquire(large.context, capacity, &storage) || !storage.data || storage.count < capacity)
            return routed;
        reset_value(event);
        reset_value(payload);
        routed = gateway_client_route_dispatch(envelope, structural_scratch, maximum_depth,
                                               {storage.data, capacity}, bulk_ids, self_user_id, event, payload);
    }
    return routed;
}

bool gateway_client_route_guild_placeholder(JsonValue guild, Slice<u32> structural_scratch,
                                            u32 maximum_depth, GatewayClientEvent* event) noexcept {
    if (!event || guild.kind != JsonValueKind::object || !guild.source.data || !structural_scratch.data ||
        !structural_scratch.count || !maximum_depth)
        return false;
    const JsonScanResult scan = json_scan_structurals_avx1(guild.source, structural_scratch);
    if (!scan.ok())
        return false;
    struct Identity {
        u64 id;
        bool seen;
    } identity{};
    const JsonReadResult read = json_visit_value_object(
        guild, ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth,
        +[](void* opaque, JsonString name, JsonValue value) noexcept {
            auto& found = *static_cast<Identity*>(opaque);
            if (!json_name_is(name, utf8_literal("id")))
                return true;
            if (found.seen)
                return false;
            found.seen = true;
            return json_value_to_snowflake(value, &found.id);
        },
        &identity);
    if (!read.ok() || !identity.seen || !identity.id)
        return false;
    reset_value<GatewayClientEvent>(event);
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_kind = GatewayClientDispatchKind::guild_create;
    event->state.id = identity.id;
    event->state.unavailable = true;
    event->state.fields = static_cast<u16>(gateway_client_state_field_id | gateway_client_state_field_unavailable);
    return true;
}

namespace {
struct RegionalContentPolicy {
    u8 seen, flags;
};
bool regional_content_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& policy = *static_cast<RegionalContentPolicy*>(opaque);
    const u32 bit = member_bit<"teen_by_default_settings", "age_gated_features">(name);
    if (!bit) return true;
    if (policy.seen & bit) return false;
    policy.seen = static_cast<u8>(policy.seen | bit);
    u64 flags = 0;
    if (!json_value_to_u64(value, &flags) || flags > 0xffffffffu) return false;
    if (bit == 1 && (flags & 1)) policy.flags |= account_content_teen_defaults;
    if (bit == 2 && (flags & 2)) policy.flags |= account_content_age_confirmation;
    return true;
}
u8 ready_content_region(JsonValue value, Slice<u32> scratch, u32 depth) noexcept {
    if (!value.source.data || value.kind != JsonValueKind::object) return 0;
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) return 0;
    RegionalContentPolicy policy{};
    const auto read = json_visit_value_object(value, {scratch.data, scan.count}, depth,
                                               regional_content_field, &policy);
    return read.ok() ? policy.flags : u8{0};
}
bool ready_content_parent(JsonValue value, Slice<u32> scratch, u32 depth) noexcept {
    if (!value.source.data || value.kind != JsonValueKind::array) return false;
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) return false;
    bool parent = false;
    return gateway_client_linked_parent({value.source, {scratch.data, scan.count}}, value, depth, &parent) && parent;
}
} // namespace

GatewayClientDispatchRouteResult gateway_client_route_ready(
    const GatewayReady& ready, Slice<u32> structural_scratch, Slice<u32> object_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (!event || !payload || ready.user_id == 0 || ready.guilds.kind != JsonValueKind::array ||
        !ready.guilds.source.data || !structural_scratch.data || structural_scratch.count == 0 ||
        !object_scratch.data || object_scratch.count == 0 || maximum_depth == 0 || !payload_scratch.data) {
        return route_error(GatewayClientDispatchRouteError::invalid_input);
    }
    GatewayClientReadStateEvent read_state{};
    if (ready.read_state.source.data) {
        const auto decoded = gateway_client_read_state_metadata(ready.read_state, structural_scratch,
                                                                maximum_depth, &read_state);
        if (!decoded.ok())
            return decoded;
    }
    const JsonScanResult scan = json_scan_structurals_avx1(ready.guilds.source, structural_scratch);
    if (!scan.ok()) {
        GatewayClientDispatchRouteResult result =
            route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
        result.scan_error = scan.error;
        result.error_offset = scan.error_offset;
        return result;
    }
    PayloadWriter writer{payload_scratch, 0};
    ReadyContext context{&writer,
                         object_scratch,
                         maximum_depth,
                         0,
                         0,
                         GatewayEventDecodeError::none,
                         JsonScanError::none,
                         JsonReadError::none};
    const JsonReadResult read =
        json_visit_value_array(ready.guilds, ConstSlice<u32>{structural_scratch.data, scan.count},
                              maximum_depth, visit_ready_guild, &context);
    if (!read.ok()) {
        GatewayClientDispatchRouteResult result = route_error(
            context.error == GatewayEventDecodeError::invalid_data
                ? GatewayClientDispatchRouteError::payload_capacity
                : GatewayClientDispatchRouteError::decode,
            context.error != GatewayEventDecodeError::none ? context.error
                                                           : GatewayEventDecodeError::invalid_json);
        result.read_error = read.error;
        if (context.scan_error != JsonScanError::none) {
            result.scan_error = context.scan_error;
        }
        if (context.read_error != JsonReadError::none) {
            result.read_error = context.read_error;
        }
        result.error_offset = context.error_offset != 0 ? context.error_offset : read.error_offset;
        return result;
    }
    reset_value<GatewayClientEvent>(event);
    event->kind = GatewayClientEventKind::ready;
    event->state.id = ready.user_id;
    event->state.read_state_partial = read_state.partial;
    if (!upload_policy_decode(ready.apex_experiments, ready.user_id, structural_scratch,
                              &event->state.upload_policy))
        return route_error(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_data);
    event->state.upload_policy_known = true;
    event->state.account_content_flags = ready_content_region(ready.regional_feature_config,
                                                               structural_scratch, maximum_depth);
    if (ready_content_parent(ready.linked_users, structural_scratch, maximum_depth))
        event->state.account_content_flags |= account_content_parent_managed;
    event->state.fields = gateway_client_state_field_id;
    event->state.record_count = context.count;
    event->dispatch_payload_bytes = writer.used;
    *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(payload_scratch.data), writer.used};
    return route_error(GatewayClientDispatchRouteError::none);
}

} // namespace maicord
