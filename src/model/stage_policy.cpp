#include "maicord/model/stage_policy.hpp"

namespace maicord {
StageRole stage_role_flags(u32 flags) noexcept {
    const bool suppressed = (flags & voice_suppress) != 0;
    const bool hand = (flags & voice_request_to_speak) != 0;
    if (suppressed)
        return hand ? StageRole::requested : StageRole::audience;
    return hand ? StageRole::invited : StageRole::speaker;
}

StageRole stage_role(const VoiceState* state) noexcept {
    return state ? stage_role_flags(state->flags) : StageRole::none;
}

u32 stage_self_actions(StageRole role, PermissionSet permissions, bool live) noexcept {
    const bool moderator = stage_moderator(permissions);
    u32 actions = 0;
    if (moderator) {
        actions |= live ? stage_action_edit_topic | stage_action_end : stage_action_start;
        actions |= stage_action_invite_others | stage_action_move_others;
    }
    switch (role) {
    case StageRole::audience:
        if (moderator)
            actions |= stage_action_become_speaker;
        else if (live && stage_can_request_to_speak(permissions))
            actions |= stage_action_raise_hand;
        break;
    case StageRole::requested:
        actions |= stage_action_lower_hand;
        if (moderator)
            actions |= stage_action_become_speaker;
        break;
    case StageRole::invited:
        actions |= stage_action_accept_invite | stage_action_decline_invite;
        break;
    case StageRole::speaker:
        actions |= stage_action_move_to_audience;
        break;
    default:
        // Outside the stage nothing applies to the local user.
        actions = 0;
        break;
    }
    return actions;
}

StageRoster stage_roster(const VoiceStateStore& store, u64 channel_id, Slice<const VoiceState*> speakers,
                         Slice<const VoiceState*> audience, Slice<const VoiceState*> requests) noexcept {
    StageRoster roster{};
    const u32 count = voice_state_store_channel_count(store, channel_id);
    for (u32 i = 0; i < count; ++i) {
        const auto* state = voice_state_store_channel_at(store, channel_id, i);
        if (!state)
            break;
        const auto role = stage_role(state);
        if (role == StageRole::speaker) {
            if (roster.speakers < speakers.count)
                speakers[roster.speakers] = state;
            ++roster.speakers;
            continue;
        }
        if (roster.audience < audience.count)
            audience[roster.audience] = state;
        ++roster.audience;
        if (role != StageRole::requested)
            continue;
        // Insertion keeps hand raises ordered by time (join order on ties).
        u32 at = minimum(roster.requests, requests.count);
        if (requests.count) {
            while (at > 0 && requests[at - 1]->request_to_speak_s > state->request_to_speak_s)
                --at;
            if (at < requests.count) {
                const u32 last = minimum(roster.requests, requests.count - 1);
                for (u32 k = last; k > at; --k)
                    requests[k] = requests[k - 1];
                requests[at] = state;
            }
        }
        ++roster.requests;
    }
    return roster;
}
} // namespace maicord
