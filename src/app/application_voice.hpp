#pragma once
#include "maicord/app/voice_preferences.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/ui/voice_panel.hpp"

namespace maicord::app_detail {
// Client side of voice calls: persisted voice preferences, pushing audio
// settings to the account module, the voice card above the account panel,
// mute/deafen, the voice user menu and the settings meter.
void application_voice_pump() noexcept;
u64 application_voice_next_wake() noexcept;
// The connected channel row's elapsed timer: a visual wake once per displayed
// second while the row is on screen; the tick repaints when the label changes.
u64 application_voice_timer_next_wake() noexcept;
void application_voice_timer_tick(u64 now_ms) noexcept;
float application_voice_panel_reserve() noexcept;

bool application_voice_join(u64 guild_id, u64 channel_id) noexcept;
bool application_voice_leave() noexcept;
bool application_voice_toggle_mute() noexcept;
bool application_voice_toggle_deafen() noexcept;
// Current voice card state (self/server mute and deafen, speaking); the tray mirrors it.
UiVoicePanelView application_voice_view() noexcept;

// Account panel buttons and the voice card. Coordinates are DIPs.
bool application_voice_shell_click(float x, float y) noexcept;
bool application_voice_shell_hover(float x, float y) noexcept;
bool application_voice_hand_cursor(float x, float y) noexcept;
// Tooltip label, anchor and a stable identity for the control under the pointer.
Utf8View application_voice_tooltip(float x, float y, UiRect* anchor, u32* identity) noexcept;

// The connected user drawn under a voice channel row at (x, y): its voice
// state, the row and its LIVE badge (empty unless streaming). Null if none.
const VoiceState* application_voice_participant_at(float x, float y, UiRect* row, UiRect* live) noexcept;

// Right-click on a connected user under a voice channel: volume slider,
// local mute and the member menu.
bool application_voice_context_at(float x, float y, UiRect anchor) noexcept;
bool application_voice_menu_action(u32 action) noexcept;
bool application_voice_menu_activate(u32 action) noexcept;
// Pointer on the open voice user menu's slider; true when consumed.
bool application_voice_menu_pointer(float x, float y, bool press) noexcept;

// Voice & video settings.
VoicePreferences& application_voice_preferences() noexcept;
// Applies edited preferences to the account module; persist saves voice.bin.
void application_voice_settings_changed(bool persist) noexcept;
// Keeps the microphone open for the input meter while the page is visible.
void application_voice_set_monitor(bool enabled) noexcept;
VoiceAudioStatus application_voice_status() noexcept;
} // namespace maicord::app_detail
