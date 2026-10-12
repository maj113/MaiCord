#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord::app_detail {
// Mute and notification-level controls for servers, categories, channels and
// direct messages. Choices apply locally at once and are written through the
// user guild settings endpoint; a rejected write restores the prior setting.
inline constexpr u32 kNotificationMenuActionFirst = 0x4e430000;
inline constexpr u32 kNotificationMenuActionLast = 0x4e43003f;
inline constexpr u32 kNotificationMenuItemCount = 2;
// Rows for a context menu: "Mute …"/"Unmute …" and "Notification settings".
// guild_id alone targets a server; channel_id targets a channel or DM.
u32 application_notification_menu_items(u64 guild_id, u64 channel_id, Slice<PopupItem> output) noexcept;
constexpr bool application_notification_menu_action(u32 action) noexcept {
    return action >= kNotificationMenuActionFirst && action <= kNotificationMenuActionLast;
}
// Opens the follow-up menu at the current popup anchor or applies a choice.
bool application_notification_menu_activate(u32 action) noexcept;
bool application_notification_controls_set_hide_muted(u64 guild_id, bool hide) noexcept;
// Channel opt-in ("Show all channels" is its inverse) on a community server:
// available once opt-in is on, a channel is opted in or onboarding is done.
// Written as the guild flags word with OPT_IN_CHANNELS_ON set or cleared.
struct GuildChannelOptIn {
    bool available, enabled;
};
GuildChannelOptIn application_notification_controls_channel_opt_in(u64 guild_id) noexcept;
bool application_notification_controls_set_channel_opt_in(u64 guild_id, bool enabled) noexcept;
// Projects effective mutes and "Hide muted channels" into the navigation
// state and re-projects when a timed mute ends. Unix time drives expiry.
void application_notification_controls_tick(u64 now_ms) noexcept;
u64 application_notification_controls_next_wake() noexcept;
bool application_notification_controls_pending(u64 now_ms) noexcept;
bool application_notification_controls_submit(u64 request_id, u64 now_ms) noexcept;
bool application_notification_controls_response(const RestClientResponse&, Utf8View) noexcept;
void application_notification_controls_clear() noexcept;
} // namespace maicord::app_detail
