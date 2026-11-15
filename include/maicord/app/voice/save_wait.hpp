#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Voice preferences that wait for the pointer before the client writes them: a slider drag waits for the mouse button,
// and a user volume waits for its menu. No event tells the client either. The main loop looks on each turn and asks
// for a turn each kVoiceSaveLookMs while a save waits. The look is not account work: the account pump does not run
// for it.
inline constexpr u64 kVoiceSaveLookMs = 250;
struct VoiceSaveWait {
    bool drag, users;
};

// One look. True: write the preferences now.
constexpr bool voice_save_look(VoiceSaveWait* wait, bool button_down, bool menu_busy) noexcept {
    const bool drag = wait->drag && !button_down, users = wait->users && !menu_busy;
    wait->drag = wait->drag && !drag;
    wait->users = wait->users && !users;
    return drag || users;
}

// The time of the next look, or zero when no save waits. It is always after `now_ms`: a wait that ends at this time
// is a loop turn, and the look of that turn gives the next time.
constexpr u64 voice_save_next_look(const VoiceSaveWait& wait, u64 now_ms) noexcept {
    return wait.drag || wait.users ? now_ms + kVoiceSaveLookMs : 0;
}

} // namespace maicord
