#include "maicord/protocol/guild/admin.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/guild/invite.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_write.hpp"


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
bool image_value_valid(const Utf8View& value) noexcept {
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
        p.game_application_ids.count > kGuildAdminProfileGameMaximum)
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

bool tag_write_valid(const GuildTagWrite& t) noexcept {
    return (!t.tag.bytes || guild_tag_text_valid(t.tag)) && guild_tag_badge_settable(t.badge) &&
           brand_color_valid(t.badge_color_primary) && brand_color_valid(t.badge_color_secondary);
}

// The operations ---------------------------------------------------------------------------------------------------------

// What a request of an operation carries: the ids it needs (the guild's or the channel's is also what its rate limit
// counts by), a body, a list of positions, an invite code, a message window, a game query or game ids; its method; and
// which of the request's optional members (kSlotOffsets) it may carry and must carry.
constexpr u32 kGuildId = 1, kChannelId = 2, kTargetId = 4, kBody = 8, kPositions = 16, kInviteCode = 32,
              kDeleteSeconds = 64, kGameQuery = 128, kGameIds = 256;
enum Slot : u8 {
    slot_guild = 1, slot_role = 2, slot_member = 4, slot_profile = 8, slot_channel = 16, slot_overwrite = 32,
    slot_invite = 64, slot_page = 128
};
constexpr u8 kSlotOffsets[]{
    __builtin_offsetof(GuildAdminRequest, guild),   __builtin_offsetof(GuildAdminRequest, role),
    __builtin_offsetof(GuildAdminRequest, member),  __builtin_offsetof(GuildAdminRequest, profile),
    __builtin_offsetof(GuildAdminRequest, channel), __builtin_offsetof(GuildAdminRequest, overwrite),
    __builtin_offsetof(GuildAdminRequest, invite),  __builtin_offsetof(GuildAdminRequest, page)};

struct OperationRow {
    u16 flags;
    RestMethod method;
    u8 may, must;
};
// One row for each GuildAdminOperation, in its order.
constexpr OperationRow kOperations[]{
    {kGuildId | kBody, RestMethod::patch, slot_guild, slot_guild},                         // guild_update
    {kGuildId, RestMethod::get, 0, 0},                                                     // role_list
    {kGuildId | kBody, RestMethod::post, slot_role, 0},                                    // role_create
    {kGuildId | kTargetId | kBody, RestMethod::patch, slot_role, slot_role},               // role_update
    {kGuildId | kTargetId, RestMethod::delete_method, 0, 0},                               // role_delete
    {kGuildId | kBody | kPositions, RestMethod::patch, 0, 0},                              // role_reorder
    {kGuildId, RestMethod::get, 0, 0},                                                     // role_member_counts
    {kGuildId | kTargetId | kBody, RestMethod::patch, slot_member, slot_member},           // member_update
    {kGuildId | kBody, RestMethod::patch, slot_member, slot_member},                       // member_update_self
    {kGuildId | kTargetId, RestMethod::delete_method, 0, 0},                               // member_kick
    {kGuildId, RestMethod::get, slot_page, 0},                                             // ban_list
    {kGuildId | kTargetId | kBody | kDeleteSeconds, RestMethod::put, 0, 0},                // ban_add
    {kGuildId | kTargetId, RestMethod::delete_method, 0, 0},                               // ban_remove
    {kGuildId, RestMethod::get, slot_page, 0},                                             // audit_log
    {kGuildId | kBody, RestMethod::post, slot_channel, slot_channel},                      // channel_create
    {kChannelId | kBody, RestMethod::patch, slot_channel, slot_channel},                   // channel_update
    {kChannelId, RestMethod::delete_method, 0, 0},                                         // channel_delete
    {kGuildId | kBody | kPositions, RestMethod::patch, 0, 0},                              // channel_reorder
    {kChannelId | kTargetId | kBody, RestMethod::put, slot_overwrite, slot_overwrite},     // overwrite_put
    {kChannelId | kTargetId, RestMethod::delete_method, 0, 0},                             // overwrite_delete
    {kGuildId, RestMethod::get, 0, 0},                                                     // invite_list_guild
    {kChannelId, RestMethod::get, 0, 0},                                                   // invite_list_channel
    {kChannelId | kBody, RestMethod::post, slot_invite, 0},                                // invite_create
    {kInviteCode, RestMethod::delete_method, 0, 0},                                        // invite_delete
    {kGuildId | kBody, RestMethod::delete_method, 0, 0},                                   // guild_leave
    {kGuildId | kBody, RestMethod::patch, slot_profile, slot_profile},                     // member_profile_self
    {kGuildId, RestMethod::get, 0, 0},                                                     // widget_get
    {kGuildId | kBody, RestMethod::patch, slot_guild, slot_guild},                         // widget_update
    {kGuildId, RestMethod::get, 0, 0},                                                     // profile_get
    {kGuildId | kBody, RestMethod::patch, slot_guild, slot_guild},                         // profile_update
    {kGuildId, RestMethod::get, 0, 0},                                                     // vanity_get
    {kGuildId | kBody, RestMethod::patch, slot_guild, slot_guild},                         // vanity_update
    {kGuildId | kTargetId, RestMethod::put, slot_member, slot_member},                     // member_role_add
    {kGuildId | kTargetId, RestMethod::delete_method, slot_member, slot_member},           // member_role_remove
    {kGuildId, RestMethod::get, 0, 0},                                                     // top_games
    {kGameQuery, RestMethod::get, 0, 0},                                                   // game_search
    {kGameIds, RestMethod::get, 0, 0},                                                     // games_list
    {kGuildId | kBody, RestMethod::patch, slot_guild, slot_guild},                         // tag_update
    {kGuildId | kBody, RestMethod::put, 0, 0},                                             // identity_adopt
};
static_assert(sizeof(kOperations) / sizeof(kOperations[0]) == static_cast<usize>(Op::count));

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

// A locale code as the API names them: en-US, es-419.
bool locale_valid(Utf8View locale) noexcept {
    if (locale.bytes < 2 || locale.bytes > kGuildAdminLocaleBytes || !locale.data)
        return false;
    for (u32 i = 0; i < locale.bytes; ++i)
        if (!ascii_is_alnum(locale.data[i]) && locale.data[i] != '-')
            return false;
    return true;
}

bool guild_patch_valid(const GuildSettingsPatch& patch) noexcept {
    const u32 f = patch.fields;
    constexpr u32 alone = guild_settings_patch_widget | guild_settings_patch_profile | guild_settings_patch_vanity;
    // A banner or splash: an original file accompanies only a new image (a data URI); otherwise a data URI, a
    // stored hash or null.
    const auto image_valid = [&](u32 field, Utf8View value, ConstSlice<u8> original) noexcept {
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
           (!(f & guild_settings_patch_explicit_content_filter) || patch.explicit_content_filter <= 2) &&
           (!(f & guild_settings_patch_discovery_splash) || image_value_valid(patch.discovery_splash)) &&
           (!(f & guild_settings_patch_description) || !patch.description.bytes ||
            text_valid(patch.description, 1, kGuildAdminProfileDescriptionCodepoints, false)) &&
           (!(f & guild_settings_patch_preferred_locale) || locale_valid(patch.preferred_locale)) &&
           (!(f & guild_settings_patch_content_level) || patch.owner_configured_content_level <= 3);
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

// A body member that a patch's field bit selects: where its value sits in the patch and how it is written.
enum class Json : u8 { text, optional_text, optional_snowflake, snowflake, number8, number32, boolean };
struct MemberRow {
    u32 field;
    u16 offset;
    Json kind;
    const char* name;
};
#define MAICORD_ROW(patch, bit, member, kind, name) \
    {bit, static_cast<u16>(__builtin_offsetof(patch, member)), Json::kind, name}
void write_rows(JsonBody& b, const void* patch, u32 fields, const MemberRow* rows, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const MemberRow& r = rows[i];
        if (!(fields & r.field))
            continue;
        b.key(r.name);
        const u8* value = static_cast<const u8*>(patch) + r.offset;
        switch (r.kind) {
        case Json::text: b.put_json(*reinterpret_cast<const Utf8View*>(value)); break;
        case Json::optional_text: {
            const auto& text = *reinterpret_cast<const Utf8View*>(value);
            b.put_optional_json(text.bytes != 0, text);
            break;
        }
        case Json::optional_snowflake: {
            const u64 id = *reinterpret_cast<const u64*>(value);
            b.put_optional_snowflake(id != 0, id);
            break;
        }
        case Json::snowflake: b.put_snowflake(*reinterpret_cast<const u64*>(value)); break;
        case Json::number8: b.put_number(*value); break;
        case Json::number32: b.put_number(*reinterpret_cast<const u32*>(value)); break;
        case Json::boolean: b.put_text(*value ? "true" : "false"); break;
        }
    }
}
constexpr MemberRow kGuildRowsBeforeFeatures[]{
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_name, name, text, "name"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_icon, icon, optional_text, "icon"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_splash, splash, optional_text, "splash"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_banner, banner, optional_text, "banner"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_discovery_splash, discovery_splash, optional_text,
                "discovery_splash"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_description, description, optional_text, "description"),
};
// The order Enable Community sends: features, then its two channels.
constexpr MemberRow kGuildRowsAfterFeatures[]{
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_rules_channel, rules_channel_id, optional_snowflake,
                "rules_channel_id"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_public_updates_channel, public_updates_channel_id,
                optional_snowflake, "public_updates_channel_id"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_safety_alerts_channel, safety_alerts_channel_id,
                optional_snowflake, "safety_alerts_channel_id"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_preferred_locale, preferred_locale, text, "preferred_locale"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_content_level, owner_configured_content_level, number8,
                "owner_configured_content_level"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_afk_channel, afk_channel_id, optional_snowflake,
                "afk_channel_id"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_afk_timeout, afk_timeout_seconds, number32, "afk_timeout"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_system_channel, system_channel_id, optional_snowflake,
                "system_channel_id"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_verification_level, verification_level, number8,
                "verification_level"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_default_notifications, default_message_notifications,
                number8, "default_message_notifications"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_explicit_content_filter, explicit_content_filter, number8,
                "explicit_content_filter"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_system_channel_flags, system_channel_flags, number32,
                "system_channel_flags"),
    MAICORD_ROW(GuildSettingsPatch, guild_settings_patch_progress_bar, premium_progress_bar_enabled, boolean,
                "premium_progress_bar_enabled"),
};
constexpr MemberRow kRoleRows[]{
    MAICORD_ROW(RolePatch, role_patch_name, name, text, "name"),
    MAICORD_ROW(RolePatch, role_patch_permissions, permissions, snowflake, "permissions"),
    MAICORD_ROW(RolePatch, role_patch_color, color, number32, "color"),
    MAICORD_ROW(RolePatch, role_patch_hoist, hoist, boolean, "hoist"),
    MAICORD_ROW(RolePatch, role_patch_mentionable, mentionable, boolean, "mentionable"),
    MAICORD_ROW(RolePatch, role_patch_icon, icon, optional_text, "icon"),
    MAICORD_ROW(RolePatch, role_patch_unicode_emoji, unicode_emoji, optional_text, "unicode_emoji"),
};
constexpr MemberRow kChannelRows[]{
    MAICORD_ROW(ChannelPatch, channel_patch_name, name, text, "name"),
    MAICORD_ROW(ChannelPatch, channel_patch_type, type, number8, "type"),
    MAICORD_ROW(ChannelPatch, channel_patch_topic, topic, optional_text, "topic"),
    MAICORD_ROW(ChannelPatch, channel_patch_parent, parent_id, optional_snowflake, "parent_id"),
    MAICORD_ROW(ChannelPatch, channel_patch_position, position, number32, "position"),
    MAICORD_ROW(ChannelPatch, channel_patch_nsfw, nsfw, boolean, "nsfw"),
    MAICORD_ROW(ChannelPatch, channel_patch_slowmode, slowmode_seconds, number32, "rate_limit_per_user"),
    MAICORD_ROW(ChannelPatch, channel_patch_bitrate, bitrate, number32, "bitrate"),
    MAICORD_ROW(ChannelPatch, channel_patch_user_limit, user_limit, number32, "user_limit"),
};
#undef MAICORD_ROW
static_assert(sizeof(PermissionSet) == sizeof(u64));

// Key order is fixed, whichever fields are set.
void write_guild(JsonBody& b, const GuildSettingsPatch& p) noexcept {
    const u32 f = p.fields;
    b.begin_object();
    write_rows(b, &p, f, kGuildRowsBeforeFeatures, sizeof(kGuildRowsBeforeFeatures) / sizeof(kGuildRowsBeforeFeatures[0]));
    if (f & guild_settings_patch_features) {
        b.begin_array("features");
        for (u32 cursor = 0; cursor < p.feature_names.count;) {
            const u32 bytes = p.feature_names[cursor];
            b.element(json_text({reinterpret_cast<const char*>(p.feature_names.data + cursor + 1), bytes}));
            cursor += 1 + bytes;
        }
        b.end_array();
    }
    // The order Enable Community sends: features, then its two channels.
    write_rows(b, &p, f, kGuildRowsAfterFeatures, sizeof(kGuildRowsAfterFeatures) / sizeof(kGuildRowsAfterFeatures[0]));
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
    write_rows(b, &p, p.fields, kRoleRows, sizeof(kRoleRows) / sizeof(kRoleRows[0]));
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
    write_rows(b, &p, p.fields, kChannelRows, sizeof(kChannelRows) / sizeof(kChannelRows[0]));
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
    case Op::tag_update: {
        const auto& t = *r.guild->tag;
        b.begin_object();
        b.member("tag", nullable_json(t.tag));
        b.member("badge", t.badge);
        b.member("badge_color_primary", nullable_json(t.badge_color_primary));
        b.member("badge_color_secondary", nullable_json(t.badge_color_secondary));
        b.end_object();
        break;
    }
    case Op::identity_adopt:
        b.begin_object();
        b.member("identity_guild_id", snowflake_text(r.guild_id));
        b.member("identity_enabled", json_bool(true));
        b.end_object();
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

} // namespace

RestMethod guild_admin_method(GuildAdminOperation op) noexcept {
    return op < Op::count ? kOperations[static_cast<u32>(op)].method : RestMethod::get;
}

bool guild_admin_has_body(GuildAdminOperation op) noexcept {
    return op < Op::count && (kOperations[static_cast<u32>(op)].flags & kBody);
}

bool guild_admin_request_valid(const GuildAdminRequest& r) noexcept {
    if (r.operation >= Op::count || !reason_valid(r) || (!r.positions.data && r.positions.count))
        return false;
    const Op op = r.operation;
    const auto& row = kOperations[static_cast<u32>(op)];
    const u32 f = row.flags;
    if ((r.guild_id != 0) != ((f & kGuildId) != 0) || (r.channel_id != 0) != ((f & kChannelId) != 0) ||
        (r.target_id != 0) != ((f & kTargetId) != 0))
        return false;
    // The optional members it carries are the ones its operation takes, and the ones it needs.
    for (u32 i = 0; i < sizeof(kSlotOffsets); ++i) {
        const void* member;
        __builtin_memcpy(&member, reinterpret_cast<const u8*>(&r) + kSlotOffsets[i], sizeof(member));
        if (member ? !(row.may >> i & 1) : (row.must >> i & 1))
            return false;
    }
    if ((r.invite_code.bytes != 0) != ((f & kInviteCode) != 0) ||
        (r.invite_code.bytes && !guild_invite_code_valid(r.invite_code.data, r.invite_code.bytes)))
        return false;
    if ((f & kDeleteSeconds) ? r.delete_message_seconds > kGuildAdminBanDeleteMaximumSeconds
                             : r.delete_message_seconds != 0)
        return false;
    if ((f & kPositions) ? !positions_valid(r.positions, op == Op::channel_reorder) : r.positions.count != 0)
        return false;
    if (r.guild) {
        if ((op == Op::tag_update) != (r.guild->tag != nullptr))
            return false;
        if (op == Op::tag_update) {
            // The tag fields alone, without field bits.
            if (r.guild->fields || r.guild->profile || r.guild->vanity_code.bytes || !tag_write_valid(*r.guild->tag))
                return false;
        } else if (!guild_patch_valid(*r.guild))
            return false;
        const u32 fields = r.guild->fields;
        const u32 own = op == Op::widget_update    ? guild_settings_patch_widget
                        : op == Op::profile_update ? guild_settings_patch_profile
                        : op == Op::vanity_update  ? guild_settings_patch_vanity
                                                   : 0;
        constexpr u32 alone =
            guild_settings_patch_widget | guild_settings_patch_profile | guild_settings_patch_vanity;
        if (own ? fields != own : (fields & alone) != 0)
            return false;
    }
    if (r.role && !role_patch_valid(*r.role, op == Op::role_create))
        return false;
    const bool member_role = op == Op::member_role_add || op == Op::member_role_remove;
    if (r.member && (member_role ? !member_role_patch_valid(*r.member)
                                 : !member_patch_valid(*r.member, op == Op::member_update_self)))
        return false;
    if (r.profile && !profile_patch_valid(*r.profile))
        return false;
    if (r.channel && !channel_patch_valid(*r.channel, op == Op::channel_create))
        return false;
    if (r.overwrite && (!overwrite_valid(*r.overwrite) || r.overwrite->id != r.target_id))
        return false;
    if (r.invite && (r.invite->max_age_seconds > 604'800 || r.invite->max_uses > 100))
        return false;
    // The search sends its normalized query; the games list 1-25 nonzero ids.
    if (f & kGameQuery) {
        char query[kGuildAdminGameQueryCodepoints * 4];
        if (!view_valid(r.game_query) || !guild_admin_game_query(r.game_query, {query, sizeof(query)}))
            return false;
    } else if (r.game_query.bytes)
        return false;
    if (f & kGameIds) {
        if (!r.game_ids.data || !r.game_ids.count || r.game_ids.count > kGuildAdminGamesListMaximum)
            return false;
        for (u32 i = 0; i < r.game_ids.count; ++i)
            if (!r.game_ids[i])
                return false;
    } else if (r.game_ids.count)
        return false;
    return page_valid(r.page, op == Op::audit_log);
}

RestRouteKey guild_admin_route(const GuildAdminRequest& r) noexcept {
    const auto& row = kOperations[static_cast<u32>(r.operation)];
    const bool guild = row.flags & kGuildId;
    const bool channel = row.flags & kChannelId;
    return {kGuildAdminRouteBase + static_cast<u64>(r.operation), guild ? r.guild_id : channel ? r.channel_id : 0,
            row.method,
            guild     ? RestMajorParameter::guild
            : channel ? RestMajorParameter::channel
                      : RestMajorParameter::none};
}

GuildAdminResult guild_admin_write_target(const GuildAdminRequest& r, Slice<u8> output) noexcept {
    if (!byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAdminError::input};
    u8 target[kGuildAdminMaximumTargetBytes];
    GuardedWriter w{{target, sizeof(target)}};
    w.put("/api/v9/");
    const Op op = r.operation;
    if (op == Op::guild_leave) {
        w.put("users/@me/guilds/", r.guild_id);
    } else if (op == Op::identity_adopt) {
        w.put("users/@me/clan");
    } else if (op == Op::invite_delete) {
        w.put("invites/", r.invite_code);
    } else if (op == Op::game_search) {
        char query[kGuildAdminGameQueryCodepoints * 4];
        const Utf8View normalized{query, guild_admin_game_query(r.game_query, {query, sizeof(query)})};
        w.put("games/autocomplete?q=");
        w.ok = w.ok && protocol_writer_append_percent_encoded(&w.writer, normalized);
        w.put("&filter_group=default");
    } else if (op == Op::games_list) {
        w.put("games");
        for (u32 i = 0; i < r.game_ids.count; ++i)
            w.put(i ? "&game_ids=" : "?game_ids=", r.game_ids[i]);
    } else {
        const u32 f = kOperations[static_cast<u32>(op)].flags;
        if (f & kChannelId)
            w.put("channels/", r.channel_id);
        else
            w.put("guilds/", r.guild_id);
        switch (op) {
        case Op::role_list:
        case Op::role_create:
        case Op::role_reorder: w.put("/roles"); break;
        case Op::role_update:
        case Op::role_delete: w.put("/roles/"); break;
        case Op::role_member_counts: w.put("/roles/member-counts"); break;
        case Op::member_update:
        case Op::member_kick:
        case Op::member_role_add:
        case Op::member_role_remove: w.put("/members/"); break;
        case Op::member_update_self: w.put("/members/@me"); break;
        case Op::member_profile_self: w.put("/profile/@me"); break;
        case Op::ban_list: w.put("/bans"); break;
        case Op::ban_add:
        case Op::ban_remove: w.put("/bans/"); break;
        case Op::audit_log: w.put("/audit-logs"); break;
        case Op::channel_create:
        case Op::channel_reorder: w.put("/channels"); break;
        case Op::invite_list_guild:
        case Op::invite_list_channel:
        case Op::invite_create: w.put("/invites"); break;
        case Op::overwrite_put:
        case Op::overwrite_delete: w.put("/permissions/"); break;
        case Op::widget_get:
        case Op::widget_update: w.put("/widget"); break;
        case Op::profile_get:
        case Op::profile_update:
        case Op::tag_update: w.put("/profile"); break;
        case Op::vanity_get:
        case Op::vanity_update: w.put("/vanity-url"); break;
        case Op::top_games: w.put("/top-games"); break;
        default: break;
        }
        if (f & kTargetId)
            w.put(r.target_id);
        if (op == Op::member_role_add || op == Op::member_role_remove)
            w.put("/roles/", r.member->role_ids[0]);
        // A kick's reason is a query parameter (encodeURIComponent), never the audit header; an empty reason still
        // sends "?reason=".
        if (op == Op::member_kick && r.reason.data) {
            w.put("?reason=");
            const Slice<u8> room{target + w.writer.used, kGuildAdminMaximumTargetBytes - w.writer.used};
            const auto encoded = w.ok ? rest_encode_audit_reason(r.reason, room) : RestAuditReasonResult{0, false};
            w.ok = encoded.ok;
            w.writer.used += w.ok ? encoded.bytes : 0;
        }
        if (op == Op::ban_list || op == Op::audit_log) {
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

bool guild_tag_text_valid(Utf8View tag) noexcept {
    if (!text_valid(tag, 2, kGuildAdminTagCharacters, true) || utf8_utf16_units(tag) > kGuildAdminTagCharacters)
        return false;
    for (u32 i = 0; i < tag.bytes; ++i)
        if (static_cast<u8>(tag.data[i]) < 0x80 && !ascii_is_alnum(tag.data[i]))
            return false;
    return true;
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
        if (g.fields & guild_settings_patch_discovery_splash)
            bytes += g.discovery_splash.bytes;
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
void write_civil_time(u64 unix_seconds, const Slice<char>& output) noexcept {
    const auto date = civil_date_from_days(static_cast<i64>(unix_seconds / 86'400));
    const u32 seconds = static_cast<u32>(unix_seconds % 86'400);
    const u32 year = static_cast<u32>(date.year);
    // Two digits each: the century and the year in it, the month, the day, the hour, the minute and the second.
    const u32 fields[]{year / 100, year % 100, date.month, date.day, seconds / 3'600, seconds / 60 % 60, seconds % 60};
    static constexpr u8 at[]{0, 2, 5, 8, 11, 14, 17};
    for (u32 i = 0; i < 7; ++i) {
        output[at[i]] = static_cast<char>('0' + fields[i] / 10 % 10);
        output[at[i] + 1] = static_cast<char>('0' + fields[i] % 10);
    }
    output[4] = output[7] = '-';
    output[10] = 'T';
    output[13] = output[16] = ':';
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

u32 guild_admin_game_query(Utf8View typed, Slice<char> output) noexcept {
    if (!view_valid(typed) || !output.data || !utf8_validate(typed).valid)
        return 0;
    u32 begin = 0, end = typed.bytes;
    const auto space = [](char c) noexcept { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (begin < end && space(typed.data[begin]))
        ++begin;
    while (end > begin && space(typed.data[end - 1]))
        --end;
    // slice(0, 100) of the UTF-16 string, never splitting a character.
    u32 used = 0, units = 0;
    for (u32 i = begin; i < end;) {
        const u8 lead = static_cast<u8>(typed.data[i]);
        const u32 length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const u32 width = length == 4 ? 2 : 1;
        if (units + width > kGuildAdminGameQueryCodepoints || used + length > output.count)
            break;
        for (u32 k = 0; k < length; ++k) {
            char c = typed.data[i + k];
            if (length == 1)
                c = c == '_' ? ' ' : ascii_lower(c);
            output[used++] = c;
        }
        units += width;
        i += length;
    }
    return used;
}

} // namespace maicord
