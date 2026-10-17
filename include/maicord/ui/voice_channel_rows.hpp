#pragma once
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/model/voice_state_store.hpp"

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
bool ui_voice_participant_row(const ChannelViewRow& row, UiRect row_rectangle,
                              const VoiceStateStore& store, u32 index,
                              UiVoiceParticipantRow* output) noexcept;
// Disconnect control of the voice connection card above the account panel.
UiRect ui_voice_disconnect_rectangle(UiRect footer) noexcept;
} // namespace maicord
