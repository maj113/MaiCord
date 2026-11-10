#pragma once
#include "maicord/core/utf.hpp"

namespace maicord::app_detail {
struct SettingsLayer;
// A game mention's click, as the official client handles it: a known game
// opens its game profile, an unknown one the "Game could not be found"
// dialog. The profile is a dialog hosted by the channel overview modal host:
// the game's cover, summary, details (genres, publishers, developers, release
// date, platforms, links, the IGDB credit) and the friends who play it. Its
// game object comes from GET /games?game_ids={id} (chat/game_lookup.hpp).
bool application_game_profile_open(u64 game_id) noexcept;
bool application_game_profile_active() noexcept;
void application_game_profile_close(bool restore_focus = true) noexcept;
void application_game_profile_release() noexcept;
bool application_game_profile_synchronize() noexcept;
SettingsLayer* application_game_profile_settings_layer() noexcept;
u64 application_game_profile_caret_wake() noexcept;
// The profile's game object arrived (ok) or failed.
void application_game_profile_response(u64 game_id, bool ok, Utf8View body) noexcept;
// Cached games changed (the name and cover the profile shows).
void application_game_profile_games_changed() noexcept;
} // namespace maicord::app_detail
