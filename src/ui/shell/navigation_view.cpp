#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild/channel_reorder.hpp"
#include "maicord/model/voice/stage_instance_store.hpp"
#include "maicord/model/voice/channel_info_store.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/sys/simd.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/model/guild/display_order.hpp"
#include "maicord/model/voice/state_store.hpp"
#include "maicord/protocol/guild/folders.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/guild/channel_controls.hpp"

// Not machine-outlined: the outliner put a call into the loop over all channels (15 % of a rebuild).
#pragma clang attribute push([[clang::no_outline]], apply_to = function)

namespace maicord {
namespace {

NavigationViewResult view_error(NavigationView* view,
                                       NavigationViewError error) noexcept {
    if (view) {
        ++view->rejected;
    }
    return NavigationViewResult{0, 0, error, false};
}

bool key_less(const ChannelSortKey& left, const ChannelSortKey& right) noexcept {
    if (left.group_rank != right.group_rank)
        return left.group_rank < right.group_rank;
    if (left.group_position != right.group_position)
        return left.group_position < right.group_position;
    if (left.group_id != right.group_id)
        return left.group_id < right.group_id;
    if (left.base_position != right.base_position)
        return left.base_position < right.base_position;
    if (left.base_id != right.base_id)
        return left.base_id < right.base_id;
    if (left.row_rank != right.row_rank)
        return left.row_rank < right.row_rank;
    if (left.row_position != right.row_position)
        return left.row_position < right.row_position;
    return left.row_id < right.row_id;
}

u16 search_slot(const ChatStateStore& state, u64 channel_id) noexcept {
    u32 low = 0;
    u32 high = state.channel_count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        if (state.channels[state.channel_order[middle]].id < channel_id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < state.channel_count && state.channels[state.channel_order[low]].id == channel_id
               ? state.channel_order[low]
               : static_cast<u16>(0xFFFF);
}
// The slots of the last channels that one rebuild searched. The channels of a server share a few parents, so
// most searches of a rebuild repeat. An entry of zero bytes is empty: find_channel does not search the ID 0.
struct SlotMemo {
    u64 ids[64];
    u16 slots[64];
};
const ChannelRecord* find_channel(const ChatStateStore& state, SlotMemo* memo, u64 channel_id) noexcept {
    if (channel_id == 0)
        return nullptr;
    const u32 at = static_cast<u32>((channel_id * 0x9E3779B97F4A7C15ull) >> 58);
    if (memo->ids[at] != channel_id) {
        memo->ids[at] = channel_id;
        memo->slots[at] = search_slot(state, channel_id);
    }
    return memo->slots[at] != 0xFFFF ? &state.channels[memo->slots[at]] : nullptr;
}

bool visual_key(const ChatStateStore& state, SlotMemo* memo, const ChannelRecord& channel,
                ChannelSortKey* key) noexcept {
    if (!channel.occupied || channel.guild_id != state.selected_guild_id) {
        return false;
    }
    if (channel_is_private(channel.type)) {
        reset_value(key);
        const auto* metadata = state.private_channels ? private_channel_store_find(*state.private_channels, channel.id) : nullptr;
        key->group_id = ~private_channel_recency(channel.id, channel.last_message_id);
        key->base_id = ~(metadata ? metadata->sort_sequence : 0ull);
        key->row_id = channel.id;
        key->row_rank = 1;
        return true;
    }
    const ChannelRecord* parent = find_channel(state, memo, channel.parent_id);
    if (parent && parent->guild_id != channel.guild_id) {
        return false;
    }
    const ChannelRecord* grandparent = parent ? find_channel(state, memo, parent->parent_id) : nullptr;
    if (grandparent && grandparent->guild_id != channel.guild_id) {
        return false;
    }

    const ChannelRecord* category = nullptr;
    if (channel.type == 4) {
        category = &channel;
    } else if (parent && parent->type == 4) {
        category = parent;
    } else if (grandparent && grandparent->type == 4) {
        category = grandparent;
    }

    const bool thread = channel.type == 10 || channel.type == 11 || channel.type == 12;
    const ChannelRecord* base = thread && parent && parent->type != 4 ? parent : &channel;
    // Uncategorized channels share one group and sort like a category's own.
    reset_value<ChannelSortKey>(key);
    key->group_rank = category ? 1 : 0;
    key->group_id = category ? category->id : 0;
    key->group_position = category ? category->position : 0;
    key->base_id = channel.type == 4 ? 0 : base->id;
    key->base_position = channel.type == 4 ? 0 : channel_list_position(base->type, base->position);
    key->row_id = channel.id;
    // Threads list the most recently joined first; unknown join times last.
    key->row_position = !thread ? channel.position : channel.thread_join_seconds ? ~channel.thread_join_seconds : ~0u;
    key->row_rank = channel.type == 4 ? 0 : thread ? 2 : 1;
    key->depth = channel.type == 4 ? 0
                 : category        ? static_cast<u8>(thread ? 2 : 1)
                                   : static_cast<u8>(thread ? 1 : 0);
    return true;
}

// Projection state of each channel slot, kept in channel_visibility_scratch.
constexpr u8 kListed = 1;          // the row is in the list
constexpr u8 kCandidate = 2;       // passes every filter, before the collapse and thread rules
constexpr u8 kHasChildren = 4;     // category: owns a channel record, listed or not
constexpr u8 kHasListedChild = 8;  // category: a child row is listed
constexpr u8 kCollapsedChild = 16; // category: a child is hidden only by the collapse
constexpr u8 kHasThreadRow = 32;   // channel: a thread row is listed under it
constexpr u8 kCategory = 64;       // a category of the selected server that is not obfuscated

// Calls visit(slot) for each slot whose flags hold a bit of mask. It tests 16 slots in one step.
template <typename Visit>
__forceinline void for_each_flagged(const u8* flags, u32 slots, u8 mask, Visit visit) noexcept {
    const __m128i wanted = _mm_set1_epi8(static_cast<char>(mask));
    u32 slot = 0;
    for (; slot + 16 <= slots; slot += 16) {
        const __m128i held =
            _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(flags + slot)), wanted);
        u32 bits = ~static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(held, _mm_setzero_si128()))) & 0xFFFFu;
        for (; bits; bits &= bits - 1)
            visit(slot + lowest_set_bit(bits));
    }
    for (; slot < slots; ++slot)
        if (flags[slot] & mask)
            visit(slot);
}

// The slot of a listed guild category (a thread's parent's parent), or 0xFFFF.
u16 category_slot(const ChatStateStore& state, SlotMemo* memo, const ChannelRecord& channel) noexcept {
    const ChannelRecord* category = find_channel(state, memo, channel.parent_id);
    if (category && category->type != 4 && channel_is_thread(channel.type))
        category = find_channel(state, memo, category->parent_id);
    return category && category->type == 4 && category->guild_id == state.selected_guild_id &&
                   (category->flags & kChannelObfuscatedFlag) == 0
               ? static_cast<u16>(category - state.channels)
               : u16{0xFFFF};
}

// A joined thread is listed under its parent when the parent is open or lists
// everything (not collapsed, not muted); otherwise only while it has mentions
// or is unread. The open thread always stays.
bool thread_listed(const ChatStateStore& state, const ChannelRecord& thread, const ChannelRecord& parent,
                   bool category_collapsed, u64 open_thread_parent_id) noexcept {
    if (thread.id == state.selected_channel_id) {
        return true;
    }
    if (!thread.thread_self_joined_known || !thread.thread_self_joined) {
        return false;
    }
    const bool open = parent.id == state.selected_channel_id || parent.id == open_thread_parent_id;
    return open || (!category_collapsed && !parent.notification_dimmed) || thread.mention_count > 0 ||
           chat_state_store_channel_unread(state, thread);
}

// A channel in a collapsed category: mentions, the open or connected channel and
// unread unmuted text channels stay; voice, muted and read text channels fold away.
bool collapsed_keeps(const ChatStateStore& state, const ChannelRecord& channel, u8 flags,
                     u64 connected_voice_id) noexcept {
    if (channel.id == state.selected_channel_id || channel.id == connected_voice_id ||
        channel.mention_count > 0 || (flags & kHasThreadRow) != 0) {
        return true;
    }
    return !channel.notification_dimmed && channel.type != 2 && channel.type != 13 &&
           (!channel_is_thread_parent(channel.type) || chat_state_store_channel_unread(state, channel));
}

bool row_changed(const GuildViewRow& left, const GuildViewRow& right) noexcept {
    return left.id != right.id || left.revision != right.revision || left.generation != right.generation ||
           left.mention_count != right.mention_count || left.selected != right.selected ||
           left.unavailable != right.unavailable || left.unread != right.unread ||
           left.has_icon != right.has_icon || left.muted != right.muted || left.folder_id != right.folder_id ||
           left.folder_color != right.folder_color || left.folder_size != right.folder_size ||
           left.folder_role != right.folder_role || left.folder_expanded != right.folder_expanded ||
           left.avatar_shape != right.avatar_shape;
}

bool row_changed(const ChannelViewRow& left, const ChannelViewRow& right) noexcept {
    return left.id != right.id || left.parent_id != right.parent_id || left.position != right.position ||
           left.revision != right.revision || left.generation != right.generation ||
           left.actions != right.actions || left.mention_count != right.mention_count ||
           left.type != right.type || left.depth != right.depth || left.selected != right.selected ||
           left.selectable != right.selectable || left.unread != right.unread || left.collapsed != right.collapsed ||
           left.voice_count != right.voice_count || left.muted != right.muted ||
           left.stage_live != right.stage_live || left.voice_subtitle != right.voice_subtitle ||
           left.voice_invite != right.voice_invite || left.controls != right.controls || left.locked != right.locked;
}
bool stage_live(const NavigationView& view, u64 channel_id, u8 type) noexcept {
    return type == 13 && view.stage_instances && stage_instance_store_find(*view.stage_instances, channel_id);
}
// Statuses belong to GUILD_VOICE channels only.
bool voice_subtitle(const NavigationView& view, u64 channel_id, u8 type) noexcept {
    const auto* extras = view.voice_extras;
    if (type != 2 || !extras)
        return false;
    if (channel_id == extras->status_channel_id)
        return true;
    const auto* info = extras->info ? voice_channel_info_store_find(*extras->info, channel_id) : nullptr;
    return info && info->status_bytes;
}
bool voice_invite(const NavigationView& view, u64 channel_id, u8 type) noexcept {
    return type == 2 && view.voice_extras && channel_id == view.voice_extras->invite_channel_id;
}

u16 voice_count(const NavigationView& view, u64 channel_id, u8 type) noexcept {
    if (!view.voice_states || (type != 2 && type != 13))
        return 0;
    return static_cast<u16>(minimum(voice_state_store_channel(*view.voice_states, channel_id).count,
                                    u32{kNavigationVoiceRowsMaximum}));
}

GuildViewRow guild_row(const ChatStateStore& state, const GuildRecord& guild) noexcept {
    GuildViewRow row{guild.id,
                            guild.revision,
                            guild.mention_count,
                            guild.generation,
                            guild.id == state.selected_guild_id,
                            guild.unavailable,
                            guild.unread_channels != 0,
                            guild.icon_variant != 0 && guild.icon_hash_bytes != 0,
                            guild.notification_muted};
    row.folder_color = kGuildFolderNoColor;
    row.folder_index = kGuildDisplayNoFolder;
    return row;
}
bool folder_expanded(const NavigationView& view, u64 id) noexcept {
    for (u32 i = 0; view.expanded_folders && i < view.expanded_folder_count; ++i)
        if (view.expanded_folders[i] == id)
            return true;
    return false;
}
void publish_row(NavigationView* view, u32 index, const GuildViewRow& row, bool* changed) noexcept {
    *changed = *changed || index >= view->guild_count || row_changed(view->guild_rows[index], row);
    view->guild_rows[index] = row;
}
// Conversations with a mention count, newest message first, as in the
// official rail. A muted conversation only counts real pings and message
// requests never count, so the count alone selects them.
struct ConversationPins {
    const ChannelRecord* channels[kNavigationConversationPinMaximum];
    u32 count, limit;
};
// The rebuild calls this for each conversation that holds a mention count, in channel ID order.
void add_conversation_pin(ConversationPins* pins, const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    if (!chat_state_store_channel_visible(state, channel))
        return;
    if (state.private_channels)
        if (const auto* metadata = private_channel_store_find(*state.private_channels, channel.id);
            metadata && private_channel_is_request(*metadata))
            return;
    u32 at = pins->count;
    while (at && pins->channels[at - 1]->last_message_id < channel.last_message_id)
        --at;
    if (at >= pins->limit)
        return;
    for (u32 i = minimum(pins->count, pins->limit - 1); i > at; --i)
        pins->channels[i] = pins->channels[i - 1];
    pins->channels[at] = &channel;
    pins->count = minimum(pins->count + 1, pins->limit);
}
u32 publish_conversation_pins(NavigationView* view, const ChatStateStore& state, const ConversationPins& pins,
                              bool* changed) noexcept {
    for (u32 i = 0; i < pins.count; ++i) {
        const ChannelRecord& channel = *pins.channels[i];
        GuildViewRow row{channel.id, channel.revision, channel.mention_count, channel.generation,
                         channel.id == state.selected_channel_id, false, true};
        row.folder_color = kGuildFolderNoColor;
        row.folder_index = kGuildDisplayNoFolder;
        row.folder_role = GuildFolderRole::conversation;
        row.avatar_shape = ui_private_avatar_shape(&state, channel.id, channel.type);
        publish_row(view, i, row, changed);
    }
    return pins.count;
}
// Display order with folder headers after `first` leading rows. When headers
// would not fit, the flat join order is used; it always fits because pins
// leave room for state.guild_count.
u32 build_guild_rows(NavigationView* view, const ChatStateStore& state, u32 first, bool* changed) noexcept {
    constexpr u32 kScratch = 1024;
    // 16 KiB on the stack made every rebuild walk its guard pages. The rebuild runs on
    // the UI thread, is not re-entered, and touches only as many entries as there are guilds.
    static GuildDisplayEntry display[kScratch];
    const auto* folders = view->guild_folders;
    u32 count = state.guild_count <= kScratch ? guild_display_order_build(folders, state, {display, kScratch}) : 0;
    u32 headers = 0;
    for (u32 i = 0; i < count; ++i)
        headers += display[i].folder != kGuildDisplayNoFolder && (!i || display[i - 1].folder != display[i].folder);
    if (count != state.guild_count || first + count + headers > view->guild_capacity) {
        folders = nullptr;
        count = 0;
        for (u32 order = 0; order < state.guild_count && order < kScratch; ++order)
            display[count++] = {state.guilds[state.guild_order[order]].id, kGuildDisplayNoFolder,
                                state.guild_order[order]};
    }
    // Each entry names a live guild of `state` by its slot (both builds above take it from the state).
    u32 rows = first;
    for (u32 i = 0; i < count;) {
        const u16 folder = folders ? display[i].folder : kGuildDisplayNoFolder;
        if (folder == kGuildDisplayNoFolder) {
            publish_row(view, rows++, guild_row(state, state.guilds[display[i].slot]), changed);
            ++i;
            continue;
        }
        u32 end = i;
        while (end < count && display[end].folder == folder)
            ++end;
        const auto& record = folders->folders[folder];
        GuildViewRow header{};
        header.id = guild_folder_row_key(*folders, folder);
        header.generation = 1;
        header.folder_id = header.id;
        header.folder_color = record.has_color ? record.color : kGuildFolderNoColor;
        header.folder_size = static_cast<u16>(end - i);
        header.folder_index = folder;
        header.folder_role = GuildFolderRole::header;
        header.folder_expanded = folder_expanded(*view, header.id);
        const u32 header_index = rows++;
        for (u32 j = i; j < end; ++j) {
            auto row = guild_row(state, state.guilds[display[j].slot]);
            row.folder_id = header.id;
            row.folder_color = header.folder_color;
            row.folder_index = folder;
            row.folder_role = GuildFolderRole::member;
            row.folder_expanded = header.folder_expanded;
            header.mention_count += row.mention_count;
            header.unread = header.unread || (!row.unavailable && row.unread);
            header.selected = header.selected || row.selected;
            header.revision = header.revision * 31u + row.revision + (row.has_icon ? 1u : 0u);
            publish_row(view, rows++, row, changed);
        }
        header.revision += header.folder_expanded ? 1u : 0u;
        publish_row(view, header_index, header, changed);
        i = end;
    }
    *changed = *changed || rows != view->guild_count;
    return rows;
}

void sync_voice_revisions(NavigationView* view) noexcept {
    view->source_voice_revision = view->voice_states ? view->voice_states->revision : 0;
    view->source_stage_revision = view->stage_instances ? view->stage_instances->revision : 0;
    view->source_extras_revision = view->voice_extras ? view->voice_extras->revision : 0;
}

} // namespace

bool navigation_view_initialize(NavigationView* view, const Slice<GuildViewRow>& guild_rows,
                                const Slice<ChannelViewRow>& channel_rows, const Slice<u16>& channel_scratch,
                                const Slice<u8>& channel_visibility_scratch,
                                const Slice<ChannelSortKey>& channel_sort_keys) noexcept {
    if (!view || !guild_rows.data || guild_rows.count == 0 || !channel_rows.data ||
        channel_rows.count == 0 || !channel_scratch.data || channel_scratch.count != channel_rows.count ||
        !channel_visibility_scratch.data || channel_visibility_scratch.count == 0 ||
        channel_visibility_scratch.count > 65'535 || guild_rows.count > 65'535 ||
        channel_rows.count > 65'535 || !channel_sort_keys.data ||
        channel_sort_keys.count != channel_rows.count) {
        return false;
    }
    reset_value<NavigationView>(view);
    view->guild_rows = guild_rows.data;
    view->channel_rows = channel_rows.data;
    view->channel_scratch = channel_scratch.data;
    view->channel_visibility_scratch = channel_visibility_scratch.data;
    view->channel_sort_keys = channel_sort_keys.data;
    view->guild_capacity = guild_rows.count;
    view->channel_capacity = channel_rows.count;
    view->channel_visibility_capacity = channel_visibility_scratch.count;
    return true;
}

NavigationViewResult navigation_view_rebuild(NavigationView* view,
                                                            const ChatStateStore& state) noexcept {
    if (!view || !view->guild_rows || !view->channel_rows || !view->channel_scratch ||
        !view->channel_visibility_scratch || !view->channel_sort_keys || !state.guilds || !state.channels ||
        !state.guild_order || !state.channel_order || state.guild_count > view->guild_capacity ||
        chat_state_store_channel_slot_bound(state) > view->channel_visibility_capacity) {
        return view_error(view, state.guild_count > (view ? view->guild_capacity : 0)
                                    ? NavigationViewError::guild_capacity
                                    : NavigationViewError::invalid_input);
    }

    // Slots at or above the bound are never occupied, so are never marked.
    const u32 slots = chat_state_store_channel_slot_bound(state);
    u8* const flags = view->channel_visibility_scratch;
    clear_bytes(flags, slots);
    SlotMemo memo;
    clear_bytes(memo.ids, sizeof(memo.ids));
    ConversationPins pins;
    pins.count = 0;
    pins.limit = minimum(kNavigationConversationPinMaximum, view->guild_capacity - state.guild_count);
    // "Hide muted channels" drops own- or category-muted rows (a server-wide
    // mute alone does not count); the open channel and its category remain.
    const GuildRecord* selected_guild =
        state.home_selected ? nullptr : chat_state_store_find_guild(state, state.selected_guild_id);
    const bool hide_muted = selected_guild && selected_guild->hide_muted_channels;
    // Channel opt-in ("Show all channels" off) lists opted-in channels only;
    // a category shows through its listed children.
    const bool opt_in = selected_guild && selected_guild->channel_opt_in;
    // The parent of the open thread counts as open.
    const ChannelRecord* open_channel = chat_state_store_find_channel(state, state.selected_channel_id);
    const u64 open_thread_parent = open_channel && channel_is_thread(open_channel->type) ? open_channel->parent_id : 0;
    // Marks one channel of the selected server's list. The passes after the walks read the marks.
    const auto mark = [&](const ChannelRecord& channel) noexcept {
        const u16 slot = static_cast<u16>(&channel - state.channels);
        if (channel.guild_id != state.selected_guild_id)
            return;
        const bool thread = channel_is_thread(channel.type);
        u16 owner = 0xFFFF;
        if (channel.type == 4) {
            if ((channel.flags & kChannelObfuscatedFlag) == 0)
                flags[slot] |= kCategory;
        } else if (!thread) {
            owner = category_slot(state, &memo, channel);
            if (owner != 0xFFFF)
                flags[owner] |= kHasChildren;
        }
        if (hide_muted && channel.notification_dimmed && channel.id != state.selected_channel_id)
            return;
        if (opt_in && !channel.opted_in && channel.id != state.selected_channel_id)
            return;
        if ((channel_is_private(channel.type) && !state.home_selected) ||
            (channel.flags & kChannelObfuscatedFlag) != 0 ||
            (!chat_state_store_channel_visible(state, channel) &&
             !(view->show_hidden && chat_state_store_channel_hidden(state, channel)))) {
            return;
        }
        // Message requests stay out of the conversation list (their page lists them); an open one stays while it
        // is the conversation shown.
        if (channel_is_private(channel.type) && state.private_channels &&
            (channel.id != state.selected_channel_id || view->friends_mode)) {
            const auto* metadata = private_channel_store_find(*state.private_channels, channel.id);
            if (metadata && private_channel_is_request(*metadata))
                return;
        }
        // Archived and unlisted threads (kept in state for discovery) stay off
        // the channel rail; the open conversation stays.
        if (thread && channel.id != state.selected_channel_id &&
            ((channel.thread_archived_known && channel.thread_archived) ||
             (channel.thread_active_known && !channel.thread_listed_active)))
            return;
        flags[slot] |= kCandidate;
        if (thread) {
            // A listed thread keeps its parent in the list.
            const ChannelRecord* parent = find_channel(state, &memo, channel.parent_id);
            if (!parent)
                return;
            owner = category_slot(state, &memo, channel);
            if (thread_listed(state, channel, *parent, owner != 0xFFFF && state.channels[owner].collapsed,
                              open_thread_parent)) {
                flags[slot] |= kListed;
                flags[parent - state.channels] |= kHasThreadRow;
            }
        } else if (channel.type != 4 && (owner == 0xFFFF || !state.channels[owner].collapsed)) {
            flags[slot] |= kListed;
            if (owner != 0xFFFF)
                flags[owner] |= kHasListedChild;
        }
    };
    // The private list gives the conversation pins and, with no server selected, the rows. Only the selected
    // server's list is read for its rows, not every channel record.
    for (auto* channel = chat_state_store_first_guild_channel(state, 0); channel;
         channel = chat_state_store_next_guild_channel(state, *channel)) {
        if (channel_is_private(channel->type) && channel->mention_count)
            add_conversation_pin(&pins, state, *channel);
        if (!state.selected_guild_id)
            mark(*channel);
    }
    if (state.selected_guild_id)
        for (auto* channel = chat_state_store_first_guild_channel(state, state.selected_guild_id); channel;
             channel = chat_state_store_next_guild_channel(state, *channel))
            mark(*channel);
    // The channels that the pass above did not list: in a collapsed category, or not a candidate but the parent
    // of a listed thread. A collapsed category folds its channels away except the ones that matter now. What
    // folds away keeps the header.
    for_each_flagged(flags, slots, kCandidate | kHasThreadRow, [&](u32 slot) noexcept {
        const ChannelRecord& channel = state.channels[slot];
        if ((flags[slot] & kListed) || channel_is_thread(channel.type) || channel.type == 4)
            return;
        const u16 owner = category_slot(state, &memo, channel);
        if (owner == 0xFFFF || !state.channels[owner].collapsed ||
            collapsed_keeps(state, channel, flags[slot], view->connected_voice_channel_id)) {
            flags[slot] |= kListed;
            if (owner != 0xFFFF)
                flags[owner] |= kHasListedChild;
        } else {
            flags[owner] |= kCollapsedChild;
        }
    });
    // Categories. A category lists while a child row does or the collapse is
    // all that hides one; an empty one stays for members who manage channels.
    constexpr u32 kManageView = channel_action_view | channel_action_manage_channel;
    for_each_flagged(flags, slots, kCategory, [&](u32 slot) noexcept {
        const u8 own = flags[slot];
        if ((own & (kHasListedChild | kCollapsedChild)) ||
            ((own & kCandidate) && !(own & kHasChildren) && (state.channels[slot].actions & kManageView) == kManageView)) {
            flags[slot] |= kListed;
        }
    });
    u32 channel_count = 0;
    // A listed slot outside the selected list is the parent of a listed thread in another server. That thread
    // fails visual_key below, so the rebuild returns invalid_hierarchy as it did.
    for (auto* channel = chat_state_store_first_guild_channel(state, state.selected_guild_id); channel;
         channel = chat_state_store_next_guild_channel(state, *channel)) {
        const u16 slot = static_cast<u16>(channel - state.channels);
        if (!(flags[slot] & kListed)) {
            continue;
        }
        if (channel_count >= view->channel_capacity) {
            return view_error(view, NavigationViewError::channel_capacity);
        }
        ChannelSortKey key{};
        if (!visual_key(state, &memo, state.channels[slot], &key)) {
            return view_error(view, NavigationViewError::invalid_hierarchy);
        }
        // Rows and keys are sized for one projection, not every store slot.
        key.slot = slot;
        // Binary insertion into the sorted row order: one side of each comparison stays this key. The keys
        // are unique (row_id), so the order does not depend on the sort.
        u32 low = 0, high = channel_count;
        while (low < high) {
            const u32 middle = (low + high) / 2;
            if (key_less(key, view->channel_sort_keys[view->channel_scratch[middle]]))
                high = middle;
            else
                low = middle + 1;
        }
        copy_bytes(view->channel_scratch + low + 1, view->channel_scratch + low,
                   (channel_count - low) * static_cast<u32>(sizeof(u16)));
        view->channel_scratch[low] = static_cast<u16>(channel_count);
        view->channel_sort_keys[channel_count] = key;
        ++channel_count;
    }

    // The events row leads a server's list when the host asks for it.
    const bool requests_lead = view->requests_row && state.home_selected;
    const u32 lead = (view->events_row && !state.home_selected && state.selected_guild_id) || requests_lead ? 1u : 0u;
    if (channel_count + lead > view->channel_capacity)
        return view_error(view, NavigationViewError::channel_capacity);
    bool guild_changed = view->selected_guild_id != state.selected_guild_id;
    bool channel_changed = view->channel_count != channel_count + lead ||
                           view->source_events_revision != view->events_revision ||
                           view->source_requests_revision != view->requests_revision ||
                           view->selected_guild_id != state.selected_guild_id ||
                           view->selected_channel_id != state.selected_channel_id;
    for (u32 order = 0; order < state.guild_count; ++order)
        if (state.guild_order[order] >= state.guild_capacity)
            return view_error(view, NavigationViewError::invalid_input);
    const u32 guild_rows =
        build_guild_rows(view, state, publish_conversation_pins(view, state, pins, &guild_changed), &guild_changed);
    if (requests_lead) {
        ChannelViewRow row{};
        row.id = kNavigationRequestsRowId;
        row.type = kNavigationRequestsRowType;
        row.revision = view->requests_revision;
        row.mention_count = view->requests_mode ? 0 : view->request_count;
        row.selected = view->requests_mode;
        row.selectable = false;
        channel_changed = channel_changed || row_changed(view->channel_rows[0], row);
        view->channel_rows[0] = row;
    } else if (lead) {
        u32 used = 0;
        const auto add = [&](const char* text) noexcept {
            while (*text && used < sizeof(view->events_label))
                view->events_label[used++] = *text++;
        };
        if (view->event_count) {
            char digits[10];
            const u32 count = decimal_write(digits, view->event_count);
            for (u32 i = 0; i < count && used < sizeof(view->events_label); ++i)
                view->events_label[used++] = digits[i];
            add(view->event_count == 1 ? " Event" : " Events");
        } else {
            add("Events");
        }
        view->events_label_bytes = static_cast<u8>(used);
        ChannelViewRow row{};
        row.id = kNavigationEventsRowId;
        row.type = kNavigationEventsRowType;
        row.revision = view->events_revision;
        row.selectable = false;
        channel_changed = channel_changed || row_changed(view->channel_rows[0], row);
        view->channel_rows[0] = row;
    }
    for (u32 order = 0; order < channel_count; ++order) {
        const ChannelSortKey& key = view->channel_sort_keys[view->channel_scratch[order]];
        const ChannelRecord& channel = state.channels[key.slot];
        const bool locked = chat_state_store_channel_hidden(state, channel);
        const ChannelViewRow row{
            channel.id,
            channel.parent_id,
            channel_is_thread(channel.type) ? 0u : channel.position,
            channel.revision,
            channel.actions,
            channel.mention_count,
            channel.generation,
            channel.type,
            key.depth,
            channel.id == state.selected_channel_id && !(state.home_selected && view->friends_mode),
            // Voice rows join the call on click; their chat opens from the call. A locked row only explains itself.
            !locked && (channel.type == 4 || channel.type == 15 || channel.type == 16 ||
                        (channel_is_message_capable(channel.type) && !channel_is_voice_chat(channel.type))),
            chat_state_store_channel_unread(state, channel),
            channel.collapsed,
            voice_count(*view, channel.id, channel.type),
            channel.notification_dimmed,
            stage_live(*view, channel.id, channel.type),
            voice_subtitle(*view, channel.id, channel.type),
            voice_invite(*view, channel.id, channel.type),
            static_cast<u8>((channel_controls_can_invite(state, channel) ? channel_row_invite : 0u) |
                            (channel_controls_can_edit(channel) ? channel_row_settings : 0u) |
                            (channel_controls_can_create_in(channel) ? channel_row_create : 0u)),
            locked,
        };
        channel_changed = channel_changed || row_changed(view->channel_rows[order + lead], row);
        view->channel_rows[order + lead] = row;
    }
    channel_count += lead;
    for (u32 order = guild_rows; order < view->guild_count; ++order) {
        reset_value<GuildViewRow>(&view->guild_rows[order]);
    }
    for (u32 order = channel_count; order < view->channel_count; ++order) {
        reset_value<ChannelViewRow>(&view->channel_rows[order]);
    }

    view->guild_count = guild_rows;
    view->channel_count = channel_count;
    view->selected_guild_id = state.selected_guild_id;
    view->selected_channel_id = state.selected_channel_id;
    view->source_guild_mutations = state.guild_mutations;
    view->source_channel_mutations = state.channel_mutations;
    view->source_selection_revision = state.selection_revision;
    view->connected_voice_stale = false;
    sync_voice_revisions(view);
    view->source_folder_revision = view->folder_revision;
    view->source_events_revision = view->events_revision;
    view->source_requests_revision = view->requests_revision;
    const bool changed = guild_changed || channel_changed;
    saturating_increment_if(guild_changed, view->guild_revision);
    saturating_increment_if(channel_changed, view->channel_revision);
    saturating_increment_if(changed, view->revision);
    return NavigationViewResult{view->guild_count, view->channel_count,
                                       NavigationViewError::none, changed};
}

const GuildViewRow* navigation_view_find_guild(const NavigationView& view,
                                                              u64 guild_id) noexcept {
    for (u32 index = 0; index < view.guild_count; ++index) {
        if (view.guild_rows[index].id == guild_id) {
            return &view.guild_rows[index];
        }
    }
    return nullptr;
}

const ChannelViewRow* navigation_view_find_channel(const NavigationView& view,
                                                                  u64 channel_id) noexcept {
    for (u32 index = 0; index < view.channel_count; ++index) {
        if (view.channel_rows[index].id == channel_id) {
            return &view.channel_rows[index];
        }
    }
    return nullptr;
}

bool navigation_view_refresh_voice(NavigationView* view) noexcept {
    if (!view || view->channel_count > view->channel_capacity || (view->channel_count && !view->channel_rows))
        return false;
    bool changed = false;
    for (u32 i = 0; i < view->channel_count; ++i) {
        auto& row = view->channel_rows[i];
        const u16 count = voice_count(*view, row.id, row.type);
        const bool live = stage_live(*view, row.id, row.type);
        const bool subtitle = voice_subtitle(*view, row.id, row.type);
        const bool invite = voice_invite(*view, row.id, row.type);
        changed = changed || count != row.voice_count || live != row.stage_live || subtitle != row.voice_subtitle ||
                  invite != row.voice_invite;
        row.voice_count = count;
        row.stage_live = live;
        row.voice_subtitle = subtitle;
        row.voice_invite = invite;
    }
    sync_voice_revisions(view);
    saturating_increment_if(changed, view->channel_revision);
    saturating_increment_if(changed, view->revision);
    return changed;
}
} // namespace maicord
#pragma clang attribute pop
