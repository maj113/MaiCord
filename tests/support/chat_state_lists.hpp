#pragma once
// The channel lists and the name accounting of ChatStateStore, for tests: checks that walk every record, and the
// link step for a store that a test builds by hand (records and channel_order written directly).
#include "maicord/model/chat/chat_state.hpp"

namespace maicord {

// Walks one list: each link names an occupied record that `member` accepts, the IDs ascend, the tail is the last
// record. Adds the length to *total. A cycle stops at `limit` links.
template <typename Member>
bool chat_state_list_valid(const ChatStateStore& store, const ChannelList& list, u32 limit, u32* total,
                           Member member) noexcept {
    u16 last = 0;
    u32 length = 0;
    for (u16 link = list.head; link; link = store.channels[link - 1].guild_next) {
        if (link > store.channel_capacity || ++length > limit)
            return false;
        const ChannelRecord& channel = store.channels[link - 1];
        if (!channel.occupied || !member(channel) || (last && store.channels[last - 1].id >= channel.id))
            return false;
        last = link;
    }
    *total += length;
    return list.tail == last;
}

// Each occupied channel is in exactly one list: its guild record's, the private list for guild 0, else the detached
// list. A free record links nothing, and a free guild record holds an empty list.
inline bool chat_state_store_lists_valid(const ChatStateStore& store) noexcept {
    const u32 slots = chat_state_store_channel_slot_bound(store);
    u32 occupied = 0;
    for (u32 slot = 0; slot < slots; ++slot) {
        const ChannelRecord& channel = store.channels[slot];
        if (channel.occupied)
            ++occupied;
        else if (channel.guild_next)
            return false;
    }
    for (u32 slot = 0; slot < store.guild_capacity; ++slot) {
        const GuildRecord& guild = store.guilds[slot];
        if (!guild.occupied && (guild.channel_list.head || guild.channel_list.tail))
            return false;
    }
    const u32 limit = store.channel_count;
    u32 total = 0;
    for (u32 order = 0; order < store.guild_count; ++order) {
        const GuildRecord& guild = store.guilds[store.guild_order[order]];
        if (!chat_state_list_valid(store, guild.channel_list, limit, &total,
                                   [&](const ChannelRecord& channel) noexcept { return channel.guild_id == guild.id; }))
            return false;
    }
    return occupied == store.channel_count &&
           chat_state_list_valid(store, store.private_channel_list, limit, &total,
                                 [](const ChannelRecord& channel) noexcept { return channel.guild_id == 0; }) &&
           chat_state_list_valid(store, store.detached_channel_list, limit, &total,
                                 [&](const ChannelRecord& channel) noexcept {
                                     return channel.guild_id != 0 &&
                                            !chat_state_store_find_guild(store, channel.guild_id);
                                 }) &&
           total == store.channel_count;
}

// Each live name (guild, channel, role) lies in the used part of the active bank, and their bytes are name_used
// minus name_wasted, which the store's preflights take as the live bytes.
inline bool chat_state_store_names_counted(const ChatStateStore& store) noexcept {
    const u32 base = static_cast<u32>(store.active_name_bank) * store.name_bank_bytes;
    u64 live = 0;
    bool valid = store.name_wasted <= store.name_used && store.name_used <= store.name_bank_bytes;
    const auto name = [&](u32 offset, u32 bytes) noexcept {
        valid = valid && (!bytes || (offset >= base && offset - base + bytes <= store.name_used));
        live += bytes;
    };
    for (u32 order = 0; order < store.guild_count; ++order) {
        const GuildRecord& guild = store.guilds[store.guild_order[order]];
        name(guild.name_offset, guild.name_bytes);
    }
    for (u32 order = 0; order < store.channel_count; ++order) {
        const ChannelRecord& channel = store.channels[store.channel_order[order]];
        name(channel.name_offset, channel.name_bytes);
    }
    for (u32 index = 0; index < store.role_count; ++index)
        name(store.roles[index].name_offset, store.roles[index].name_bytes);
    return valid && live == store.name_used - store.name_wasted;
}

// Links every channel of channel_order (ascending IDs) into its list, for a store built without the upserts.
inline void chat_state_store_link_lists(ChatStateStore* store) noexcept {
    for (u32 order = 0; order < store->guild_count; ++order)
        store->guilds[store->guild_order[order]].channel_list = {};
    store->private_channel_list = store->detached_channel_list = {};
    for (u32 order = 0; order < store->channel_count; ++order) {
        const u16 slot = store->channel_order[order];
        ChannelRecord& channel = store->channels[slot];
        const GuildRecord* guild = channel.guild_id ? chat_state_store_find_guild(*store, channel.guild_id) : nullptr;
        ChannelList& list = guild ? store->guilds[guild - store->guilds].channel_list
                            : channel.guild_id ? store->detached_channel_list
                                               : store->private_channel_list;
        channel.guild_next = 0;
        if (list.tail)
            store->channels[list.tail - 1].guild_next = static_cast<u16>(slot + 1);
        else
            list.head = static_cast<u16>(slot + 1);
        list.tail = static_cast<u16>(slot + 1);
    }
}

} // namespace maicord
