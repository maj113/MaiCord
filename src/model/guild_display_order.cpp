#include "maicord/model/guild_display_order.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool placed(Slice<GuildDisplayEntry> output, u32 count, u64 id) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (output[i].guild_id == id)
            return true;
    return false;
}
bool append(Slice<GuildDisplayEntry> output, u32* count, const ChatStateStore& state, u64 id,
            u16 folder) noexcept {
    if (!id || !chat_state_store_find_guild(state, id) || placed(output, *count, id))
        return true;
    if (*count >= output.count)
        return false;
    output[(*count)++] = {id, folder};
    return true;
}
} // namespace

u32 guild_display_order_build(const GuildFolderSettings* settings, const ChatStateStore& state,
                              Slice<GuildDisplayEntry> output) noexcept {
    if (!output.data || !state.guilds || !state.guild_order)
        return 0;
    u32 count = 0;
    const bool folders = settings && settings->present && settings->folder_count;
    if (folders) {
        for (u32 f = 0; f < settings->folder_count && f < kGuildFoldersMaximumFolders; ++f) {
            const auto& folder = settings->folders[f];
            const u32 end = minimum<u32>(folder.first_guild + folder.guild_count, settings->guild_id_count);
            for (u32 g = folder.first_guild; g < end; ++g)
                if (!append(output, &count, state, settings->guild_ids[g],
                            folder.has_id ? static_cast<u16>(f) : kGuildDisplayNoFolder))
                    return count;
        }
    } else if (settings && settings->present) {
        for (u32 p = 0; p < settings->position_count && p < kGuildFoldersMaximumPositions; ++p)
            if (!append(output, &count, state, settings->positions[p], kGuildDisplayNoFolder))
                return count;
    }
    // Unlisted guilds go to the top in reverse join order, as each is
    // inserted at index 0.
    for (u32 order = 0; order < state.guild_count; ++order) {
        const u16 slot = state.guild_order[order];
        if (slot >= state.guild_capacity || !state.guilds[slot].occupied)
            continue;
        const u64 id = state.guilds[slot].id;
        if (placed(output, count, id) || count >= output.count)
            continue;
        const bool listed = settings && settings->present && (folders || settings->position_count);
        if (!listed) {
            output[count++] = {id, kGuildDisplayNoFolder};
            continue;
        }
        copy_bytes(output.data + 1, output.data, count * static_cast<u32>(sizeof(GuildDisplayEntry)));
        output[0] = {id, kGuildDisplayNoFolder};
        ++count;
    }
    return count;
}

u64 guild_folder_row_key(const GuildFolderSettings& settings, u32 folder) noexcept {
    if (folder >= settings.folder_count)
        return 0;
    const auto& record = settings.folders[folder];
    const u64 key = record.has_id && record.id ? record.id : 0x100000000ull + folder;
    return kGuildFolderRowMarker | (key & ~kGuildFolderRowMarker);
}

u32 guild_folder_display_name(const GuildFolderSettings& settings, u32 folder, const ChatStateStore& state,
                              Slice<char> output) noexcept {
    if (folder >= settings.folder_count || !output.data)
        return 0;
    const auto& record = settings.folders[folder];
    if (record.name_bytes) {
        if (record.name_bytes > output.count)
            return 0;
        copy_bytes(output.data, record.name, record.name_bytes);
        return record.name_bytes;
    }
    char text[kGuildFolderFallbackNameBytes + 8];
    u32 used = 0;
    bool truncated = false;
    const u32 end = minimum<u32>(record.first_guild + record.guild_count, settings.guild_id_count);
    for (u32 g = record.first_guild; g < end; ++g) {
        const auto* guild = chat_state_store_find_guild(state, settings.guild_ids[g]);
        if (!guild)
            continue;
        const Utf8View name = chat_state_store_guild_name(state, *guild);
        if (!name.bytes)
            continue;
        const u32 separator = used ? 2u : 0u;
        if (used + separator + name.bytes > kGuildFolderFallbackNameBytes) {
            truncated = true;
            break;
        }
        if (separator)
            copy_bytes(text + used, ", ", 2);
        copy_bytes(text + used + separator, name.data, name.bytes);
        used += separator + name.bytes;
    }
    if (!used) {
        constexpr Utf8View fallback = utf8_literal("Server folder");
        copy_bytes(text, fallback.data, fallback.bytes);
        used = fallback.bytes;
    } else if (truncated) {
        copy_bytes(text + used, ", ...", 5);
        used += 5;
    }
    if (used > output.count)
        return 0;
    copy_bytes(output.data, text, used);
    return used;
}

} // namespace maicord
