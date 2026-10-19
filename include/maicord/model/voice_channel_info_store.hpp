#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
// Voice channel statuses and session start times by channel. Only channels
// with a value hold a record; statuses share one packed text arena. A guild
// has at most 500 channels, and only the selected guild's values are asked for.
inline constexpr u32 kVoiceChannelInfoCapacity = 512;
inline constexpr u32 kVoiceChannelInfoTextBytes = 32 * 1024;

struct VoiceChannelInfo {
    u64 channel_id, guild_id;
    u32 start_s;       // Unix seconds the session started; 0 when none.
    u32 status_offset; // Into text.
    u16 status_bytes;
    u8 reserved[6];
};
static_assert(sizeof(VoiceChannelInfo) == 32);
struct VoiceChannelInfoStore {
    VoiceChannelInfo records[kVoiceChannelInfoCapacity];
    char text[kVoiceChannelInfoTextBytes];
    u32 count, text_used, revision, dropped;
};

void voice_channel_info_store_reset(VoiceChannelInfoStore*) noexcept;
// Sets or clears (start_s 0, empty status) one value. When full, records of
// other guilds are evicted first; a value that still does not fit is dropped.
bool voice_channel_info_store_set_start(VoiceChannelInfoStore*, u64 guild_id, u64 channel_id,
                                        u64 start_s) noexcept;
bool voice_channel_info_store_set_status(VoiceChannelInfoStore*, u64 guild_id, u64 channel_id,
                                         Utf8View status) noexcept;
bool voice_channel_info_store_clear_guild(VoiceChannelInfoStore*, u64 guild_id) noexcept;
bool voice_channel_info_store_remove(VoiceChannelInfoStore*, u64 channel_id) noexcept;
const VoiceChannelInfo* voice_channel_info_store_find(const VoiceChannelInfoStore&,
                                                      u64 channel_id) noexcept;
inline Utf8View voice_channel_info_status(const VoiceChannelInfoStore& store,
                                          const VoiceChannelInfo& info) noexcept {
    return {store.text + info.status_offset, info.status_bytes};
}
} // namespace maicord
