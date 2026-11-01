#include "maicord/protocol/guild/admin.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/guild/invite.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_write.hpp"
#include "../encoding/json_field_lists.hpp"


namespace maicord {
namespace {

using namespace protocol_detail;
using Op = GuildAdminOperation;

static_assert(kGuildAdminInviteCodeBytes == kGuildInviteCodeMaximumBytes);

// 10000-01-01T00:00:00Z: timestamps stay four-digit years.
constexpr u64 kTimestampLimitSeconds = 253'402'300'800ull;
constexpr u64 kTimestampLimitMs = kTimestampLimitSeconds * 1000;

// Single-line or multi-line UTF-8 text with a codepoint range and no NULs.
bool text_valid(const Utf8View& text, u32 minimum_codepoints, u32 maximum_codepoints, bool single_line) noexcept {
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
    static constexpr char prefix[] = "data:image/";
    if (!view_valid(uri) || uri.bytes < sizeof(prefix) - 1 + 8)
        return false;
    for (u32 i = 0; i < sizeof(prefix) - 1; ++i)
        if (uri.data[i] != prefix[i])
            return false;
    bool comma = false;
    for (u32 i = sizeof(prefix) - 1; i < uri.bytes; ++i) {
        const char c = uri.data[i];
        const bool base64 = ascii_is_alnum(c) ||
                            c == '+' || c == '/' || c == '=';
        if (c == ',')
            comma = true;
        else if (!base64 && (comma || (c != ';' && c != '-' && c != '.')))
            return false;
    }
    return comma;
}

// A stored image hash: 32 lowercase hex characters, "a_" first when animated.
bool image_hash_valid(Utf8View hash) noexcept {
    if (!view_valid(hash) || (hash.bytes != 32 && hash.bytes != 34))
        return false;
    u32 at = 0;
    if (hash.bytes == 34) {
        if (hash.data[0] != 'a' || hash.data[1] != '_')
            return false;
        at = 2;
    }
    for (; at < hash.bytes; ++at) {
        const char c = hash.data[at];
        if (!ascii_is_lower_hex_digit(c))
            return false;
    }
    return true;
}

// An image field: empty (null), a data URI or the stored hash.
bool image_value_valid(Utf8View value) noexcept {
    return !value.bytes || data_uri_valid(value) || image_hash_valid(value);
}

// The code is counted in UTF-16 code units, as a text input's maxLength does.
bool vanity_code_valid(Utf8View code) noexcept {
    return (!code.bytes || text_valid(code, 1, kGuildAdminVanityCodeCharacters, true)) &&
           utf8_utf16_units(code) <= kGuildAdminVanityCodeCharacters;
}

// "#RRGGBB", either case.
bool brand_color_valid(Utf8View color) noexcept {
    if (!color.bytes)
        return true;
    if (!view_valid(color) || color.bytes != 7 || color.data[0] != '#')
        return false;
    for (u32 i = 1; i < 7; ++i) {
        const char c = color.data[i];
        if (!ascii_is_hex_digit(c))
            return false;
    }
    return true;
}

bool profile_write_valid(const GuildServerProfileWrite& p) noexcept {
    const u8 f = p.fields;
    if ((f & ~guild_profile_write_all) || (!p.traits.data && p.traits.count) ||
        p.traits.count > kGuildAdminProfileTraits ||
        (!p.game_application_ids.data && p.game_application_ids.count) ||
        p.game_application_ids.count > kGuildAdminProfileGames)
        return false;
    if ((f & guild_profile_write_name) ? !text_valid(p.name, 2, kGuildAdminNameCodepoints, true) : p.name.bytes != 0)
        return false;
    if (p.description.bytes && !text_valid(p.description, 1, kGuildAdminProfileDescriptionCodepoints, false))
        return false;
    if (((f & guild_profile_write_icon) ? !image_value_valid(p.icon) : p.icon.bytes != 0) ||
        ((f & guild_profile_write_custom_banner) ? !image_value_valid(p.custom_banner) : p.custom_banner.bytes != 0) ||
        !brand_color_valid(p.brand_color_primary) ||
        ((f & guild_profile_write_visibility) ? p.visibility > 3 : p.visibility != 0))
        return false;
    for (u32 i = 0; i < p.traits.count; ++i) {
        const auto& t = p.traits[i];
        if (t.position >= kGuildAdminProfileTraits ||
            (t.label.bytes && !text_valid(t.label, 1, kGuildAdminProfileTraitCodepoints, true)) ||
            t.emoji_name.bytes > kGuildAdminProfileEmojiNameBytes ||
            (t.emoji_name.bytes && !text_valid(t.emoji_name, 1, kGuildAdminProfileEmojiNameBytes, true)))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (p.traits[j].position == t.position)
                return false;
    }
    for (u32 i = 0; i < p.game_application_ids.count; ++i)
        if (!p.game_application_ids[i])
            return false;
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

// A complete run of non-empty names whose count matches.
bool feature_names_valid(ConstSlice<u8> names, u16 count) noexcept {
    if ((!names.data && names.count) || names.count > 0xFFFF)
        return false;
    u32 cursor = 0, seen = 0;
    while (cursor < names.count) {
        const u32 bytes = names[cursor];
        if (!bytes || bytes > names.count - cursor - 1 ||
            !text_valid({reinterpret_cast<const char*>(names.data + cursor + 1), bytes}, 1, 255, true))
            return false;
        cursor += 1 + bytes;
        ++seen;
    }
    return seen == count;
}

bool guild_patch_valid(const GuildSettingsPatch& patch) noexcept {
    const u16 f = patch.fields;
    constexpr u16 alone = guild_settings_patch_widget | guild_settings_patch_profile | guild_settings_patch_vanity;
    // A banner or splash: an original file accompanies only a new image (a data URI); otherwise a data URI, a
    // stored hash or null.
    const auto image_valid = [&](u16 field, Utf8View value, ConstSlice<u8> original) noexcept {
        if (!(f & field))
            return !original.data && !original.count;
        return original.data ? original.count && data_uri_valid(value) : !original.count && image_value_valid(value);
    };
    return f && (!(f & alone) || !(f & (f - 1))) &&
           ((f & guild_settings_patch_profile) ? patch.profile && profile_write_valid(*patch.profile)
                                               : !patch.profile) &&
           ((f & guild_settings_patch_vanity) ? vanity_code_valid(patch.vanity_code) : !patch.vanity_code.bytes) &&
           image_valid(guild_settings_patch_banner, patch.banner, patch.banner_original) &&
           image_valid(guild_settings_patch_splash, patch.splash, patch.splash_original) &&
           (!(f & guild_settings_patch_features) || feature_names_valid(patch.feature_names, patch.feature_count)) &&
           (!(f & guild_settings_patch_name) || text_valid(patch.name, 2, kGuildAdminNameCodepoints, true)) &&
           (!(f & guild_settings_patch_icon) || image_value_valid(patch.icon)) &&
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
    constexpr u8 create_fields = role_patch_name | role_patch_color | role_patch_permissions;
    return (create || f) && !(f & ~(create ? create_fields : role_patch_all)) &&
           (!(f & role_patch_name) || text_valid(patch.name, 1, kGuildAdminNameCodepoints, true)) &&
           (!(f & role_patch_color) || patch.color <= 0xFFFFFFu) &&
           (!(f & role_patch_icon) || !patch.icon.bytes || data_uri_valid(patch.icon)) &&
           (!(f & role_patch_unicode_emoji) || !patch.unicode_emoji.bytes ||
            text_valid(patch.unicode_emoji, 1, 8, true));
}

bool member_patch_valid(const MemberPatch& patch, bool self) noexcept {
    const u8 f = patch.fields;
    // Oneself: nickname and per-server avatar; others: never the avatar.
    if (!f || (f & ~member_patch_all) || (self && (f & ~(member_patch_nickname | member_patch_avatar))) ||
        (!self && (f & member_patch_avatar)))
        return false;
    if ((!(f & member_patch_avatar) && patch.avatar.bytes) ||
        (patch.avatar.bytes && !data_uri_valid(patch.avatar)))
        return false;
    if ((f & member_patch_nickname) && patch.nickname.bytes &&
        !text_valid(patch.nickname, 1, kGuildAdminNicknameCodepoints, true))
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
    return !(f & member_patch_timeout) || patch.communication_disabled_until_ms < kTimestampLimitMs;
}

bool member_role_patch_valid(const MemberPatch& patch) noexcept {
    return patch.fields == member_patch_roles && patch.role_ids.count == 1 && patch.role_ids.data &&
           byte_range_valid({reinterpret_cast<const u8*>(patch.role_ids.data), sizeof(u64)}) &&
           patch.role_ids[0] && !patch.nickname.data && !patch.nickname.bytes && !patch.avatar.data &&
           !patch.avatar.bytes && !patch.communication_disabled_until_ms && !patch.voice_channel_id &&
           !patch.mute && !patch.deafen;
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
    if (!f || (f & ~guild_profile_patch_all))
        return false;
    if ((!(f & guild_profile_patch_pronouns) && patch.pronouns.bytes) ||
        (!(f & guild_profile_patch_bio) && patch.bio.bytes) || (!(f & guild_profile_patch_banner) && patch.banner.bytes))
        return false;
    return (!patch.pronouns.bytes || text_valid(patch.pronouns, 1, kGuildAdminPronounsCodepoints, true)) &&
           (!patch.bio.bytes || text_valid(patch.bio, 1, kGuildAdminBioCodepoints, false)) &&
           (!patch.banner.bytes || data_uri_valid(patch.banner));
}

// A null reason sends nothing; any other, even empty, is sent.
bool reason_valid(const GuildAdminRequest& request) noexcept {
    if (!view_valid(request.reason))
        return false;
    if (!request.reason.data)
        return true;
    // Lifting a ban and revoking an invite carry no reason.
    if (request.operation == Op::ban_remove || request.operation == Op::invite_delete)
        return false;
    return guild_admin_method(request.operation) != RestMethod::get && audit_reason_encodes(request.reason);
}

// Body writers ------------------------------------------------------------

// Key order is fixed, whichever fields are set.
void write_guild(JsonBody& b, const GuildSettingsPatch& p) noexcept {
    const u16 f = p.fields;
    b.begin_object();
    b.member_if(f & guild_settings_patch_name, "name", json_text(p.name));
    b.member_if(f & guild_settings_patch_icon, "icon", nullable_json(p.icon));
    b.member_if(f & guild_settings_patch_splash, "splash", nullable_json(p.splash));
    b.member_if(f & guild_settings_patch_banner, "banner", nullable_json(p.banner));
    if (f & guild_settings_patch_features) {
        b.begin_array("features");
        for (u32 cursor = 0; cursor < p.feature_names.count;) {
            const u32 bytes = p.feature_names[cursor];
            b.element(json_text({reinterpret_cast<const char*>(p.feature_names.data + cursor + 1), bytes}));
            cursor += 1 + bytes;
        }
        b.end_array();
    }
    b.member_if(f & guild_settings_patch_afk_channel, "afk_channel_id", nullable_snowflake(p.afk_channel_id));
    b.member_if(f & guild_settings_patch_afk_timeout, "afk_timeout", p.afk_timeout_seconds);
    b.member_if(f & guild_settings_patch_system_channel, "system_channel_id", nullable_snowflake(p.system_channel_id));
    b.member_if(f & guild_settings_patch_verification_level, "verification_level", p.verification_level);
    b.member_if(f & guild_settings_patch_default_notifications, "default_message_notifications",
                p.default_message_notifications);
    b.member_if(f & guild_settings_patch_explicit_content_filter, "explicit_content_filter", p.explicit_content_filter);
    b.member_if(f & guild_settings_patch_system_channel_flags, "system_channel_flags", p.system_channel_flags);
    b.member_if(f & guild_settings_patch_progress_bar, "premium_progress_bar_enabled",
                json_bool(p.premium_progress_bar_enabled));
    b.end_object();
}

void write_server_profile(JsonBody& b, const GuildServerProfileWrite& p) noexcept {
    b.begin_object();
    b.member_if(p.fields & guild_profile_write_name, "name", json_text(p.name));
    b.member("description", json_text(p.description));
    b.member_if(p.fields & guild_profile_write_icon, "icon", nullable_json(p.icon));
    b.member_if(p.fields & guild_profile_write_custom_banner, "custom_banner", nullable_json(p.custom_banner));
    b.member_if(p.fields & guild_profile_write_visibility, "visibility", p.visibility);
    b.member("brand_color_primary", nullable_json(p.brand_color_primary));
    b.begin_array("traits");
    for (u32 i = 0; i < p.traits.count; ++i) {
        const auto& t = p.traits[i];
        if (!t.label.bytes)
            continue;
        b.next();
        b.begin_object();
        b.member("label", json_text(t.label));
        b.member("position", t.position);
        b.member_if(t.emoji_id, "emoji_id", snowflake_text(t.emoji_id));
        b.member_if(t.emoji_name.bytes, "emoji_name", json_text(t.emoji_name));
        b.member_if(t.emoji_animated_set, "emoji_animated", json_bool(t.emoji_animated));
        b.end_object();
    }
    b.end_array();
    b.begin_array("game_application_ids");
    for (u32 i = 0; i < p.game_application_ids.count; ++i)
        b.element(snowflake_text(p.game_application_ids[i]));
    b.end_array();
    b.end_object();
}

void write_role(JsonBody& b, const RolePatch& p) noexcept {
    b.begin_object();
    b.member_if(p.fields & role_patch_name, "name", json_text(p.name));
    b.member_if(p.fields & role_patch_permissions, "permissions", snowflake_text(p.permissions));
    b.member_if(p.fields & role_patch_color, "color", p.color);
    b.member_if(p.fields & role_patch_hoist, "hoist", json_bool(p.hoist));
    b.member_if(p.fields & role_patch_mentionable, "mentionable", json_bool(p.mentionable));
    b.member_if(p.fields & role_patch_icon, "icon", nullable_json(p.icon));
    b.member_if(p.fields & role_patch_unicode_emoji, "unicode_emoji", nullable_json(p.unicode_emoji));
    b.end_object();
}

// The role create body: every key, in this order.
void write_role_create(JsonBody& b, const RolePatch* p) noexcept {
    const u8 f = p ? p->fields : 0;
    const u32 color = (f & role_patch_color) ? p->color : 0;
    b.begin_object();
    b.member("name", json_text((f & role_patch_name) ? p->name : utf8_literal("new role")));
    b.member("color", color);
    b.key("colors");
    b.begin_object();
    b.member("primary_color", color);
    b.member("secondary_color", "null");
    b.member("tertiary_color", "null");
    b.end_object();
    b.member("permissions", snowflake_text((f & role_patch_permissions) ? p->permissions : 0));
    b.end_object();
}

void write_member(JsonBody& b, const MemberPatch& p, bool self) noexcept {
    b.begin_object();
    if (p.fields & member_patch_nickname)
        b.member("nick", self ? OrNull<JsonText>{true, {p.nickname}} : nullable_json(p.nickname));
    b.member_if(p.fields & member_patch_avatar, "avatar", nullable_json(p.avatar));
    if (p.fields & member_patch_roles) {
        b.begin_array("roles");
        for (u32 i = 0; i < p.role_ids.count; ++i)
            b.element(snowflake_text(p.role_ids[i]));
        b.end_array();
    }
    if (p.fields & member_patch_timeout) {
        b.key("communication_disabled_until");
        if (p.communication_disabled_until_ms) {
            char stamp[32];
            const u32 bytes =
                guild_admin_format_timestamp_ms(p.communication_disabled_until_ms, {stamp, sizeof(stamp)});
            b.ok = b.ok && bytes;
            b.put(json_text({stamp, bytes}));
        } else
            b.put("null");
    }
    b.member_if(p.fields & member_patch_voice_channel, "channel_id", nullable_snowflake(p.voice_channel_id));
    b.member_if(p.fields & member_patch_mute, "mute", json_bool(p.mute));
    b.member_if(p.fields & member_patch_deafen, "deaf", json_bool(p.deafen));
    b.end_object();
}

void write_profile(JsonBody& b, const GuildProfilePatch& p) noexcept {
    b.begin_object();
    b.member_if(p.fields & guild_profile_patch_pronouns, "pronouns", json_text(p.pronouns));
    b.member_if(p.fields & guild_profile_patch_bio, "bio", json_text(p.bio));
    b.member_if(p.fields & guild_profile_patch_banner, "banner", nullable_json(p.banner));
    b.end_object();
}

void write_overwrite(JsonBody& b, const PermissionOverwriteWrite& o) noexcept {
    b.begin_object();
    b.member("id", snowflake_text(o.id));
    b.member("type", o.type);
    b.member("allow", snowflake_text(o.allow));
    b.member("deny", snowflake_text(o.deny));
    b.end_object();
}

void write_channel(JsonBody& b, const ChannelPatch& p) noexcept {
    b.begin_object();
    b.member_if(p.fields & channel_patch_name, "name", json_text(p.name));
    b.member_if(p.fields & channel_patch_type, "type", p.type);
    b.member_if(p.fields & channel_patch_topic, "topic", nullable_json(p.topic));
    b.member_if(p.fields & channel_patch_parent, "parent_id", nullable_snowflake(p.parent_id));
    b.member_if(p.fields & channel_patch_position, "position", p.position);
    b.member_if(p.fields & channel_patch_nsfw, "nsfw", json_bool(p.nsfw));
    b.member_if(p.fields & channel_patch_slowmode, "rate_limit_per_user", p.slowmode_seconds);
    b.member_if(p.fields & channel_patch_bitrate, "bitrate", p.bitrate);
    b.member_if(p.fields & channel_patch_user_limit, "user_limit", p.user_limit);
    if (p.fields & channel_patch_overwrites) {
        b.begin_array("permission_overwrites");
        for (u32 i = 0; i < p.overwrites.count; ++i) {
            b.next();
            write_overwrite(b, p.overwrites[i]);
        }
        b.end_array();
    }
    b.end_object();
}

void write_positions(JsonBody& b, ConstSlice<GuildAdminPosition> positions, bool channels) noexcept {
    b.begin_array();
    for (u32 i = 0; i < positions.count; ++i) {
        const auto& p = positions[i];
        b.next();
        b.begin_object();
        b.member("id", snowflake_text(p.id));
        b.member("position", p.position);
        if (channels && (p.flags & guild_admin_position_parent)) {
            b.member("parent_id", nullable_snowflake(p.parent_id));
            b.member("lock_permissions", json_bool((p.flags & guild_admin_position_lock_permissions) != 0));
        }
        b.end_object();
    }
    b.end_array();
}

void write_body(const GuildAdminRequest& r, JsonBody& b) noexcept {
    switch (r.operation) {
    case Op::guild_update:
        write_guild(b, *r.guild);
        break;
    case Op::profile_update:
        write_server_profile(b, *r.guild->profile);
        break;
    case Op::vanity_update:
        b.begin_object();
        b.member("code", json_text(r.guild->vanity_code));
        b.end_object();
        break;
    case Op::widget_update:
        b.begin_object();
        b.member("enabled", json_bool(r.guild->widget_enabled));
        b.member("channel_id", nullable_snowflake(r.guild->widget_channel_id));
        b.end_object();
        break;
    case Op::role_create:
        write_role_create(b, r.role);
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
        b.member("delete_message_seconds", r.delete_message_seconds);
        b.end_object();
        break;
    case Op::channel_create:
    case Op::channel_update:
        write_channel(b, *r.channel);
        break;
    case Op::overwrite_put:
        write_overwrite(b, *r.overwrite);
        break;
    case Op::guild_leave:
        // A member is never lurking here.
        b.put("{\"lurking\":false}");
        break;
    case Op::invite_create: {
        const InviteCreate defaults{86'400, 0, false, false};
        const auto& i = r.invite ? *r.invite : defaults;
        b.begin_object();
        b.member("max_age", i.max_age_seconds);
        b.member("max_uses", i.max_uses);
        b.member("temporary", json_bool(i.temporary));
        b.member("unique", json_bool(i.unique));
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

// Field lookup with duplicate detection over a fixed key table.
struct Fields {
    const u8* names; // MemberNames bytes: the count, then the length-prefixed names
    u64 seen;
    GuildAdminError* error;

    // Returns the field index, count for unknown keys, or ~0u after a duplicate.
    __declspec(noinline) u32 find(JsonString name) noexcept {
        const u32 i = member_index(name, names);
        if (i == names[0])
            return i;
        if (seen & (1ull << i)) {
            *error = GuildAdminError::duplicate;
            return ~0u;
        }
        seen |= 1ull << i;
        return i;
    }
};

bool fail(GuildAdminError* error, GuildAdminError value = GuildAdminError::field) noexcept {
    if (*error == GuildAdminError::none)
        *error = value;
    return false;
}

// What every decode shares: the document being read and the first error.
struct ParseState {
    const JsonIndexedDocument* document;
    GuildAdminError error;
};
// A JSON object read through a fixed key table.
struct ObjectParse : ParseState {
    Fields fields;
};
// Items counted as they are stored.
struct CollectionParse : ParseState {
    u32 count;
};

// Visits `value` (an object) into `p`, with its keys in `names`; the first error, or none.
GuildAdminError visit_object_with(ObjectParse& p, void* opaque, const u8* names, JsonValue value,
                                  JsonMemberVisitor visitor) noexcept {
    p.fields = {names, 0, &p.error};
    if (json_visit_indexed_object(*p.document, value, kDepth, visitor, opaque).ok())
        return GuildAdminError::none;
    return p.error == GuildAdminError::none ? GuildAdminError::json : p.error;
}
template <typename Parse>
inline GuildAdminError visit_object(Parse& p, const u8* names, JsonValue value, JsonMemberVisitor visitor) noexcept {
    return visit_object_with(p, &p, names, value, visitor);
}

// User --------------------------------------------------------------------

struct UserParse : ObjectParse {
    GuildAdminUser* user;
};
constexpr const u8* kUserFields = MemberNames<"id", "username", "global_name", "avatar", "discriminator">::data.bytes;

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
    reset_value(user);
    UserParse p{};
    p.document = &document;
    p.user = user;
    const auto result = visit_object(p, kUserFields, value, visit_user);
    return result == GuildAdminError::none ? user->id || fail(error, GuildAdminError::identity) : fail(error, result);
}

// Widget and server profile -------------------------------------------------

struct WidgetParse : ObjectParse {
    GuildAdminWidget value;
};
constexpr const u8* kWidgetFields = MemberNames<"enabled", "channel_id">::data.bytes;

bool visit_widget(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<WidgetParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    const bool valid = field == 0   ? json_value_to_bool(value, &p.value.enabled)
                       : field == 1 ? json_value_to_nullable_snowflake(value, &p.value.channel_id)
                                    : true;
    return valid || fail(&p.error);
}

struct ProfileParse : ObjectParse {
    GuildAdminProfile* value;
};
constexpr const u8* kProfileFields = MemberNames<"visibility", "name", "description", "icon_hash", "custom_banner_hash",
                                                 "brand_color_primary", "traits", "game_application_ids",
                                                 "features">::data.bytes;

struct TraitParse : ObjectParse {
    GuildAdminProfileTrait trait;
    u32 position;
};
constexpr const u8* kTraitFields =
    MemberNames<"label", "position", "emoji_id", "emoji_name", "emoji_animated">::data.bytes;

bool visit_trait(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<TraitParse*>(opaque);
    auto& t = p.trait;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    switch (field) {
    case 0: valid = decode_text(value, t.label, kGuildAdminProfileTraitBytes, &t.label_bytes, true); break;
    case 1: valid = decode_u32(value, &p.position); break;
    case 2: valid = json_value_to_nullable_snowflake(value, &t.emoji_id); break;
    case 3: valid = decode_text(value, t.emoji_name, kGuildAdminProfileEmojiNameBytes, &t.emoji_name_bytes, true); break;
    case 4: {
        bool animated = false; // Validated, not kept.
        valid = value.kind == JsonValueKind::null_value || json_value_to_bool(value, &animated);
        break;
    }
    default: break;
    }
    return valid || fail(&p.error);
}

// Traits outside positions 0-4 are dropped, as the official client reads them.
bool visit_trait_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    TraitParse trait{};
    trait.document = p.document;
    trait.position = ~0u;
    const auto result = visit_object(trait, kTraitFields, value, visit_trait);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (trait.position < kGuildAdminProfileTraits)
        p.value->traits[trait.position] = trait.trait;
    return true;
}

bool visit_game_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    auto& v = *p.value;
    if (v.game_count >= kGuildAdminProfileGames)
        return fail(&p.error, GuildAdminError::capacity);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id) || !id)
        return fail(&p.error);
    v.game_application_ids[v.game_count++] = id;
    return true;
}

bool visit_feature_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    JsonString name{};
    if (json_value_as_string(value, &name) && json_string_equals_ascii(name, utf8_literal("DISCOVERABLE")))
        p.value->discoverable = true;
    return true;
}

// An unknown or null visibility leaves it unknown.
bool visit_profile(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    auto& v = *p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    const bool null = value.kind == JsonValueKind::null_value;
    u32 number = 0;
    u16 bytes = 0;
    bool valid = true;
    switch (field) {
    case 0:
        if (!null && decode_u32(value, &number, 255)) {
            v.visibility = static_cast<u8>(number);
            v.visibility_known = true;
        }
        break;
    case 1:
        valid = decode_text(value, v.name, kGuildAdminNameBytes, &v.name_bytes, true);
        v.name_known = !null;
        break;
    case 2: valid = decode_text(value, v.description, kGuildAdminProfileDescriptionBytes, &v.description_bytes, true); break;
    case 3:
        valid = decode_text(value, v.icon, kGuildAdminHashBytes, &bytes, true);
        v.icon_bytes = static_cast<u8>(bytes);
        v.icon_known = true;
        break;
    case 4:
        valid = decode_text(value, v.custom_banner, kGuildAdminHashBytes, &bytes, true);
        v.custom_banner_bytes = static_cast<u8>(bytes);
        v.custom_banner_known = true;
        break;
    case 5:
        valid = decode_text(value, v.brand_color, kGuildAdminBrandColorBytes, &bytes, true);
        v.brand_color_bytes = static_cast<u8>(bytes);
        break;
    case 6:
        valid = null || (value.kind == JsonValueKind::array &&
                         json_visit_indexed_array(*p.document, value, kDepth, visit_trait_item, &p).ok());
        break;
    case 7:
        valid = null || (value.kind == JsonValueKind::array &&
                         json_visit_indexed_array(*p.document, value, kDepth, visit_game_item, &p).ok());
        break;
    case 8:
        valid = null || (value.kind == JsonValueKind::array &&
                         json_visit_indexed_array(*p.document, value, kDepth, visit_feature_item, &p).ok());
        break;
    default: break;
    }
    return valid || fail(&p.error);
}

struct VanityParse : ObjectParse {
    GuildAdminVanity value;
};
constexpr const u8* kVanityFields = MemberNames<"code", "uses">::data.bytes;

bool visit_vanity(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<VanityParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    if (field == 0)
        valid = decode_text(value, p.value.code, kGuildAdminVanityCodeBytes, &p.value.code_bytes, true);
    else if (field == 1)
        valid = value.kind == JsonValueKind::null_value || decode_u32(value, &p.value.uses);
    return valid || fail(&p.error);
}

// Roles -------------------------------------------------------------------

struct RoleParse : ObjectParse {
    GuildAdminRole value;
};
constexpr const u8* kRoleFields = MemberNames<"id", "name", "permissions", "position", "color", "hoist", "managed",
                                              "mentionable", "icon", "unicode_emoji", "flags">::data.bytes;

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

template <typename Item> struct ArrayParse : CollectionParse {
    Slice<Item> output;
};

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

struct BanParse : ObjectParse {
    GuildAdminBan* value;
};
constexpr const u8* kBanFields = MemberNames<"user", "reason">::data.bytes;

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
    BanParse parse{};
    parse.document = p.document;
    parse.value = &ban;
    const auto result = visit_object(parse, kBanFields, value, visit_ban);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (!(parse.fields.seen & 1u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].user.id == ban.user.id)
            return fail(&p.error, GuildAdminError::duplicate);
    copy_value(&p.output[p.count++], ban);
    return true;
}

// Audit log ---------------------------------------------------------------

struct AuditEntryParse : ObjectParse {
    GuildAdminAuditEntry value;
};
constexpr const u8* kAuditEntryFields =
    MemberNames<"id", "user_id", "target_id", "action_type", "reason">::data.bytes;

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
    case 1: valid = json_value_to_nullable_snowflake(value, &v.user_id); break;
    case 2: valid = json_value_to_nullable_snowflake(value, &v.target_id); break;
    case 3: valid = decode_u32(value, &number, 0xFFFF); v.action_type = static_cast<u16>(number); break;
    case 4: valid = decode_text(value, v.reason, kGuildAdminReasonBytes, &v.reason_bytes, true); break;
    default: break;
    }
    return valid || fail(&p.error);
}

struct AuditParse : CollectionParse {
    GuildAdminAuditPage page;
    u64 seen;
};

bool visit_audit_entry_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    if (p.page.entry_count >= p.page.entries.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    AuditEntryParse entry{};
    entry.document = p.document;
    const auto result = visit_object(entry, kAuditEntryFields, value, visit_audit_entry);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (!(entry.fields.seen & 1u) || !(entry.fields.seen & 8u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.page.entry_count; ++i)
        if (p.page.entries[i].id == entry.value.id)
            return fail(&p.error, GuildAdminError::duplicate);
    copy_value(&p.page.entries[p.page.entry_count++], entry.value);
    return true;
}

bool visit_audit_user_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    if (p.page.user_count >= p.page.users.count)
        return fail(&p.error, GuildAdminError::capacity);
    GuildAdminUser user;
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

struct InviteParse : ObjectParse {
    GuildAdminInvite value;
};
constexpr const u8* kInviteFields = MemberNames<"code", "channel", "inviter", "uses", "max_uses", "max_age",
                                                "temporary", "expires_at">::data.bytes;

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
                guild_invite_code_valid(v.code, v.code_bytes);
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
        else
            valid = json_value_to_timestamp(value, &v.expires_at_ms);
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
    InviteParse parse{};
    parse.document = p.document;
    const auto result = visit_object(parse, kInviteFields, value, visit_invite);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (!(parse.fields.seen & 1u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].code_bytes == parse.value.code_bytes &&
            bytes_equal(p.output[i].code, parse.value.code, parse.value.code_bytes))
            return fail(&p.error, GuildAdminError::duplicate);
    p.output[p.count++] = parse.value;
    return true;
}

// Scans `body` and visits its root with `object` or `array`; the result's count is unset.
GuildAdminResult decode_scanned(Utf8View body, Slice<u32> scratch, ParseState& p, void* opaque,
                                JsonMemberVisitor object, JsonArrayVisitor array) noexcept {
    const auto s = scan_json<GuildAdminError>(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    p.document = &s.document;
    const auto read = array ? json_visit_root_array(s.document.input, s.document.structurals, kDepth, array, opaque)
                            : json_visit_root_object(s.document.input, s.document.structurals, kDepth, object, opaque);
    if (!read.ok())
        return {0, 0, p.error == GuildAdminError::none ? GuildAdminError::json : p.error};
    return {body.bytes, 0, GuildAdminError::none};
}

// A root object of fixed fields; `required` is the mask of those that must appear.
template <typename Parse>
inline GuildAdminResult decode_root(Utf8View body, Slice<u32> scratch, Parse& p, const u8* names,
                                    JsonMemberVisitor visitor, u64 required) noexcept {
    p.fields = {names, 0, &p.error};
    auto result = decode_scanned(body, scratch, p, &p, visitor, nullptr);
    if (result.ok())
        result = (p.fields.seen & required) == required ? GuildAdminResult{body.bytes, 1, GuildAdminError::none}
                                                        : GuildAdminResult{0, 0, GuildAdminError::identity};
    return result;
}

// A root array (`array`) or object (`object`) of items counted in p.count.
template <typename Parse>
inline GuildAdminResult decode_collection(Utf8View body, Slice<u32> scratch, Parse& p, JsonArrayVisitor array,
                                          JsonMemberVisitor object) noexcept {
    auto result = decode_scanned(body, scratch, p, &p, object, array);
    result.count = p.count;
    return result;
}

template <typename Item>
GuildAdminResult decode_array(Utf8View body, Slice<u32> scratch, Slice<Item> output,
                              JsonArrayVisitor visitor) noexcept {
    if (!output.data && output.count)
        return {0, 0, GuildAdminError::input};
    ArrayParse<Item> p{};
    p.output = output;
    return decode_collection(body, scratch, p, visitor, nullptr);
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
    case Op::widget_update:
    case Op::profile_update:
    case Op::vanity_update:
        return RestMethod::patch;
    case Op::role_create:
    case Op::channel_create:
    case Op::invite_create:
        return RestMethod::post;
    case Op::ban_add:
    case Op::overwrite_put:
    case Op::member_role_add:
        return RestMethod::put;
    case Op::role_delete:
    case Op::member_kick:
    case Op::ban_remove:
    case Op::channel_delete:
    case Op::overwrite_delete:
    case Op::invite_delete:
    case Op::guild_leave:
    case Op::member_role_remove:
        return RestMethod::delete_method;
    default:
        return RestMethod::get;
    }
}

bool guild_admin_has_body(GuildAdminOperation op) noexcept {
    if (op == Op::member_role_add)
        return false;
    const auto method = guild_admin_method(op);
    return method == RestMethod::patch || method == RestMethod::post ||
           method == RestMethod::put || op == Op::guild_leave;
}

bool guild_admin_request_valid(const GuildAdminRequest& r) noexcept {
    if (r.operation >= Op::count || !reason_valid(r) || (!r.positions.data && r.positions.count))
        return false;
    const Op op = r.operation;
    const bool needs_guild = guild_major(op);
    const bool needs_channel = channel_major(op);
    if ((needs_guild && !r.guild_id) || (needs_channel && !r.channel_id) || (!needs_guild && r.guild_id) ||
        (!needs_channel && r.channel_id))
        return false;
    const bool needs_target = op == Op::role_update || op == Op::role_delete || op == Op::member_update ||
                              op == Op::member_kick || op == Op::ban_add || op == Op::ban_remove ||
                              op == Op::overwrite_put || op == Op::overwrite_delete ||
                              op == Op::member_role_add || op == Op::member_role_remove;
    if (needs_target != (r.target_id != 0))
        return false;
    if ((op == Op::invite_delete) != (r.invite_code.bytes != 0) ||
        (r.invite_code.bytes && !guild_invite_code_valid(r.invite_code.data, r.invite_code.bytes)))
        return false;
    if ((op == Op::ban_add) ? r.delete_message_seconds > kGuildAdminBanDeleteMaximumSeconds
                            : r.delete_message_seconds != 0)
        return false;
    const bool positions = op == Op::role_reorder || op == Op::channel_reorder;
    if (positions ? !positions_valid(r.positions, op == Op::channel_reorder) : r.positions.count != 0)
        return false;
    const bool guild_patch =
        op == Op::guild_update || op == Op::widget_update || op == Op::profile_update || op == Op::vanity_update;
    if (guild_patch != (r.guild != nullptr) || (r.guild && !guild_patch_valid(*r.guild)))
        return false;
    if (r.guild) {
        const u16 f = r.guild->fields;
        const u16 own = op == Op::widget_update    ? guild_settings_patch_widget
                        : op == Op::profile_update ? guild_settings_patch_profile
                        : op == Op::vanity_update  ? guild_settings_patch_vanity
                                                   : 0;
        constexpr u16 alone =
            guild_settings_patch_widget | guild_settings_patch_profile | guild_settings_patch_vanity;
        if (own ? f != own : (f & alone) != 0)
            return false;
    }
    if (r.role && op != Op::role_create && op != Op::role_update)
        return false;
    if ((op == Op::role_update && !r.role) || (r.role && !role_patch_valid(*r.role, op == Op::role_create)))
        return false;
    const bool member_role = op == Op::member_role_add || op == Op::member_role_remove;
    const bool member = op == Op::member_update || op == Op::member_update_self || member_role;
    if (member != (r.member != nullptr) ||
        (r.member && (member_role ? !member_role_patch_valid(*r.member)
                                  : !member_patch_valid(*r.member, op == Op::member_update_self))))
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
    if (!byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    u8 target[kGuildAdminMaximumTargetBytes];
    GuardedWriter w{{target, sizeof(target)}};
    w.put("/api/v10/");
    const Op op = r.operation;
    if (op == Op::guild_leave) {
        w.put("users/@me/guilds/", r.guild_id);
    } else if (op == Op::invite_delete) {
        w.put("invites/", r.invite_code);
    } else if (channel_major(op)) {
        w.put("channels/", r.channel_id);
        if (op == Op::overwrite_put || op == Op::overwrite_delete) {
            w.put("/permissions/", r.target_id);
        } else if (op == Op::invite_list_channel || op == Op::invite_create)
            w.put("/invites");
    } else {
        w.put("guilds/", r.guild_id);
        switch (op) {
        case Op::role_list:
        case Op::role_create:
        case Op::role_reorder: w.put("/roles"); break;
        case Op::role_update:
        case Op::role_delete: w.put("/roles/", r.target_id); break;
        case Op::role_member_counts: w.put("/roles/member-counts"); break;
        case Op::member_update:
        case Op::member_kick: w.put("/members/", r.target_id); break;
        case Op::member_role_add:
        case Op::member_role_remove: w.put("/members/", r.target_id, "/roles/", r.member->role_ids[0]); break;
        case Op::member_update_self: w.put("/members/@me"); break;
        case Op::member_profile_self: w.put("/profile/@me"); break;
        case Op::ban_list: w.put("/bans"); break;
        case Op::ban_add:
        case Op::ban_remove: w.put("/bans/", r.target_id); break;
        case Op::audit_log: w.put("/audit-logs"); break;
        case Op::channel_create:
        case Op::channel_reorder: w.put("/channels"); break;
        case Op::invite_list_guild: w.put("/invites"); break;
        case Op::widget_get:
        case Op::widget_update: w.put("/widget"); break;
        case Op::profile_get:
        case Op::profile_update: w.put("/profile"); break;
        case Op::vanity_get:
        case Op::vanity_update: w.put("/vanity-url"); break;
        default: break;
        }
        // A kick's reason is a query parameter (encodeURIComponent), never
        // the audit header; an empty reason still sends "?reason=".
        if (op == Op::member_kick && r.reason.data) {
            w.put("?reason=");
            const Slice<u8> room{target + w.writer.used, kGuildAdminMaximumTargetBytes - w.writer.used};
            const auto encoded = w.ok ? rest_encode_audit_reason(r.reason, room) : RestAuditReasonResult{0, false};
            w.ok = encoded.ok;
            w.writer.used += w.ok ? encoded.bytes : 0;
        }
        if ((op == Op::ban_list || op == Op::audit_log)) {
            const GuildAdminPageQuery none{};
            const auto& page = r.page ? *r.page : none;
            const u32 limit = page.limit ? page.limit : (op == Op::audit_log ? 50u : kGuildAdminPageMaximum);
            w.put("?limit=", limit);
            if (page.before)
                w.put("&before=", page.before);
            if (page.after)
                w.put("&after=", page.after);
            if (page.user_id)
                w.put("&user_id=", page.user_id);
            if (page.action_type_set)
                w.put("&action_type=", page.action_type);
        }
    }
    if (!w.ok)
        return {0, 0, GuildAdminError::capacity};
    if (output.count < w.writer.used)
        return {w.writer.used, 0, GuildAdminError::capacity};
    copy_bytes(output.data, target, w.writer.used);
    return {w.writer.used, 0, GuildAdminError::none};
}

GuildAdminResult guild_admin_write_body(const GuildAdminRequest& r, Slice<u8> output) noexcept {
    if (!byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    if (!guild_admin_has_body(r.operation))
        return {0, 0, GuildAdminError::none};
    if (!output.data || !output.count)
        return {0, 0, GuildAdminError::capacity};
    JsonBody body{output};
    write_body(r, body);
    if (!body.ok) {
        secure_clear_bytes(output.data, body.writer.used);
        return {0, 0, GuildAdminError::capacity};
    }
    return {body.writer.used, 0, GuildAdminError::none};
}

bool guild_admin_reason_header(const GuildAdminRequest& r) noexcept {
    return r.reason.data && r.operation != Op::member_kick;
}

u32 guild_admin_image_bytes(const GuildAdminRequest& r) noexcept {
    u32 bytes = 0;
    if (r.member && (r.member->fields & member_patch_avatar))
        bytes += r.member->avatar.bytes;
    if (r.profile && (r.profile->fields & guild_profile_patch_banner))
        bytes += r.profile->banner.bytes;
    if (r.guild) {
        const auto& g = *r.guild;
        if (g.fields & guild_settings_patch_icon)
            bytes += g.icon.bytes;
        if (g.fields & guild_settings_patch_banner)
            bytes += g.banner.bytes;
        if (g.fields & guild_settings_patch_splash)
            bytes += g.splash.bytes;
        if (g.profile)
            bytes += g.profile->icon.bytes + g.profile->custom_banner.bytes;
    }
    return bytes;
}

namespace {
bool md5_valid(Utf8View md5) noexcept {
    if (md5.bytes != 32 || !md5.data)
        return false;
    for (u32 i = 0; i < 32; ++i) {
        const char c = md5.data[i];
        if (!ascii_is_lower_hex_digit(c))
            return false;
    }
    return true;
}
} // namespace

u32 guild_admin_write_md5_header(Utf8View banner_md5, Utf8View splash_md5, Slice<u8> output) noexcept {
    // Sorted by key: GUILD_BANNER before GUILD_INVITE_SPLASH.
    const struct {
        const char* key;
        Utf8View md5;
    } entries[]{{"guild_banner", banner_md5}, {"guild_invite_splash", splash_md5}};
    if (!output.data && output.count)
        return 0;
    ProtocolWriter w{output, 0};
    bool ok = true, any = false;
    for (const auto& entry : entries) {
        if (!md5_valid(entry.md5))
            continue;
        ok = ok && (!any || protocol_write(&w, ", ")) &&
             protocol_write(&w, entry.key, "=\"", Utf8View{entry.md5.data, 32}, '"');
        any = true;
    }
    return ok && any ? w.used : 0;
}

GuildAdminResult guild_admin_write_reason(const GuildAdminRequest& r, Slice<u8> output) noexcept {
    if (!byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    if (!r.reason.bytes || r.operation == Op::member_kick)
        return {0, 0, GuildAdminError::none};
    const auto encoded = rest_encode_audit_reason(r.reason, output);
    return encoded.ok ? GuildAdminResult{encoded.bytes, 0, GuildAdminError::none}
                      : GuildAdminResult{encoded.bytes, 0, GuildAdminError::capacity};
}

namespace {
// YYYY-MM-DDTHH:MM:SS into output[0..19); the caller checked the bounds.
void write_civil_time(u64 unix_seconds, Slice<char> output) noexcept {
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
}
} // namespace

u32 guild_admin_format_timestamp(u64 unix_seconds, Slice<char> output) noexcept {
    constexpr u32 bytes = 25; // YYYY-MM-DDTHH:MM:SS+00:00
    if (!output.data || output.count < bytes || unix_seconds >= kTimestampLimitSeconds)
        return 0;
    write_civil_time(unix_seconds, output);
    static constexpr char zone[] = "+00:00";
    for (u32 i = 0; i < 6; ++i)
        output[19 + i] = zone[i];
    return bytes;
}

u32 guild_admin_format_timestamp_ms(u64 unix_milliseconds, Slice<char> output) noexcept {
    constexpr u32 bytes = 24; // YYYY-MM-DDTHH:MM:SS.mmmZ
    if (!output.data || output.count < bytes || unix_milliseconds >= kTimestampLimitMs)
        return 0;
    write_civil_time(unix_milliseconds / 1000, output);
    const u32 millis = static_cast<u32>(unix_milliseconds % 1000);
    output[19] = '.';
    output[20] = static_cast<char>('0' + millis / 100);
    output[21] = static_cast<char>('0' + millis / 10 % 10);
    output[22] = static_cast<char>('0' + millis % 10);
    output[23] = 'Z';
    return bytes;
}

u32 guild_admin_member_roles_toggle(ConstSlice<u64> current, u64 role_id, bool add,
                                    Slice<u64> output) noexcept {
    if (!role_id || (!current.data && current.count) || (!output.data && output.count))
        return ~0u;
    u32 count = 0;
    bool held = false;
    for (u32 i = 0; i < current.count; ++i) {
        if (current[i] == role_id) {
            held = true;
            if (!add)
                continue;
        }
        if (count == output.count)
            return ~0u;
        output[count++] = current[i];
    }
    if (add && !held) {
        if (count == output.count)
            return ~0u;
        output[count++] = role_id;
    }
    return count;
}

GuildAdminResult guild_admin_decode_widget(Utf8View body, Slice<u32> scratch, GuildAdminWidget* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    WidgetParse p{};
    const auto result = decode_root(body, scratch, p, kWidgetFields, visit_widget, 1u);
    if (result.ok())
        *output = p.value;
    return result;
}

GuildAdminResult guild_admin_decode_profile(Utf8View body, Slice<u32> scratch, GuildAdminProfile* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminProfile value{};
    ProfileParse p{};
    p.value = &value;
    const auto result = decode_root(body, scratch, p, kProfileFields, visit_profile, 0);
    if (result.ok())
        *output = value;
    return result;
}

GuildAdminResult guild_admin_decode_vanity(Utf8View body, Slice<u32> scratch, GuildAdminVanity* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    VanityParse p{};
    const auto result = decode_root(body, scratch, p, kVanityFields, visit_vanity, 0);
    if (result.ok())
        *output = p.value;
    return result;
}

GuildAdminResult guild_admin_decode_role(Utf8View body, Slice<u32> scratch, GuildAdminRole* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    RoleParse p{};
    const auto result = decode_root(body, scratch, p, kRoleFields, visit_role, 1u | 2u | 4u | 8u);
    if (!result.ok() || !p.value.id)
        return result.ok() ? GuildAdminResult{0, 0, GuildAdminError::identity} : result;
    copy_value(output, p.value);
    return result;
}

GuildAdminResult guild_admin_decode_role_counts(Utf8View body, Slice<u32> scratch,
                                                Slice<GuildAdminRoleCount> output) noexcept {
    if (!output.data && output.count)
        return {0, 0, GuildAdminError::input};
    ArrayParse<GuildAdminRoleCount> p{};
    p.output = output;
    return decode_collection(body, scratch, p, nullptr, visit_role_count);
}

GuildAdminResult guild_admin_decode_bans(Utf8View body, Slice<u32> scratch, Slice<GuildAdminBan> output) noexcept {
    return decode_array(body, scratch, output, visit_ban_item);
}

GuildAdminResult guild_admin_decode_audit_log(Utf8View body, Slice<u32> scratch, GuildAdminAuditPage* page) noexcept {
    if (!page || (!page->entries.data && page->entries.count) || (!page->users.data && page->users.count))
        return {0, 0, GuildAdminError::input};
    AuditParse p{};
    p.page = {page->entries, page->users, 0, 0};
    const auto result = decode_collection(body, scratch, p, nullptr, visit_audit_root);
    if (!result.ok() || !(p.seen & 1u))
        return result.ok() ? GuildAdminResult{0, 0, GuildAdminError::identity} : result;
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
    InviteParse p{};
    const auto result = decode_root(body, scratch, p, kInviteFields, visit_invite, 1u);
    if (result.ok())
        *output = p.value;
    return result;
}

} // namespace maicord
