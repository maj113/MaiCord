#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {

// PreloadedUserSettings.guild_folders (settings-proto type 1, field 14):
//   GuildFolders { repeated GuildFolder folders = 1; repeated fixed64 guild_positions = 2; }
//   GuildFolder  { repeated fixed64 guild_ids = 1; Int64Value id = 2; StringValue name = 3;
//                  UInt64Value color = 4; }
inline constexpr u32 kGuildFoldersSettingsField = 14;
inline constexpr u32 kGuildFoldersMaximumGroupBytes = 64 * 1024;
inline constexpr u32 kGuildFoldersMaximumProtoBytes = 16 * 1024 * 1024;
inline constexpr u32 kGuildFoldersMaximumFolders = 256;
inline constexpr u32 kGuildFoldersMaximumGuilds = 512;
inline constexpr u32 kGuildFoldersMaximumPositions = 256;
inline constexpr u32 kGuildFolderNameBytes = 128;
// The default brand colour stands for "no custom colour".
inline constexpr u32 kGuildFolderDefaultColor = 0x5865F2;

enum class GuildFoldersError : u8 { none, input, base64, protobuf, capacity };

struct GuildFolderRecord {
    u64 id;
    u32 color;            // 0xRRGGBB when has_color
    u16 first_guild;      // index into GuildFolderSettings::guild_ids
    u16 guild_count;
    u8 name_bytes;
    bool has_id, has_color;
    char name[kGuildFolderNameBytes];
};
struct GuildFolderSettings {
    GuildFolderRecord folders[kGuildFoldersMaximumFolders];
    u64 guild_ids[kGuildFoldersMaximumGuilds];
    u64 positions[kGuildFoldersMaximumPositions];
    u32 folder_count, guild_id_count, position_count;
    bool present;
};

// Copies field 14 verbatim (unknown nested fields included), concatenating
// repeated occurrences in merge order. present=false when the field is absent.
// Validates the outer message before writing output.
GuildFoldersError guild_folders_extract_base64(Utf8View settings, Slice<u8> group, u32* bytes,
                                               bool* present) noexcept;
GuildFoldersError guild_folders_extract_proto(ConstSlice<u8> settings, Slice<u8> group, u32* bytes,
                                              bool* present) noexcept;
// Decodes a GuildFolders message. Unknown fields are skipped; malformed known
// fields reject the whole group and leave output untouched.
GuildFoldersError guild_folders_decode(ConstSlice<u8> group, GuildFolderSettings* output) noexcept;
// Serializes settings as a GuildFolders message: each folder with packed
// guild_ids, then the Int64Value id, the StringValue name (omitted when empty)
// and the UInt64Value color (omitted without a custom color); packed
// guild_positions last. Returns the bytes written, 0 on invalid input or short
// output (output is then unspecified).
u32 guild_folders_encode(const GuildFolderSettings& settings, Slice<u8> output) noexcept;
// A partial PreloadedUserSettings message holding only field 14 (the group).
u32 guild_folders_encode_settings_patch(const GuildFolderSettings& settings, Slice<u8> output) noexcept;

} // namespace maicord
