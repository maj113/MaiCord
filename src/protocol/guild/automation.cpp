#include "maicord/protocol/guild/automation.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_field_lists.hpp"

namespace maicord {
namespace {

using namespace protocol_detail;
using Op = GuildAutomationOperation;
constexpr u32 kDepth = 16;

bool space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Every item of a list holds 1 to `codepoints` code points; at most `maximum` items.
bool list_valid(const AutoModList& list, u32 maximum, u32 codepoints) noexcept {
    if (!view_valid(list.text) || list.count > maximum || (!list.count && list.text.bytes))
        return false;
    u32 at = 0, seen = 0;
    Utf8View item{};
    while (automod_list_next(list.text, &at, &item)) {
        const auto valid = utf8_validate(item);
        if (!valid.valid || !valid.codepoints || valid.codepoints > codepoints)
            return false;
        ++seen;
    }
    return seen == list.count;
}

bool ids_valid(ConstSlice<u64> ids, u32 maximum) noexcept {
    if ((!ids.data && ids.count) || ids.count > maximum)
        return false;
    for (u32 i = 0; i < ids.count; ++i)
        if (!ids[i])
            return false;
    return true;
}

bool webhook_valid(const GuildWebhookWrite* w, bool update) noexcept {
    if (!w || !name_text_valid(w->name, kGuildWebhookNameBytes, kGuildWebhookNameCodepoints, true) ||
        !view_valid(w->avatar))
        return false;
    if (!update)
        return !w->avatar_set && !w->avatar.bytes && !w->channel_id;
    if (!w->channel_id || (!w->avatar_set && w->avatar.bytes))
        return false;
    // A new avatar is a data URI.
    return !w->avatar.bytes || (w->avatar.bytes > 5 && w->avatar.data[0] == 'd' && w->avatar.data[1] == 'a' &&
                                w->avatar.data[2] == 't' && w->avatar.data[3] == 'a' && w->avatar.data[4] == ':');
}

bool permissions_valid(ConstSlice<GuildCommandPermission> entries) noexcept {
    if ((!entries.data && entries.count) || entries.count > kGuildCommandPermissionMaximum)
        return false;
    for (u32 i = 0; i < entries.count; ++i)
        if (!entries[i].id || entries[i].type < guild_command_permission_role ||
            entries[i].type > guild_command_permission_channel)
            return false;
    return true;
}

void write_list(JsonBody& b, const char* name, const AutoModList& list) noexcept {
    b.begin_array(name);
    u32 at = 0;
    Utf8View item{};
    while (automod_list_next(list.text, &at, &item))
        b.element(json_text(item));
    b.end_array();
}

void write_ids(JsonBody& b, const char* name, ConstSlice<u64> ids) noexcept {
    b.begin_array(name);
    for (u32 i = 0; i < ids.count; ++i)
        b.element(snowflake_text(ids[i]));
    b.end_array();
}

void write_action(JsonBody& b, u32 type) noexcept {
    b.next();
    b.put("{\"type\":", type, ",\"metadata\":");
    b.begin_object();
}

void write_rule(JsonBody& b, u64 guild_id, const AutoModRuleWrite& r, bool update) noexcept {
    b.begin_object();
    b.member_if(update, "id", snowflake_text(r.id));
    b.member("name", json_text(r.name));
    b.member("guild_id", snowflake_text(guild_id));
    b.member("event_type", u32{automod_event(r.trigger_type)});
    b.member("trigger_type", u32{r.trigger_type});
    switch (r.trigger_type) {
    case automod_trigger_keyword:
    case automod_trigger_member_profile:
        b.begin_object("trigger_metadata");
        write_list(b, "keyword_filter", r.keywords);
        write_list(b, "regex_patterns", r.regex_patterns);
        write_list(b, "allow_list", r.allow_list);
        b.end_object();
        break;
    case automod_trigger_keyword_preset:
        b.begin_object("trigger_metadata");
        write_list(b, "allow_list", r.allow_list);
        b.begin_array("presets");
        for (u32 preset = 0; preset < 3; ++preset)
            if (r.presets & (1u << preset))
                b.element(preset + 1);
        b.end_array();
        b.end_object();
        break;
    case automod_trigger_mention_spam:
        b.begin_object("trigger_metadata");
        b.member("mention_total_limit", u32{r.mention_limit});
        b.member("mention_raid_protection_enabled", json_bool(r.mention_raid_protection));
        b.end_object();
        break;
    default: break;
    }
    b.begin_array("actions");
    if (r.actions & automod_action_block) {
        write_action(b, 1);
        b.member_if(r.custom_message.bytes != 0, "custom_message", json_text(r.custom_message));
        b.end_object();
        b.end_object();
    }
    if (r.actions & automod_action_alert) {
        write_action(b, 2);
        b.member("channel_id", snowflake_text(r.alert_channel_id));
        b.end_object();
        b.end_object();
    }
    if (r.actions & automod_action_timeout) {
        write_action(b, 3);
        b.member("duration_seconds", r.timeout_seconds);
        b.end_object();
        b.end_object();
    }
    if (r.actions & automod_action_quarantine) {
        write_action(b, 4);
        b.end_object();
        b.end_object();
    }
    b.end_array();
    b.member("enabled", json_bool(r.enabled));
    b.member("creator_id", nullable_snowflake(r.creator_id));
    b.member("position", r.position);
    write_ids(b, "exempt_channels", r.exempt_channels);
    write_ids(b, "exempt_roles", r.exempt_roles);
    b.end_object();
}

// Decoders --------------------------------------------------------------------

template <u32 Capacity, typename Size> bool label(const JsonValue& value, char (&output)[Capacity], Size* bytes) noexcept {
    *bytes = 0;
    if (value.kind == JsonValueKind::null_value)
        return true;
    if (value.kind != JsonValueKind::string)
        return false;
    *bytes = static_cast<Size>(copy_text_truncated<1024>(value, output, Capacity));
    return true;
}

struct UserParse {
    u64* id;
    char* name;
    u32 capacity;
    u8* bytes;
    bool global;
};
bool visit_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<UserParse*>(opaque);
    const u32 index = member_index<"id", "username", "global_name">(name);
    if (index == 0)
        return json_value_to_snowflake(value, p.id);
    if (index > 2 || value.kind == JsonValueKind::null_value)
        return true;
    if (value.kind != JsonValueKind::string)
        return false;
    // The display name wins over the username, in either order.
    if (p.global && index == 1)
        return true;
    const u32 copied = copy_text_truncated<1024>(value, p.name, p.capacity);
    if (copied) {
        *p.bytes = static_cast<u8>(copied);
        p.global = index == 2;
    }
    return true;
}
template <u32 Capacity>
bool user(const JsonIndexedDocument& d, const JsonValue& value, u64* id, char (&name)[Capacity], u8* bytes) noexcept {
    static_assert(Capacity <= 255);
    if (value.kind == JsonValueKind::null_value)
        return true;
    UserParse p{id, name, Capacity, bytes, false};
    return value.kind == JsonValueKind::object && json_visit_indexed_object(d, value, kDepth, visit_user, &p).ok();
}

// Webhooks.
struct WebhookParse {
    const JsonIndexedDocument* document;
    GuildWebhook* hook;
    bool id_seen;
};
// The name of a source_guild or source_channel object.
template <u32 Capacity>
bool source_name(const JsonIndexedDocument& d, const JsonValue& value, char (&output)[Capacity], u8* bytes) noexcept {
    if (value.kind != JsonValueKind::object)
        return value.kind == JsonValueKind::null_value;
    const JsonValue name = object_member(d, value, utf8_literal("name"), kDepth);
    return !name.source.bytes || label(name, output, bytes);
}
bool visit_webhook(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<WebhookParse*>(opaque);
    auto& h = *p.hook;
    switch (member_index<"id", "type", "channel_id", "name", "avatar", "token", "application_id", "user",
                         "source_guild", "source_channel">(name)) {
    case 0: return p.id_seen = json_value_to_snowflake(value, &h.id);
    case 1: {
        u64 type = 0;
        if (!json_value_to_u64(value, &type) || type > 255)
            return false;
        h.type = static_cast<u8>(type);
        return true;
    }
    case 2: return json_value_to_nullable_snowflake(value, &h.channel_id);
    case 3: return label(value, h.name, &h.name_bytes);
    case 4: return label(value, h.avatar, &h.avatar_bytes);
    case 5: return label(value, h.token, &h.token_bytes);
    case 6: return json_value_to_nullable_snowflake(value, &h.application_id);
    case 7: return user(*p.document, value, &h.creator_id, h.creator, &h.creator_bytes);
    case 8: return source_name(*p.document, value, h.source_guild, &h.source_guild_bytes);
    case 9: return source_name(*p.document, value, h.source_channel, &h.source_channel_bytes);
    default: return true;
    }
}

// A list being read: how many rows the output holds and how many were read, whatever the row.
struct ListHead {
    const JsonIndexedDocument* document;
    u32 capacity, count;
    GuildAutomationError error;
};
template <typename Row> struct ListParse : ListHead {
    Row* rows;
};

bool webhook_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ListParse<GuildWebhook>*>(opaque);
    if (p.count >= p.capacity) {
        p.error = GuildAutomationError::capacity;
        return false;
    }
    GuildWebhook& hook = p.rows[p.count];
    reset_value(&hook);
    WebhookParse parse{p.document, &hook, false};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_webhook, &parse).ok() || !parse.id_seen) {
        p.error = GuildAutomationError::field;
        return false;
    }
    ++p.count;
    return true;
}

// Integrations.
struct IntegrationParse {
    const JsonIndexedDocument* document;
    GuildIntegration* row;
    bool id_seen;
};
bool visit_application(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<IntegrationParse*>(opaque);
    auto& r = *p.row;
    switch (member_index<"id", "icon", "bot">(name)) {
    case 0: return json_value_to_snowflake(value, &r.application_id);
    case 1: return label(value, r.icon, &r.icon_bytes);
    case 2: {
        if (value.kind != JsonValueKind::object)
            return value.kind == JsonValueKind::null_value;
        const JsonValue id = object_member(*p.document, value, utf8_literal("id"), kDepth);
        return !id.source.bytes || json_value_to_snowflake(id, &r.bot_id);
    }
    default: return true;
    }
}
bool visit_integration(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<IntegrationParse*>(opaque);
    auto& r = *p.row;
    switch (member_index<"id", "name", "type", "enabled", "application", "user">(name)) {
    case 0:
        // "twitch-partners" is an ID too; only snowflakes can be removed.
        p.id_seen = value.kind == JsonValueKind::string;
        if (!json_value_to_snowflake(value, &r.id))
            r.id = 0;
        return p.id_seen;
    case 1: return label(value, r.name, &r.name_bytes);
    case 2: {
        JsonString type{};
        if (!json_value_as_string(value, &type))
            return false;
        r.type = json_string_equals_ascii(type, utf8_literal("discord"))   ? GuildIntegrationType::discord
                 : json_string_equals_ascii(type, utf8_literal("twitch"))  ? GuildIntegrationType::twitch
                 : json_string_equals_ascii(type, utf8_literal("youtube")) ? GuildIntegrationType::youtube
                 : json_string_equals_ascii(type, utf8_literal("guild_subscription"))
                     ? GuildIntegrationType::guild_subscription
                     : GuildIntegrationType::other;
        return true;
    }
    case 3: return value.kind == JsonValueKind::null_value || json_value_to_bool(value, &r.enabled);
    case 4:
        return value.kind == JsonValueKind::null_value ||
               (value.kind == JsonValueKind::object &&
                json_visit_indexed_object(*p.document, value, kDepth, visit_application, &p).ok());
    case 5: return user(*p.document, value, &r.user_id, r.user, &r.user_bytes);
    default: return true;
    }
}
bool integration_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ListParse<GuildIntegration>*>(opaque);
    if (p.count >= p.capacity) {
        p.error = GuildAutomationError::capacity;
        return false;
    }
    GuildIntegration& row = p.rows[p.count];
    reset_value(&row);
    IntegrationParse parse{p.document, &row, false};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_integration, &parse).ok() || !parse.id_seen) {
        p.error = GuildAutomationError::field;
        return false;
    }
    ++p.count;
    return true;
}

// Command permissions.
struct EntryParse {
    GuildCommandPermission entry;
    bool id_seen, type_seen, permission_seen;
};
bool visit_permission(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<EntryParse*>(opaque);
    switch (member_index<"id", "type", "permission">(name)) {
    case 0: return p.id_seen = json_value_to_snowflake(value, &p.entry.id);
    case 1: {
        u64 type = 0;
        if (!json_value_to_u64(value, &type) || type < guild_command_permission_role ||
            type > guild_command_permission_channel)
            return false;
        p.entry.type = static_cast<u8>(type);
        return p.type_seen = true;
    }
    case 2: return p.permission_seen = json_value_to_bool(value, &p.entry.permission);
    default: return true;
    }
}
struct CommandParse {
    const JsonIndexedDocument* document;
    GuildCommandPermissionsPage* page;
    u64 application_id;
    u64 id;
    JsonValue permissions;
    GuildAutomationError error;
};
bool permission_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<CommandParse*>(opaque);
    EntryParse entry{};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_permission, &entry).ok() || !entry.id_seen ||
        !entry.type_seen || !entry.permission_seen) {
        p.error = GuildAutomationError::field;
        return false;
    }
    if (p.page->count >= p.page->entries.count) {
        p.error = GuildAutomationError::capacity;
        return false;
    }
    p.page->entries[p.page->count++] = entry.entry;
    return true;
}
bool visit_command(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<CommandParse*>(opaque);
    switch (member_index<"id", "permissions">(name)) {
    case 0: return json_value_to_snowflake(value, &p.id);
    case 1:
        p.permissions = value;
        return value.kind == JsonValueKind::array;
    default: return true;
    }
}
bool command_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<CommandParse*>(opaque);
    p.id = 0;
    p.permissions = {};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_command, &p).ok() || !p.id ||
        p.permissions.kind != JsonValueKind::array) {
        p.error = p.error == GuildAutomationError::none ? GuildAutomationError::field : p.error;
        return false;
    }
    if (p.id != p.application_id) {
        ++p.page->command_overrides;
        return true;
    }
    p.page->count = 0;
    return json_visit_indexed_array(*p.document, p.permissions, kDepth, permission_item, &p).ok();
}

// The application's commands (GET /guilds/{g}/application-commands/{a}).
struct CommandListParse {
    const JsonIndexedDocument* document;
    Slice<GuildApplicationCommand> rows;
    u32 count;
    u64 application_id;
    GuildApplicationCommand row;
    u64 owner;
    bool id_seen;
    GuildAutomationError error;
};
bool visit_command_row(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<CommandListParse*>(opaque);
    auto& r = p.row;
    switch (member_index<"id", "application_id", "type", "name", "description">(name)) {
    case 0: return p.id_seen = json_value_to_snowflake(value, &r.id);
    case 1: return json_value_to_snowflake(value, &p.owner);
    case 2: {
        u64 type = 0;
        if (!json_value_to_u64(value, &type) || type > 255)
            return false;
        r.type = static_cast<u8>(type);
        return true;
    }
    case 3: return label(value, r.name, &r.name_bytes);
    case 4: return label(value, r.description, &r.description_bytes);
    default: return true;
    }
}
bool command_row_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<CommandListParse*>(opaque);
    reset_value(&p.row);
    p.owner = 0;
    p.id_seen = false;
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_command_row, &p).ok() || !p.id_seen) {
        p.error = GuildAutomationError::field;
        return false;
    }
    if (p.owner && p.owner != p.application_id)
        return true;
    if (p.row.type == 0)
        p.row.type = guild_command_chat_input;
    if (p.count < p.rows.count)
        copy_value(&p.rows[p.count++], p.row);
    return true;
}
// Stops at the first element: the array is not empty.
bool first_item(void* opaque, u32, JsonValue) noexcept {
    *static_cast<bool*>(opaque) = true;
    return false;
}
// An entry of "permissions": marks its command when it has overwrites.
bool command_mark_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<CommandListParse*>(opaque);
    if (value.kind != JsonValueKind::object)
        return false;
    u64 id = 0;
    const JsonValue id_value = object_member(*p.document, value, utf8_literal("id"), kDepth);
    const JsonValue entries = object_member(*p.document, value, utf8_literal("permissions"), kDepth);
    if (!json_value_to_snowflake(id_value, &id))
        return false;
    // An empty array is no overwrite.
    bool any = false;
    if (entries.kind == JsonValueKind::array)
        static_cast<void>(json_visit_indexed_array(*p.document, entries, kDepth, first_item, &any));
    for (u32 i = 0; any && i < p.count; ++i)
        if (p.rows[i].id == id)
            p.rows[i].overridden = true;
    return true;
}

// Ids of a Gateway event.
bool visit_event(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<GuildAutomationEvent*>(opaque);
    u64* slots[]{&e.guild_id, &e.id, &e.application_id, &e.channel_id};
    const u32 index = member_index<"guild_id", "id", "application_id", "channel_id">(name);
    if (index >= 4 || value.kind != JsonValueKind::string)
        return true;
    return json_value_to_snowflake(value, slots[index]);
}

// AutoMod rules.
struct RuleParse {
    const JsonIndexedDocument* document;
    AutoModRule* rule;
    AutoModPool* pool;
    JsonValue action_metadata;
    u64* ids;
    u32 list_at, list_count, ids_capacity;
    u8 action_type;
    bool id_seen, trigger_seen, capacity;
};
bool pool_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return false;
    auto& pool = *p.pool;
    const u32 separator = p.list_count ? 1 : 0;
    if (pool.used + separator > pool.bytes.count) {
        p.capacity = true;
        return false;
    }
    char* at = pool.bytes.data + pool.used + separator;
    const auto decoded = json_string_decode_utf8(string, {at, pool.bytes.count - pool.used - separator});
    if (!decoded.ok()) {
        p.capacity = decoded.error == JsonStringDecodeError::output_too_small;
        return false;
    }
    if (!decoded.bytes)
        return true;
    // An item never holds the separator.
    for (u32 i = 0; i < decoded.bytes; ++i)
        if (at[i] == '\n')
            at[i] = ' ';
    if (separator)
        pool.bytes[pool.used] = '\n';
    pool.used += separator + decoded.bytes;
    return ++p.list_count <= 0xFFFF;
}
bool pool_list(RuleParse& p, const JsonValue& value, u32* at, u32* bytes, u16* count) noexcept {
    if (value.kind == JsonValueKind::null_value)
        return true;
    p.list_at = p.pool->used;
    p.list_count = 0;
    if (value.kind != JsonValueKind::array ||
        !json_visit_indexed_array(*p.document, value, kDepth, pool_item, &p).ok())
        return false;
    *at = p.list_at;
    *bytes = p.pool->used - p.list_at;
    *count = static_cast<u16>(p.list_count);
    return true;
}
bool preset_item(void* opaque, u32, JsonValue value) noexcept {
    auto& r = *static_cast<RuleParse*>(opaque)->rule;
    u64 preset = 0;
    if (!json_value_to_u64(value, &preset))
        return false;
    if (preset >= 1 && preset <= 3)
        r.presets = static_cast<u8>(r.presets | (1u << (preset - 1)));
    else
        r.foreign = true;
    return true;
}
bool visit_metadata(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    auto& r = *p.rule;
    switch (member_index<"keyword_filter", "regex_patterns", "allow_list", "presets", "mention_total_limit",
                         "mention_raid_protection_enabled">(name)) {
    case 0: return pool_list(p, value, &r.keywords_at, &r.keywords_bytes, &r.keyword_count);
    case 1: return pool_list(p, value, &r.regex_at, &r.regex_bytes, &r.regex_count);
    case 2: return pool_list(p, value, &r.allow_at, &r.allow_bytes, &r.allow_count);
    case 3:
        return value.kind == JsonValueKind::null_value ||
               (value.kind == JsonValueKind::array &&
                json_visit_indexed_array(*p.document, value, kDepth, preset_item, &p).ok());
    case 4: {
        u64 limit = 0;
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (!json_value_to_u64(value, &limit) || limit > 255)
            return false;
        r.mention_limit = static_cast<u8>(limit);
        return true;
    }
    case 5: return value.kind == JsonValueKind::null_value || json_value_to_bool(value, &r.mention_raid_protection);
    default: return true;
    }
}
bool visit_action_metadata(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    auto& r = *p.rule;
    switch (member_index<"custom_message", "channel_id", "duration_seconds">(name)) {
    case 0: return p.action_type != 1 || label(value, r.custom_message, &r.custom_message_bytes);
    case 1: return p.action_type != 2 || json_value_to_nullable_snowflake(value, &r.alert_channel_id);
    case 2: {
        u64 seconds = 0;
        if (p.action_type != 3 || value.kind == JsonValueKind::null_value)
            return true;
        if (!json_value_to_u64(value, &seconds) || seconds > kAutoModTimeoutMaximumSeconds)
            return false;
        r.timeout_seconds = static_cast<u32>(seconds);
        return true;
    }
    default: return true;
    }
}
bool visit_action(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    switch (member_index<"type", "metadata">(name)) {
    case 0: {
        u64 type = 0;
        if (!json_value_to_u64(value, &type))
            return false;
        p.action_type = type >= 1 && type <= 4 ? static_cast<u8>(type) : 0;
        return true;
    }
    case 1:
        if (value.kind == JsonValueKind::object)
            p.action_metadata = value;
        return value.kind == JsonValueKind::object || value.kind == JsonValueKind::null_value;
    default: return true;
    }
}
bool action_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    p.action_type = 0;
    p.action_metadata = {};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_action, &p).ok())
        return false;
    if (!p.action_type) {
        p.rule->foreign = true;
        return true;
    }
    p.rule->actions = static_cast<u8>(p.rule->actions | (1u << (p.action_type - 1)));
    // Metadata may come before the type: it is read once the type is known.
    return !p.action_metadata.source.bytes ||
           json_visit_indexed_object(*p.document, p.action_metadata, kDepth, visit_action_metadata, &p).ok();
}
bool id_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    if (p.list_count >= p.ids_capacity)
        return false;
    return json_value_to_snowflake(value, &p.ids[p.list_count++]);
}
bool ids(RuleParse& p, const JsonValue& value, u64* output, u32 capacity, u8* count) noexcept {
    if (value.kind == JsonValueKind::null_value)
        return true;
    p.ids = output;
    p.ids_capacity = capacity;
    p.list_count = 0;
    const bool ok = value.kind == JsonValueKind::array &&
                    json_visit_indexed_array(*p.document, value, kDepth, id_item, &p).ok();
    *count = static_cast<u8>(p.list_count);
    return ok;
}
bool visit_rule(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RuleParse*>(opaque);
    auto& r = *p.rule;
    switch (member_index<"id", "name", "creator_id", "event_type", "trigger_type", "trigger_metadata", "actions",
                         "enabled", "exempt_roles", "exempt_channels", "position">(name)) {
    case 0: return p.id_seen = json_value_to_snowflake(value, &r.id);
    case 1: return label(value, r.name, &r.name_bytes);
    case 2: return json_value_to_nullable_snowflake(value, &r.creator_id);
    case 3:
    case 4: {
        u64 number = 0;
        if (!json_value_to_u64(value, &number) || number > 255)
            return false;
        (name_is(name, "event_type") ? r.event_type : r.trigger_type) = static_cast<u8>(number);
        p.trigger_seen = p.trigger_seen || name_is(name, "trigger_type");
        return true;
    }
    case 5:
        return value.kind == JsonValueKind::null_value ||
               (value.kind == JsonValueKind::object &&
                json_visit_indexed_object(*p.document, value, kDepth, visit_metadata, &p).ok());
    case 6:
        return value.kind == JsonValueKind::array &&
               json_visit_indexed_array(*p.document, value, kDepth, action_item, &p).ok();
    case 7: return json_value_to_bool(value, &r.enabled);
    case 8: return ids(p, value, r.exempt_roles, kAutoModExemptRoleMaximum, &r.exempt_role_count);
    case 9: return ids(p, value, r.exempt_channels, kAutoModExemptChannelMaximum, &r.exempt_channel_count);
    case 10: {
        u64 position = 0;
        if (value.kind != JsonValueKind::null_value && (!json_value_to_u64(value, &position) || position > 0xFFFF'FFFFu))
            return false;
        r.position = static_cast<u32>(position);
        return true;
    }
    default: return true;
    }
}
struct RulesParse {
    const JsonIndexedDocument* document;
    Slice<AutoModRule> rows;
    AutoModPool* pool;
    u32 count;
    GuildAutomationError error;
};
bool decode_rule(RulesParse& p, const JsonValue& value) noexcept {
    if (p.count >= p.rows.count) {
        p.error = GuildAutomationError::capacity;
        return false;
    }
    AutoModRule& rule = p.rows[p.count];
    reset_value(&rule);
    RuleParse parse{p.document, &rule, p.pool, {}, nullptr, 0, 0, 0, 0, false, false, false};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*p.document, value, kDepth, visit_rule, &parse).ok() || !parse.id_seen ||
        !parse.trigger_seen) {
        p.error = parse.capacity ? GuildAutomationError::capacity : GuildAutomationError::field;
        return false;
    }
    rule.foreign = rule.foreign || !automod_trigger_editable(rule.trigger_type) ||
                   (rule.actions & ~automod_actions_allowed(rule.trigger_type));
    ++p.count;
    return true;
}
bool rule_item(void* opaque, u32, JsonValue value) noexcept {
    return decode_rule(*static_cast<RulesParse*>(opaque), value);
}

GuildAutomationResult failure(const JsonReadResult& read, GuildAutomationError error) noexcept {
    return {0, error != GuildAutomationError::none ? error
               : read.ok()                         ? GuildAutomationError::field
                                                   : GuildAutomationError::json};
}

// The array `body` holds, a row for each of its elements; `*count` rows.
GuildAutomationResult decode_list(Utf8View body, Slice<u32> scratch, ListHead* list,
                                  bool (*item)(void* opaque, u32, JsonValue) noexcept, u32* count) noexcept {
    *count = 0;
    const auto s = scan_json<GuildAutomationError>(body, scratch);
    if (s.error != GuildAutomationError::none)
        return {0, s.error};
    list->document = &s.document;
    const auto read = json_visit_root_array(s.document.input, s.document.structurals, kDepth, item, list);
    if (!read.ok())
        return failure(read, list->error);
    *count = list->count;
    return {body.bytes, GuildAutomationError::none};
}

} // namespace

bool automod_list_next(Utf8View list, u32* at, Utf8View* item) noexcept {
    if (!at || !item || *at >= list.bytes)
        return false;
    u32 end = *at;
    while (end < list.bytes && list.data[end] != '\n')
        ++end;
    *item = {list.data + *at, end - *at};
    *at = end + 1;
    return true;
}

namespace {

// Appends items split at `separators`, each with its runs of white space folded (fold) and trimmed.
bool split(Utf8View input, Slice<char> output, AutoModList* list, bool fold) noexcept {
    if (!list || !view_valid(input) || (!output.data && output.count))
        return false;
    u32 used = 0, count = 0;
    for (u32 start = 0; start <= input.bytes;) {
        u32 end = start;
        while (end < input.bytes && input.data[end] != '\n' && (!fold || (input.data[end] != ',' && input.data[end] != '\t')))
            ++end;
        u32 from = start, to = end;
        while (from < to && space(input.data[from]))
            ++from;
        while (to > from && space(input.data[to - 1]))
            --to;
        if (from < to) {
            if (used + (count ? 1 : 0) + (to - from) > output.count)
                return false;
            if (count)
                output[used++] = '\n';
            bool gap = false;
            for (u32 i = from; i < to; ++i) {
                const bool white = space(input.data[i]);
                // Two or more white spaces become one space; a lone one is kept as it is.
                if (fold && white && i + 1 < to && space(input.data[i + 1])) {
                    gap = true;
                    continue;
                }
                output[used++] = gap && white ? ' ' : input.data[i] == '\r' ? ' ' : input.data[i];
                gap = false;
            }
            ++count;
        }
        start = end + 1;
    }
    *list = {{output.data, used}, count};
    return true;
}

} // namespace

bool automod_split_keywords(Utf8View input, Slice<char> output, AutoModList* list) noexcept {
    return split(input, output, list, true);
}
bool automod_split_lines(Utf8View input, Slice<char> output, AutoModList* list) noexcept {
    return split(input, output, list, false);
}

AutoModRuleProblem automod_rule_problem(const AutoModRuleWrite& r) noexcept {
    using P = AutoModRuleProblem;
    const u8 allowed = automod_actions_allowed(r.trigger_type);
    if (!allowed)
        return P::trigger;
    if (!name_text_valid(r.name, kAutoModRuleNameBytes, kAutoModRuleNameCodepoints, true))
        return P::name;
    const bool words = r.trigger_type == automod_trigger_keyword || r.trigger_type == automod_trigger_member_profile;
    if (!words && (r.keywords.count || r.regex_patterns.count || r.keywords.text.bytes || r.regex_patterns.text.bytes))
        return P::trigger;
    if (words) {
        if (r.trigger_type == automod_trigger_keyword && !r.keywords.count && !r.regex_patterns.count)
            return P::keyword_required;
        if (r.keywords.count > kAutoModKeywordMaximum)
            return P::keyword_count;
        if (!list_valid(r.keywords, kAutoModKeywordMaximum, kAutoModKeywordCodepoints))
            return P::keyword_length;
        if (r.regex_patterns.count > kAutoModRegexMaximum)
            return P::regex_count;
        if (!list_valid(r.regex_patterns, kAutoModRegexMaximum, kAutoModRegexCodepoints))
            return P::regex_length;
    }
    const bool allow = words || r.trigger_type == automod_trigger_keyword_preset;
    const u32 allow_maximum = r.trigger_type == automod_trigger_keyword_preset ? kAutoModAllowPresetMaximum
                                                                               : kAutoModAllowKeywordMaximum;
    if (!allow && (r.allow_list.count || r.allow_list.text.bytes))
        return P::trigger;
    if (r.allow_list.count > allow_maximum)
        return P::allow_count;
    if (!list_valid(r.allow_list, allow_maximum, kAutoModKeywordCodepoints))
        return P::allow_length;
    if ((r.trigger_type == automod_trigger_keyword_preset) != (r.presets != 0) || (r.presets & ~automod_preset_all))
        return P::trigger;
    if (r.trigger_type == automod_trigger_mention_spam
            ? r.mention_limit < kAutoModMentionLimitMinimum || r.mention_limit > kAutoModMentionLimitMaximum
            : r.mention_limit || r.mention_raid_protection)
        return P::mention_limit;
    if (!r.actions)
        return P::action_required;
    if (r.actions & ~allowed)
        return P::trigger;
    const bool alert = (r.actions & automod_action_alert) != 0;
    if (alert != (r.alert_channel_id != 0))
        return P::alert_channel;
    if ((r.actions & automod_action_timeout) ? !r.timeout_seconds || r.timeout_seconds > kAutoModTimeoutMaximumSeconds
                                             : r.timeout_seconds != 0)
        return P::timeout;
    if (r.custom_message.bytes) {
        const auto valid = view_valid(r.custom_message) ? utf8_validate(r.custom_message) : Utf8Validation{};
        if (!(r.actions & automod_action_block) || !valid.valid || valid.codepoints > kAutoModCustomMessageCodepoints)
            return P::custom_message;
    }
    if (!ids_valid(r.exempt_roles, kAutoModExemptRoleMaximum) ||
        !ids_valid(r.exempt_channels, kAutoModExemptChannelMaximum) ||
        (r.trigger_type == automod_trigger_member_profile && r.exempt_channels.count))
        return P::exemptions;
    return P::none;
}

RestMethod guild_automation_method(GuildAutomationOperation op) noexcept {
    switch (op) {
    case Op::integration_delete:
    case Op::webhook_delete:
    case Op::automod_delete: return RestMethod::delete_method;
    case Op::webhook_create:
    case Op::automod_create: return RestMethod::post;
    case Op::webhook_update:
    case Op::automod_update: return RestMethod::patch;
    case Op::command_permissions_update: return RestMethod::put;
    default: return RestMethod::get;
    }
}

bool guild_automation_request_valid(const GuildAutomationRequest& r) noexcept {
    if (r.operation >= Op::count || !r.guild_id)
        return false;
    const bool target = r.operation != Op::integration_list && r.operation != Op::webhook_list &&
                        r.operation != Op::automod_list && r.operation != Op::automod_create;
    if (target != (r.target_id != 0))
        return false;
    const bool webhook = r.operation == Op::webhook_create || r.operation == Op::webhook_update;
    const bool rule = r.operation == Op::automod_create || r.operation == Op::automod_update;
    const bool permissions = r.operation == Op::command_permissions_update;
    if ((r.webhook != nullptr) != webhook || (r.rule != nullptr) != rule || (!permissions && r.permissions.count) ||
        (!permissions && r.command_id) || !permissions_valid(r.permissions))
        return false;
    if (webhook)
        return webhook_valid(r.webhook, r.operation == Op::webhook_update);
    if (rule)
        return automod_rule_problem(*r.rule) == AutoModRuleProblem::none &&
               (r.operation == Op::automod_create ? !r.rule->id : r.rule->id == r.target_id);
    return true;
}

RestRouteKey guild_automation_route(const GuildAutomationRequest& r) noexcept {
    const RestMethod method = guild_automation_method(r.operation);
    const u64 route = kGuildAutomationRouteBase + static_cast<u64>(r.operation);
    switch (r.operation) {
    case Op::webhook_create: return {route, r.target_id, method, RestMajorParameter::channel};
    case Op::webhook_update:
    case Op::webhook_delete: return {route, r.target_id, method, RestMajorParameter::webhook};
    case Op::command_permissions_list:
    case Op::command_permissions_update: return {route, 0, method, RestMajorParameter::none};
    default: return {route, r.guild_id, method, RestMajorParameter::guild};
    }
}

GuildAutomationResult guild_automation_write_target(const GuildAutomationRequest& r, Slice<u8> output) noexcept {
    if (!guild_automation_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, GuildAutomationError::input};
    u8 target[kGuildAutomationMaximumTargetBytes];
    ProtocolWriter w{{target, sizeof(target)}, 0};
    bool ok = true;
    switch (r.operation) {
    case Op::integration_list:
        ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id,
                            "/integrations?include_applications=true&include_role_connections_metadata=true");
        break;
    case Op::integration_delete: ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id, "/integrations/", r.target_id); break;
    case Op::webhook_list: ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id, "/webhooks"); break;
    case Op::webhook_create: ok = protocol_write(&w, "/api/v9/channels/", r.target_id, "/webhooks"); break;
    case Op::webhook_update:
    case Op::webhook_delete: ok = protocol_write(&w, "/api/v9/webhooks/", r.target_id); break;
    case Op::command_permissions_list:
        ok = protocol_write(&w, "/api/v9/applications/", r.target_id, "/guilds/", r.guild_id, "/commands/permissions");
        break;
    case Op::command_permissions_update:
        ok = protocol_write(&w, "/api/v9/applications/", r.target_id, "/guilds/", r.guild_id, "/commands/",
                            r.command_id ? r.command_id : r.target_id, "/permissions");
        break;
    case Op::command_list:
        ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id, "/application-commands/", r.target_id);
        break;
    case Op::automod_list:
    case Op::automod_create: ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id, "/auto-moderation/rules"); break;
    default:
        ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id, "/auto-moderation/rules/", r.target_id);
        break;
    }
    if (!ok)
        return {0, GuildAutomationError::capacity};
    return protocol_writer_finish<GuildAutomationResult>(w, true, output, target);
}

GuildAutomationResult guild_automation_write_body(const GuildAutomationRequest& r, const Slice<u8>& output) noexcept {
    if (!guild_automation_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, GuildAutomationError::input};
    const bool webhook = r.webhook != nullptr, rule = r.rule != nullptr;
    if (!webhook && !rule && r.operation != Op::command_permissions_update)
        return {0, GuildAutomationError::none};
    if (!output.data || !output.count)
        return {0, GuildAutomationError::capacity};
    JsonBody b{output};
    if (rule)
        write_rule(b, r.guild_id, *r.rule, r.operation == Op::automod_update);
    else if (webhook) {
        b.begin_object();
        b.member("name", json_text(r.webhook->name));
        b.member_if(r.webhook->avatar_set, "avatar", nullable_json(r.webhook->avatar));
        b.member_if(r.operation == Op::webhook_update, "channel_id", snowflake_text(r.webhook->channel_id));
        b.end_object();
    } else {
        b.begin_object();
        b.begin_array("permissions");
        for (u32 i = 0; i < r.permissions.count; ++i) {
            const auto& e = r.permissions[i];
            b.next();
            b.put("{\"id\":", snowflake_text(e.id), ",\"type\":", u32{e.type}, ",\"permission\":",
                  json_bool(e.permission), "}");
        }
        b.end_array();
        b.end_object();
    }
    if (!b.ok) {
        secure_clear_bytes(output.data, b.writer.used);
        return {0, GuildAutomationError::capacity};
    }
    return {b.writer.used, GuildAutomationError::none};
}

GuildAutomationResult guild_webhooks_decode(Utf8View body, Slice<u32> scratch, Slice<GuildWebhook> output,
                                            u32* count) noexcept {
    if (!count || (!output.data && output.count))
        return {0, GuildAutomationError::input};
    ListParse<GuildWebhook> list{{nullptr, output.count, 0, GuildAutomationError::none}, output.data};
    return decode_list(body, scratch, &list, webhook_item, count);
}

GuildAutomationResult guild_webhook_decode(Utf8View body, Slice<u32> scratch, GuildWebhook* output) noexcept {
    if (!output)
        return {0, GuildAutomationError::input};
    const auto s = scan_json<GuildAutomationError>(body, scratch);
    if (s.error != GuildAutomationError::none)
        return {0, s.error};
    reset_value(output);
    WebhookParse p{&s.document, output, false};
    const auto read = json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_webhook, &p);
    if (!read.ok() || !p.id_seen)
        return failure(read, GuildAutomationError::none);
    return {body.bytes, GuildAutomationError::none};
}

GuildAutomationResult guild_integrations_decode(Utf8View body, Slice<u32> scratch, Slice<GuildIntegration> output,
                                                u32* count) noexcept {
    if (!count || (!output.data && output.count))
        return {0, GuildAutomationError::input};
    ListParse<GuildIntegration> list{{nullptr, output.count, 0, GuildAutomationError::none}, output.data};
    return decode_list(body, scratch, &list, integration_item, count);
}

GuildAutomationResult guild_command_permissions_decode(Utf8View body, Slice<u32> scratch, u64 application_id,
                                                       GuildCommandPermissionsPage* page) noexcept {
    if (!page || !application_id || (!page->entries.data && page->entries.count))
        return {0, GuildAutomationError::input};
    page->count = page->command_overrides = 0;
    const auto s = scan_json<GuildAutomationError>(body, scratch);
    if (s.error != GuildAutomationError::none)
        return {0, s.error};
    CommandParse p{&s.document, page, application_id, 0, {}, GuildAutomationError::none};
    const auto read = json_visit_root_array(s.document.input, s.document.structurals, kDepth, command_item, &p);
    if (!read.ok()) {
        page->count = page->command_overrides = 0;
        return failure(read, p.error);
    }
    return {body.bytes, GuildAutomationError::none};
}

GuildAutomationResult automod_rules_decode(Utf8View body, Slice<u32> scratch, Slice<AutoModRule> output, u32* count,
                                           AutoModPool* pool) noexcept {
    if (!count || !pool || (!output.data && output.count) || (!pool->bytes.data && pool->bytes.count) ||
        pool->used > pool->bytes.count)
        return {0, GuildAutomationError::input};
    *count = 0;
    const auto s = scan_json<GuildAutomationError>(body, scratch);
    if (s.error != GuildAutomationError::none)
        return {0, s.error};
    const u32 pool_start = pool->used;
    RulesParse p{&s.document, output, pool, 0, GuildAutomationError::none};
    JsonReadResult read{};
    if (body.bytes && body.data[0] == '{') {
        const JsonValue root{body, 0, JsonValueKind::object, false};
        read = decode_rule(p, root) ? JsonReadResult{} : JsonReadResult{0, 0, JsonReadError::visitor_rejected};
    } else
        read = json_visit_root_array(s.document.input, s.document.structurals, kDepth, rule_item, &p);
    if (!read.ok()) {
        pool->used = pool_start;
        return failure(read, p.error);
    }
    *count = p.count;
    return {body.bytes, GuildAutomationError::none};
}

GuildAutomationResult guild_application_commands_decode(Utf8View body, Slice<u32> scratch, u64 application_id,
                                                        Slice<GuildApplicationCommand> output, u32* count) noexcept {
    if (!count || !application_id || (!output.data && output.count))
        return {0, GuildAutomationError::input};
    *count = 0;
    const auto s = scan_json<GuildAutomationError>(body, scratch);
    if (s.error != GuildAutomationError::none)
        return {0, s.error};
    if (!body.bytes || body.data[0] != '{')
        return {0, GuildAutomationError::field};
    const JsonValue root{body, 0, JsonValueKind::object, false};
    const JsonValue commands = object_member(s.document, root, utf8_literal("application_commands"), kDepth);
    const JsonValue permissions = object_member(s.document, root, utf8_literal("permissions"), kDepth);
    if (commands.kind != JsonValueKind::array)
        return {0, GuildAutomationError::field};
    CommandListParse p{&s.document, output, 0, application_id, {}, 0, false, GuildAutomationError::none};
    auto read = json_visit_indexed_array(s.document, commands, kDepth, command_row_item, &p);
    if (read.ok() && permissions.kind == JsonValueKind::array)
        read = json_visit_indexed_array(s.document, permissions, kDepth, command_mark_item, &p);
    if (!read.ok())
        return failure(read, p.error);
    *count = p.count;
    return {body.bytes, GuildAutomationError::none};
}

bool guild_automation_event_decode(Utf8View body, Slice<u32> scratch, GuildAutomationEvent* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    const auto s = scan_json<GuildAutomationError>(body, scratch);
    return s.error == GuildAutomationError::none &&
           json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_event, output).ok() &&
           output->guild_id;
}

} // namespace maicord
