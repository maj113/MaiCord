#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// Guild and channel administration routes. One request describes one REST
// call; the account layer serializes it through guild_admin_write_* and the
// shared DETAILS owner. All views are borrowed for the call only.
enum class GuildAdminOperation : u8 {
    guild_fetch,         // GET    /guilds/{g}
    guild_update,        // PATCH  /guilds/{g}
    role_list,           // GET    /guilds/{g}/roles
    role_create,         // POST   /guilds/{g}/roles
    role_update,         // PATCH  /guilds/{g}/roles/{r}
    role_delete,         // DELETE /guilds/{g}/roles/{r}
    role_reorder,        // PATCH  /guilds/{g}/roles
    role_member_counts,  // GET    /guilds/{g}/roles/member-counts
    member_update,       // PATCH  /guilds/{g}/members/{u}
    member_update_self,  // PATCH  /guilds/{g}/members/@me
    member_kick,         // DELETE /guilds/{g}/members/{u}
    ban_list,            // GET    /guilds/{g}/bans
    ban_add,             // PUT    /guilds/{g}/bans/{u}
    ban_remove,          // DELETE /guilds/{g}/bans/{u}
    audit_log,           // GET    /guilds/{g}/audit-logs
    channel_create,      // POST   /guilds/{g}/channels
    channel_update,      // PATCH  /channels/{c}
    channel_delete,      // DELETE /channels/{c}
    channel_reorder,     // PATCH  /guilds/{g}/channels
    overwrite_put,       // PUT    /channels/{c}/permissions/{o}
    overwrite_delete,    // DELETE /channels/{c}/permissions/{o}
    invite_list_guild,   // GET    /guilds/{g}/invites
    invite_list_channel, // GET    /channels/{c}/invites
    invite_create,       // POST   /channels/{c}/invites
    invite_delete,       // DELETE /invites/{code}
    guild_leave,         // DELETE /users/@me/guilds/{g}  {"lurking":false}
    member_profile_self, // PATCH  /guilds/{g}/profile/@me    {"pronouns","bio","banner"}
    count,
};

inline constexpr u64 kGuildAdminRouteBase = 0x4741444D494E0000ull;
inline constexpr u32 kGuildAdminMaximumTargetBytes = 192;
inline constexpr u32 kGuildAdminNameCodepoints = 100;
inline constexpr u32 kGuildAdminNameBytes = kGuildAdminNameCodepoints * 4;
inline constexpr u32 kGuildAdminNicknameCodepoints = 32;
// Per-server profile text limits (the guild member profile).
inline constexpr u32 kGuildAdminPronounsCodepoints = 40;
inline constexpr u32 kGuildAdminBioCodepoints = 190;
inline constexpr u32 kGuildAdminTopicCodepoints = 1'024;
inline constexpr u32 kGuildAdminReasonBytes = kRestAuditReasonMaximumBytes;
inline constexpr u32 kGuildAdminHashBytes = 34;
inline constexpr u32 kGuildAdminEmojiBytes = 32;
inline constexpr u32 kGuildAdminUsernameBytes = 128;
inline constexpr u32 kGuildAdminInviteCodeBytes = 32;
inline constexpr u32 kGuildAdminMaximumPositions = 512;
inline constexpr u32 kGuildAdminMaximumOverwrites = 128;
inline constexpr u32 kGuildAdminMaximumMemberRoles = 256;
inline constexpr u32 kGuildAdminBanDeleteMaximumSeconds = 604'800;
inline constexpr u32 kGuildAdminPageMaximum = 1'000;
inline constexpr u32 kGuildAdminAuditPageMaximum = 100;

enum GuildSettingsPatchField : u16 {
    guild_settings_patch_name = 1u << 0,
    guild_settings_patch_icon = 1u << 1,
    guild_settings_patch_banner = 1u << 2,
    guild_settings_patch_system_channel = 1u << 3,
    guild_settings_patch_afk_channel = 1u << 4,
    guild_settings_patch_afk_timeout = 1u << 5,
    guild_settings_patch_verification_level = 1u << 6,
    guild_settings_patch_default_notifications = 1u << 7,
    guild_settings_patch_explicit_content_filter = 1u << 8,
    guild_settings_patch_all = (1u << 9) - 1,
};

// Image fields carry a complete data URI; an empty view with the field set
// clears the image. Channel IDs of zero serialize as null.
struct GuildSettingsPatch {
    Utf8View name;
    Utf8View icon;
    Utf8View banner;
    u64 system_channel_id;
    u64 afk_channel_id;
    u32 afk_timeout_seconds;
    u16 fields;
    u8 verification_level;
    u8 default_message_notifications;
    u8 explicit_content_filter;
};

enum RolePatchField : u8 {
    role_patch_name = 1u << 0,
    role_patch_permissions = 1u << 1,
    role_patch_color = 1u << 2,
    role_patch_hoist = 1u << 3,
    role_patch_mentionable = 1u << 4,
    role_patch_icon = 1u << 5,
    role_patch_unicode_emoji = 1u << 6,
    role_patch_all = (1u << 7) - 1,
};

// Icon is a data URI and the emoji a short UTF-8 sequence; empty clears.
struct RolePatch {
    Utf8View name;
    Utf8View icon;
    Utf8View unicode_emoji;
    PermissionSet permissions;
    u32 color;
    u8 fields;
    bool hoist;
    bool mentionable;
};

enum MemberPatchField : u8 {
    member_patch_nickname = 1u << 0,
    member_patch_roles = 1u << 1,
    member_patch_timeout = 1u << 2,
    member_patch_voice_channel = 1u << 3,
    member_patch_mute = 1u << 4,
    member_patch_deafen = 1u << 5,
    // Self only: the per-server avatar.
    member_patch_avatar = 1u << 6,
    member_patch_all = (1u << 7) - 1,
};

// Empty nickname resets it (null for another member, "" for oneself, as the
// official client sends). A zero timeout clears it; otherwise it is a Unix time in
// seconds. A zero voice channel disconnects the member. The avatar is a
// complete data URI; empty removes the per-server avatar (null).
struct MemberPatch {
    Utf8View nickname;
    Utf8View avatar;
    ConstSlice<u64> role_ids;
    u64 communication_disabled_until;
    u64 voice_channel_id;
    u8 fields;
    bool mute;
    bool deafen;
};

enum ChannelPatchField : u16 {
    channel_patch_name = 1u << 0,
    channel_patch_type = 1u << 1,
    channel_patch_topic = 1u << 2,
    channel_patch_parent = 1u << 3,
    channel_patch_position = 1u << 4,
    channel_patch_nsfw = 1u << 5,
    channel_patch_slowmode = 1u << 6,
    channel_patch_bitrate = 1u << 7,
    channel_patch_user_limit = 1u << 8,
    channel_patch_overwrites = 1u << 9,
    channel_patch_all = (1u << 10) - 1,
};

enum GuildProfilePatchField : u8 {
    guild_profile_patch_pronouns = 1u << 0,
    guild_profile_patch_bio = 1u << 1,
    guild_profile_patch_banner = 1u << 2,
    guild_profile_patch_all = (1u << 3) - 1,
};

// Per-server profile (member_profile_self). Empty pronouns or bio send ""
// (the account value shows instead); the banner is a complete data URI and
// empty removes it (null).
struct GuildProfilePatch {
    Utf8View pronouns;
    Utf8View bio;
    Utf8View banner;
    u8 fields;
};

struct PermissionOverwriteWrite {
    u64 id;
    PermissionSet allow;
    PermissionSet deny;
    // 0 role, 1 member.
    u8 type;
};

// Create requires a name. A zero parent serializes as null.
struct ChannelPatch {
    Utf8View name;
    Utf8View topic;
    ConstSlice<PermissionOverwriteWrite> overwrites;
    u64 parent_id;
    u32 position;
    u32 slowmode_seconds;
    u32 bitrate;
    u32 user_limit;
    u16 fields;
    u8 type;
    bool nsfw;
};

enum GuildAdminPositionFlag : u8 {
    guild_admin_position_parent = 1u << 0,
    guild_admin_position_lock_permissions = 1u << 1,
};

// Role reorder uses id/position only; channel reorder may move parents.
struct GuildAdminPosition {
    u64 id;
    u64 parent_id;
    u32 position;
    u8 flags;
};

struct InviteCreate {
    u32 max_age_seconds;
    u32 max_uses;
    bool temporary;
    bool unique;
};

struct GuildAdminPageQuery {
    u64 before;
    u64 after;
    u64 user_id;
    u32 limit;
    u16 action_type;
    bool action_type_set;
};

struct GuildAdminRequest {
    GuildAdminOperation operation;
    u64 guild_id;
    u64 channel_id;
    // Role, user or overwrite target depending on the operation.
    u64 target_id;
    // Recorded in the audit log for mutations; empty sends no header.
    Utf8View reason;
    Utf8View invite_code;
    const GuildSettingsPatch* guild;
    const RolePatch* role;
    const MemberPatch* member;
    const ChannelPatch* channel;
    const PermissionOverwriteWrite* overwrite;
    const InviteCreate* invite;
    const GuildAdminPageQuery* page;
    ConstSlice<GuildAdminPosition> positions;
    u32 delete_message_seconds;
    const GuildProfilePatch* profile;
};

enum class GuildAdminError : u8 {
    none,
    input,
    capacity,
    json,
    duplicate,
    field,
    identity,
};

struct GuildAdminResult {
    u32 bytes;
    u32 count;
    GuildAdminError error;
    constexpr bool ok() const noexcept {
        return error == GuildAdminError::none;
    }
};

RestMethod guild_admin_method(GuildAdminOperation operation) noexcept;
bool guild_admin_has_body(GuildAdminOperation operation) noexcept;
bool guild_admin_request_valid(const GuildAdminRequest& request) noexcept;
// Rate identity: one template per operation, guild- or channel-major.
RestRouteKey guild_admin_route(const GuildAdminRequest& request) noexcept;
GuildAdminResult guild_admin_write_target(const GuildAdminRequest& request, Slice<u8> output) noexcept;
// Zero bytes for bodyless operations. Partial output is cleared on failure.
GuildAdminResult guild_admin_write_body(const GuildAdminRequest& request, Slice<u8> output) noexcept;
// Percent-encoded audit reason for the request; zero bytes when none.
GuildAdminResult guild_admin_write_reason(const GuildAdminRequest& request, Slice<u8> output) noexcept;
// Bytes of the data URIs the body carries (member avatar, profile banner).
u32 guild_admin_image_bytes(const GuildAdminRequest& request) noexcept;
// ISO 8601 UTC with an explicit offset, as the member timeout field expects.
u32 guild_admin_format_timestamp(u64 unix_seconds, Slice<char> output) noexcept;

// Response models. Strings are owned, not NUL-terminated; zero lengths mean
// absent or null. Decoders tolerate unknown fields and refuse duplicates of
// known ones. Single-object outputs are untouched on failure; list slices hold
// unspecified rows after a failure and only `count` rows after success.
struct GuildSettingsSnapshot {
    u64 id;
    u64 owner_id;
    u64 system_channel_id;
    u64 afk_channel_id;
    u32 afk_timeout_seconds;
    u16 name_bytes;
    u8 icon_bytes;
    u8 banner_bytes;
    u8 mfa_level;
    u8 verification_level;
    u8 default_message_notifications;
    u8 explicit_content_filter;
    char name[kGuildAdminNameBytes];
    char icon[kGuildAdminHashBytes];
    char banner[kGuildAdminHashBytes];
    // guild_feature_* flags from "features"; unknown names are ignored.
    u16 features;
    u8 premium_tier;
};

// Features that change what Server Settings may upload.
enum GuildFeatureFlag : u16 {
    guild_feature_more_emoji = 1u << 0,
    guild_feature_more_stickers = 1u << 1,
    guild_feature_role_icons = 1u << 2,
    guild_feature_banner = 1u << 3,
    guild_feature_animated_icon = 1u << 4,
    guild_feature_animated_banner = 1u << 5,
    guild_feature_community = 1u << 6,
};

struct GuildAdminRole {
    u64 id;
    PermissionSet permissions;
    u32 position;
    u32 color;
    u32 flags;
    u16 name_bytes;
    u8 icon_bytes;
    u8 emoji_bytes;
    bool hoist;
    bool managed;
    bool mentionable;
    char name[kGuildAdminNameBytes];
    char icon[kGuildAdminHashBytes];
    char emoji[kGuildAdminEmojiBytes];
};

struct GuildAdminRoleCount {
    u64 role_id;
    u32 members;
};

struct GuildAdminUser {
    u64 id;
    u16 username_bytes;
    u16 display_bytes;
    // Legacy four-digit discriminator; zero for migrated usernames ("0").
    u16 discriminator;
    u8 avatar_bytes;
    char username[kGuildAdminUsernameBytes];
    char display[kGuildAdminUsernameBytes];
    // Avatar hash ("a_" prefix when animated); empty for the default avatar.
    char avatar[kGuildAdminHashBytes];
};

struct GuildAdminBan {
    GuildAdminUser user;
    u16 reason_bytes;
    char reason[kGuildAdminReasonBytes];
};

struct GuildAdminAuditEntry {
    u64 id;
    u64 user_id;
    u64 target_id;
    u16 action_type;
    u16 reason_bytes;
    char reason[kGuildAdminReasonBytes];
};

struct GuildAdminAuditPage {
    Slice<GuildAdminAuditEntry> entries;
    Slice<GuildAdminUser> users;
    u32 entry_count;
    u32 user_count;
};

struct GuildAdminInvite {
    u64 channel_id;
    u64 inviter_id;
    // Unix milliseconds; zero means never.
    u64 expires_at_ms;
    u32 uses;
    u32 max_uses;
    u32 max_age_seconds;
    u8 code_bytes;
    bool temporary;
    char code[kGuildAdminInviteCodeBytes];
};

GuildAdminResult guild_admin_decode_settings(Utf8View body, Slice<u32> scratch,
                                             GuildSettingsSnapshot* output) noexcept;
GuildAdminResult guild_admin_decode_role(Utf8View body, Slice<u32> scratch, GuildAdminRole* output) noexcept;
GuildAdminResult guild_admin_decode_roles(Utf8View body, Slice<u32> scratch, Slice<GuildAdminRole> output) noexcept;
GuildAdminResult guild_admin_decode_role_counts(Utf8View body, Slice<u32> scratch,
                                                Slice<GuildAdminRoleCount> output) noexcept;
GuildAdminResult guild_admin_decode_bans(Utf8View body, Slice<u32> scratch, Slice<GuildAdminBan> output) noexcept;
// Entries and users are both bounded by the page slices; count is entries.
GuildAdminResult guild_admin_decode_audit_log(Utf8View body, Slice<u32> scratch, GuildAdminAuditPage* page) noexcept;
GuildAdminResult guild_admin_decode_invites(Utf8View body, Slice<u32> scratch,
                                            Slice<GuildAdminInvite> output) noexcept;
// One invite object (the invite_create response).
GuildAdminResult guild_admin_decode_invite(Utf8View body, Slice<u32> scratch, GuildAdminInvite* output) noexcept;

static_assert(sizeof(GuildAdminRequest) == 152);

} // namespace maicord
