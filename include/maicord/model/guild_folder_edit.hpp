#pragma once
#include "maicord/model/guild_display_order.hpp"

namespace maicord {

// Guild rail edits. Settings are always rewritten in canonical form: every
// displayed guild in order, a guild outside folders as a folder entry without
// an id, real folders keeping their id, name and color. Guild positions are kept.

enum class GuildFolderDrop : u8 {
    before,  // above the target row
    after,   // below the target row
    combine, // onto the target: a guild makes a new folder, a folder takes the guild
};

// A rail item: a guild (id is the guild snowflake) or a folder (id is the
// folder's own id, never a header row id).
struct GuildFolderItem {
    u64 id;
    bool folder;
};

enum class GuildFolderEditError : u8 {
    none,
    input,     // unknown items or a refused move
    capacity,  // more folders or guilds than the settings hold
    unchanged, // valid, but the layout would not change
};

// Canonical settings for what the rail shows now (unlisted guilds prepended,
// missing guilds and empty folders dropped; an id-less entry holding several
// guilds splits into single guilds).
GuildFolderEditError guild_folders_canonical(const GuildFolderSettings* current, const ChatStateStore& state,
                                             GuildFolderSettings* output) noexcept;

// Moves source next to (before/after) or onto (combine) target. A folder stays
// top level: it never combines or lands next to a guild inside a folder. A guild
// dropped next to a guild inside a folder joins that folder; combining onto
// such a guild is refused. new_folder_id names the folder a guild-on-guild
// combine creates. settings must be canonical and changes only on success. A
// folder left empty disappears.
GuildFolderEditError guild_folders_move(GuildFolderSettings* settings, GuildFolderItem source,
                                        GuildFolderItem target, GuildFolderDrop drop,
                                        u64 new_folder_id) noexcept;

// Folder name (empty clears it) and color (has_color false restores the default).
GuildFolderEditError guild_folders_edit(GuildFolderSettings* settings, u64 folder_id, Utf8View name,
                                        u32 color, bool has_color) noexcept;

// Index of a real folder; ~0u when absent.
u32 guild_folders_find_folder(const GuildFolderSettings& settings, u64 folder_id) noexcept;
// A nonzero 31-bit folder id unused by settings, derived from seed.
u64 guild_folders_new_id(const GuildFolderSettings& settings, u64 seed) noexcept;

} // namespace maicord
