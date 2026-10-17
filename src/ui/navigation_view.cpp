#include "maicord/ui/navigation_view.hpp"
#include "maicord/model/channel_reorder.hpp"
#include "maicord/model/stage_instance_store.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/guild_display_order.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/protocol/guild_folders.hpp"
#include "maicord/ui/text_style.hpp"

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
    if (left.group_rank != right.group_rank) {
        return left.group_rank < right.group_rank;
    }
    if (left.group_position != right.group_position) {
        return left.group_position < right.group_position;
    }
    if (left.group_id != right.group_id) {
        return left.group_id < right.group_id;
    }
    if (left.base_position != right.base_position) {
        return left.base_position < right.base_position;
    }
    if (left.base_id != right.base_id) {
        return left.base_id < right.base_id;
    }
    if (left.row_rank != right.row_rank) {
        return left.row_rank < right.row_rank;
    }
    if (left.row_position != right.row_position) {
        return left.row_position < right.row_position;
    }
    return left.row_id < right.row_id;
}

bool visual_key(const ChatStateStore& state, const ChannelRecord& channel,
                ChannelSortKey* key) noexcept {
    if (!channel.occupied || channel.guild_id != state.selected_guild_id) {
        return false;
    }
    if (channel_is_private(channel.type)) {
        *key = {};
        const auto* metadata = state.private_channels ? private_channel_store_find(*state.private_channels, channel.id) : nullptr;
        key->group_id = ~private_channel_recency(channel.id, channel.last_message_id);
        key->base_id = ~(metadata ? metadata->sort_sequence : 0ull);
        key->row_id = channel.id;
        key->row_rank = 1;
        return true;
    }
    const ChannelRecord* parent =
        channel.parent_id != 0 ? chat_state_store_find_channel(state, channel.parent_id) : nullptr;
    if (parent && parent->guild_id != channel.guild_id) {
        return false;
    }
    const ChannelRecord* grandparent =
        parent && parent->parent_id != 0 ? chat_state_store_find_channel(state, parent->parent_id)
                                         : nullptr;
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
    *key = ChannelSortKey{};
    key->group_rank = category ? 1 : 0;
    key->group_id = category ? category->id : 0;
    key->group_position = category ? category->position : 0;
    key->base_id = channel.type == 4 ? 0 : base->id;
    key->base_position = channel.type == 4 ? 0 : channel_list_position(base->type, base->position);
    key->row_id = channel.id;
    key->row_position = channel.position;
    key->row_rank = channel.type == 4 ? 0 : thread ? 2 : 1;
    key->depth = channel.type == 4 ? 0
                 : category        ? static_cast<u8>(thread ? 2 : 1)
                                   : static_cast<u8>(thread ? 1 : 0);
    return true;
}

u16 channel_slot(const ChatStateStore& state, u64 channel_id) noexcept {
    u32 low = 0;
    u32 high = state.channel_count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        const u16 slot = state.channel_order[middle];
        if (state.channels[slot].id < channel_id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < state.channel_count && state.channels[state.channel_order[low]].id == channel_id
               ? state.channel_order[low]
               : static_cast<u16>(0xFFFF);
}

void mark_visible_category(NavigationView* view, const ChatStateStore& state,
                           const ChannelRecord& channel) noexcept {
    u16 parent_slot = channel.parent_id != 0 ? channel_slot(state, channel.parent_id) : 0xFFFF;
    if (parent_slot == 0xFFFF || parent_slot >= state.channel_capacity) {
        return;
    }
    const ChannelRecord* category = &state.channels[parent_slot];
    if (category->type != 4 && (channel.type == 10 || channel.type == 11 || channel.type == 12) &&
        category->parent_id != 0) {
        parent_slot = channel_slot(state, category->parent_id);
        if (parent_slot == 0xFFFF || parent_slot >= state.channel_capacity) {
            return;
        }
        category = &state.channels[parent_slot];
    }
    if (category->type == 4 && category->guild_id == state.selected_guild_id &&
        (category->flags & kChannelObfuscatedFlag) == 0) {
        view->channel_visibility_scratch[parent_slot] = 1;
    }
}

bool row_changed(const GuildViewRow& left, const GuildViewRow& right) noexcept {
    return left.id != right.id || left.revision != right.revision || left.generation != right.generation ||
           left.mention_count != right.mention_count || left.selected != right.selected ||
           left.unavailable != right.unavailable || left.unread != right.unread ||
           left.has_icon != right.has_icon || left.muted != right.muted || left.folder_id != right.folder_id ||
           left.folder_color != right.folder_color || left.folder_size != right.folder_size ||
           left.folder_role != right.folder_role || left.folder_expanded != right.folder_expanded;
}

bool row_changed(const ChannelViewRow& left, const ChannelViewRow& right) noexcept {
    return left.id != right.id || left.parent_id != right.parent_id || left.position != right.position ||
           left.revision != right.revision || left.generation != right.generation ||
           left.actions != right.actions || left.mention_count != right.mention_count ||
           left.type != right.type || left.depth != right.depth || left.selected != right.selected ||
           left.selectable != right.selectable || left.unread != right.unread || left.collapsed != right.collapsed ||
           left.voice_count != right.voice_count || left.muted != right.muted ||
           left.stage_live != right.stage_live;
}
bool stage_live(const NavigationView& view, u64 channel_id, u8 type) noexcept {
    return type == 13 && view.stage_instances && stage_instance_store_find(*view.stage_instances, channel_id);
}

u16 voice_count(const NavigationView& view, u64 channel_id, u8 type) noexcept {
    if (!view.voice_states || (type != 2 && type != 13))
        return 0;
    return static_cast<u16>(minimum(voice_state_store_channel_count(*view.voice_states, channel_id),
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
// Display order with folder headers. When headers would not fit, the flat
// join order is used; it always fits because state.guild_count is checked.
u32 build_guild_rows(NavigationView* view, const ChatStateStore& state, bool* changed) noexcept {
    constexpr u32 kScratch = 1024;
    GuildDisplayEntry display[kScratch];
    const auto* folders = view->guild_folders;
    u32 count = state.guild_count <= kScratch ? guild_display_order_build(folders, state, {display, kScratch}) : 0;
    u32 headers = 0;
    for (u32 i = 0; i < count; ++i)
        headers += display[i].folder != kGuildDisplayNoFolder && (!i || display[i - 1].folder != display[i].folder);
    if (count != state.guild_count || count + headers > view->guild_capacity) {
        folders = nullptr;
        count = 0;
        for (u32 order = 0; order < state.guild_count && order < kScratch; ++order)
            display[count++] = {state.guilds[state.guild_order[order]].id, kGuildDisplayNoFolder};
    }
    u32 rows = 0;
    for (u32 i = 0; i < count;) {
        const u16 folder = folders ? display[i].folder : kGuildDisplayNoFolder;
        if (folder == kGuildDisplayNoFolder) {
            const auto* guild = chat_state_store_find_guild(state, display[i].guild_id);
            if (guild)
                publish_row(view, rows++, guild_row(state, *guild), changed);
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
            const auto* guild = chat_state_store_find_guild(state, display[j].guild_id);
            if (!guild)
                continue;
            auto row = guild_row(state, *guild);
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

bool initialize_view(NavigationView* view, Slice<GuildViewRow> guild_rows,
                                        Slice<ChannelViewRow> channel_rows,
                                        Slice<u16> channel_scratch, Slice<u8> channel_visibility_scratch,
                                        Slice<ChannelSortKey> channel_sort_keys, bool sparse) noexcept {
    if (!view || !guild_rows.data || guild_rows.count == 0 || !channel_rows.data ||
        channel_rows.count == 0 || !channel_scratch.data || channel_scratch.count != channel_rows.count ||
        !channel_visibility_scratch.data || channel_visibility_scratch.count == 0 ||
        channel_visibility_scratch.count > 65'535 || guild_rows.count > 65'535 ||
        channel_rows.count > 65'535 || !channel_sort_keys.data ||
        channel_sort_keys.count != channel_rows.count ||
        (sparse && (view->guild_count || view->channel_count))) {
        return false;
    }
    *view = NavigationView{};
    view->guild_rows = guild_rows.data;
    view->channel_rows = channel_rows.data;
    view->channel_scratch = channel_scratch.data;
    view->channel_visibility_scratch = channel_visibility_scratch.data;
    view->channel_sort_keys = channel_sort_keys.data;
    view->guild_capacity = guild_rows.count;
    view->channel_capacity = channel_rows.count;
    view->channel_visibility_capacity = channel_visibility_scratch.count;
    if (sparse) return true;
    for (u32 index = 0; index < guild_rows.count; ++index) {
        guild_rows[index] = GuildViewRow{};
    }
    for (u32 index = 0; index < channel_rows.count; ++index) {
        channel_rows[index] = ChannelViewRow{};
        channel_scratch[index] = 0xFFFF;
        channel_sort_keys[index] = ChannelSortKey{};
    }
    for (u32 index = 0; index < channel_visibility_scratch.count; ++index) {
        channel_visibility_scratch[index] = 0;
    }
    return true;
}

} // namespace

bool navigation_view_initialize(NavigationView* view, Slice<GuildViewRow> guild_rows,
                               Slice<ChannelViewRow> channel_rows, Slice<u16> channel_scratch,
                               Slice<u8> channel_visibility_scratch,
                               Slice<ChannelSortKey> channel_sort_keys) noexcept {
    return initialize_view(view, guild_rows, channel_rows, channel_scratch,
                           channel_visibility_scratch, channel_sort_keys, false);
}

bool navigation_view_initialize_sparse(NavigationView* view, Slice<GuildViewRow> guild_rows,
                                      Slice<ChannelViewRow> channel_rows, Slice<u16> channel_scratch,
                                      Slice<u8> channel_visibility_scratch,
                                      Slice<ChannelSortKey> channel_sort_keys) noexcept {
    return initialize_view(view, guild_rows, channel_rows, channel_scratch,
                           channel_visibility_scratch, channel_sort_keys, true);
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
    for (u32 index = 0; index < slots; ++index) {
        view->channel_visibility_scratch[index] = 0;
    }
    for (u32 order = 0; order < state.channel_count; ++order) {
        if (state.channel_order[order] >= state.channel_capacity) {
            return view_error(view, NavigationViewError::invalid_input);
        }
    }
    // "Hide muted channels" drops own- or category-muted rows (a server-wide
    // mute alone does not count); the open channel and its category remain.
    const GuildRecord* selected_guild =
        state.home_selected ? nullptr : chat_state_store_find_guild(state, state.selected_guild_id);
    const bool hide_muted = selected_guild && selected_guild->hide_muted_channels;
    // Channel opt-in ("Show all channels" off) lists opted-in channels only;
    // a category shows through its listed children.
    const bool opt_in = selected_guild && selected_guild->channel_opt_in;
    for (u32 order = 0; order < state.channel_count; ++order) {
        const u16 slot = state.channel_order[order];
        const ChannelRecord& channel = state.channels[slot];
        if (hide_muted && channel.notification_dimmed && channel.id != state.selected_channel_id &&
            channel.guild_id == state.selected_guild_id)
            continue;
        if (opt_in && !channel.opted_in && channel.id != state.selected_channel_id &&
            channel.guild_id == state.selected_guild_id)
            continue;
        if (channel.guild_id != state.selected_guild_id ||
            (channel_is_private(channel.type) && !state.home_selected) ||
            (channel.flags & kChannelObfuscatedFlag) != 0 ||
            !chat_state_store_channel_visible(state, channel)) {
            continue;
        }
        // Message requests stay out of the conversation list (and the list
        // shows only them in requests mode); the open conversation stays.
        if (channel_is_private(channel.type) && state.private_channels && channel.id != state.selected_channel_id) {
            const auto* metadata = private_channel_store_find(*state.private_channels, channel.id);
            if ((metadata && private_channel_is_request(*metadata)) != view->requests_mode)
                continue;
        }
        // Archived and unlisted threads (kept in state for discovery) stay off
        // the channel rail; the open conversation stays.
        if (channel_is_thread(channel.type) && channel.id != state.selected_channel_id &&
            ((channel.thread_archived_known && channel.thread_archived) ||
             (channel.thread_active_known && !channel.thread_listed_active)))
            continue;
        view->channel_visibility_scratch[slot] = 1;
        if (channel.type != 4) {
            mark_visible_category(view, state, channel);
        }
    }
    u32 channel_count = 0;
    for (u32 order = 0; order < state.channel_count; ++order) {
        const u16 slot = state.channel_order[order];
        if (view->channel_visibility_scratch[slot] == 0) {
            continue;
        }
        // Collapse only the projection. Keep channel state and the selected
        // conversation intact so expanding does not fetch history again.
        const auto* parent = chat_state_store_find_channel(state, state.channels[slot].parent_id);
        bool hidden = false;
        for (u32 depth = 0; parent && depth < state.channel_count; ++depth) {
            if (parent->type == 4 && parent->collapsed) { hidden = true; break; }
            parent = chat_state_store_find_channel(state, parent->parent_id);
        }
        if (hidden) continue;
        if (channel_count >= view->channel_capacity) {
            return view_error(view, NavigationViewError::channel_capacity);
        }
        ChannelSortKey key{};
        if (!visual_key(state, state.channels[slot], &key)) {
            return view_error(view, NavigationViewError::invalid_hierarchy);
        }
        // Rows and keys are sized for one projection, not every store slot.
        key.slot = slot;
        view->channel_scratch[channel_count] = static_cast<u16>(channel_count);
        view->channel_sort_keys[channel_count] = key;
        ++channel_count;
    }
    heap_sort(view->channel_scratch, channel_count,
              [&](u16 left, u16 right) noexcept {
                  return key_less(view->channel_sort_keys[left], view->channel_sort_keys[right]);
              });

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
    const u32 guild_rows = build_guild_rows(view, state, &guild_changed);
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
        const ChannelViewRow row{
            channel.id,
            channel.parent_id,
            channel.position,
            channel.revision,
            channel.actions,
            channel.mention_count,
            channel.generation,
            channel.type,
            key.depth,
            channel.id == state.selected_channel_id,
            // Voice rows join the call on click; their chat opens from the call.
            channel.type == 4 || channel.type == 15 || channel.type == 16 ||
                (channel_is_message_capable(channel.type) && !channel_is_voice_chat(channel.type)),
            chat_state_store_channel_unread(state, channel),
            channel.collapsed,
            voice_count(*view, channel.id, channel.type),
            channel.notification_dimmed,
            stage_live(*view, channel.id, channel.type),
        };
        channel_changed = channel_changed || row_changed(view->channel_rows[order + lead], row);
        view->channel_rows[order + lead] = row;
    }
    channel_count += lead;
    for (u32 order = guild_rows; order < view->guild_count; ++order) {
        view->guild_rows[order] = GuildViewRow{};
    }
    for (u32 order = channel_count; order < view->channel_count; ++order) {
        view->channel_rows[order] = ChannelViewRow{};
    }

    view->guild_count = guild_rows;
    view->channel_count = channel_count;
    view->selected_guild_id = state.selected_guild_id;
    view->selected_channel_id = state.selected_channel_id;
    view->source_guild_mutations = state.guild_mutations;
    view->source_channel_mutations = state.channel_mutations;
    view->source_selection_revision = state.selection_revision;
    view->source_voice_revision = view->voice_states ? view->voice_states->revision : 0;
    view->source_stage_revision = view->stage_instances ? view->stage_instances->revision : 0;
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

} // namespace maicord

namespace maicord {
bool navigation_view_refresh_voice(NavigationView* view) noexcept {
    if (!view || view->channel_count > view->channel_capacity || (view->channel_count && !view->channel_rows))
        return false;
    bool changed = false;
    for (u32 i = 0; i < view->channel_count; ++i) {
        auto& row = view->channel_rows[i];
        const u16 count = voice_count(*view, row.id, row.type);
        const bool live = stage_live(*view, row.id, row.type);
        changed = changed || count != row.voice_count || live != row.stage_live;
        row.voice_count = count;
        row.stage_live = live;
    }
    view->source_voice_revision = view->voice_states ? view->voice_states->revision : 0;
    view->source_stage_revision = view->stage_instances ? view->stage_instances->revision : 0;
    saturating_increment_if(changed, view->channel_revision);
    saturating_increment_if(changed, view->revision);
    return changed;
}
} // namespace maicord
