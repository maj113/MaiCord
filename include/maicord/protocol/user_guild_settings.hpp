#pragma once
#include "maicord/model/message_notifications.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// Notification settings writes, matching the client's routing: direct messages
// PATCH /users/@me/guilds/@me/settings with the partial settings object; guilds
// PATCH /users/@me/guilds/settings with {"guilds":{"<id>":partial}}. A channel
// patch carries only that channel inside channel_overrides.
enum UserGuildSettingsField : u16 {
    user_guild_settings_muted = 1,
    user_guild_settings_level = 2,
    user_guild_settings_suppress_everyone = 4,
    user_guild_settings_suppress_roles = 8,
    // Guild scope only: "Hide muted channels".
    user_guild_settings_hide_muted = 16,
    // Guild scope only: the whole guild flags word (channel opt-in
    // OPT_IN_CHANNELS_ON 1 << 14 and friends), as the official client writes it.
    user_guild_settings_flags = 32,
};
inline constexpr u32 kUserGuildOptInChannelsOn = 1u << 14;
inline constexpr u32 kUserChannelOptInEnabled = 1u << 12;
inline constexpr u16 kUserGuildSettingsChannelFields = user_guild_settings_muted | user_guild_settings_level;
// Mute submenu windows in seconds; -1 is "until I turn it back on".
inline constexpr i32 kUserGuildMuteWindows[] = {900, 3600, 10800, 28800, 86400, -1};
inline constexpr u32 kUserGuildMuteWindowCount = sizeof(kUserGuildMuteWindows) / sizeof(kUserGuildMuteWindows[0]);
inline constexpr u32 kUserGuildSettingsMaximumTargetBytes = 64;
inline constexpr u32 kUserGuildSettingsMaximumBodyBytes = 320;
inline constexpr u64 kUserGuildSettingsRouteId = 0x5547534554544e47ull;

struct UserGuildSettingsPatch {
    u64 guild_id;   // 0 addresses direct messages (@me)
    u64 channel_id; // 0 patches guild-level fields
    // Unix milliseconds the mute ends; 0 with a window of -1 or 0 has no end.
    u64 mute_end_ms;
    // selected_time_window; 0 sends no mute_config (a plain mute or unmute).
    i32 mute_window_seconds;
    u16 fields;
    MessageNotificationLevel level;
    bool muted, suppress_everyone, suppress_roles, hide_muted_channels;
    u32 flags;
};
bool user_guild_settings_patch_valid(const UserGuildSettingsPatch&) noexcept;
// Mute end for a submenu window: now + window, or 0 for -1.
constexpr u64 user_guild_settings_mute_end(i32 window_seconds, u64 now_ms) noexcept {
    return window_seconds > 0 ? now_ms + static_cast<u64>(window_seconds) * 1000 : 0;
}
// The setting this patch produces when merged over the current one; the
// optimistic projection a later USER_GUILD_SETTINGS_UPDATE replaces.
MessageNotificationSetting user_guild_settings_project(MessageNotificationSetting current,
                                                       const UserGuildSettingsPatch&) noexcept;
struct UserGuildSettingsWriteResult {
    u32 target_bytes, body_bytes;
    bool ok;
};
UserGuildSettingsWriteResult user_guild_settings_write(const UserGuildSettingsPatch&, Slice<u8> target,
                                                       Slice<u8> body) noexcept;

// GET /users/@me/mentions. The inbox pages with limit 25 and before=<oldest>.
inline constexpr u8 kMentionsPageLimit = 25;
inline constexpr u32 kMentionsMaximumTargetBytes = 160;
inline constexpr u64 kMentionsRouteId = 0x4d454e54494f4e53ull;
struct MentionsQuery {
    u64 before, guild_id; // guild_id 0 searches all servers
    u8 limit;
    bool roles, everyone;
};
bool mentions_query_valid(const MentionsQuery&) noexcept;
u32 write_mentions_target(const MentionsQuery&, Slice<u8> output) noexcept;
// A full page means more may follow, as the client assumes.
constexpr bool mentions_has_more(u32 count, u8 limit) noexcept {
    return limit && count >= limit;
}

} // namespace maicord
