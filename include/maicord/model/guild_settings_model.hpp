#pragma once

#include "maicord/protocol/guild_admin.hpp"

namespace maicord {
struct ChannelRecord;
struct ChatStateStore;
struct GuildDetailStore;
struct RoleRecord;

// Server Settings: what the current account may open and change in one
// guild, plus the drafts and request bodies of its pages. Pure functions over
// the state cache; the application layer owns transport and presentation.
// Sections visible in the sidebar. Server Profile, Engagement and Boost Perks
// show to anyone who may open Server Settings; editing needs Manage Server.
enum GuildSettingsSection : u32 {
    guild_settings_profile = 1u << 0,
    guild_settings_engagement = 1u << 1,
    // Emoji, Stickers and Soundboard: Manage or Create Expressions.
    guild_settings_expressions = 1u << 2,
    guild_settings_members = 1u << 3,
    guild_settings_roles = 1u << 4,
    guild_settings_invites = 1u << 5,
    // Verification level, content filter and the 2FA requirement.
    guild_settings_safety = 1u << 6,
    guild_settings_audit_log = 1u << 7,
    // Ban Members or View Audit Log; lifting a ban needs Ban Members.
    guild_settings_bans = 1u << 8,
    // Shown like Server Profile; the custom invite link needs Administrator.
    guild_settings_boost_perks = 1u << 9,
};
// Any of these opens Server Settings; Kick and Timeout Members alone do not.
inline constexpr PermissionSet kGuildSettingsOpenPermissions =
    kPermissionManageGuild | kPermissionManageRoles | kPermissionAdministrator | kPermissionBanMembers |
    kPermissionManageNicknames | kPermissionCreateExpressions | kPermissionManageExpressions |
    kPermissionManageWebhooks | kPermissionViewAuditLog | kPermissionViewGuildInsights;
// Any of these lists the Members page.
inline constexpr PermissionSet kGuildSettingsMemberPermissions =
    kPermissionAdministrator | kPermissionManageGuild | kPermissionBanMembers | kPermissionKickMembers |
    kPermissionModerateMembers | kPermissionManageRoles | kPermissionManageNicknames;

struct GuildSettingsAccess {
    u64 guild_id, self_user_id, owner_id;
    // Guild-level permissions after the two-factor gate.
    PermissionSet permissions;
    // Highest assigned role; zero id when the account holds none.
    u64 top_role_id;
    u32 top_position;
    u32 sections;
    bool owner, resolved;
    // Manage Expressions: edit and delete any emoji or sticker. Create
    // Expressions alone only uploads.
    bool manage_expressions;
    // The owner, when the account has 2FA or the server does not require it.
    bool delete_allowed;
};
// Sections a set of guild-level permissions opens.
u32 guild_settings_sections(PermissionSet permissions) noexcept;
bool guild_settings_can_manage_guild(const GuildSettingsAccess&) noexcept;
bool guild_settings_can_unban(const GuildSettingsAccess&) noexcept;
// Owner-only actions: deleting the server and the moderation 2FA requirement.
bool guild_settings_can_delete(const GuildSettingsAccess&) noexcept;
bool guild_settings_can_set_mfa(const GuildSettingsAccess&) noexcept;

GuildSettingsAccess guild_settings_access(const ChatStateStore&, u64 guild_id) noexcept;
// Name, colour, toggles and permissions may change; @everyone keeps its name.
bool guild_settings_role_editable(const GuildSettingsAccess&, const RoleRecord&) noexcept;
bool guild_settings_role_deletable(const GuildSettingsAccess&, const RoleRecord&) noexcept;
// Bits this account may grant to a role or override; the owner and
// administrators may grant any known bit.
PermissionSet guild_settings_grantable(const GuildSettingsAccess&) noexcept;
// Why a role is read-only to this account: no Manage Roles, it is the
// account's highest role, or it ranks above it.
enum class GuildRoleLock : u8 {
    none,
    no_permission,
    highest,
    above,
};
GuildRoleLock guild_settings_role_lock(const GuildSettingsAccess&, const RoleRecord&) noexcept;
// View Server as Role: Manage Server and Manage Roles (the owner and
// administrators hold both), for a role below the account's highest role or
// that role itself; the owner may view as any role.
bool guild_settings_can_view_as_role(const GuildSettingsAccess&, const RoleRecord&) noexcept;
// The account's guild-level permissions (its real roles, two-factor gate
// applied) with one role's permissions replaced by a draft's.
PermissionSet guild_settings_self_permissions_with(const ChatStateStore&, const GuildSettingsAccess&, u64 role_id,
                                                   PermissionSet role_permissions) noexcept;

// Roles of a guild by hierarchy, highest first, @everyone last. Returns the
// number written; rows beyond output are dropped.
u32 guild_settings_sorted_roles(const ChatStateStore&, u64 guild_id,
                                Slice<const RoleRecord*> output) noexcept;
// Moving one row of a highest-first role list (@everyone last and fixed).
// Rejects moves of roles at or above the account's highest role and drops
// below it; returns false and leaves order untouched when not allowed.
bool guild_settings_role_move(const GuildSettingsAccess&, Slice<const RoleRecord*> order, u32 from,
                              u32 to) noexcept;
// Positions for a role reorder request: n..1 down the list, @everyone
// excluded, only rows whose position changes. Returns the count or ~0u when
// output is short.
u32 guild_settings_role_positions(ConstSlice<const RoleRecord*> order, u64 guild_id,
                                  Slice<GuildAdminPosition> output) noexcept;

// Server Profile page draft: the profile as GET /guilds/{g}/profile returned
// it, with the page's edits applied in place. The icon keeps its stored hash
// unless it is replaced (by the page's data URI) or removed.
enum class GuildProfileImageEdit : u8 {
    keep,
    replace,
    remove,
};
struct GuildServerProfileDraft {
    GuildAdminProfile profile;
    GuildProfileImageEdit icon;
};
// The Banner presets after "Server Icon Color": brand_color_primary values.
inline constexpr u32 kGuildProfileBannerColorCount = 9;
extern const Utf8View kGuildProfileBannerColors[kGuildProfileBannerColorCount];
extern const Utf8View kGuildProfileBannerNames[kGuildProfileBannerColorCount];
GuildServerProfileDraft guild_server_profile_draft(const GuildAdminProfile& profile) noexcept;
bool guild_server_profile_equal(const GuildServerProfileDraft&, const GuildServerProfileDraft&) noexcept;
// Server names are 2–100 characters without line breaks.
bool guild_server_name_valid(Utf8View) noexcept;
bool guild_server_description_valid(Utf8View) noexcept;
// "Private Profile": on unless the visibility is public (with or without
// recruitment). Turning it off restores the stored public kind.
bool guild_server_profile_private(const GuildServerProfileDraft&) noexcept;
void guild_server_profile_set_private(GuildServerProfileDraft*, const GuildAdminProfile& stored, bool on) noexcept;
// The selected Banner swatch: 0 for the icon colour, 1 + preset index, or
// ~0u for anything else (an uploaded banner or another colour).
u32 guild_server_profile_banner_choice(const GuildServerProfileDraft&) noexcept;
// Picks a swatch: the preset colour (none for 0) and no uploaded banner.
void guild_server_profile_set_banner_choice(GuildServerProfileDraft*, u32 choice) noexcept;
// The PATCH /guilds/{g}/profile body of a draft. `icon_uri` is the new icon
// when it is replaced. Trait emoji resolve against the guild's custom emoji;
// an unknown custom emoji is left out. `traits` receives the trait views the
// write borrows, as do the draft's strings. False for an invalid draft.
bool guild_server_profile_write(const GuildServerProfileDraft& draft, Utf8View icon_uri, const ChatStateStore& state,
                                u64 guild_id, Slice<GuildServerProfileTraitWrite> traits,
                                GuildServerProfileWrite* write) noexcept;

// Boost Perks draft: the guild PATCH sends all three every time; unchanged
// images resend their stored hash (empty for none).
struct GuildBoostDraft {
    GuildProfileImageEdit banner;
    GuildProfileImageEdit splash;
    bool progress_bar;
};
bool guild_boost_equal(const GuildBoostDraft&, const GuildBoostDraft&) noexcept;
// banner_uri and splash_uri are the replaced images; the stored hashes come
// from the guild record and its details. False when a replaced image has no
// data URI.
bool guild_boost_patch(const GuildBoostDraft& draft, Utf8View banner_hash, Utf8View splash_hash,
                       Utf8View banner_uri, Utf8View splash_uri, GuildSettingsPatch* patch) noexcept;
// Custom invite link input: spaces become dashes and the code stops at 25
// UTF-16 units, as the official field does. Returns the bytes written.
u32 guild_vanity_code_input(Utf8View typed, Slice<char> output) noexcept;
// The message of a rejected vanity change (the error body's code).
Utf8View guild_vanity_error_text(u64 code) noexcept;
inline constexpr u32 kGuildAfkTimeouts[]{60, 300, 900, 1'800, 3'600};

// Channel picker orders: the system message and inactive channel pickers
// list channels as the channel list does (by category); the invite and
// widget picker lists text-like channels, then voice and stage channels,
// each by raw position. Equal keys fall back to ascending ID.
enum class GuildChannelPickerOrder : u8 {
    channel_list,
    invite,
};
bool guild_settings_channel_before(const ChatStateStore&, GuildChannelPickerOrder, const ChannelRecord& a,
                                   const ChannelRecord& b) noexcept;

// system_channel_flags bits; each suppresses one kind of system message.
enum GuildSystemChannelFlag : u32 {
    guild_system_suppress_join = 1u << 0,
    guild_system_suppress_boost = 1u << 1,
    guild_system_suppress_setup_tips = 1u << 2,
    guild_system_suppress_join_replies = 1u << 3,
    guild_system_suppress_subscription = 1u << 4,
    guild_system_suppress_subscription_replies = 1u << 5,
    guild_system_suppress_voice_session = 1u << 9,
};
// Below this member count the activity feed defaults to on.
inline constexpr u32 kGuildActivityFeedDefaultMembers = 10'000;
// From this member count, non-mention messages send no mobile push.
inline constexpr u32 kGuildLargeNotificationMembers = 2'500;

// Engagement page draft. Feature edits have set semantics: a name added is
// appended after the server's names, a name removed leaves the others in
// place.
struct GuildEngagementDraft {
    u64 system_channel_id;
    u64 afk_channel_id;
    u64 widget_channel_id;
    // GuildFeature bits as edited.
    u64 features;
    // Feature bits appended by edits, in the order they were added.
    u64 appended[4];
    u32 afk_timeout_seconds;
    u32 system_channel_flags;
    u8 appended_count;
    u8 default_notifications;
    bool widget_enabled;
};
// Widget fields stay off until loaded (guild_engagement_set_widget).
GuildEngagementDraft guild_engagement_from_state(const ChatStateStore&, const GuildDetailStore&,
                                                 u64 guild_id) noexcept;
void guild_engagement_set_widget(GuildEngagementDraft*, bool enabled, u64 channel_id) noexcept;
bool guild_engagement_equal(const GuildEngagementDraft&, const GuildEngagementDraft&) noexcept;
bool guild_engagement_widget_changed(const GuildEngagementDraft& baseline, const GuildEngagementDraft& draft) noexcept;
// The features set differs, ignoring order.
bool guild_engagement_features_changed(const GuildEngagementDraft& baseline,
                                       const GuildEngagementDraft& draft) noexcept;
// A toggle shows "send" when its suppress bit is clear; other bits survive.
bool guild_engagement_message_enabled(const GuildEngagementDraft&, u32 suppress_bit) noexcept;
void guild_engagement_set_message(GuildEngagementDraft*, u32 suppress_bit, bool send) noexcept;
// Set add/delete on one feature the page edits.
void guild_engagement_add_feature(GuildEngagementDraft*, u64 bit) noexcept;
void guild_engagement_remove_feature(GuildEngagementDraft*, u64 bit) noexcept;
// Explicit choices win; otherwise on below 10,000 known members.
bool guild_engagement_activity_feed(const GuildEngagementDraft&, u32 member_count, bool member_count_known) noexcept;
void guild_engagement_set_activity_feed(GuildEngagementDraft*, bool on) noexcept;
// The guild PATCH: all five scalars, plus the complete features list (the
// stored names with the draft's edits) only when the set changed. Names
// are written to `names`, which the patch borrows. False when the draft is
// invalid, or features changed while the stored names are unavailable.
bool guild_engagement_patch(const GuildEngagementDraft& baseline, const GuildEngagementDraft& draft,
                            ConstSlice<u8> stored_names, u16 stored_count, Slice<u8> names,
                            GuildSettingsPatch* patch) noexcept;
void guild_engagement_widget_patch(const GuildEngagementDraft& draft, GuildSettingsPatch* patch) noexcept;
// Save order: the guild PATCH for any change (the widget included), then
// the widget PATCH when it changed, whether or not the first one succeeded.
enum class GuildEngagementStep : u8 {
    none,
    guild,
    widget,
};
GuildEngagementStep guild_engagement_next_step(const GuildEngagementDraft& baseline,
                                               const GuildEngagementDraft& draft, bool guild_sent) noexcept;

// Role editor draft.
struct GuildRoleDraft {
    u64 role_id;
    PermissionSet permissions;
    u32 color;
    u16 name_bytes;
    bool hoist, mentionable;
    char name[kGuildAdminNameBytes];
};
GuildRoleDraft guild_role_draft_from(const ChatStateStore&, const RoleRecord&) noexcept;
bool guild_role_draft_equal(const GuildRoleDraft&, const GuildRoleDraft&) noexcept;
bool guild_role_name_valid(Utf8View) noexcept;
bool guild_role_patch(const GuildRoleDraft& baseline, const GuildRoleDraft& draft, RolePatch* patch) noexcept;
// Sets or clears one bit when grantable; unknown bits of the role survive.
bool guild_role_toggle(GuildRoleDraft*, const GuildSettingsAccess&, PermissionSet bit, bool enabled) noexcept;
// Why one permission of a role draft cannot change: the account lacks it, or
// clearing it would take it from the account (its own role or @everyone).
enum class GuildPermissionLock : u8 {
    none,
    missing,
    self,
};
GuildPermissionLock guild_role_permission_lock(const ChatStateStore&, const GuildSettingsAccess&,
                                               const GuildRoleDraft&, PermissionSet bit) noexcept;
// Clear permissions: the draft grants something and clearing it all takes
// nothing from the account.
bool guild_role_can_clear(const ChatStateStore&, const GuildSettingsAccess&, const GuildRoleDraft&) noexcept;
// Whether a role list order differs from the stored hierarchy (both highest
// first, @everyone last): the Roles list's unsaved state.
bool guild_settings_order_changed(const ChatStateStore&, u64 guild_id, ConstSlice<u64> order) noexcept;

// Role colours: 0 means none. Hex parsing accepts "#RRGGBB" or "RRGGBB".
inline constexpr u32 kGuildRoleColorCount = 20;
extern const u32 kGuildRoleColors[kGuildRoleColorCount];
bool guild_role_color_parse(Utf8View text, u32* color) noexcept;
u32 guild_role_color_format(u32 color, Slice<char> output) noexcept;
// Packed RGBA for presentation, with a neutral grey for no colour.
u32 guild_role_color_rgba(u32 color) noexcept;

// Boost-tier limits. Emoji slots apply to static and animated emoji
// separately; MORE_EMOJI raises them to 200 and MORE_STICKERS at tier 3 to 120.
u32 guild_emoji_slots(u8 premium_tier, u64 features) noexcept;
u32 guild_sticker_slots(u8 premium_tier, u64 features) noexcept;
bool guild_role_icons_available(u8 premium_tier, u64 features) noexcept;
struct GuildEmojiCounts {
    u32 static_count, animated_count;
};
GuildEmojiCounts guild_emoji_counts(const ChatStateStore&, u64 guild_id) noexcept;
// Emoji name suggested by a file name: extension dropped, other characters
// replaced by '_', clipped to 32 and padded to 2. Returns bytes written.
u32 guild_emoji_name_from_filename(Utf8View filename, Slice<char> output) noexcept;

// Safety page draft. Verification and the content filter save through the
// guild PATCH; the moderation 2FA level has its own owner-only request.
struct GuildSafetyDraft {
    u8 verification_level, explicit_content_filter, mfa_level;
};
GuildSafetyDraft guild_safety_from_state(const ChatStateStore&, u64 guild_id) noexcept;
bool guild_safety_equal(const GuildSafetyDraft&, const GuildSafetyDraft&) noexcept;
bool guild_safety_patch(const GuildSafetyDraft& baseline, const GuildSafetyDraft& draft,
                        GuildSettingsPatch* patch) noexcept;
inline constexpr u32 kGuildVerificationLevels = 5;
inline constexpr u32 kGuildContentFilterLevels = 3;

// Deleting a server asks for its exact name; surrounding spaces are ignored.
bool guild_delete_confirmed(Utf8View typed, Utf8View name) noexcept;

// ASCII case-insensitive substring match; empty query matches everything.
bool guild_settings_text_matches(const Utf8View& query, const Utf8View& text) noexcept;

} // namespace maicord
