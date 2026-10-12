#pragma once

#include "maicord/model/permission_bits.hpp"
#include "maicord/model/voice_state_store.hpp"

namespace maicord {
// Stage channel roles and permissions: suppress + request_to_speak_timestamp
// classify a voice state, and stage moderators hold Manage Channels, Mute
// Members and Move Members together (administrators hold every permission).
enum class StageRole : u8 {
    none,      // Not in the stage.
    audience,  // Suppressed, no raised hand.
    requested, // Suppressed with a raised hand (request_to_speak_timestamp).
    invited,   // Unsuppressed with a timestamp: invited, awaiting the user's answer.
    speaker,   // Unsuppressed, no timestamp: on stage.
};
StageRole stage_role(const VoiceState* state) noexcept;
StageRole stage_role_flags(u32 flags) noexcept;

inline constexpr PermissionSet kStageModeratorPermissions =
    kPermissionManageChannels | kPermissionMuteMembers | kPermissionMoveMembers;
constexpr bool stage_moderator(PermissionSet permissions) noexcept {
    return permission_has(permissions, kStageModeratorPermissions);
}
constexpr bool stage_can_request_to_speak(PermissionSet permissions) noexcept {
    return permission_has(permissions, kPermissionRequestToSpeak);
}

// What the local user can do in a stage now.
enum StageAction : u32 {
    stage_action_raise_hand = 1u << 0,
    stage_action_lower_hand = 1u << 1,
    stage_action_become_speaker = 1u << 2, // Moderators in the audience.
    stage_action_accept_invite = 1u << 3,
    stage_action_decline_invite = 1u << 4,
    stage_action_move_to_audience = 1u << 5,
    stage_action_start = 1u << 6,      // Moderators: no live stage yet.
    stage_action_edit_topic = 1u << 7, // Moderators: live stage.
    stage_action_end = 1u << 8,        // Moderators: live stage.
    stage_action_invite_others = 1u << 9,
    stage_action_move_others = 1u << 10,
};
u32 stage_self_actions(StageRole role, PermissionSet permissions, bool live) noexcept;

// Partition of a stage channel's voice states in join order; hand raises are
// ordered by when they were raised. Each output receives up to its count and
// the totals are returned regardless.
struct StageRoster {
    u32 speakers, audience, requests;
};
StageRoster stage_roster(const VoiceStateStore& store, u64 channel_id, Slice<const VoiceState*> speakers,
                         Slice<const VoiceState*> audience, Slice<const VoiceState*> requests) noexcept;
} // namespace maicord
