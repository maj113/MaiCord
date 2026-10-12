#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/guild_admin.hpp"

namespace maicord {

// Channel Settings overview for voice, stage and category channels.
struct ChannelSettingsDraft {
    u64 channel_id, guild_id, parent_id;
    u32 bitrate, user_limit, slowmode_seconds;
    u16 name_bytes;
    u8 type;
    bool nsfw;
    char name[kGuildAdminNameBytes];
};

bool channel_settings_supported(u8 channel_type) noexcept;
// Full GET /channels/{id} document of a supported type; output is untouched on failure.
bool channel_settings_decode(Utf8View body, ChannelSettingsDraft* output) noexcept;
bool channel_settings_equal(const ChannelSettingsDraft&, const ChannelSettingsDraft&) noexcept;
bool channel_settings_name_valid(Utf8View) noexcept;
// Changed fields as a channel PATCH borrowing draft text; false when nothing
// changed or the draft is invalid for its type.
bool channel_settings_patch(const ChannelSettingsDraft& baseline, const ChannelSettingsDraft& draft,
                            ChannelPatch* patch) noexcept;

inline constexpr u32 kChannelSettingsBitrates[]{8'000, 16'000, 24'000, 32'000, 48'000, 64'000, 80'000, 96'000};
inline constexpr u32 kChannelSettingsUserLimitMaximum = 99;

} // namespace maicord
