#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/guild_admin.hpp"

namespace maicord {

// Channel list drag-and-drop in the official client's ordering. Text-like channels (text,
// announcement, forum, media), voice-like channels (voice, stage) and
// categories are three independent sort groups. A group's guild-wide order is
// its uncategorized channels, then each category's channels, categories in
// (position, id) order and channels in (position, id) order. A move reassigns
// positions 0..n-1 along the new order and sends only the changed entries
// (as the official client computes position deltas), plus parent_id for a channel that
// changes category. Hidden channels take no part.

enum class ChannelReorderDrop : u8 {
    before, // above the target row (same group, or a category for categories)
    after,  // below the target row
    into,   // onto a category header: first in that category's group
};

enum class ChannelSortGroup : u8 { none, text, voice, category };
ChannelSortGroup channel_sort_group(u8 type) noexcept;

// Whether a drop is meaningful (same guild, compatible groups, visible rows).
// Permission checks belong to the caller.
bool channel_reorder_valid(const ChatStateStore&, u64 source_id, u64 target_id, ChannelReorderDrop) noexcept;

// Writes the PATCH /guilds/{id}/channels entries. Returns the entry count,
// zero when the layout would not change, ~0u when the move is invalid or the
// output is too small.
u32 channel_reorder_plan(const ChatStateStore&, u64 source_id, u64 target_id, ChannelReorderDrop,
                         Slice<GuildAdminPosition> output) noexcept;

} // namespace maicord
