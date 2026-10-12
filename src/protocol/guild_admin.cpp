#include "maicord/protocol/guild_admin.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/writer.hpp"

#include <Windows.h>

namespace maicord {
namespace {

using Op = GuildAdminOperation;

bool view_valid(Utf8View text) noexcept {
    return byte_range_valid({reinterpret_cast<const u8*>(text.data), text.bytes});
}

// Single-line or multi-line UTF-8 text with a codepoint range and no NULs.
bool text_valid(Utf8View text, u32 minimum_codepoints, u32 maximum_codepoints, bool single_line) noexcept {
    if (!view_valid(text) || text.bytes > maximum_codepoints * 4)
        return false;
    const auto validation = utf8_validate(text);
    if (!validation.valid || validation.codepoints < minimum_codepoints ||
        validation.codepoints > maximum_codepoints)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (!text.data[i] || (single_line && (text.data[i] == '\r' || text.data[i] == '\n')))
            return false;
    return true;
}

// data:image/<type>;base64,<payload>
bool data_uri_valid(Utf8View uri) noexcept {
    constexpr char prefix[] = "data:image/";
    if (!view_valid(uri) || uri.bytes < sizeof(prefix) - 1 + 8)
        return false;
    for (u32 i = 0; i < sizeof(prefix) - 1; ++i)
        if (uri.data[i] != prefix[i])
            return false;
    bool comma = false;
    for (u32 i = sizeof(prefix) - 1; i < uri.bytes; ++i) {
        const char c = uri.data[i];
        const bool base64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '+' || c == '/' || c == '=';
        if (c == ',')
            comma = true;
        else if (!base64 && (comma || (c != ';' && c != '-' && c != '.')))
            return false;
    }
    return comma;
}

bool invite_code_valid(Utf8View code) noexcept {
    if (!view_valid(code) || !code.bytes || code.bytes > kGuildAdminInviteCodeBytes)
        return false;
    for (u32 i = 0; i < code.bytes; ++i) {
        const char c = code.data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

bool guild_major(Op op) noexcept {
    return op != Op::channel_update && op != Op::channel_delete && op != Op::overwrite_put &&
           op != Op::overwrite_delete && op != Op::invite_list_channel && op != Op::invite_create &&
           op != Op::invite_delete;
}

bool channel_major(Op op) noexcept {
    return !guild_major(op) && op != Op::invite_delete;
}

bool guild_patch_valid(const GuildSettingsPatch& patch) noexcept {
    const u16 f = patch.fields;
    return f && !(f & ~guild_settings_patch_all) &&
           (!(f & guild_settings_patch_name) || text_valid(patch.name, 2, kGuildAdminNameCodepoints, true)) &&
           (!(f & guild_settings_patch_icon) || !patch.icon.bytes || data_uri_valid(patch.icon)) &&
           (!(f & guild_settings_patch_banner) || !patch.banner.bytes || data_uri_valid(patch.banner)) &&
           (!(f & guild_settings_patch_afk_timeout) ||
            patch.afk_timeout_seconds == 60 || patch.afk_timeout_seconds == 300 ||
            patch.afk_timeout_seconds == 900 || patch.afk_timeout_seconds == 1800 ||
            patch.afk_timeout_seconds == 3600) &&
           (!(f & guild_settings_patch_verification_level) || patch.verification_level <= 4) &&
           (!(f & guild_settings_patch_default_notifications) || patch.default_message_notifications <= 1) &&
           (!(f & guild_settings_patch_explicit_content_filter) || patch.explicit_content_filter <= 2);
}

bool role_patch_valid(const RolePatch& patch, bool create) noexcept {
    const u8 f = patch.fields;
    return (create || f) && !(f & ~role_patch_all) &&
           (!(f & role_patch_name) || text_valid(patch.name, 1, kGuildAdminNameCodepoints, true)) &&
           (!(f & role_patch_color) || patch.color <= 0xFFFFFFu) &&
           (!(f & role_patch_icon) || !patch.icon.bytes || data_uri_valid(patch.icon)) &&
           (!(f & role_patch_unicode_emoji) ||
            (patch.unicode_emoji.bytes <= kGuildAdminEmojiBytes &&
             (!patch.unicode_emoji.bytes || text_valid(patch.unicode_emoji, 1, 8, true))));
}

bool member_patch_valid(const MemberPatch& patch, bool self) noexcept {
    const u8 f = patch.fields;
    // Oneself: nickname and per-server avatar; others: never the avatar.
    if (!f || (f & ~member_patch_all) || (self && (f & ~(member_patch_nickname | member_patch_avatar))) ||
        (!self && (f & member_patch_avatar)))
        return false;
    if (!view_valid(patch.avatar) || (!(f & member_patch_avatar) && patch.avatar.bytes) ||
        (patch.avatar.bytes && !data_uri_valid(patch.avatar)))
        return false;
    if ((f & member_patch_nickname) && patch.nickname.bytes &&
        !text_valid(patch.nickname, 1, kGuildAdminNicknameCodepoints, true))
        return false;
    if ((f & member_patch_nickname) && !view_valid(patch.nickname))
        return false;
    if (f & member_patch_roles) {
        if ((!patch.role_ids.data && patch.role_ids.count) || patch.role_ids.count > kGuildAdminMaximumMemberRoles)
            return false;
        for (u32 i = 0; i < patch.role_ids.count; ++i) {
            if (!patch.role_ids[i])
                return false;
            for (u32 j = 0; j < i; ++j)
                if (patch.role_ids[i] == patch.role_ids[j])
                    return false;
        }
    }
    // Year 9999 bound keeps the timestamp formatter fixed-width.
    return !(f & member_patch_timeout) || patch.communication_disabled_until < 253'402'300'800ull;
}

bool overwrite_valid(const PermissionOverwriteWrite& overwrite) noexcept {
    return overwrite.id && overwrite.type <= 1 && !(overwrite.allow & overwrite.deny);
}

bool channel_patch_valid(const ChannelPatch& patch, bool create) noexcept {
    const u16 f = patch.fields;
    if ((!create && !f) || (f & ~channel_patch_all) || (create && !(f & channel_patch_name)))
        return false;
    if ((f & channel_patch_name) && !text_valid(patch.name, 1, kGuildAdminNameCodepoints, true))
        return false;
    if ((f & channel_patch_topic) && patch.topic.bytes &&
        !text_valid(patch.topic, 1, kGuildAdminTopicCodepoints, false))
        return false;
    if ((f & channel_patch_topic) && !view_valid(patch.topic))
        return false;
    if ((f & channel_patch_type) && patch.type != 0 && patch.type != 2 && patch.type != 4 && patch.type != 5 &&
        patch.type != 13 && patch.type != 15 && patch.type != 16)
        return false;
    if ((f & channel_patch_slowmode) && patch.slowmode_seconds > 21'600)
        return false;
    if ((f & channel_patch_bitrate) && (patch.bitrate < 8'000 || patch.bitrate > 384'000))
        return false;
    if ((f & channel_patch_user_limit) && patch.user_limit > 99)
        return false;
    if (f & channel_patch_overwrites) {
        if ((!patch.overwrites.data && patch.overwrites.count) || patch.overwrites.count > kGuildAdminMaximumOverwrites)
            return false;
        for (u32 i = 0; i < patch.overwrites.count; ++i) {
            if (!overwrite_valid(patch.overwrites[i]))
                return false;
            for (u32 j = 0; j < i; ++j)
                if (patch.overwrites[i].id == patch.overwrites[j].id)
                    return false;
        }
    }
    return true;
}

bool positions_valid(ConstSlice<GuildAdminPosition> positions, bool channels) noexcept {
    if (!positions.data || !positions.count || positions.count > kGuildAdminMaximumPositions)
        return false;
    for (u32 i = 0; i < positions.count; ++i) {
        const auto& p = positions[i];
        if (!p.id || (!channels && (p.flags || p.parent_id)) ||
            (p.flags & ~(guild_admin_position_parent | guild_admin_position_lock_permissions)) ||
            (!(p.flags & guild_admin_position_parent) && p.parent_id))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (positions[j].id == p.id)
                return false;
    }
    return true;
}

bool page_valid(const GuildAdminPageQuery* page, bool audit) noexcept {
    if (!page)
        return true;
    if (page->limit > (audit ? kGuildAdminAuditPageMaximum : kGuildAdminPageMaximum))
        return false;
    if (!audit && (page->user_id || page->action_type_set || page->action_type))
        return false;
    return page->action_type_set || !page->action_type;
}

bool profile_patch_valid(const GuildProfilePatch& patch) noexcept {
    const u8 f = patch.fields;
    if (!f || (f & ~guild_profile_patch_all) || !view_valid(patch.pronouns) || !view_valid(patch.bio) ||
        !view_valid(patch.banner))
        return false;
    if ((!(f & guild_profile_patch_pronouns) && patch.pronouns.bytes) ||
        (!(f & guild_profile_patch_bio) && patch.bio.bytes) || (!(f & guild_profile_patch_banner) && patch.banner.bytes))
        return false;
    return (!patch.pronouns.bytes || text_valid(patch.pronouns, 1, kGuildAdminPronounsCodepoints, true)) &&
           (!patch.bio.bytes || text_valid(patch.bio, 1, kGuildAdminBioCodepoints, false)) &&
           (!patch.banner.bytes || data_uri_valid(patch.banner));
}

bool reason_valid(const GuildAdminRequest& request) noexcept {
    if (!view_valid(request.reason))
        return false;
    if (!request.reason.bytes)
        return true;
    u8 scratch[kRestAuditReasonMaximumEncodedBytes];
    const bool ok = guild_admin_method(request.operation) != RestMethod::get &&
                    rest_encode_audit_reason(request.reason, {scratch, sizeof(scratch)}).ok;
    SecureZeroMemory(scratch, sizeof(scratch));
    return ok;
}

// Body writers ------------------------------------------------------------

struct Body {
    ProtocolWriter writer;
    bool first = true;
    bool ok = true;

    void raw(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
    }
    void byte(u8 value) noexcept {
        ok = ok && protocol_writer_append_byte(&writer, value);
    }
    void number(u64 value) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, value);
    }
    void string(Utf8View value) noexcept {
        ok = ok && protocol_writer_append_json_string(&writer, value);
    }
    void snowflake(u64 value) noexcept {
        byte('"');
        number(value);
        byte('"');
    }
    void nullable_snowflake(u64 value) noexcept {
        if (value)
            snowflake(value);
        else
            raw("null");
    }
    void nullable_string(Utf8View value) noexcept {
        if (value.bytes)
            string(value);
        else
            raw("null");
    }
    void boolean(bool value) noexcept {
        raw(value ? "true" : "false");
    }
    void key(const char* name) noexcept {
        if (!first)
            byte(',');
        first = false;
        byte('"');
        raw(name);
        raw("\":");
    }
    void begin_object() noexcept {
        byte('{');
        first = true;
    }
    void end_object() noexcept {
        byte('}');
        first = false;
    }
};

void write_guild(Body& b, const GuildSettingsPatch& p) noexcept {
    b.begin_object();
    if (p.fields & guild_settings_patch_name) {
        b.key("name");
        b.string(p.name);
    }
    if (p.fields & guild_settings_patch_icon) {
        b.key("icon");
        b.nullable_string(p.icon);
    }
    if (p.fields & guild_settings_patch_banner) {
        b.key("banner");
        b.nullable_string(p.banner);
    }
    if (p.fields & guild_settings_patch_system_channel) {
        b.key("system_channel_id");
        b.nullable_snowflake(p.system_channel_id);
    }
    if (p.fields & guild_settings_patch_afk_channel) {
        b.key("afk_channel_id");
        b.nullable_snowflake(p.afk_channel_id);
    }
    if (p.fields & guild_settings_patch_afk_timeout) {
        b.key("afk_timeout");
        b.number(p.afk_timeout_seconds);
    }
    if (p.fields & guild_settings_patch_verification_level) {
        b.key("verification_level");
        b.number(p.verification_level);
    }
    if (p.fields & guild_settings_patch_default_notifications) {
        b.key("default_message_notifications");
        b.number(p.default_message_notifications);
    }
    if (p.fields & guild_settings_patch_explicit_content_filter) {
        b.key("explicit_content_filter");
        b.number(p.explicit_content_filter);
    }
    b.end_object();
}

void write_role(Body& b, const RolePatch& p) noexcept {
    b.begin_object();
    if (p.fields & role_patch_name) {
        b.key("name");
        b.string(p.name);
    }
    if (p.fields & role_patch_permissions) {
        b.key("permissions");
        b.snowflake(p.permissions);
    }
    if (p.fields & role_patch_color) {
        b.key("color");
        b.number(p.color);
    }
    if (p.fields & role_patch_hoist) {
        b.key("hoist");
        b.boolean(p.hoist);
    }
    if (p.fields & role_patch_mentionable) {
        b.key("mentionable");
        b.boolean(p.mentionable);
    }
    if (p.fields & role_patch_icon) {
        b.key("icon");
        b.nullable_string(p.icon);
    }
    if (p.fields & role_patch_unicode_emoji) {
        b.key("unicode_emoji");
        b.nullable_string(p.unicode_emoji);
    }
    b.end_object();
}

void write_member(Body& b, const MemberPatch& p, bool self) noexcept {
    b.begin_object();
    if (p.fields & member_patch_nickname) {
        b.key("nick");
        if (self)
            b.string(p.nickname);
        else
            b.nullable_string(p.nickname);
    }
    if (p.fields & member_patch_avatar) {
        b.key("avatar");
        b.nullable_string(p.avatar);
    }
    if (p.fields & member_patch_roles) {
        b.key("roles");
        b.byte('[');
        for (u32 i = 0; i < p.role_ids.count; ++i) {
            if (i)
                b.byte(',');
            b.snowflake(p.role_ids[i]);
        }
        b.byte(']');
    }
    if (p.fields & member_patch_timeout) {
        b.key("communication_disabled_until");
        if (p.communication_disabled_until) {
            char stamp[32];
            const u32 bytes = guild_admin_format_timestamp(p.communication_disabled_until, {stamp, sizeof(stamp)});
            b.ok = b.ok && bytes;
            b.string({stamp, bytes});
        } else
            b.raw("null");
    }
    if (p.fields & member_patch_voice_channel) {
        b.key("channel_id");
        b.nullable_snowflake(p.voice_channel_id);
    }
    if (p.fields & member_patch_mute) {
        b.key("mute");
        b.boolean(p.mute);
    }
    if (p.fields & member_patch_deafen) {
        b.key("deaf");
        b.boolean(p.deafen);
    }
    b.end_object();
}

void write_profile(Body& b, const GuildProfilePatch& p) noexcept {
    b.begin_object();
    if (p.fields & guild_profile_patch_pronouns) {
        b.key("pronouns");
        b.string(p.pronouns);
    }
    if (p.fields & guild_profile_patch_bio) {
        b.key("bio");
        b.string(p.bio);
    }
    if (p.fields & guild_profile_patch_banner) {
        b.key("banner");
        b.nullable_string(p.banner);
    }
    b.end_object();
}

void write_overwrite(Body& b, const PermissionOverwriteWrite& o) noexcept {
    b.begin_object();
    b.key("id");
    b.snowflake(o.id);
    b.key("type");
    b.number(o.type);
    b.key("allow");
    b.snowflake(o.allow);
    b.key("deny");
    b.snowflake(o.deny);
    b.end_object();
}

void write_channel(Body& b, const ChannelPatch& p) noexcept {
    b.begin_object();
    if (p.fields & channel_patch_name) {
        b.key("name");
        b.string(p.name);
    }
    if (p.fields & channel_patch_type) {
        b.key("type");
        b.number(p.type);
    }
    if (p.fields & channel_patch_topic) {
        b.key("topic");
        b.nullable_string(p.topic);
    }
    if (p.fields & channel_patch_parent) {
        b.key("parent_id");
        b.nullable_snowflake(p.parent_id);
    }
    if (p.fields & channel_patch_position) {
        b.key("position");
        b.number(p.position);
    }
    if (p.fields & channel_patch_nsfw) {
        b.key("nsfw");
        b.boolean(p.nsfw);
    }
    if (p.fields & channel_patch_slowmode) {
        b.key("rate_limit_per_user");
        b.number(p.slowmode_seconds);
    }
    if (p.fields & channel_patch_bitrate) {
        b.key("bitrate");
        b.number(p.bitrate);
    }
    if (p.fields & channel_patch_user_limit) {
        b.key("user_limit");
        b.number(p.user_limit);
    }
    if (p.fields & channel_patch_overwrites) {
        b.key("permission_overwrites");
        b.byte('[');
        for (u32 i = 0; i < p.overwrites.count; ++i) {
            if (i)
                b.byte(',');
            write_overwrite(b, p.overwrites[i]);
        }
        b.byte(']');
    }
    b.end_object();
}

void write_positions(Body& b, ConstSlice<GuildAdminPosition> positions, bool channels) noexcept {
    b.byte('[');
    for (u32 i = 0; i < positions.count; ++i) {
        const auto& p = positions[i];
        if (i)
            b.byte(',');
        b.begin_object();
        b.key("id");
        b.snowflake(p.id);
        b.key("position");
        b.number(p.position);
        if (channels && (p.flags & guild_admin_position_parent)) {
            b.key("parent_id");
            b.nullable_snowflake(p.parent_id);
            b.key("lock_permissions");
            b.boolean((p.flags & guild_admin_position_lock_permissions) != 0);
        }
        b.end_object();
    }
    b.byte(']');
}

void write_body(const GuildAdminRequest& r, Body& b) noexcept {
    switch (r.operation) {
    case Op::guild_update:
        write_guild(b, *r.guild);
        break;
    case Op::role_create:
        if (r.role)
            write_role(b, *r.role);
        else
            b.raw("{}");
        break;
    case Op::role_update:
        write_role(b, *r.role);
        break;
    case Op::role_reorder:
        write_positions(b, r.positions, false);
        break;
    case Op::channel_reorder:
        write_positions(b, r.positions, true);
        break;
    case Op::member_update:
    case Op::member_update_self:
        write_member(b, *r.member, r.operation == Op::member_update_self);
        break;
    case Op::member_profile_self:
        write_profile(b, *r.profile);
        break;
    case Op::ban_add:
        b.begin_object();
        b.key("delete_message_seconds");
        b.number(r.delete_message_seconds);
        b.end_object();
        break;
    case Op::channel_create:
    case Op::channel_update:
        write_channel(b, *r.channel);
        break;
    case Op::overwrite_put: {
        const auto& o = *r.overwrite;
        b.begin_object();
        b.key("id");
        b.snowflake(o.id);
        b.key("type");
        b.number(o.type);
        b.key("allow");
        b.snowflake(o.allow);
        b.key("deny");
        b.snowflake(o.deny);
        b.end_object();
        break;
    }
    case Op::guild_leave:
        // The official client's leave body; a member is never lurking here.
        b.raw("{\"lurking\":false}");
        break;
    case Op::invite_create: {
        const InviteCreate defaults{86'400, 0, false, false};
        const auto& i = r.invite ? *r.invite : defaults;
        b.begin_object();
        b.key("max_age");
        b.number(i.max_age_seconds);
        b.key("max_uses");
        b.number(i.max_uses);
        b.key("temporary");
        b.boolean(i.temporary);
        b.key("unique");
        b.boolean(i.unique);
        b.end_object();
        break;
    }
    default:
        b.ok = false;
        break;
    }
}

// Decoder helpers -----------------------------------------------------------

constexpr u32 kDepth = 32;

template <typename Size>
bool decode_text(JsonValue value, char* output, u32 capacity, Size* bytes, bool nullable) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *bytes = 0;
        return nullable;
    }
    JsonString encoded{};
    if (!json_value_as_string(value, &encoded))
        return false;
    const auto decoded = json_string_decode_utf8(encoded, {output, capacity});
    if (!decoded.ok() || !utf8_validate({output, decoded.bytes}).valid)
        return false;
    *bytes = static_cast<Size>(decoded.bytes);
    return true;
}

bool decode_u32(JsonValue value, u32* output, u64 maximum = 0xFFFFFFFFull) noexcept {
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || number > maximum)
        return false;
    *output = static_cast<u32>(number);
    return true;
}

bool decode_permissions(JsonValue value, PermissionSet* output) noexcept {
    JsonString text{};
    return json_value_as_string(value, &text) && json_string_to_u64(text, output);
}

bool nullable_snowflake(JsonValue value, u64* output) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *output = 0;
        return true;
    }
    return json_value_to_snowflake(value, output);
}

// Field lookup with duplicate detection over a fixed key table.
struct Fields {
    const char* const* names;
    u32 count;
    u64 seen;
    GuildAdminError* error;

    // Returns the field index, count for unknown keys, or ~0u after a duplicate.
    u32 find(JsonString name) noexcept {
        for (u32 i = 0; i < count; ++i) {
            if (!json_string_equals_ascii(name, {names[i], static_cast<u32>(c_string_bytes(names[i]))}))
                continue;
            if (seen & (1ull << i)) {
                *error = GuildAdminError::duplicate;
                return ~0u;
            }
            seen |= 1ull << i;
            return i;
        }
        return count;
    }
    static u32 c_string_bytes(const char* text) noexcept {
        u32 bytes = 0;
        while (text[bytes])
            ++bytes;
        return bytes;
    }
};

bool fail(GuildAdminError* error, GuildAdminError value = GuildAdminError::field) noexcept {
    if (*error == GuildAdminError::none)
        *error = value;
    return false;
}

struct Scan {
    JsonIndexedDocument document;
    GuildAdminError error;
};

Scan scan(Utf8View body, Slice<u32> scratch) noexcept {
    if (!view_valid(body) || !body.bytes || !scratch.data || !scratch.count || !utf8_validate(body).valid)
        return {{}, GuildAdminError::input};
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return {{}, scanned.error == JsonScanError::output_overflow ? GuildAdminError::capacity
                                                                   : GuildAdminError::json};
    return {{body, {scratch.data, scanned.count}}, GuildAdminError::none};
}

// User --------------------------------------------------------------------

struct UserParse {
    GuildAdminUser* user;
    Fields fields;
    GuildAdminError error;
};
constexpr const char* kUserFields[] = {"id", "username", "global_name", "avatar", "discriminator"};

// "0" (migrated username) or up to four digits.
bool decode_discriminator(JsonValue value, u16* output) noexcept {
    *output = 0;
    if (value.kind == JsonValueKind::null_value)
        return true;
    char digits[8];
    u16 bytes = 0;
    if (!decode_text(value, digits, sizeof(digits), &bytes, false) || !bytes || bytes > 4)
        return false;
    u32 parsed = 0;
    for (u16 i = 0; i < bytes; ++i) {
        if (digits[i] < '0' || digits[i] > '9')
            return false;
        parsed = parsed * 10 + static_cast<u32>(digits[i] - '0');
    }
    *output = static_cast<u16>(parsed);
    return true;
}

bool visit_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<UserParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    if (field == 0)
        valid = json_value_to_snowflake(value, &p.user->id);
    else if (field == 1)
        valid = decode_text(value, p.user->username, kGuildAdminUsernameBytes, &p.user->username_bytes, false);
    else if (field == 2)
        valid = decode_text(value, p.user->display, kGuildAdminUsernameBytes, &p.user->display_bytes, true);
    else if (field == 3) {
        u16 bytes = 0;
        valid = decode_text(value, p.user->avatar, kGuildAdminHashBytes, &bytes, true);
        p.user->avatar_bytes = static_cast<u8>(bytes);
    } else if (field == 4)
        valid = decode_discriminator(value, &p.user->discriminator);
    return valid || fail(&p.error);
}

bool decode_user(const JsonIndexedDocument& document, JsonValue value, GuildAdminUser* user,
                 GuildAdminError* error) noexcept {
    if (value.kind != JsonValueKind::object)
        return fail(error);
    *user = {};
    UserParse p{user, {kUserFields, 5, 0, nullptr}, GuildAdminError::none};
    p.fields.error = &p.error;
    const auto read = json_visit_indexed_object(document, value, kDepth, visit_user, &p);
    if (!read.ok())
        return fail(error, p.error == GuildAdminError::none ? GuildAdminError::json : p.error);
    return user->id || fail(error, GuildAdminError::identity);
}

// Settings ----------------------------------------------------------------

struct SettingsParse {
    GuildSettingsSnapshot value;
    Fields fields;
    GuildAdminError error;
    const JsonIndexedDocument* document;
};
constexpr const char* kSettingsFields[] = {"id", "name", "icon", "banner", "owner_id", "mfa_level",
                                           "verification_level", "default_message_notifications",
                                           "explicit_content_filter", "system_channel_id", "afk_channel_id",
                                           "afk_timeout", "premium_tier", "features"};

constexpr struct {
    const char* name;
    u16 flag;
} kFeatureNames[] = {{"MORE_EMOJI", guild_feature_more_emoji},     {"MORE_STICKERS", guild_feature_more_stickers},
                     {"ROLE_ICONS", guild_feature_role_icons},     {"BANNER", guild_feature_banner},
                     {"ANIMATED_ICON", guild_feature_animated_icon}, {"ANIMATED_BANNER", guild_feature_animated_banner},
                     {"COMMUNITY", guild_feature_community}};

bool visit_feature(void* opaque, u32, JsonValue value) noexcept {
    JsonString name{};
    if (!json_value_as_string(value, &name))
        return false;
    for (const auto& feature : kFeatureNames)
        if (json_string_equals_ascii(name, {feature.name, Fields::c_string_bytes(feature.name)}))
            *static_cast<u16*>(opaque) |= feature.flag;
    return true;
}

bool visit_settings(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SettingsParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    u32 number = 0;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_snowflake(value, &v.id); break;
    case 1:
        valid = decode_text(value, v.name, kGuildAdminNameBytes, &v.name_bytes, false) && v.name_bytes;
        break;
    case 2: valid = decode_text(value, v.icon, kGuildAdminHashBytes, &v.icon_bytes, true); break;
    case 3: valid = decode_text(value, v.banner, kGuildAdminHashBytes, &v.banner_bytes, true); break;
    case 4: valid = json_value_to_snowflake(value, &v.owner_id); break;
    case 5: valid = decode_u32(value, &number, 1); v.mfa_level = static_cast<u8>(number); break;
    case 6: valid = decode_u32(value, &number, 4); v.verification_level = static_cast<u8>(number); break;
    case 7:
        valid = decode_u32(value, &number, 1);
        v.default_message_notifications = static_cast<u8>(number);
        break;
    case 8:
        valid = decode_u32(value, &number, 2);
        v.explicit_content_filter = static_cast<u8>(number);
        break;
    case 9: valid = nullable_snowflake(value, &v.system_channel_id); break;
    case 10: valid = nullable_snowflake(value, &v.afk_channel_id); break;
    case 11: valid = decode_u32(value, &v.afk_timeout_seconds, 86'400); break;
    case 12: valid = decode_u32(value, &number, 3); v.premium_tier = static_cast<u8>(number); break;
    case 13:
        valid = value.kind == JsonValueKind::array && p.document &&
                json_visit_indexed_array(*p.document, value, kDepth, visit_feature, &v.features).ok();
        break;
    default: break;
    }
    return valid || fail(&p.error);
}

// Roles -------------------------------------------------------------------

struct RoleParse {
    GuildAdminRole value;
    Fields fields;
    GuildAdminError error;
};
constexpr const char* kRoleFields[] = {"id", "name", "permissions", "position", "color", "hoist",
                                       "managed", "mentionable", "icon", "unicode_emoji", "flags"};

bool visit_role(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RoleParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_snowflake(value, &v.id); break;
    case 1: valid = decode_text(value, v.name, kGuildAdminNameBytes, &v.name_bytes, false); break;
    case 2: valid = decode_permissions(value, &v.permissions); break;
    case 3: valid = decode_u32(value, &v.position); break;
    case 4: valid = decode_u32(value, &v.color, 0xFFFFFF); break;
    case 5: valid = json_value_to_bool(value, &v.hoist); break;
    case 6: valid = json_value_to_bool(value, &v.managed); break;
    case 7: valid = json_value_to_bool(value, &v.mentionable); break;
    case 8: valid = decode_text(value, v.icon, kGuildAdminHashBytes, &v.icon_bytes, true); break;
    case 9: valid = decode_text(value, v.emoji, kGuildAdminEmojiBytes, &v.emoji_bytes, true); break;
    case 10: valid = decode_u32(value, &v.flags); break;
    default: break;
    }
    return valid || fail(&p.error);
}

bool decode_role_value(const JsonIndexedDocument& document, JsonValue value, GuildAdminRole* output,
                       GuildAdminError* error) noexcept {
    if (value.kind != JsonValueKind::object)
        return fail(error);
    RoleParse p{};
    p.fields = {kRoleFields, 11, 0, &p.error};
    const auto read = json_visit_indexed_object(document, value, kDepth, visit_role, &p);
    if (!read.ok())
        return fail(error, p.error == GuildAdminError::none ? GuildAdminError::json : p.error);
    constexpr u64 required = 1u | 2u | 4u | 8u;
    if ((p.fields.seen & required) != required || !p.value.id)
        return fail(error, GuildAdminError::identity);
    *output = p.value;
    return true;
}

template <typename Item> struct ArrayParse {
    const JsonIndexedDocument* document;
    Slice<Item> output;
    u32 count;
    GuildAdminError error;
};

bool visit_role_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminRole>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (!decode_role_value(*p.document, value, &p.output[p.count], &p.error))
        return false;
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].id == p.output[p.count].id)
            return fail(&p.error, GuildAdminError::duplicate);
    ++p.count;
    return true;
}

// Role member counts: {"<role id>": <count>, ...}
bool visit_role_count(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminRoleCount>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    u64 id = 0;
    u32 members = 0;
    if (!json_string_to_u64(name, &id) || !id || !decode_u32(value, &members))
        return fail(&p.error);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].role_id == id)
            return fail(&p.error, GuildAdminError::duplicate);
    p.output[p.count++] = {id, members};
    return true;
}

// Bans --------------------------------------------------------------------

struct BanParse {
    const JsonIndexedDocument* document;
    GuildAdminBan* value;
    Fields fields;
    GuildAdminError error;
};
constexpr const char* kBanFields[] = {"user", "reason"};

bool visit_ban(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<BanParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    if (field == 0)
        return decode_user(*p.document, value, &p.value->user, &p.error);
    if (field == 1)
        return decode_text(value, p.value->reason, kGuildAdminReasonBytes, &p.value->reason_bytes, true) ||
               fail(&p.error);
    return true;
}

bool visit_ban_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminBan>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    GuildAdminBan ban{};
    BanParse parse{p.document, &ban, {kBanFields, 2, 0, nullptr}, GuildAdminError::none};
    parse.fields.error = &parse.error;
    const auto read = json_visit_indexed_object(*p.document, value, kDepth, visit_ban, &parse);
    if (!read.ok())
        return fail(&p.error, parse.error == GuildAdminError::none ? GuildAdminError::json : parse.error);
    if (!(parse.fields.seen & 1u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].user.id == ban.user.id)
            return fail(&p.error, GuildAdminError::duplicate);
    p.output[p.count++] = ban;
    return true;
}

// Audit log ---------------------------------------------------------------

struct AuditEntryParse {
    GuildAdminAuditEntry value;
    Fields fields;
    GuildAdminError error;
};
constexpr const char* kAuditEntryFields[] = {"id", "user_id", "target_id", "action_type", "reason"};

bool visit_audit_entry(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<AuditEntryParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    u32 number = 0;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_snowflake(value, &v.id); break;
    case 1: valid = nullable_snowflake(value, &v.user_id); break;
    case 2: valid = nullable_snowflake(value, &v.target_id); break;
    case 3: valid = decode_u32(value, &number, 0xFFFF); v.action_type = static_cast<u16>(number); break;
    case 4: valid = decode_text(value, v.reason, kGuildAdminReasonBytes, &v.reason_bytes, true); break;
    default: break;
    }
    return valid || fail(&p.error);
}

struct AuditParse {
    const JsonIndexedDocument* document;
    GuildAdminAuditPage page;
    u64 seen;
    GuildAdminError error;
};

bool visit_audit_entry_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    if (p.page.entry_count >= p.page.entries.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    AuditEntryParse entry{};
    entry.fields = {kAuditEntryFields, 5, 0, &entry.error};
    const auto read = json_visit_indexed_object(*p.document, value, kDepth, visit_audit_entry, &entry);
    if (!read.ok())
        return fail(&p.error, entry.error == GuildAdminError::none ? GuildAdminError::json : entry.error);
    if (!(entry.fields.seen & 1u) || !(entry.fields.seen & 8u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.page.entry_count; ++i)
        if (p.page.entries[i].id == entry.value.id)
            return fail(&p.error, GuildAdminError::duplicate);
    p.page.entries[p.page.entry_count++] = entry.value;
    return true;
}

bool visit_audit_user_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    if (p.page.user_count >= p.page.users.count)
        return fail(&p.error, GuildAdminError::capacity);
    GuildAdminUser user{};
    if (!decode_user(*p.document, value, &user, &p.error))
        return false;
    for (u32 i = 0; i < p.page.user_count; ++i)
        if (p.page.users[i].id == user.id)
            return fail(&p.error, GuildAdminError::duplicate);
    p.page.users[p.page.user_count++] = user;
    return true;
}

bool visit_audit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    const bool entries = json_string_equals_ascii(name, utf8_literal("audit_log_entries"));
    const bool users = !entries && json_string_equals_ascii(name, utf8_literal("users"));
    if (!entries && !users)
        return true;
    const u64 bit = entries ? 1u : 2u;
    if (p.seen & bit)
        return fail(&p.error, GuildAdminError::duplicate);
    p.seen |= bit;
    if (value.kind != JsonValueKind::array)
        return fail(&p.error);
    const auto read = json_visit_indexed_array(*p.document, value, kDepth,
                                               entries ? visit_audit_entry_item : visit_audit_user_item, &p);
    return read.ok() || fail(&p.error, GuildAdminError::json);
}

// Invites -----------------------------------------------------------------

struct NestedIdParse {
    u64 id;
    bool seen;
    GuildAdminError error;
};

bool visit_nested_id(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<NestedIdParse*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("id")))
        return true;
    if (p.seen)
        return fail(&p.error, GuildAdminError::duplicate);
    p.seen = true;
    return json_value_to_snowflake(value, &p.id) || fail(&p.error);
}

struct InviteParse {
    const JsonIndexedDocument* document;
    GuildAdminInvite value;
    Fields fields;
    GuildAdminError error;
};
constexpr const char* kInviteFields[] = {"code", "channel", "inviter", "uses", "max_uses", "max_age",
                                         "temporary", "expires_at"};

bool visit_invite(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<InviteParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    switch (field) {
    case 0:
        valid = decode_text(value, v.code, kGuildAdminInviteCodeBytes, &v.code_bytes, false) &&
                invite_code_valid({v.code, v.code_bytes});
        break;
    case 1:
    case 2: {
        if (value.kind == JsonValueKind::null_value && field == 2)
            break;
        NestedIdParse nested{};
        valid = value.kind == JsonValueKind::object &&
                json_visit_indexed_object(*p.document, value, kDepth, visit_nested_id, &nested).ok() && nested.seen;
        (field == 1 ? v.channel_id : v.inviter_id) = nested.id;
        break;
    }
    case 3: valid = decode_u32(value, &v.uses); break;
    case 4: valid = decode_u32(value, &v.max_uses); break;
    case 5: valid = decode_u32(value, &v.max_age_seconds); break;
    case 6: valid = json_value_to_bool(value, &v.temporary); break;
    case 7:
        if (value.kind == JsonValueKind::null_value)
            v.expires_at_ms = 0;
        else {
            JsonString stamp{};
            valid = json_value_as_string(value, &stamp) && parse_iso_timestamp(stamp, &v.expires_at_ms);
        }
        break;
    default: break;
    }
    return valid || fail(&p.error);
}

bool visit_invite_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminInvite>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    InviteParse parse{p.document, {}, {kInviteFields, 8, 0, nullptr}, GuildAdminError::none};
    parse.fields.error = &parse.error;
    const auto read = json_visit_indexed_object(*p.document, value, kDepth, visit_invite, &parse);
    if (!read.ok())
        return fail(&p.error, parse.error == GuildAdminError::none ? GuildAdminError::json : parse.error);
    if (!(parse.fields.seen & 1u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].code_bytes == parse.value.code_bytes &&
            bytes_equal(p.output[i].code, parse.value.code, parse.value.code_bytes))
            return fail(&p.error, GuildAdminError::duplicate);
    p.output[p.count++] = parse.value;
    return true;
}

template <typename Item>
GuildAdminResult decode_array(Utf8View body, Slice<u32> scratch, Slice<Item> output,
                              JsonArrayVisitor visitor) noexcept {
    if (!output.data && output.count)
        return {0, 0, GuildAdminError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    ArrayParse<Item> probe{&s.document, output, 0, GuildAdminError::none};
    const auto read = json_visit_root_array(s.document.input, s.document.structurals, kDepth, visitor, &probe);
    if (!read.ok())
        return {0, 0, probe.error == GuildAdminError::none ? GuildAdminError::json : probe.error};
    return {body.bytes, probe.count, GuildAdminError::none};
}

} // namespace

RestMethod guild_admin_method(GuildAdminOperation op) noexcept {
    switch (op) {
    case Op::guild_update:
    case Op::role_update:
    case Op::role_reorder:
    case Op::member_update:
    case Op::member_update_self:
    case Op::member_profile_self:
    case Op::channel_update:
    case Op::channel_reorder:
        return RestMethod::patch;
    case Op::role_create:
    case Op::channel_create:
    case Op::invite_create:
        return RestMethod::post;
    case Op::ban_add:
    case Op::overwrite_put:
        return RestMethod::put;
    case Op::role_delete:
    case Op::member_kick:
    case Op::ban_remove:
    case Op::channel_delete:
    case Op::overwrite_delete:
    case Op::invite_delete:
    case Op::guild_leave:
        return RestMethod::delete_method;
    default:
        return RestMethod::get;
    }
}

bool guild_admin_has_body(GuildAdminOperation op) noexcept {
    const auto method = guild_admin_method(op);
    return method == RestMethod::patch || method == RestMethod::post ||
           method == RestMethod::put || op == Op::guild_leave;
}

bool guild_admin_request_valid(const GuildAdminRequest& r) noexcept {
    if (r.operation >= Op::count || !reason_valid(r) || !view_valid(r.invite_code) ||
        (!r.positions.data && r.positions.count))
        return false;
    const Op op = r.operation;
    const bool needs_guild = guild_major(op);
    const bool needs_channel = channel_major(op);
    if ((needs_guild && !r.guild_id) || (needs_channel && !r.channel_id) || (!needs_guild && r.guild_id) ||
        (!needs_channel && r.channel_id))
        return false;
    const bool needs_target = op == Op::role_update || op == Op::role_delete || op == Op::member_update ||
                              op == Op::member_kick || op == Op::ban_add || op == Op::ban_remove ||
                              op == Op::overwrite_put || op == Op::overwrite_delete;
    if (needs_target != (r.target_id != 0))
        return false;
    if ((op == Op::invite_delete) != (r.invite_code.bytes != 0) ||
        (r.invite_code.bytes && !invite_code_valid(r.invite_code)))
        return false;
    if ((op == Op::ban_add) ? r.delete_message_seconds > kGuildAdminBanDeleteMaximumSeconds
                            : r.delete_message_seconds != 0)
        return false;
    const bool positions = op == Op::role_reorder || op == Op::channel_reorder;
    if (positions ? !positions_valid(r.positions, op == Op::channel_reorder) : r.positions.count != 0)
        return false;
    if ((op == Op::guild_update) != (r.guild != nullptr) || (r.guild && !guild_patch_valid(*r.guild)))
        return false;
    if (r.role && op != Op::role_create && op != Op::role_update)
        return false;
    if ((op == Op::role_update && !r.role) || (r.role && !role_patch_valid(*r.role, op == Op::role_create)))
        return false;
    const bool member = op == Op::member_update || op == Op::member_update_self;
    if (member != (r.member != nullptr) || (r.member && !member_patch_valid(*r.member, op == Op::member_update_self)))
        return false;
    if ((op == Op::member_profile_self) != (r.profile != nullptr) || (r.profile && !profile_patch_valid(*r.profile)))
        return false;
    const bool channel = op == Op::channel_create || op == Op::channel_update;
    if (channel != (r.channel != nullptr) || (r.channel && !channel_patch_valid(*r.channel, op == Op::channel_create)))
        return false;
    if ((op == Op::overwrite_put) != (r.overwrite != nullptr) ||
        (r.overwrite && (!overwrite_valid(*r.overwrite) || r.overwrite->id != r.target_id)))
        return false;
    if (r.invite && (op != Op::invite_create || r.invite->max_age_seconds > 604'800 || r.invite->max_uses > 100))
        return false;
    if (r.page && op != Op::ban_list && op != Op::audit_log)
        return false;
    return page_valid(r.page, op == Op::audit_log);
}

RestRouteKey guild_admin_route(const GuildAdminRequest& r) noexcept {
    const bool guild = guild_major(r.operation);
    const bool channel = channel_major(r.operation);
    return {kGuildAdminRouteBase + static_cast<u64>(r.operation), guild ? r.guild_id : channel ? r.channel_id : 0,
            guild_admin_method(r.operation),
            guild     ? RestMajorParameter::guild
            : channel ? RestMajorParameter::channel
                      : RestMajorParameter::none};
}

GuildAdminResult guild_admin_write_target(const GuildAdminRequest& r, Slice<u8> output) noexcept {
    if (!guild_admin_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    u8 target[kGuildAdminMaximumTargetBytes];
    ProtocolWriter w{{target, sizeof(target)}, 0};
    bool ok = protocol_writer_append_literal(&w, "/api/v10/");
    const auto text = [&](const char* value) noexcept { ok = ok && protocol_writer_append_c_string(&w, value); };
    const auto id = [&](u64 value) noexcept { ok = ok && protocol_writer_append_u64(&w, value); };
    const Op op = r.operation;
    if (op == Op::guild_leave) {
        text("users/@me/guilds/");
        id(r.guild_id);
    } else if (op == Op::invite_delete) {
        text("invites/");
        ok = ok && protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(r.invite_code.data), r.invite_code.bytes);
    } else if (channel_major(op)) {
        text("channels/");
        id(r.channel_id);
        if (op == Op::overwrite_put || op == Op::overwrite_delete) {
            text("/permissions/");
            id(r.target_id);
        } else if (op == Op::invite_list_channel || op == Op::invite_create)
            text("/invites");
    } else {
        text("guilds/");
        id(r.guild_id);
        switch (op) {
        case Op::role_list:
        case Op::role_create:
        case Op::role_reorder: text("/roles"); break;
        case Op::role_update:
        case Op::role_delete: text("/roles/"); id(r.target_id); break;
        case Op::role_member_counts: text("/roles/member-counts"); break;
        case Op::member_update:
        case Op::member_kick: text("/members/"); id(r.target_id); break;
        case Op::member_update_self: text("/members/@me"); break;
        case Op::member_profile_self: text("/profile/@me"); break;
        case Op::ban_list: text("/bans"); break;
        case Op::ban_add:
        case Op::ban_remove: text("/bans/"); id(r.target_id); break;
        case Op::audit_log: text("/audit-logs"); break;
        case Op::channel_create:
        case Op::channel_reorder: text("/channels"); break;
        case Op::invite_list_guild: text("/invites"); break;
        default: break;
        }
        if ((op == Op::ban_list || op == Op::audit_log)) {
            const GuildAdminPageQuery none{};
            const auto& page = r.page ? *r.page : none;
            const u32 limit = page.limit ? page.limit : (op == Op::audit_log ? 50u : kGuildAdminPageMaximum);
            text("?limit=");
            id(limit);
            if (page.before) {
                text("&before=");
                id(page.before);
            }
            if (page.after) {
                text("&after=");
                id(page.after);
            }
            if (page.user_id) {
                text("&user_id=");
                id(page.user_id);
            }
            if (page.action_type_set) {
                text("&action_type=");
                id(page.action_type);
            }
        }
    }
    if (!ok)
        return {0, 0, GuildAdminError::capacity};
    if (output.count < w.used)
        return {w.used, 0, GuildAdminError::capacity};
    copy_bytes(output.data, target, w.used);
    return {w.used, 0, GuildAdminError::none};
}

GuildAdminResult guild_admin_write_body(const GuildAdminRequest& r, Slice<u8> output) noexcept {
    if (!guild_admin_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    if (!guild_admin_has_body(r.operation))
        return {0, 0, GuildAdminError::none};
    if (!output.data || !output.count)
        return {0, 0, GuildAdminError::capacity};
    Body body{{output, 0}};
    write_body(r, body);
    if (!body.ok) {
        SecureZeroMemory(output.data, body.writer.used);
        return {0, 0, GuildAdminError::capacity};
    }
    return {body.writer.used, 0, GuildAdminError::none};
}

u32 guild_admin_image_bytes(const GuildAdminRequest& r) noexcept {
    u32 bytes = 0;
    if (r.member && (r.member->fields & member_patch_avatar))
        bytes += r.member->avatar.bytes;
    if (r.profile && (r.profile->fields & guild_profile_patch_banner))
        bytes += r.profile->banner.bytes;
    return bytes;
}

GuildAdminResult guild_admin_write_reason(const GuildAdminRequest& r, Slice<u8> output) noexcept {
    if (!guild_admin_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    if (!r.reason.bytes)
        return {0, 0, GuildAdminError::none};
    const auto encoded = rest_encode_audit_reason(r.reason, output);
    return encoded.ok ? GuildAdminResult{encoded.bytes, 0, GuildAdminError::none}
                      : GuildAdminResult{encoded.bytes, 0, GuildAdminError::capacity};
}

u32 guild_admin_format_timestamp(u64 unix_seconds, Slice<char> output) noexcept {
    constexpr u32 bytes = 25; // YYYY-MM-DDTHH:MM:SS+00:00
    if (!output.data || output.count < bytes || unix_seconds >= 253'402'300'800ull)
        return 0;
    const u64 days = unix_seconds / 86'400;
    const u32 seconds = static_cast<u32>(unix_seconds % 86'400);
    // Civil date from days since 1970-01-01 (proleptic Gregorian).
    const u64 z = days + 719'468;
    const u64 era = z / 146'097;
    const u32 doe = static_cast<u32>(z - era * 146'097);
    const u32 yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u32 mp = (5 * doy + 2) / 153;
    const u32 day = doy - (153 * mp + 2) / 5 + 1;
    const u32 month = mp < 10 ? mp + 3 : mp - 9;
    const u32 year = static_cast<u32>(yoe + era * 400) + (month <= 2 ? 1u : 0u);
    const auto two = [&](u32 at, u32 value) noexcept {
        output[at] = static_cast<char>('0' + value / 10 % 10);
        output[at + 1] = static_cast<char>('0' + value % 10);
    };
    two(0, year / 100);
    two(2, year % 100);
    output[4] = '-';
    two(5, month);
    output[7] = '-';
    two(8, day);
    output[10] = 'T';
    two(11, seconds / 3'600);
    output[13] = ':';
    two(14, seconds / 60 % 60);
    output[16] = ':';
    two(17, seconds % 60);
    constexpr char zone[] = "+00:00";
    for (u32 i = 0; i < 6; ++i)
        output[19 + i] = zone[i];
    return bytes;
}

GuildAdminResult guild_admin_decode_settings(Utf8View body, Slice<u32> scratch,
                                             GuildSettingsSnapshot* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    SettingsParse p{};
    p.fields = {kSettingsFields, sizeof(kSettingsFields) / sizeof(kSettingsFields[0]), 0, &p.error};
    p.document = &s.document;
    const auto read = json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_settings, &p);
    if (!read.ok())
        return {0, 0, p.error == GuildAdminError::none ? GuildAdminError::json : p.error};
    if (!(p.fields.seen & 1u) || !(p.fields.seen & 2u) || !p.value.id)
        return {0, 0, GuildAdminError::identity};
    *output = p.value;
    return {body.bytes, 1, GuildAdminError::none};
}

GuildAdminResult guild_admin_decode_role(Utf8View body, Slice<u32> scratch, GuildAdminRole* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    RoleParse p{};
    p.fields = {kRoleFields, 11, 0, &p.error};
    const auto read = json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_role, &p);
    if (!read.ok())
        return {0, 0, p.error == GuildAdminError::none ? GuildAdminError::json : p.error};
    constexpr u64 required = 1u | 2u | 4u | 8u;
    if ((p.fields.seen & required) != required || !p.value.id)
        return {0, 0, GuildAdminError::identity};
    *output = p.value;
    return {body.bytes, 1, GuildAdminError::none};
}

GuildAdminResult guild_admin_decode_roles(Utf8View body, Slice<u32> scratch, Slice<GuildAdminRole> output) noexcept {
    return decode_array(body, scratch, output, visit_role_item);
}

GuildAdminResult guild_admin_decode_role_counts(Utf8View body, Slice<u32> scratch,
                                                Slice<GuildAdminRoleCount> output) noexcept {
    if (!output.data && output.count)
        return {0, 0, GuildAdminError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    ArrayParse<GuildAdminRoleCount> p{&s.document, output, 0, GuildAdminError::none};
    const auto read =
        json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_role_count, &p);
    if (!read.ok())
        return {0, 0, p.error == GuildAdminError::none ? GuildAdminError::json : p.error};
    return {body.bytes, p.count, GuildAdminError::none};
}

GuildAdminResult guild_admin_decode_bans(Utf8View body, Slice<u32> scratch, Slice<GuildAdminBan> output) noexcept {
    return decode_array(body, scratch, output, visit_ban_item);
}

GuildAdminResult guild_admin_decode_audit_log(Utf8View body, Slice<u32> scratch, GuildAdminAuditPage* page) noexcept {
    if (!page || (!page->entries.data && page->entries.count) || (!page->users.data && page->users.count))
        return {0, 0, GuildAdminError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    AuditParse p{&s.document, {page->entries, page->users, 0, 0}, 0, GuildAdminError::none};
    const auto read = json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_audit_root, &p);
    if (!read.ok())
        return {0, 0, p.error == GuildAdminError::none ? GuildAdminError::json : p.error};
    if (!(p.seen & 1u))
        return {0, 0, GuildAdminError::identity};
    page->entry_count = p.page.entry_count;
    page->user_count = p.page.user_count;
    return {body.bytes, p.page.entry_count, GuildAdminError::none};
}

GuildAdminResult guild_admin_decode_invites(Utf8View body, Slice<u32> scratch,
                                            Slice<GuildAdminInvite> output) noexcept {
    return decode_array(body, scratch, output, visit_invite_item);
}

GuildAdminResult guild_admin_decode_invite(Utf8View body, Slice<u32> scratch, GuildAdminInvite* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    const Scan s = scan(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    InviteParse parse{&s.document, {}, {kInviteFields, 8, 0, nullptr}, GuildAdminError::none};
    parse.fields.error = &parse.error;
    const auto read =
        json_visit_root_object(s.document.input, s.document.structurals, kDepth, visit_invite, &parse);
    if (!read.ok())
        return {0, 0, parse.error == GuildAdminError::none ? GuildAdminError::json : parse.error};
    if (!(parse.fields.seen & 1u))
        return {0, 0, GuildAdminError::identity};
    *output = parse.value;
    return {body.bytes, 1, GuildAdminError::none};
}

} // namespace maicord
