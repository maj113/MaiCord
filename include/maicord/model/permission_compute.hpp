#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {

// One member as seen by permission resolution. Role IDs exclude @everyone;
// unknown IDs are ignored. All views are borrowed for the call only.
struct PermissionMember {
    u64 user_id;
    ConstSlice<u64> role_ids;
    // Unix seconds; zero or past values mean no timeout.
    u64 communication_disabled_until;
    // Indefinite restriction from the member's active AutoMod flags.
    bool quarantined = false;
};

// Guild role table may contain other guilds' rows; they are filtered by ID.
struct PermissionGuild {
    u64 guild_id;
    u64 owner_id;
    ConstSlice<RoleRecord> roles;
    // Guild moderation requires two-factor authentication (mfa_level 1).
    bool elevated_requires_mfa;
};

struct PermissionOptions {
    // Zero disables timeout masking (no clock available).
    u64 now_seconds;
    // The member is known not to have two-factor authentication enabled.
    bool member_lacks_mfa;
};

// @everyone plus assigned roles; owner and administrator resolve to every bit.
// No masks are applied.
PermissionSet permission_compute_base(const PermissionGuild& guild, const PermissionMember& member) noexcept;
// The same with one role's permissions replaced (an unsaved role draft).
PermissionSet permission_compute_base_with(const PermissionGuild& guild, const PermissionMember& member,
                                           u64 role_id, PermissionSet role_permissions) noexcept;
// Complete resolution from permission_compute_base for the same guild and member.
// Keep this base before the timeout and MFA masks. Reuse it only while roles and ownership stay unchanged.
PermissionSet permission_compute_from_base(PermissionSet base, const PermissionGuild& guild,
                                           const PermissionMember& member,
                                           ConstSlice<PermissionOverwriteRecord> overwrites,
                                           const PermissionOptions& options) noexcept;
// Full resolution. An empty overwrite slice yields guild-level permissions.
// Timeout masking leaves view/read history; quarantine also permits changing
// one's nickname. Owners and administrators are exempt from both masks.
// The two-factor gate strips elevated bits for everyone, including the owner.
PermissionSet permission_compute(const PermissionGuild& guild, const PermissionMember& member,
                                 ConstSlice<PermissionOverwriteRecord> overwrites,
                                 const PermissionOptions& options) noexcept;

// Store-backed helpers. Channel zero resolves guild-level permissions; a
// thread resolves through its parent. Missing guild/channel yields zero.
PermissionGuild chat_state_store_permission_guild(const ChatStateStore& store, u64 guild_id) noexcept;
PermissionSet chat_state_store_member_permissions(const ChatStateStore& store, u64 guild_id,
                                                     u64 channel_id, const PermissionMember& member,
                                                     const PermissionOptions& options) noexcept;
// The current account lacks two-factor authentication and the guild requires it.
bool chat_state_store_self_mfa_blocked(const ChatStateStore& store, u64 guild_id) noexcept;
// View server as role: the account's permissions in one guild resolve from
// the viewed roles and @everyone instead of its own, without the owner's
// exemption, and every channel's permissions and visibility follow. Local
// only; nothing is sent. Unknown or other guilds' ids are ignored and an
// empty set views as @everyone. Both recompute the guild and return false
// when the guild or its @everyone role is missing. Role records carry the
// view, so a deleted role drops out and a replaced role table ends it.
bool chat_state_store_view_as_roles(ChatStateStore* store, u64 guild_id, ConstSlice<u64> role_ids) noexcept;
bool chat_state_store_end_view_as(ChatStateStore* store, u64 guild_id) noexcept;
bool chat_state_store_viewing_as(const ChatStateStore& store, u64 guild_id) noexcept;
// Viewed roles of an active view, @everyone excluded; returns the count.
u32 chat_state_store_view_roles(const ChatStateStore& store, u64 guild_id, Slice<u64> output) noexcept;
// Highest assigned role by service hierarchy; null when the member has none.
const RoleRecord* chat_state_store_highest_role(const ChatStateStore& store, u64 guild_id,
                                                          ConstSlice<u64> role_ids) noexcept;

} // namespace maicord
