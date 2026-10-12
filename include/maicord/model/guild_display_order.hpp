#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/guild_folders.hpp"

namespace maicord {

inline constexpr u16 kGuildDisplayNoFolder = 0xFFFF;
// Folder header rows share the guild row ID space; snowflakes never set bit 63.
inline constexpr u64 kGuildFolderRowMarker = 1ull << 63;
inline constexpr u32 kGuildFolderFallbackNameBytes = 64;

struct GuildDisplayEntry {
    u64 guild_id;
    u16 folder; // index into GuildFolderSettings::folders, or kGuildDisplayNoFolder
};

// The official client's sorted guild list. Folders without an id are single guilds; guilds
// absent from the state are dropped, empty folders vanish, and loaded guilds
// absent from the settings are prepended (each inserted at the top). Without
// folders, guild_positions orders the list; null settings keep guild_order.
// Never mutates the state's ID-lookup order. Returns the entry count.
u32 guild_display_order_build(const GuildFolderSettings* settings, const ChatStateStore& state,
                              Slice<GuildDisplayEntry> output) noexcept;

constexpr bool guild_folder_row_id(u64 id) noexcept {
    return (id & kGuildFolderRowMarker) != 0;
}
// Stable nonzero header identity: the folder's own id, else its position.
u64 guild_folder_row_key(const GuildFolderSettings& settings, u32 folder) noexcept;
// "Name A, Name B, ..." within a 64-byte budget, else "Server folder".
u32 guild_folder_display_name(const GuildFolderSettings& settings, u32 folder, const ChatStateStore& state,
                              Slice<char> output) noexcept;

} // namespace maicord
