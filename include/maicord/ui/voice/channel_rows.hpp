#pragma once
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/model/voice/state_store.hpp"

namespace maicord {
struct UiVoiceParticipantRow {
    const VoiceState* state;
    UiRect row, avatar, label;
    UiRect muted, deafened; // Empty when the indicator is not shown.
    UiRect video;           // Camera on (self_video); opens the call view focused on the user.
    UiRect live;            // Go Live badge (voice_state_live): hover previews, click watches.
    bool speaking;          // Draw the speaking ring around the avatar.
};
inline constexpr float kUiVoiceLiveBadgeWidth = 34, kUiVoiceLiveBadgeHeight = 16;
// The index-th connected user drawn inside a voice channel row; row_rectangle is
// the channel row's layout rectangle. False past the last drawn user.
bool ui_voice_participant_row(const ChannelViewRow& row, const UiRect& row_rectangle,
                              const VoiceStateStore& store, u32 index,
                              UiVoiceParticipantRow* output) noexcept;
// Disconnect control of the voice connection card above the account panel.
UiRect ui_voice_disconnect_rectangle(UiRect footer) noexcept;

// Voice channel extras the channel list borrows (navigation_view.voice_extras).
struct UiVoiceChannelExtras {
    const struct VoiceChannelInfoStore* info; // Statuses.
    u64 status_channel_id; // The connected channel offering "Set a channel status".
    u64 invite_channel_id; // The connected channel showing "Invite to Voice".
    u32 revision;          // Bumped when info, status_channel_id or invite_channel_id change.
    bool status_hovered, invite_hovered, invite_close_hovered;
};

// "Invite to Voice" under the connected channel's users: shown on each new
// connection and dismissed after 10 s of being on screen without the pointer
// over it; hovering or leaving the screen restarts the full 10 s.
inline constexpr u64 kUiVoiceInviteDismissMs = 10'000;
struct UiVoiceInviteCountdown {
    u64 channel_id; // Connected channel; 0 when not connected.
    u64 deadline_ms;
    bool counting, dismissed;
};
// connected_channel_id: the RTC-connected voice channel, or 0. True when the
// row's visibility changed.
bool ui_voice_invite_observe(UiVoiceInviteCountdown* countdown, u64 connected_channel_id) noexcept;
// running: on screen, not hovered, reduced motion off. True when it dismissed the row.
bool ui_voice_invite_tick(UiVoiceInviteCountdown* countdown, u64 now_ms, bool running) noexcept;
bool ui_voice_invite_dismiss(UiVoiceInviteCountdown* countdown) noexcept;
constexpr u64 ui_voice_invite_next_wake(const UiVoiceInviteCountdown& countdown) noexcept {
    return countdown.counting && !countdown.dismissed ? countdown.deadline_ms : 0;
}
constexpr u64 ui_voice_invite_channel(const UiVoiceInviteCountdown& countdown) noexcept {
    return countdown.dismissed ? 0 : countdown.channel_id;
}

// A voice session's length as read aloud: "1 hour, 2 minutes, 3 seconds" (zero parts left out, "0 seconds" for none).
// Returns the bytes written; 0 for a buffer under kUiVoiceDurationBytes.
inline constexpr u32 kUiVoiceDurationBytes = 64;
u32 ui_voice_duration_text(u64 elapsed_s, char* output, u32 capacity) noexcept;

// Status line under a voice channel's name (stage topic geometry) and the
// invite row after its users; row_rectangle is the channel row's layout box.
UiRect ui_voice_status_rectangle(const ChannelViewRow& row, UiRect row_rectangle) noexcept;
struct UiVoiceInviteRow {
    UiRect row, icon, label, close;
};
bool ui_voice_invite_row(const ChannelViewRow& row, const UiRect& row_rectangle, UiVoiceInviteRow* output) noexcept;
} // namespace maicord
