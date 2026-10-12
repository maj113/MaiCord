#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/model/guild_folder_edit.hpp"

namespace maicord {
struct GatewayClientEvent;
struct GuildViewRow;
struct RestClientResponse;
} // namespace maicord

namespace maicord::app_detail {
// Observes guild_folders dispatches before the timeline consumes the mailbox.
void application_guild_folders_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept;
// Frame time: binds the layout to the navigation view, applies pending
// rebuilds and advances folder expand/collapse motion.
bool application_guild_folders_synchronize(u64 now_ms) noexcept;
u64 application_guild_folders_next_wake(u64 now_ms) noexcept;
bool application_guild_folders_toggle(u64 folder_row_id) noexcept;
// Folder or guild name for a rail row (tooltips); empty when unknown.
Utf8View application_guild_rail_label(const GuildViewRow& row, Slice<char> output) noexcept;
// A folder header row's own folder id; zero for other rows.
u64 application_guild_folders_folder_id(const GuildViewRow& row) noexcept;
struct ApplicationGuildFolder {
    u64 id;
    u32 color; // 0xRRGGBB; the default brand colour without a custom one
    u16 guild_count;
    u8 name_bytes, placeholder_bytes;
    bool has_color;
    char name[kGuildFolderNameBytes];
    char placeholder[kGuildFolderFallbackNameBytes + 8]; // "Server A, Server B, ..."
};
bool application_guild_folders_folder(u64 folder_id, ApplicationGuildFolder* output) noexcept;
// Optimistic rail edits (drag-and-drop moves and folder settings). The rail
// changes at once; the canonical layout is written back through settings-
// proto/1 (group 14) on the PRIMARY lane and rolled back when the server refuses.
GuildFolderEditError application_guild_folders_move(GuildFolderItem source, GuildFolderItem target,
                                                    GuildFolderDrop drop) noexcept;
GuildFolderEditError application_guild_folders_edit(u64 folder_id, Utf8View name, u32 color,
                                                    bool has_color) noexcept;
bool application_guild_folders_pending(u64 now_ms) noexcept;
bool application_guild_folders_submit(u64 request_id, u64 now_ms) noexcept;
bool application_guild_folders_owns_response(u64 request_id) noexcept;
bool application_guild_folders_response(const RestClientResponse&, Utf8View body) noexcept;
u64 application_guild_folders_request_wake() noexcept;
void application_guild_folders_release() noexcept;
} // namespace maicord::app_detail
