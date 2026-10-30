#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// Guild and channel administration routes. One request describes one REST
// call; the account layer serializes it through guild_admin_write_* and the
// shared DETAILS owner. All views are borrowed for the call only.
enum class GuildAdminOperation : u8 {
    guild_update,        // PATCH  /guilds/{g}
    role_list,           // GET    /guilds/{g}/roles
    role_create,         // POST   /guilds/{g}/roles     {"name","color","colors","permissions"}
    role_update,         // PATCH  /guilds/{g}/roles/{r}
    role_delete,         // DELETE /guilds/{g}/roles/{r}
    role_reorder,        // PATCH  /guilds/{g}/roles
    role_member_counts,  // GET    /guilds/{g}/roles/member-counts
    member_update,       // PATCH  /guilds/{g}/members/{u}  (roles: the full list)
    member_update_self,  // PATCH  /guilds/{g}/members/@me
    member_kick,         // DELETE /guilds/{g}/members/{u}[?reason=]
    ban_list,            // GET    /guilds/{g}/bans?limit=1000[&after=]
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
    widget_get,          // GET    /guilds/{g}/widget
    widget_update,       // PATCH  /guilds/{g}/widget           {"enabled","channel_id"}
    profile_get,         // GET    /guilds/{g}/profile
    profile_update,      // PATCH  /guilds/{g}/profile          (GuildServerProfileWrite)
    vanity_get,          // GET    /guilds/{g}/vanity-url
    vanity_update,       // PATCH  /guilds/{g}/vanity-url       {"code"}
    member_role_add,     // PUT    /guilds/{g}/members/{u}/roles/{r} (no body)
    member_role_remove,  // DELETE /guilds/{g}/members/{u}/roles/{r} (no body)
    count,
};

inline constexpr u64 kGuildAdminRouteBase = 0x4741444D494E0000ull;
// The longest path plus a kick reason at its encoded bound.
inline constexpr u32 kGuildAdminMaximumTargetBytes = 128 + kRestAuditReasonMaximumEncodedBytes;
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
    guild_settings_patch_system_channel_flags = 1u << 9,
    guild_settings_patch_features = 1u << 10,
    // widget_update only, and alone: enabled and channel_id.
    guild_settings_patch_widget = 1u << 11,
    guild_settings_patch_splash = 1u << 12,
    guild_settings_patch_progress_bar = 1u << 13,
    // profile_update only, and alone: the server profile.
    guild_settings_patch_profile = 1u << 14,
    // vanity_update only, and alone: the code.
    guild_settings_patch_vanity = 1u << 15,
};

inline constexpr u32 kGuildAdminProfileDescriptionCodepoints = 300;
inline constexpr u32 kGuildAdminProfileDescriptionBytes = kGuildAdminProfileDescriptionCodepoints * 4;
inline constexpr u32 kGuildAdminProfileTraits = 5;
inline constexpr u32 kGuildAdminProfileTraitCodepoints = 24;
inline constexpr u32 kGuildAdminProfileTraitBytes = kGuildAdminProfileTraitCodepoints * 4;
inline constexpr u32 kGuildAdminProfileEmojiNameBytes = 64;
inline constexpr u32 kGuildAdminProfileGames = 32;
inline constexpr u32 kGuildAdminBrandColorBytes = 16;
inline constexpr u32 kGuildAdminVanityCodeCharacters = 25;
inline constexpr u32 kGuildAdminVanityCodeBytes = kGuildAdminVanityCodeCharacters * 4;

// One trait of the server profile. An empty label drops the trait; emoji
// keys are sent only when set (a zero id or an empty name leaves it out).
struct GuildServerProfileTraitWrite {
    Utf8View label;
    Utf8View emoji_name;
    u64 emoji_id;
    u8 position;
    bool emoji_animated;
    bool emoji_animated_set;
};

enum GuildServerProfileWriteField : u8 {
    guild_profile_write_name = 1u << 0,
    guild_profile_write_icon = 1u << 1,
    guild_profile_write_custom_banner = 1u << 2,
    guild_profile_write_visibility = 1u << 3,
    guild_profile_write_all = (1u << 4) - 1,
};

// PATCH /guilds/{g}/profile: name, description, icon, custom_banner,
// visibility, brand_color_primary, traits, game_application_ids, in that
// order. The description, brand colour, traits and games are always sent;
// the others only with their field bit. The icon and custom banner are a
// hash, a data URI or empty for null; an empty brand colour sends null.
struct GuildServerProfileWrite {
    Utf8View name;
    Utf8View description;
    Utf8View icon;
    Utf8View custom_banner;
    Utf8View brand_color_primary;
    ConstSlice<GuildServerProfileTraitWrite> traits;
    ConstSlice<u64> game_application_ids;
    u8 visibility;
    u8 fields;
};

// Image fields carry a data URI or the stored image hash; an empty view with
// the field set clears the image. Channel IDs of zero serialize as null. The
// guild PATCH writes keys in one fixed order whatever the set fields.
// Features are the complete list, each name one length byte and its bytes,
// in the order sent.
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
    u32 system_channel_flags;
    ConstSlice<u8> feature_names;
    u16 feature_count;
    bool widget_enabled;
    u64 widget_channel_id;
    Utf8View splash;
    bool premium_progress_bar_enabled;
    // The original files of a new banner and splash: the account layer sends
    // their MD5s as X-Discord-Original-MD5. Empty sends no entry.
    ConstSlice<u8> banner_original;
    ConstSlice<u8> splash_original;
    const GuildServerProfileWrite* profile;
    // vanity_update: the code; empty clears it ("").
    Utf8View vanity_code;
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
// role_create takes only name, color and permissions (or no patch) and always
// sends the full body: unset fields default to "new role", colour 0 and no
// permissions.
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

// Empty nickname resets it (null for another member, "" for oneself). A zero
// timeout clears it; otherwise it is a Unix time in
// milliseconds, sent as YYYY-MM-DDTHH:MM:SS.mmmZ. Roles replace the member's
// whole list (guild_admin_member_roles_toggle). member_role_add/remove instead
// require only member_patch_roles and exactly one role ID, carried in the URL.
// A zero voice channel
// disconnects the member. The avatar is a complete data URI; empty removes
// the per-server avatar (null).
struct MemberPatch {
    Utf8View nickname;
    Utf8View avatar;
    ConstSlice<u64> role_ids;
    u64 communication_disabled_until_ms;
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
    // Recorded in the audit log for mutations: the X-Audit-Log-Reason header,
    // or the reason query parameter for a kick. A null view (no data) sends
    // nothing, while any other view is sent even when empty (an empty header
    // value, or "?reason=").
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
// The writers below, the reason header and the image sizes take a request that passed this; they do not check it again.
bool guild_admin_request_valid(const GuildAdminRequest& request) noexcept;
// Rate identity: one template per operation, guild- or channel-major.
RestRouteKey guild_admin_route(const GuildAdminRequest& request) noexcept;
GuildAdminResult guild_admin_write_target(const GuildAdminRequest& request, Slice<u8> output) noexcept;
// Zero bytes for bodyless operations. Partial output is cleared on failure.
GuildAdminResult guild_admin_write_body(const GuildAdminRequest& request, Slice<u8> output) noexcept;
// Percent-encoded X-Audit-Log-Reason value; zero bytes when none, when the
// reason is empty, and for a kick, whose reason travels in the target.
GuildAdminResult guild_admin_write_reason(const GuildAdminRequest& request, Slice<u8> output) noexcept;
// Whether the request carries the X-Audit-Log-Reason header, possibly with an
// empty value: a reason view with data on any operation but a kick.
bool guild_admin_reason_header(const GuildAdminRequest& request) noexcept;
// Bytes of the data URIs the body carries (member avatar, profile banner,
// server images).
u32 guild_admin_image_bytes(const GuildAdminRequest& request) noexcept;
// X-Discord-Original-MD5 for guild images: `key="md5"` entries sorted by key
// (guild_banner, then guild_invite_splash) and joined by ", ". Values that
// are not 32 lowercase hex characters are skipped; zero bytes when none is
// left or the output is short.
u32 guild_admin_write_md5_header(Utf8View banner_md5, Utf8View splash_md5, Slice<u8> output) noexcept;
// Lowercase hex of a 16-byte digest into output[0..32).
void guild_admin_md5_hex(const u8 (&digest)[16], char (&output)[32]) noexcept;
// ISO 8601 UTC with an explicit offset (YYYY-MM-DDTHH:MM:SS+00:00).
u32 guild_admin_format_timestamp(u64 unix_seconds, Slice<char> output) noexcept;
// Date.prototype.toISOString's form, YYYY-MM-DDTHH:MM:SS.mmmZ (24 bytes), as
// the member timeout field takes it. Zero for short output or years past 9999.
u32 guild_admin_format_timestamp_ms(u64 unix_milliseconds, Slice<char> output) noexcept;
// The member's role list after one role toggle, as the member PATCH sends
// it: the current IDs in their current order, with an
// added role appended unless already held, or a removed role filtered out.
// Returns the count, or ~0u for a zero role, invalid views or short output.
u32 guild_admin_member_roles_toggle(ConstSlice<u64> current, u64 role_id, bool add,
                                    Slice<u64> output) noexcept;

// Response models. Strings are owned, not NUL-terminated; zero lengths mean
// absent or null. Decoders tolerate unknown fields and refuse duplicates of
// known ones. Single-object outputs are untouched on failure; list slices hold
// unspecified rows after a failure and only `count` rows after success.
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

// GET /guilds/{g}/widget: a null channel decodes as zero.
struct GuildAdminWidget {
    u64 channel_id;
    bool enabled;
};
GuildAdminResult guild_admin_decode_widget(Utf8View body, Slice<u32> scratch, GuildAdminWidget* output) noexcept;
// GET /guilds/{g}/profile (and the PATCH reply), as the profile save resends
// it. Visibility: 0 not specified, 1 public, 2 restricted, 3 public with
// recruitment. Traits are indexed by position; unused slots have no label.
// A missing description reads as empty and an empty brand colour as none.
struct GuildAdminProfileTrait {
    u64 emoji_id;
    u8 label_bytes;
    u8 emoji_name_bytes;
    char label[kGuildAdminProfileTraitBytes];
    char emoji_name[kGuildAdminProfileEmojiNameBytes];
};
struct GuildAdminProfile {
    u64 game_application_ids[kGuildAdminProfileGames];
    GuildAdminProfileTrait traits[kGuildAdminProfileTraits];
    u16 name_bytes;
    u16 description_bytes;
    u8 icon_bytes;
    u8 custom_banner_bytes;
    u8 brand_color_bytes;
    u8 game_count;
    u8 visibility;
    bool visibility_known;
    bool name_known;
    // The key was present (a null value included).
    bool icon_known;
    bool custom_banner_known;
    // DISCOVERABLE is among the profile's features.
    bool discoverable;
    char name[kGuildAdminNameBytes];
    char description[kGuildAdminProfileDescriptionBytes];
    char icon[kGuildAdminHashBytes];
    char custom_banner[kGuildAdminHashBytes];
    char brand_color[kGuildAdminBrandColorBytes];
};
GuildAdminResult guild_admin_decode_profile(Utf8View body, Slice<u32> scratch, GuildAdminProfile* output) noexcept;
// GET or PATCH /guilds/{g}/vanity-url: the code (null as empty) and uses.
struct GuildAdminVanity {
    u32 uses;
    u8 code_bytes;
    char code[kGuildAdminVanityCodeBytes];
};
GuildAdminResult guild_admin_decode_vanity(Utf8View body, Slice<u32> scratch, GuildAdminVanity* output) noexcept;
GuildAdminResult guild_admin_decode_role(Utf8View body, Slice<u32> scratch, GuildAdminRole* output) noexcept;
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
