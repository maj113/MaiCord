#include "maicord/model/channel_reorder.hpp"

namespace maicord {
namespace {
constexpr u32 kMaximum = kGuildAdminMaximumPositions;

struct Entry {
    u64 id, parent;
    u32 position;
};
struct Ordering {
    Entry entries[kMaximum];
    u32 count;
};

bool listed(const ChatStateStore& state, const ChannelRecord& channel, u64 guild_id) noexcept {
    return channel.occupied && channel.guild_id == guild_id && !(channel.flags & kChannelObfuscatedFlag) &&
           channel_sort_group(channel.type) != ChannelSortGroup::none && chat_state_store_channel_visible(state, channel);
}
bool before(const Entry& a, const Entry& b) noexcept {
    return a.position != b.position ? a.position < b.position : a.id < b.id;
}
void sort(Entry* entries, u32 count) noexcept {
    for (u32 i = 1; i < count; ++i) {
        const Entry moving = entries[i];
        u32 j = i;
        for (; j && before(moving, entries[j - 1]); --j)
            entries[j] = entries[j - 1];
        entries[j] = moving;
    }
}
// Parent as the list shows it: a visible category of the guild, else none.
u64 visible_parent(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    const auto* parent = channel.parent_id ? chat_state_store_find_channel(state, channel.parent_id) : nullptr;
    return parent && parent->type == 4 && listed(state, *parent, channel.guild_id) ? parent->id : 0;
}
bool categories(const ChatStateStore& state, u64 guild_id, Ordering* output) noexcept {
    output->count = 0;
    for (u32 order = 0; order < state.channel_count; ++order) {
        const auto& channel = state.channels[state.channel_order[order]];
        if (channel.type != 4 || !listed(state, channel, guild_id))
            continue;
        if (output->count >= kMaximum)
            return false;
        output->entries[output->count++] = {channel.id, 0, channel.position};
    }
    sort(output->entries, output->count);
    return true;
}
// Guild-wide order of one channel group: uncategorized, then by category.
bool group(const ChatStateStore& state, u64 guild_id, ChannelSortGroup kind, const Ordering& category_order,
           Ordering* output) noexcept {
    static Ordering members;
    members.count = 0;
    for (u32 order = 0; order < state.channel_count; ++order) {
        const auto& channel = state.channels[state.channel_order[order]];
        if (channel_sort_group(channel.type) != kind || !listed(state, channel, guild_id))
            continue;
        if (members.count >= kMaximum)
            return false;
        members.entries[members.count++] = {channel.id, visible_parent(state, channel), channel.position};
    }
    sort(members.entries, members.count);
    output->count = 0;
    for (u32 c = 0; c <= category_order.count; ++c) {
        const u64 parent = c ? category_order.entries[c - 1].id : 0;
        for (u32 i = 0; i < members.count; ++i)
            if (members.entries[i].parent == parent)
                output->entries[output->count++] = members.entries[i];
    }
    return true;
}
u32 index_of(const Ordering& ordering, u64 id) noexcept {
    for (u32 i = 0; i < ordering.count; ++i)
        if (ordering.entries[i].id == id)
            return i;
    return ~0u;
}
struct Resolved {
    const ChannelRecord *source, *target;
    ChannelSortGroup group;
    u64 destination_parent;
};
bool resolve(const ChatStateStore& state, u64 source_id, u64 target_id, ChannelReorderDrop drop,
             Resolved* output) noexcept {
    if (!source_id || !target_id || source_id == target_id || static_cast<u8>(drop) > 2)
        return false;
    const auto* source = chat_state_store_find_channel(state, source_id);
    const auto* target = chat_state_store_find_channel(state, target_id);
    if (!source || !target || !source->guild_id || source->guild_id != target->guild_id ||
        !listed(state, *source, source->guild_id) || !listed(state, *target, target->guild_id))
        return false;
    const auto source_group = channel_sort_group(source->type), target_group = channel_sort_group(target->type);
    if (source_group == ChannelSortGroup::category) {
        if (target_group != ChannelSortGroup::category || drop == ChannelReorderDrop::into)
            return false;
    } else if (drop == ChannelReorderDrop::into) {
        if (target_group != ChannelSortGroup::category)
            return false;
    } else if (target_group != source_group) {
        return false;
    }
    *output = {source, target, source_group,
               drop == ChannelReorderDrop::into ? target->id : visible_parent(state, *target)};
    return true;
}
} // namespace

ChannelSortGroup channel_sort_group(u8 type) noexcept {
    switch (type) {
    case 0:
    case 5:
    case 15:
    case 16:
        return ChannelSortGroup::text;
    case 2:
    case 13:
        return ChannelSortGroup::voice;
    case 4:
        return ChannelSortGroup::category;
    default:
        return ChannelSortGroup::none;
    }
}

bool channel_reorder_valid(const ChatStateStore& state, u64 source_id, u64 target_id,
                           ChannelReorderDrop drop) noexcept {
    Resolved resolved{};
    return resolve(state, source_id, target_id, drop, &resolved);
}

u32 channel_reorder_plan(const ChatStateStore& state, u64 source_id, u64 target_id, ChannelReorderDrop drop,
                         Slice<GuildAdminPosition> output) noexcept {
    Resolved r{};
    if (!output.data || !resolve(state, source_id, target_id, drop, &r))
        return ~0u;
    static Ordering category_order, old_order, new_order;
    if (!categories(state, r.source->guild_id, &category_order))
        return ~0u;
    if (r.group == ChannelSortGroup::category)
        old_order = category_order;
    else if (!group(state, r.source->guild_id, r.group, category_order, &old_order))
        return ~0u;
    const u32 from = index_of(old_order, source_id);
    if (from == ~0u)
        return ~0u;
    const u64 source_parent = r.group == ChannelSortGroup::category ? 0 : old_order.entries[from].parent;
    // The new order: remove the source, then insert at the destination.
    new_order.count = 0;
    for (u32 i = 0; i < old_order.count; ++i)
        if (i != from)
            new_order.entries[new_order.count++] = old_order.entries[i];
    u32 at = 0;
    if (drop == ChannelReorderDrop::into) {
        // First among the category's channels of this group, or right after
        // the channels of every earlier category when it has none yet.
        u32 category_rank = 0;
        while (category_rank < category_order.count && category_order.entries[category_rank].id != r.target->id)
            ++category_rank;
        at = new_order.count;
        for (u32 i = 0; i < new_order.count; ++i) {
            const u64 parent = new_order.entries[i].parent;
            u32 rank = 0;
            if (parent)
                while (rank < category_order.count && category_order.entries[rank].id != parent)
                    ++rank;
            // Uncategorized channels lead every category.
            if (parent && rank >= category_rank) {
                at = i;
                break;
            }
        }
    } else {
        const u32 target = index_of(new_order, target_id);
        if (target == ~0u)
            return ~0u;
        at = target + (drop == ChannelReorderDrop::after ? 1u : 0u);
    }
    Entry moved = old_order.entries[from];
    moved.parent = r.group == ChannelSortGroup::category ? 0 : r.destination_parent;
    for (u32 i = new_order.count; i > at; --i)
        new_order.entries[i] = new_order.entries[i - 1];
    new_order.entries[at] = moved;
    ++new_order.count;
    const bool reparent = r.group != ChannelSortGroup::category && moved.parent != source_parent;
    u32 count = 0;
    for (u32 i = 0; i < new_order.count; ++i) {
        const auto& entry = new_order.entries[i];
        const bool is_source = entry.id == source_id;
        if (entry.position == i && !(is_source && reparent))
            continue;
        if (count >= output.count)
            return ~0u;
        GuildAdminPosition& position = output[count++];
        position = {entry.id, 0, i, 0};
        if (is_source && reparent) {
            position.parent_id = moved.parent;
            position.flags = guild_admin_position_parent;
        }
    }
    return count;
}

} // namespace maicord
