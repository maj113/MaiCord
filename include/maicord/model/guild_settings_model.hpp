#pragma once

#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/protocol/guild_admin.hpp"

namespace maicord {

// Server Settings: what the current account may open and change in one
// guild, plus the drafts and request bodies of its pages. Pure functions over
// the state cache; the application layer owns transport and presentation.
enum GuildSettingsSection : u8 {
    guild_settings_overview = 1u << 0,
    guild_settings_roles = 1u << 1,
    guild_settings_members = 1u << 2,
    guild_settings_bans = 1u << 3,
    guild_settings_invites = 1u << 4,
    guild_settings_audit_log = 1u << 5,
    // Emoji and Stickers: Manage or Create Expressions.
    guild_settings_expressions = 1u << 6,
    // Verification level and content filter (Manage Server).
    guild_settings_safety = 1u << 7,
};

struct GuildSettingsAccess {
    u64 guild_id, self_user_id, owner_id;
    // Guild-level permissions after the two-factor gate.
    PermissionSet permissions;
    // Highest assigned role; zero id when the account holds none.
    u64 top_role_id;
    u32 top_position;
    u8 sections;
    bool owner, resolved;
    // Manage Expressions: edit and delete any emoji or sticker. Create
    // Expressions alone only uploads.
    bool manage_expressions;
};
// Owner-only actions: deleting the server and the moderation 2FA requirement.
bool guild_settings_can_delete(const GuildSettingsAccess&) noexcept;
bool guild_settings_can_set_mfa(const GuildSettingsAccess&) noexcept;

GuildSettingsAccess guild_settings_access(const ChatStateStore&, u64 guild_id) noexcept;
// Strictly below the account's highest role; the owner outranks every role.
bool guild_settings_role_below(const GuildSettingsAccess&, const RoleRecord&) noexcept;
// Name, colour, toggles and permissions may change; @everyone keeps its name.
bool guild_settings_role_editable(const GuildSettingsAccess&, const RoleRecord&) noexcept;
bool guild_settings_role_deletable(const GuildSettingsAccess&, const RoleRecord&) noexcept;
// Bits this account may grant to a role or override; the owner and
// administrators may grant any known bit.
PermissionSet guild_settings_grantable(const GuildSettingsAccess&) noexcept;

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

// Overview page draft.
struct GuildOverviewDraft {
    u64 system_channel_id, afk_channel_id;
    u32 afk_timeout_seconds;
    u16 name_bytes;
    u8 default_notifications;
    char name[kGuildAdminNameBytes];
};
GuildOverviewDraft guild_overview_from_state(const ChatStateStore&, u64 guild_id) noexcept;
bool guild_overview_equal(const GuildOverviewDraft&, const GuildOverviewDraft&) noexcept;
// Server names are 2–100 characters without line breaks.
bool guild_overview_name_valid(Utf8View) noexcept;
// Changed fields as a patch borrowing draft text. False when nothing changed
// or the draft is invalid.
bool guild_overview_patch(const GuildOverviewDraft& baseline, const GuildOverviewDraft& draft,
                          GuildSettingsPatch* patch) noexcept;
inline constexpr u32 kGuildAfkTimeouts[]{60, 300, 900, 1'800, 3'600};

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

// Role colours: 0 means none. Hex parsing accepts "#RRGGBB" or "RRGGBB".
inline constexpr u32 kGuildRoleColorCount = 20;
extern const u32 kGuildRoleColors[kGuildRoleColorCount];
bool guild_role_color_parse(Utf8View text, u32* color) noexcept;
u32 guild_role_color_format(u32 color, Slice<char> output) noexcept;
// Packed RGBA for presentation, with a neutral grey for no colour.
u32 guild_role_color_rgba(u32 color) noexcept;

// Boost-tier limits. Emoji slots apply to static and animated emoji
// separately; MORE_EMOJI raises them to 200 and MORE_STICKERS at tier 3 to 120.
u32 guild_emoji_slots(u8 premium_tier, u16 features) noexcept;
u32 guild_sticker_slots(u8 premium_tier, u16 features) noexcept;
bool guild_banner_available(u8 premium_tier, u16 features) noexcept;
// Animated (GIF) banners: boost level 3 or ANIMATED_BANNER.
bool guild_animated_banner_available(u8 premium_tier, u16 features) noexcept;
bool guild_role_icons_available(u8 premium_tier, u16 features) noexcept;
bool guild_animated_icon_available(u8 premium_tier, u16 features) noexcept;
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
bool guild_settings_text_matches(Utf8View query, Utf8View text) noexcept;

} // namespace maicord
