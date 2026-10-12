#pragma once
#include "maicord/app/account_module_api.hpp"
#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "settings_catalog.hpp"

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
// Reapplies display consumers (compact mode, group spacing, time format and
// the Chat page: media, embeds, reactions, animation, spoilers).
void application_user_settings_apply_display() noexcept;
// Chat "Show spoiler content" for a channel: always, or where you moderate.
bool application_user_settings_reveal_spoilers(u64 channel_id) noexcept;

// Reusable group API for other settings owners (reaction preferences etc.):
// typed reads and optimistic writes of any field in a stored group.
bool application_user_settings_read(UserSettingsField, UserSettingsValue*) noexcept;
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
