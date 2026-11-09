#pragma once
#include "../shell/state.hpp"

namespace maicord::app_detail {
// Streamer Mode (Settings > Appearance): on by the user's switch, or while a streaming tool runs (OBS, XSplit and the
// others the activity scan knows) when "Automatically enable" is on. Its options say what it hides and silences.
inline bool g_streamer_tool_running; // kept by application_streamer_mode_synchronize
inline bool application_streamer_mode_active() noexcept {
    const auto& preferences = g_app.preferences;
    return preferences.streamer_mode || (preferences.streamer_auto && g_streamer_tool_running);
}
// Email, phone, connected accounts and notes are masked or hidden.
inline bool application_streamer_hide_personal() noexcept {
    return application_streamer_mode_active() && g_app.preferences.streamer_hide_personal;
}
// Invite links and codes to the account's servers are masked.
inline bool application_streamer_hide_invites() noexcept {
    return application_streamer_mode_active() && g_app.preferences.streamer_hide_invites;
}
// No sound effects play.
inline bool application_streamer_disable_sounds() noexcept {
    return application_streamer_mode_active() && g_app.preferences.streamer_disable_sounds;
}
// No desktop notifications show.
inline bool application_streamer_disable_notifications() noexcept {
    return application_streamer_mode_active() && g_app.preferences.streamer_disable_notifications;
}
// Takes the activity scan's streaming-tool state, keeps the window out of screen captures as the options say and
// refreshes what Streamer Mode hides when it turns on or off. The main loop calls it after the activity tick; a
// Streamer Mode setting's change calls it too.
void application_streamer_mode_synchronize() noexcept;
// Flips Streamer Mode as its keybind does (Settings > Keybinds "Toggle Streamer Mode"), saving the choice.
bool application_streamer_mode_toggle() noexcept;
} // namespace maicord::app_detail
