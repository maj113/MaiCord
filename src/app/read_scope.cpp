#include "maicord/app/read_scope.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {
namespace {
constexpr u64 read_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
constexpr u32 read_actions = channel_action_view | channel_action_read_history;
bool supported(u8 type) noexcept {
    return type == 0 || type == 5 || type == 2 || type == 13 || type == 15 || type == 16 ||
           channel_is_thread(type);
}
bool readable(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    return channel.occupied && channel.id && supported(channel.type) && channel.permissions_resolved &&
           !(channel.flags & kChannelObfuscatedFlag) &&
           (channel.effective_permissions & read_permissions) == read_permissions &&
           (channel.actions & read_actions) == read_actions &&
           chat_state_store_channel_visible(state, channel);
}
bool eligible(const ChatStateStore& state, const ReadScopeSource& source,
              const ChannelRecord& channel) noexcept {
    if (channel.guild_id != source.guild_id || !readable(state, channel))
        return false;
    const auto* parent =
        channel.parent_id ? chat_state_store_find_channel(state, channel.parent_id) : nullptr;
    if (channel.parent_id && (!parent || !parent->occupied || parent->guild_id != channel.guild_id ||
                              (parent->flags & kChannelObfuscatedFlag)))
        return false;
    if (channel_is_thread(channel.type)) {
        if (!channel.thread_active_known || !channel.thread_listed_active ||
            !channel.thread_self_joined_known || !channel.thread_self_joined ||
            (channel.thread_archived_known && channel.thread_archived) || !parent ||
            !channel_is_thread_parent(parent->type) || !readable(state, *parent) ||
            !chat_state_store_thread_accessible(state, channel))
            return false;
        return source.kind == ReadScopeKind::guild || parent->parent_id == source.id;
    }
    return source.kind == ReadScopeKind::guild || channel.parent_id == source.id;
}
bool capture_eligible(const ChatStateStore& state, const ReadScopeSource& source,
                      const ChannelRecord& channel) noexcept {
    return eligible(state, source, channel) &&
           (source.kind == ReadScopeKind::category || channel.type == 15 || channel.type == 16 ||
            (channel.last_message_id &&
             (channel.last_message_id > channel.last_read_message_id || channel.mention_count)));
}
u64 clock_cursor(u64 time_ms) noexcept {
    constexpr u64 epoch = 1420070400000ull;
    if (time_ms <= epoch)
        return 0;
    const u64 delta = time_ms - epoch;
    constexpr u64 limit = ~u64{0} >> 22;
    return (delta > limit ? limit : delta) << 22;
}
ReadScopeTarget target(const ChatStateStore& state, const ReadScopeSource& source,
                       const ChannelRecord& channel, u64 cursor) noexcept {
    ReadScopeTarget result{};
    result.channel_id = channel.id;
    result.parent_id = channel.parent_id;
    result.message_id = source.kind == ReadScopeKind::guild && (channel.type == 15 || channel.type == 16)
                            ? cursor
                            : channel.last_message_id;
    result.effective_permissions = channel.effective_permissions;
    result.actions = channel.actions;
    result.generation = channel.generation;
    result.type = channel.type;
    result.outbound = result.message_id != 0;
    if (const auto* parent = chat_state_store_find_channel(state, channel.parent_id)) {
        result.parent_parent_id = parent->parent_id;
        result.parent_effective_permissions = parent->effective_permissions;
        result.parent_actions = parent->actions;
        result.parent_generation = parent->generation;
        result.parent_type = parent->type;
    }
    return result;
}
} // namespace
bool read_scope_source(const ChatStateStore& state, ReadScopeKind kind, u64 id,
                       ReadScopeSource* output) noexcept {
    if (!output || !id || (kind != ReadScopeKind::guild && kind != ReadScopeKind::category))
        return false;
    const auto* category =
        kind == ReadScopeKind::category ? chat_state_store_find_channel(state, id) : nullptr;
    if (kind == ReadScopeKind::category &&
        (!category || category->type != 4 || !category->guild_id || !category->permissions_resolved ||
         (category->flags & kChannelObfuscatedFlag) ||
         !(category->effective_permissions & kPermissionViewChannel) ||
         !(category->actions & channel_action_view)))
        return false;
    const auto* guild = chat_state_store_find_guild(state, category ? category->guild_id : id);
    if (!guild || !guild->occupied || guild->unavailable || !guild->permissions_resolved)
        return false;
    ReadScopeSource source{};
    source.id = id;
    source.guild_id = guild->id;
    source.guild_generation = guild->generation;
    source.guild_revision = guild->revision;
    source.kind = kind;
    source.generation = category ? category->generation : guild->generation;
    source.revision = category ? category->revision : guild->revision;
    source.effective_permissions = category ? category->effective_permissions : guild->base_permissions;
    source.actions = category ? category->actions : 0;
    *output = source;
    return true;
}
bool read_scope_source_current(const ChatStateStore& state, const ReadScopeSource& source) noexcept {
    ReadScopeSource current{};
    return read_scope_source(state, source.kind, source.id, &current) &&
           current.guild_id == source.guild_id && current.generation == source.generation &&
           current.guild_generation == source.guild_generation &&
           current.effective_permissions == source.effective_permissions &&
           current.actions == source.actions;
}
ReadScopePlanResult read_scope_plan_size(const ChatStateStore& state, ReadScopeKind kind, u64 id,
                                         u64 action_time_ms) noexcept {
    ReadScopePlanResult result{};
    if (!read_scope_source(state, kind, id, &result.source) || (!state.channels && state.channel_capacity))
        return {};
    const u64 cursor = clock_cursor(action_time_ms);
    u32 occupied = 0;
    const u32 slots = chat_state_store_channel_slot_bound(state);
    for (u32 i = 0; i < slots; ++i) {
        const auto& channel = state.channels[i];
        if (!channel.occupied)
            continue;
        if (!channel.id || ++occupied > kReadScopeTargetMaximum)
            return {};
        // Core's canonical index must resolve every occupied record.
        // Duplicate or missing entries cannot silently truncate the inventory.
        if (chat_state_store_find_channel(state, channel.id) != &channel)
            return {};
        if (capture_eligible(state, result.source, channel)) {
            ++result.count;
            result.outbound_count += target(state, result.source, channel, cursor).outbound ? 1u : 0u;
        }
    }
    if (occupied != state.channel_count)
        return {};
    result.valid = true;
    return result;
}
ReadScopePlanResult read_scope_plan(const ChatStateStore& state, ReadScopeKind kind, u64 id,
                                    u64 action_time_ms, Slice<ReadScopeTarget> output) noexcept {
    const auto result = read_scope_plan_size(state, kind, id, action_time_ms);
    if (!result.valid || (result.count && (!output.data || output.count < result.count)) ||
        byte_ranges_overlap_or_wrap(output.data, static_cast<u64>(output.count) * sizeof(ReadScopeTarget), state.channels,
                                    static_cast<u64>(state.channel_capacity) * sizeof(ChannelRecord)) ||
        byte_ranges_overlap_or_wrap(output.data, static_cast<u64>(output.count) * sizeof(ReadScopeTarget), state.guilds,
                                    static_cast<u64>(state.guild_capacity) * sizeof(GuildRecord)) ||
        byte_ranges_overlap_or_wrap(output.data, static_cast<u64>(output.count) * sizeof(ReadScopeTarget), &state,
                                    sizeof(state)))
        return {};
    const u64 cursor = clock_cursor(action_time_ms);
    u32 used = 0;
    for (u32 i = 0; i < state.channel_count; ++i) {
        const auto& channel = state.channels[state.channel_order[i]];
        if (!capture_eligible(state, result.source, channel))
            continue;
        output[used++] = target(state, result.source, channel, cursor);
    }
    return result;
}
bool read_scope_target_current(const ChatStateStore& state, const ReadScopeSource& source,
                               const ReadScopeTarget& saved) noexcept {
    if (!read_scope_source_current(state, source))
        return false;
    const auto* channel = chat_state_store_find_channel(state, saved.channel_id);
    if (!channel || !eligible(state, source, *channel))
        return false;
    const auto current = target(state, source, *channel, saved.message_id);
    return current.parent_id == saved.parent_id && current.parent_parent_id == saved.parent_parent_id &&
           current.generation == saved.generation && current.type == saved.type &&
           current.effective_permissions == saved.effective_permissions &&
           current.actions == saved.actions && current.parent_generation == saved.parent_generation &&
           current.parent_type == saved.parent_type &&
           current.parent_effective_permissions == saved.parent_effective_permissions &&
           current.parent_actions == saved.parent_actions && saved.outbound == (saved.message_id != 0);
}
} // namespace maicord
