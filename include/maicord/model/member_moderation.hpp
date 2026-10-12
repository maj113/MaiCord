#pragma once

#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_compute.hpp"

namespace maicord {

enum ModerationAction : u8 {
    moderation_action_nickname = 1u << 0,
    moderation_action_timeout = 1u << 1,
    moderation_action_end_timeout = 1u << 2,
    moderation_action_kick = 1u << 3,
    moderation_action_ban = 1u << 4,
};

// Why a held permission cannot be used against this target.
enum class ModerationBlock : u8 {
    none,
    unresolved,      // permissions or the target's roles are not known yet
    target_owner,
    hierarchy,       // the target's highest role is not below ours
    target_admin,    // administrators cannot be timed out
    two_factor,      // guild requires two-factor and this account lacks it
};

// `visible` lists actions backed by a held permission; `allowed` is the
// subset that passes ownership, hierarchy and two-factor gating now.
struct ModerationCapabilities {
    u8 visible;
    u8 allowed;
    ModerationBlock block;
    bool self_target;
    bool target_timed_out;
    // Editing one's own nickname uses the self route.
    bool nickname_self;
};

// Pure resolution over the state and member caches. now_seconds is Unix time.
ModerationCapabilities moderation_capabilities(const ChatStateStore& state,
                                               const GuildMemberStore& members, u64 guild_id,
                                               u64 target_user_id, u64 now_seconds) noexcept;

inline constexpr u32 kModerationTimeoutChoices[] = {60, 300, 600, 3'600, 86'400, 604'800};
inline constexpr u32 kModerationBanDeleteChoices[] = {0, 3'600, 21'600, 43'200, 86'400, 259'200, 604'800};

} // namespace maicord
