#pragma once
#include "maicord/app/account/account_module_api.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/chat/messages/media_policy.hpp"
#include "catalog.hpp"

namespace maicord::app_detail {
// Synced account settings (settings-proto/1). The store is allocated on the
// first READY snapshot and released with the account. Edits are optimistic,
// batched for a second and rolled back when the server rejects them.
void application_user_settings_gateway(const GatewayClientEvent&, ConstSlice<u8> payload) noexcept;
void application_user_settings_synchronize() noexcept;
bool application_user_settings_pending(u64 now_ms) noexcept;
bool application_user_settings_submit(u64 request_id, u64 now_ms) noexcept;
bool application_user_settings_response(const RestClientResponse&, Utf8View body) noexcept;
u64 application_user_settings_next_wake() noexcept;
void application_user_settings_release() noexcept;
// Reapplies display consumers (compact mode and its avatars, group spacing, time format, UI Density and the Chat
// page: media, embeds, reactions, animation, spoilers, image descriptions) and mirrors the account's Developer
// Mode into the local preferences.
void application_user_settings_apply_display() noexcept;
// Saves Developer Mode to the account (appearance.developer_mode); false while the synced settings are not loaded.
bool application_user_settings_set_developer_mode(bool on) noexcept;
// Appearance > Chat Box "Show games in autocomplete results" (text_and_images.include_game_mentions_in_autocomplete;
// true while unset or not loaded).
bool application_user_settings_game_mentions_autocomplete() noexcept;
// Chat Box "Show stickers in autocomplete results" (include_stickers_in_autocomplete; false while unset).
bool application_user_settings_sticker_autocomplete() noexcept;
// "Allow playback and usage of /tts command" (enable_tts_command; true while unset or not loaded).
bool application_user_settings_tts_command() noexcept;
// Chat "Show spoiler content" for a channel: always, or where you moderate.
bool application_user_settings_reveal_spoilers(u64 channel_id) noexcept;
// Context defaults and stored overrides share one account/revision cache.
MessageExplicitFilter application_user_settings_explicit_filter(MessageMediaContext) noexcept;
bool application_user_settings_content_confirmation_required() noexcept;
// Explains how to confirm age in Discord; never changes account eligibility.
void application_user_settings_request_content_confirmation() noexcept;

// Revision of the synced settings store; zero before the first READY.
u64 application_user_settings_revision() noexcept;
// READY auth_session_id_hash of this session; empty before READY.
Utf8View application_user_settings_session_hash() noexcept;

// Per-server privacy (privacy.*_guild_ids). A listed server is restricted:
// no DMs from its members, no message requests, no activity status or no
// activity joining there. The official client edits these lists as sets.
enum class GuildPrivacyList : u8 { direct_messages, message_requests, activity_status, activity_joining };
// Whether the synced settings are loaded (edits are accepted).
bool application_user_settings_ready() noexcept;
// The last save failure, or empty.
Utf8View application_user_settings_failure() noexcept;
bool application_user_settings_guild_restricted(GuildPrivacyList, u64 guild_id) noexcept;
bool application_user_settings_set_guild_restricted(GuildPrivacyList, u64 guild_id, bool restricted) noexcept;

bool application_user_settings_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*) noexcept;
bool application_user_settings_activate(SettingBinding, bool checked, u32 choice) noexcept;
} // namespace maicord::app_detail
