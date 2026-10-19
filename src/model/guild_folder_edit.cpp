#include "maicord/model/guild_folder_edit.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using E = GuildFolderEditError;

// Working tree: top-level nodes in display order. Each node is a record
// (folder metadata, or an id-less single guild) owning a run of guilds in a
// shared arena that grows by relocating the run being extended.
constexpr u32 kArena = kGuildFoldersMaximumGuilds * 2;
struct Tree {
    struct Slot {
        GuildFolderRecord record; // first_guild/guild_count unused while editing
        u16 first, count;
    } slots[kGuildFoldersMaximumFolders + 1];
    u16 order[kGuildFoldersMaximumFolders + 1];
    u64 arena[kArena];
    u64 compact[kArena];
    u64 positions[kGuildFoldersMaximumPositions];
    u32 count, arena_used, position_count;
};

bool load(const GuildFolderSettings& settings, Tree* tree) noexcept {
    tree->count = tree->arena_used = 0;
    if (settings.folder_count > kGuildFoldersMaximumFolders || settings.guild_id_count > kGuildFoldersMaximumGuilds ||
        settings.position_count > kGuildFoldersMaximumPositions)
        return false;
    for (u32 f = 0; f < settings.folder_count; ++f) {
        const auto& record = settings.folders[f];
        if (record.first_guild > settings.guild_id_count ||
            record.guild_count > settings.guild_id_count - record.first_guild ||
            tree->arena_used + record.guild_count > kArena)
            return false;
        auto& slot = tree->slots[tree->count];
        slot.record = record;
        slot.first = static_cast<u16>(tree->arena_used);
        slot.count = record.guild_count;
        copy_bytes(tree->arena + tree->arena_used, settings.guild_ids + record.first_guild,
                   record.guild_count * static_cast<u32>(sizeof(u64)));
        tree->arena_used += record.guild_count;
        tree->order[tree->count] = static_cast<u16>(tree->count);
        ++tree->count;
    }
    copy_bytes(tree->positions, settings.positions, settings.position_count * static_cast<u32>(sizeof(u64)));
    tree->position_count = settings.position_count;
    return true;
}

// Compact settings in display order; empty folders vanish.
E store(const Tree& tree, GuildFolderSettings* output) noexcept {
    *output = {};
    for (u32 n = 0; n < tree.count; ++n) {
        const auto& slot = tree.slots[tree.order[n]];
        if (!slot.count)
            continue;
        if (output->folder_count >= kGuildFoldersMaximumFolders ||
            output->guild_id_count + slot.count > kGuildFoldersMaximumGuilds)
            return E::capacity;
        auto& record = output->folders[output->folder_count++];
        record = slot.record;
        record.first_guild = static_cast<u16>(output->guild_id_count);
        record.guild_count = slot.count;
        copy_bytes(output->guild_ids + output->guild_id_count, tree.arena + slot.first,
                   slot.count * static_cast<u32>(sizeof(u64)));
        output->guild_id_count += slot.count;
    }
    copy_bytes(output->positions, tree.positions, tree.position_count * static_cast<u32>(sizeof(u64)));
    output->position_count = tree.position_count;
    output->present = true;
    return E::none;
}

bool locate_guild(const Tree& tree, u64 guild, u32* node, u32* index) noexcept {
    for (u32 n = 0; n < tree.count; ++n) {
        const auto& slot = tree.slots[tree.order[n]];
        for (u32 g = 0; g < slot.count; ++g)
            if (tree.arena[slot.first + g] == guild) {
                *node = n;
                *index = g;
                return true;
            }
    }
    return false;
}
bool locate_folder(const Tree& tree, u64 folder, u32* node) noexcept {
    for (u32 n = 0; n < tree.count; ++n) {
        const auto& record = tree.slots[tree.order[n]].record;
        if (record.has_id && record.id == folder) {
            *node = n;
            return true;
        }
    }
    return false;
}
bool real_folder(const Tree& tree, u32 node) noexcept {
    return tree.slots[tree.order[node]].record.has_id;
}
// Moves a slot's run to the arena end with room for `extra` more guilds.
bool relocate(Tree* tree, u32 slot_index, u32 extra) noexcept {
    if (tree->arena_used + tree->slots[slot_index].count + extra > kArena) {
        u32 used = 0;
        for (u32 s = 0; s < tree->count; ++s) {
            auto& other = tree->slots[s];
            copy_bytes(tree->compact + used, tree->arena + other.first, other.count * static_cast<u32>(sizeof(u64)));
            other.first = static_cast<u16>(used);
            used += other.count;
        }
        copy_bytes(tree->arena, tree->compact, used * static_cast<u32>(sizeof(u64)));
        tree->arena_used = used;
        if (tree->arena_used + tree->slots[slot_index].count + extra > kArena)
            return false;
    }
    auto& slot = tree->slots[slot_index];
    copy_bytes(tree->arena + tree->arena_used, tree->arena + slot.first, slot.count * static_cast<u32>(sizeof(u64)));
    slot.first = static_cast<u16>(tree->arena_used);
    tree->arena_used += slot.count + extra;
    return true;
}
bool insert_guild(Tree* tree, u32 node, u32 index, u64 guild) noexcept {
    const u32 slot_index = tree->order[node];
    if (index > tree->slots[slot_index].count || !relocate(tree, slot_index, 1))
        return false;
    auto& slot = tree->slots[slot_index];
    for (u32 g = slot.count; g > index; --g)
        tree->arena[slot.first + g] = tree->arena[slot.first + g - 1];
    tree->arena[slot.first + index] = guild;
    ++slot.count;
    return true;
}
void remove_guild(Tree* tree, u32 node, u32 index) noexcept {
    auto& slot = tree->slots[tree->order[node]];
    for (u32 g = index; g + 1 < slot.count; ++g)
        tree->arena[slot.first + g] = tree->arena[slot.first + g + 1];
    --slot.count;
}
// A new single-guild (or given record) node at order position `at`.
bool insert_node(Tree* tree, u32 at, const GuildFolderRecord& record, u64 guild) noexcept {
    if (tree->count > kGuildFoldersMaximumFolders || at > tree->count)
        return false;
    const u32 slot_index = tree->count;
    auto& slot = tree->slots[slot_index];
    slot.record = record;
    slot.first = static_cast<u16>(tree->arena_used);
    slot.count = 0;
    for (u32 n = tree->count; n > at; --n)
        tree->order[n] = tree->order[n - 1];
    tree->order[at] = static_cast<u16>(slot_index);
    ++tree->count;
    return insert_guild(tree, at, 0, guild);
}
void move_node(Tree* tree, u32 from, u32 to) noexcept {
    const u16 moving = tree->order[from];
    if (from < to)
        for (u32 n = from; n < to; ++n)
            tree->order[n] = tree->order[n + 1];
    else
        for (u32 n = from; n > to; --n)
            tree->order[n] = tree->order[n - 1];
    tree->order[to] = moving;
}
bool same_layout(const GuildFolderSettings& a, const GuildFolderSettings& b) noexcept {
    if (a.folder_count != b.folder_count || a.guild_id_count != b.guild_id_count)
        return false;
    for (u32 g = 0; g < a.guild_id_count; ++g)
        if (a.guild_ids[g] != b.guild_ids[g])
            return false;
    for (u32 f = 0; f < a.folder_count; ++f) {
        const auto &x = a.folders[f], &y = b.folders[f];
        if (x.guild_count != y.guild_count || x.has_id != y.has_id || x.id != y.id)
            return false;
    }
    return true;
}
} // namespace

u32 guild_folders_find_folder(const GuildFolderSettings& settings, u64 folder_id) noexcept {
    for (u32 f = 0; f < settings.folder_count && f < kGuildFoldersMaximumFolders; ++f)
        if (settings.folders[f].has_id && settings.folders[f].id == folder_id)
            return f;
    return ~0u;
}

u64 guild_folders_new_id(const GuildFolderSettings& settings, u64 seed) noexcept {
    u64 state = seed ^ 0x9E3779B97F4A7C15ull;
    for (u32 attempt = 0; attempt < 4096; ++attempt) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const u64 id = (state >> 33) & 0x7FFFFFFFull;
        if (id && guild_folders_find_folder(settings, id) == ~0u)
            return id;
    }
    return 0;
}

GuildFolderEditError guild_folders_canonical(const GuildFolderSettings* current, const ChatStateStore& state,
                                             GuildFolderSettings* output) noexcept {
    if (!output)
        return E::input;
    // Editing scratch is far larger than a comfortable stack frame.
    struct Scratch {
        GuildDisplayEntry entries[kGuildFoldersMaximumGuilds];
        GuildFolderSettings built;
    };
    ScratchPages<Scratch> scratch;
    if (!scratch.data)
        return E::capacity;
    const GuildDisplayEntry* entries = scratch.data->entries;
    const u32 count = guild_display_order_build(current, state, {scratch.data->entries, kGuildFoldersMaximumGuilds});
    auto& built = scratch.data->built;
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        const bool folder =
            entry.folder != kGuildDisplayNoFolder && current && entry.folder < current->folder_count;
        const bool continues = folder && i && entries[i - 1].folder == entry.folder;
        if (built.guild_id_count >= kGuildFoldersMaximumGuilds)
            return E::capacity;
        if (!continues) {
            if (built.folder_count >= kGuildFoldersMaximumFolders)
                return E::capacity;
            auto& record = built.folders[built.folder_count++];
            record = folder ? current->folders[entry.folder] : GuildFolderRecord{};
            record.first_guild = static_cast<u16>(built.guild_id_count);
            record.guild_count = 0;
        }
        built.guild_ids[built.guild_id_count++] = entry.guild_id;
        ++built.folders[built.folder_count - 1].guild_count;
    }
    if (current && current->present) {
        built.position_count = minimum<u32>(current->position_count, kGuildFoldersMaximumPositions);
        copy_bytes(built.positions, current->positions, built.position_count * static_cast<u32>(sizeof(u64)));
    }
    built.present = true;
    copy_bytes(output, &built, sizeof(built));
    return E::none;
}

GuildFolderEditError guild_folders_move(GuildFolderSettings* settings, GuildFolderItem source,
                                        GuildFolderItem target, GuildFolderDrop drop,
                                        u64 new_folder_id) noexcept {
    if (!settings || !source.id || !target.id || (source.id == target.id && source.folder == target.folder) ||
        static_cast<u8>(drop) > static_cast<u8>(GuildFolderDrop::combine))
        return E::input;
    struct Scratch {
        Tree tree;
        GuildFolderSettings built;
    };
    ScratchPages<Scratch> scratch;
    if (!scratch.data)
        return E::capacity;
    auto& tree = scratch.data->tree;
    if (!load(*settings, &tree))
        return E::input;
    u32 target_node = 0, target_index = 0;
    const bool target_found = target.folder ? locate_folder(tree, target.id, &target_node)
                                            : locate_guild(tree, target.id, &target_node, &target_index);
    if (!target_found)
        return E::input;
    const bool target_nested = !target.folder && real_folder(tree, target_node);
    if (source.folder) {
        u32 node = 0;
        // Folders stay top level: no combining, no neighbours inside a folder.
        if (!locate_folder(tree, source.id, &node) || drop == GuildFolderDrop::combine || target_nested)
            return E::input;
        u32 to = target_node + (drop == GuildFolderDrop::after ? 1u : 0u);
        if (node < to)
            --to;
        move_node(&tree, node, to);
    } else {
        u32 node = 0, index = 0;
        if (!locate_guild(tree, source.id, &node, &index))
            return E::input;
        if (drop == GuildFolderDrop::combine && target_nested)
            return E::input;
        if (drop == GuildFolderDrop::combine && target.folder) {
            // Into an existing folder, at its end.
            if (node == target_node)
                return E::unchanged;
            remove_guild(&tree, node, index);
            if (!insert_guild(&tree, target_node, tree.slots[tree.order[target_node]].count, source.id))
                return E::capacity;
        } else if (drop == GuildFolderDrop::combine) {
            // Guild onto a top-level guild: that entry becomes a new folder.
            if (!new_folder_id || new_folder_id > 0x7FFFFFFFFFFFFFFFull ||
                guild_folders_find_folder(*settings, new_folder_id) != ~0u)
                return E::input;
            remove_guild(&tree, node, index);
            auto& slot = tree.slots[tree.order[target_node]];
            slot.record = {};
            slot.record.id = new_folder_id;
            slot.record.has_id = true;
            if (!insert_guild(&tree, target_node, slot.count, source.id))
                return E::capacity;
        } else if (target_nested) {
            // Next to a guild inside a folder: join that folder there.
            remove_guild(&tree, node, index);
            if (node == target_node && index < target_index)
                --target_index;
            if (!insert_guild(&tree, target_node, target_index + (drop == GuildFolderDrop::after ? 1u : 0u),
                              source.id))
                return E::capacity;
        } else {
            // Top level next to a guild or folder: a single-guild entry.
            remove_guild(&tree, node, index);
            if (!insert_node(&tree, target_node + (drop == GuildFolderDrop::after ? 1u : 0u), GuildFolderRecord{},
                             source.id))
                return E::capacity;
        }
    }
    auto& built = scratch.data->built;
    const E stored = store(tree, &built);
    if (stored != E::none)
        return stored;
    if (same_layout(built, *settings))
        return E::unchanged;
    copy_bytes(settings, &built, sizeof(built));
    return E::none;
}

GuildFolderEditError guild_folders_edit(GuildFolderSettings* settings, u64 folder_id, Utf8View name, u32 color,
                                        bool has_color) noexcept {
    if (!settings || !folder_id || name.bytes > kGuildFolderNameBytes || (name.bytes && !name.data) ||
        !utf8_validate(name).valid || (has_color && color > 0xFFFFFF))
        return E::input;
    const u32 index = guild_folders_find_folder(*settings, folder_id);
    if (index == ~0u)
        return E::input;
    auto& record = settings->folders[index];
    const bool color_on = has_color && color != kGuildFolderDefaultColor;
    if (record.name_bytes == name.bytes && bytes_equal(record.name, name.data, name.bytes) &&
        record.has_color == color_on && (!color_on || record.color == color))
        return E::unchanged;
    copy_bytes(record.name, name.data, name.bytes);
    record.name_bytes = static_cast<u8>(name.bytes);
    record.has_color = color_on;
    record.color = color_on ? color : 0;
    return E::none;
}

} // namespace maicord
