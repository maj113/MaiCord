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

// Elapsed time of the local voice connection, right-aligned on the connected
// channel's row. Times are monotonic milliseconds.
inline constexpr u32 kUiVoiceElapsedBytes = 16;
struct UiVoiceElapsedClock {
    u64 channel_id;   // Connected channel; 0 when not in voice.
    u64 started_ms;   // 0 until the connection is established.
    u64 shown_second; // Elapsed second of label.
    bool visible;     // Row on screen in a shown window at the last tick.
    u8 label_bytes;
    char label[kUiVoiceElapsedBytes];
};
// "m:ss" under an hour, then "h:mm:ss". Returns the bytes written (capacity
// kUiVoiceElapsedBytes suffices).
u32 ui_voice_elapsed_format(u64 elapsed_ms, char* output, u32 capacity) noexcept;
// Follows the connection: established in a new channel starts the clock,
// leaving voice clears it. True when the label changed.
bool ui_voice_elapsed_observe(UiVoiceElapsedClock* clock, u64 channel_id, bool established, u64 now_ms) noexcept;
// Advances the label; visible is the row on screen and the window shown.
// True when a visible label changed (one repaint per displayed second).
bool ui_voice_elapsed_tick(UiVoiceElapsedClock* clock, u64 now_ms, bool visible) noexcept;
// Next second boundary while running and visible, else 0 (no wake).
u64 ui_voice_elapsed_next_wake(const UiVoiceElapsedClock& clock) noexcept;
// Timer text box at the right end of a voice row head, from the row's label
// content box; *name_right receives where the channel name must end.
// Subtext digits are tabular, so the box only grows with the digit count.
UiRect ui_voice_elapsed_rectangle(UiRect label, u32 bytes, float gap, float* name_right) noexcept;
} // namespace maicord
