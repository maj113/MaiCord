#include "maicord/model/chat_state.hpp"

#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/sorted_search.hpp"

namespace maicord {
namespace {

#include "chat_state_backing.inc"

u32 find_guild_order(const ChatStateStore& store, u64 id, bool* found) noexcept {
    const u32 first = sorted_lower_bound(
        store.guild_count, [&](u32 at) noexcept { return store.guilds[store.guild_order[at]].id < id; });
    *found = first < store.guild_count && store.guilds[store.guild_order[first]].id == id;
    return first;
}

u32 find_channel_order(const ChatStateStore& store, u64 id, bool* found) noexcept {
    const u32 first = sorted_lower_bound(
        store.channel_count, [&](u32 at) noexcept { return store.channels[store.channel_order[at]].id < id; });
    *found = first < store.channel_count && store.channels[store.channel_order[first]].id == id;
    return first;
}

u32 find_role_index(const ChatStateStore& store, u64 guild_id, u64 role_id, bool* found) noexcept {
    const u32 first = sorted_lower_bound(store.role_count, [&](u32 at) noexcept {
        const RoleRecord& role = store.roles[at];
        return role.guild_id < guild_id || (role.guild_id == guild_id && role.id < role_id);
    });
    *found = first < store.role_count && store.roles[first].guild_id == guild_id &&
             store.roles[first].id == role_id;
    return first;
}

u32 empty_guild_slot(const ChatStateStore& store) noexcept {
    for (u32 slot = store.guild_free_hint; slot < store.guild_capacity; ++slot) {
        if (!store.guilds[slot].occupied) {
            return slot;
        }
    }
    return store.guild_capacity;
}

// Backs the first `required` channel slots (records and order) before use.
bool back_channels(const ChatStateStore& store, u32 required) noexcept {
    return back_range(store, ChatStateBacking::channels, 0, store.channel_backed_count, store.channel_capacity,
                      required);
}

// Backs the first `bytes` of name bank `bank` before use.
bool back_names(const ChatStateStore& store, u32 bank, u32 bytes) noexcept {
    return back_range(store, ChatStateBacking::names, bank, store.name_backed_bytes[bank], store.name_bank_bytes,
                      bytes);
}

// With channel_count slots occupied, slot channel_count or an earlier one is
// free, so the search reads only slots up to channel_count. Callers back
// channel_count + 1 slots first.
u32 empty_channel_slot(const ChatStateStore& store) noexcept {
    const u32 limit = minimum(store.channel_capacity, store.channel_count + 1);
    for (u32 slot = store.channel_free_hint; slot < limit; ++slot) {
        if (!store.channels[slot].occupied) {
            return slot;
        }
    }
    return store.channel_capacity;
}

u32 active_name_base(const ChatStateStore& store) noexcept {
    return static_cast<u32>(store.active_name_bank) * store.name_bank_bytes;
}

bool compact_names(ChatStateStore* store) noexcept {
    const u8 target_bank = store->active_name_bank == 0 ? 1 : 0;
    const u32 target_base = static_cast<u32>(target_bank) * store->name_bank_bytes;
    // Visits every record that carries a name, in the order the names are laid out.
    const auto each_name = [&](auto&& visit) noexcept {
        for (u32 order = 0; order < store->guild_count; ++order)
            if (!visit(store->guilds[store->guild_order[order]]))
                return false;
        for (u32 order = 0; order < store->channel_count; ++order)
            if (!visit(store->channels[store->channel_order[order]]))
                return false;
        for (u32 index = 0; index < store->role_count; ++index)
            if (!visit(store->roles[index]))
                return false;
        return true;
    };
    // Back the target bank for every live name before moving any of them.
    u64 live = 0;
    each_name([&](auto& record) noexcept {
        live += record.name_bytes;
        return true;
    });
    if (live > store->name_bank_bytes || !back_names(*store, target_bank, static_cast<u32>(live)))
        return false;
    u32 used = 0;
    if (!each_name([&](auto& record) noexcept {
            if (record.name_bytes == 0) {
                record.name_offset = 0;
                return true;
            }
            if (record.name_bytes > store->name_bank_bytes - used)
                return false;
            copy_bytes(store->names + target_base + used, store->names + record.name_offset, record.name_bytes);
            record.name_offset = target_base + used;
            used += record.name_bytes;
            return true;
        }))
        return false;
    store->active_name_bank = target_bank;
    store->name_used = used;
    store->name_wasted = 0;
    ++store->compactions;
    return true;
}

bool allocate_name(ChatStateStore* store, Utf8View name, u32* offset) noexcept {
    if (name.bytes == 0) {
        *offset = 0;
        return true;
    }
    if (name.bytes > store->name_bank_bytes - store->name_used &&
        (!compact_names(store) || name.bytes > store->name_bank_bytes - store->name_used)) {
        return false;
    }
    if (!back_names(*store, store->active_name_bank, store->name_used + name.bytes))
        return false;
    *offset = active_name_base(*store) + store->name_used;
    copy_bytes(store->names + *offset, reinterpret_cast<const u8*>(name.data), name.bytes);
    store->name_used += name.bytes;
    return true;
}

bool name_equals(const ChatStateStore& store, u32 offset, u16 bytes, Utf8View name) noexcept {
    if (bytes != name.bytes) {
        return false;
    }
    return bytes_equal(store.names + offset, name.data, name.bytes);
}

Utf8View stored_name(const ChatStateStore& store, bool present, u32 offset, u32 bytes) noexcept {
    return present && bytes != 0 && offset + bytes <= store.name_bank_bytes * 2
               ? Utf8View{reinterpret_cast<const char*>(store.names + offset), bytes}
               : Utf8View{};
}

struct NamePreflight {
    u32 used;
    u32 live;
    // The furthest a write reaches in either bank, and whether the batch
    // compacts into the other bank on the way.
    u32 peak;
    bool compacts;
};

bool preflight_live_name(const ChatStateStore& store, NamePreflight* budget, u32 offset,
                         u32 bytes) noexcept {
    if (bytes == 0)
        return true;
    const u32 base = active_name_base(store);
    if (offset < base || offset - base > store.name_used || bytes > store.name_used - (offset - base) ||
        bytes > store.name_bank_bytes - budget->live)
        return false;
    budget->live += bytes;
    return true;
}

bool preflight_header(const ChatStateStore& store) noexcept {
    return store.guilds && store.guild_order && store.channels && store.channel_order && store.names &&
           store.guild_count <= store.guild_capacity && store.channel_count <= store.channel_capacity &&
           store.role_backed_count <= store.role_capacity &&
           store.overwrite_backed_count <= store.overwrite_capacity &&
           store.role_count <= store.role_backed_count &&
           store.overwrite_count <= store.overwrite_backed_count &&
           (store.roles || store.role_backed_count == 0) &&
           (store.overwrites || store.overwrite_backed_count == 0) && store.active_name_bank <= 1 &&
           store.name_used <= store.name_bank_bytes;
}

bool preflight_names(const ChatStateStore& store, NamePreflight* budget) noexcept {
    if (!preflight_header(store))
        return false;
    *budget = NamePreflight{store.name_used, 0, store.name_used, false};
    for (u32 order = 0; order < store.guild_count; ++order) {
        if (store.guild_order[order] >= store.guild_capacity)
            return false;
        const auto& record = store.guilds[store.guild_order[order]];
        if (!preflight_live_name(store, budget, record.name_offset, record.name_bytes))
            return false;
    }
    for (u32 order = 0; order < store.channel_count; ++order) {
        if (store.channel_order[order] >= store.channel_capacity)
            return false;
        const auto& record = store.channels[store.channel_order[order]];
        if (!preflight_live_name(store, budget, record.name_offset, record.name_bytes))
            return false;
    }
    for (u32 index = 0; index < store.role_count; ++index) {
        const auto& role = store.roles[index];
        if (!preflight_live_name(store, budget, role.name_offset, role.name_bytes))
            return false;
    }
    return budget->live <= budget->used;
}

ChatStateStoreError preflight_channel_slot(const ChatStateStore& store, u64 id, bool* found,
                                              u32* slot) noexcept {
    u32 first = 0, count = store.channel_count;
    while (count != 0) {
        const u32 step = count / 2, current = first + step;
        const u32 current_slot = store.channel_order[current];
        if (current_slot >= store.channel_capacity)
            return ChatStateStoreError::invalid_input;
        const u64 current_id = store.channels[current_slot].id;
        if (current_id < id) {
            first = current + 1;
            count -= step + 1;
        } else
            count = step;
    }
    *found = false;
    if (first < store.channel_count) {
        const u32 candidate = store.channel_order[first];
        if (candidate >= store.channel_capacity)
            return ChatStateStoreError::invalid_input;
        if (store.channels[candidate].id == id) {
            if (!store.channels[candidate].occupied)
                return ChatStateStoreError::invalid_input;
            *found = true;
            *slot = candidate;
            return ChatStateStoreError::none;
        }
    }
    if (store.channel_count >= store.channel_capacity || !back_channels(store, store.channel_count + 1))
        return ChatStateStoreError::channel_capacity;
    *slot = empty_channel_slot(store);
    return *slot < store.channel_capacity ? ChatStateStoreError::none
                                          : ChatStateStoreError::channel_capacity;
}

bool preflight_replace_name(const ChatStateStore& store, NamePreflight* budget, u32 old_offset,
                            u16 old_bytes, Utf8View name) noexcept {
    if (name.bytes == 0 || name_equals(store, old_offset, old_bytes, name))
        return true;
    if (old_bytes > budget->live)
        return false;
    // Allocation precedes retiring the old name, as allocate_name does.
    if (name.bytes > store.name_bank_bytes - budget->used) {
        budget->used = budget->live;
        budget->compacts = true;
    }
    if (name.bytes > store.name_bank_bytes - budget->used)
        return false;
    budget->used += name.bytes;
    budget->peak = maximum(budget->peak, budget->used);
    budget->live = budget->live - old_bytes + name.bytes;
    return true;
}

// Backs every name byte an admitted batch writes, so applying it cannot fail
// for backing: the active bank up to the peak, and the other bank too when the
// batch compacts (each compaction writes at most the then-live bytes, which
// the peak bounds).
ChatStateStoreError back_name_budget(const ChatStateStore& store, const NamePreflight& budget) noexcept {
    const u32 active = store.active_name_bank;
    const bool backed = back_names(store, active, budget.peak) &&
                        (!budget.compacts || back_names(store, active ^ 1u, budget.peak));
    return backed ? ChatStateStoreError::none : ChatStateStoreError::name_capacity;
}

ChatStateStoreResult state_error(ChatStateStore* store, ChatStateStoreError error) noexcept {
    if (store) {
        ++store->rejected;
    }
    return ChatStateStoreResult{0, error, false, false};
}

bool valid_name(Utf8View name) noexcept {
    return name.bytes <= 400 && (!name.data ? name.bytes == 0 : utf8_validate(name).valid);
}

#include "asset_hash_validation.inc"

u32 guild_icon_variant(Utf8View hash) noexcept {
    u32 value = 2'166'136'261u;
    for (u32 index = 0; index < hash.bytes; ++index) {
        value ^= static_cast<u8>(hash.data[index]);
        value *= 16'777'619u;
    }
    return value == 0 ? 1u : value;
}

// Writes `hash` into a zero-padded record field; returns its length.
u8 store_asset_hash(char (&field)[kGuildIconHashMaximumBytes], Utf8View hash) noexcept {
    for (u32 index = 0; index < kGuildIconHashMaximumBytes; ++index)
        field[index] = index < hash.bytes ? hash.data[index] : 0;
    return static_cast<u8>(hash.bytes);
}

bool guild_icon_equals(const GuildRecord& record, Utf8View hash) noexcept {
    if (record.icon_hash_bytes != hash.bytes) {
        return false;
    }
    return bytes_equal(record.icon_hash, hash.data, hash.bytes);
}

#include "guild_activity.inc"

bool guild_unavailable(const ChatStateStore& store, u64 guild_id) noexcept {
    const GuildRecord* guild = chat_state_store_find_guild(store, guild_id);
    return !guild || guild->unavailable;
}

bool better_channel(const ChatStateStore& state, const ChannelRecord& candidate,
                    const ChannelRecord& current) noexcept {
    if (channel_is_private(candidate.type) && channel_is_private(current.type)) {
        const u64 left = private_channel_recency(candidate.id, candidate.last_message_id);
        const u64 right = private_channel_recency(current.id, current.last_message_id);
        if (left != right)
            return left > right;
        const auto* a = state.private_channels
                            ? private_channel_store_find(*state.private_channels, candidate.id)
                            : nullptr;
        const auto* b = state.private_channels
                            ? private_channel_store_find(*state.private_channels, current.id)
                            : nullptr;
        const u64 a_order = a ? a->sort_sequence : 0, b_order = b ? b->sort_sequence : 0;
        return a_order != b_order ? a_order > b_order : candidate.id < current.id;
    }
    if (candidate.guild_id != current.guild_id) {
        return candidate.guild_id < current.guild_id;
    }
    const u32 candidate_rank = candidate.type == 0 || candidate.type == 5 ? 0u : 1u;
    const u32 current_rank = current.type == 0 || current.type == 5 ? 0u : 1u;
    if (candidate_rank != current_rank) {
        return candidate_rank < current_rank;
    }
    if (candidate.position != current.position) {
        return candidate.position < current.position;
    }
    return candidate.id < current.id;
}

bool selectable_channel(const ChatStateStore& store, const ChannelRecord& channel) noexcept {
    return channel.occupied && channel_is_message_capable(channel.type) &&
           (channel.flags & kChannelObfuscatedFlag) == 0 &&
           chat_state_store_channel_visible(store, channel) &&
           (channel_is_private(channel.type) || !guild_unavailable(store, channel.guild_id));
}

const ChannelRecord* best_channel(const ChatStateStore& store, u64 guild_id,
                                         bool private_only = false) noexcept {
    const ChannelRecord* selected = nullptr;
    for (u32 order = 0; order < store.channel_count; ++order) {
        const ChannelRecord& candidate = store.channels[store.channel_order[order]];
        // A guild never lands on a voice channel's chat by itself; it opens
        // from the call view, a link or an explicit selection.
        if ((guild_id != 0 && candidate.guild_id != guild_id) ||
            (private_only && !channel_is_private(candidate.type)) || channel_is_voice_chat(candidate.type) ||
            !selectable_channel(store, candidate)) {
            continue;
        }
        if (!selected || better_channel(store, candidate, *selected)) {
            selected = &candidate;
        }
    }
    return selected;
}

bool set_selection(ChatStateStore* store, const ChannelRecord* channel) noexcept {
    const u64 guild_id = channel ? channel->guild_id : 0;
    const u64 channel_id = channel ? channel->id : 0;
    if (guild_id == store->selected_guild_id && channel_id == store->selected_channel_id) {
        return false;
    }
    read_state_leave_channel(store, store->selected_channel_id);
    store->selected_guild_id = guild_id;
    store->selected_channel_id = channel_id;
    if (channel)
        store->home_selected = channel_is_private(channel->type);
    if (channel && store->home_selected)
        store->last_private_channel_id = channel_id;
    saturating_increment(store->selection_revision);
    return true;
}

} // namespace

bool chat_state_store_initialize(ChatStateStore* store, Slice<GuildRecord> guilds, Slice<u16> guild_order,
                                 Slice<ChannelRecord> channels, Slice<u16> channel_order,
                                 Slice<u8> double_buffered_names, bool zeroed) noexcept {
    if (!store || !guilds.data || guilds.count == 0 || guilds.count > 65'535 || !guild_order.data ||
        guild_order.count != guilds.count || !channels.data || channels.count == 0 ||
        channels.count > 65'535 || !channel_order.data || channel_order.count != channels.count ||
        !double_buffered_names.data || double_buffered_names.count < 2 ||
        (double_buffered_names.count & 1u) != 0) {
        return false;
    }
    reset_value<ChatStateStore>(store);
    store->guilds = guilds.data;
    store->guild_order = guild_order.data;
    store->channels = channels.data;
    store->channel_order = channel_order.data;
    store->names = double_buffered_names.data;
    store->guild_capacity = guilds.count;
    store->channel_capacity = channels.count;
    store->name_bank_bytes = double_buffered_names.count / 2;
    store->channel_backed_count = channels.count;
    store->name_backed_bytes[0] = store->name_backed_bytes[1] = store->name_bank_bytes;
    if (zeroed)
        return true;
    for (u32 index = 0; index < guilds.count; ++index) {
        reset_value<GuildRecord>(&guilds[index]);
        guild_order[index] = 0xFFFF;
    }
    for (u32 index = 0; index < channels.count; ++index) {
        reset_value<ChannelRecord>(&channels[index]);
        channel_order[index] = 0xFFFF;
    }
    return true;
}

bool chat_state_store_bind_backing(ChatStateStore* store, ChatStateBackingReserve reserve,
                                   void* context) noexcept {
    if (!store || !store->channels || !store->names || !reserve || store->backing_reserve ||
        store->guild_count || store->channel_count || store->name_used)
        return false;
    store->backing_reserve = reserve;
    store->backing_context = context;
    store->channel_backed_count = 0;
    store->name_backed_bytes[0] = store->name_backed_bytes[1] = 0;
    store->emoji_backed_count = store->emoji_role_backed_count = 0;
    return true;
}

u32 chat_state_store_channel_slot_bound(const ChatStateStore& store) noexcept {
    return store.backing_reserve ? minimum(store.channel_backed_count, store.channel_capacity)
                                 : store.channel_capacity;
}

static bool merge_guild_settings(GuildSettings* out, const GuildSettings& in) noexcept {
    const bool mfa_changed = (in.known & guild_settings_mfa_level) &&
                             (!(out->known & guild_settings_mfa_level) || out->mfa_level != in.mfa_level);
    const bool quarantine_changed = (in.known & guild_settings_self_quarantine) &&
                                   guild_self_quarantined(*out) != guild_self_quarantined(in);
    if (in.known & guild_settings_mfa_level)
        out->mfa_level = in.mfa_level;
    if (in.known & guild_settings_verification_level)
        out->verification_level = in.verification_level;
    if (in.known & guild_settings_default_notifications)
        out->default_message_notifications = in.default_message_notifications;
    if (in.known & guild_settings_explicit_content_filter)
        out->explicit_content_filter = in.explicit_content_filter;
    if (in.known & guild_settings_system_channel)
        out->system_channel_id = in.system_channel_id;
    if (in.known & guild_settings_afk_channel)
        out->afk_channel_id = in.afk_channel_id;
    if (in.known & guild_settings_afk_timeout)
        out->afk_timeout_seconds = in.afk_timeout_seconds;
    if (in.known & guild_settings_self_timeout)
        out->self_communication_disabled_until = in.self_communication_disabled_until;
    if (in.known & guild_settings_self_member)
        out->self_member = static_cast<u8>((out->self_member & guild_self_member_quarantined) |
                                          (in.self_member & ~guild_self_member_quarantined));
    if (in.known & guild_settings_self_quarantine)
        out->self_member = static_cast<u8>((out->self_member & ~guild_self_member_quarantined) |
                                          (in.self_member & guild_self_member_quarantined));
    out->known = static_cast<u16>(out->known | in.known);
    return mfa_changed || quarantine_changed;
}

static bool guild_settings_valid(const GuildSettings& settings) noexcept {
    return !(settings.known & ~guild_settings_all) && settings.mfa_level <= 1 &&
           settings.verification_level <= 4 && settings.default_message_notifications <= 1 &&
           settings.explicit_content_filter <= 2 && settings.afk_timeout_seconds <= 86'400;
}

ChatStateStoreResult chat_state_store_upsert_guild(ChatStateStore* store,
                                                         const GuildInput& input) noexcept {
    if (!store || !store->guilds || input.id == 0 || !valid_name(input.name) || input.nsfw_level > 3 ||
        (input.settings && !guild_settings_valid(*input.settings)) ||
        (!input.nsfw_level_known && input.nsfw_level) || input.kind == GuildMutationKind::remove ||
        (input.icon_known && !asset_hash_valid(input.icon_hash))) {
        return state_error(store, ChatStateStoreError::invalid_input);
    }
    bool found = false;
    const u32 order_index = find_guild_order(*store, input.id, &found);
    u32 slot = found ? store->guild_order[order_index] : empty_guild_slot(*store);
    if (!found && (store->guild_count >= store->guild_capacity || slot >= store->guild_capacity)) {
        return state_error(store, ChatStateStoreError::guild_capacity);
    }
    GuildRecord& record = store->guilds[slot];
    const bool replace_name =
        input.name.bytes != 0 &&
        (!found || !name_equals(*store, record.name_offset, record.name_bytes, input.name));
    const bool replace_icon = input.icon_known && (!found || !guild_icon_equals(record, input.icon_hash));
    u32 name_offset = record.name_offset;
    if (replace_name && !allocate_name(store, input.name, &name_offset)) {
        return state_error(store, ChatStateStoreError::name_capacity);
    }
    if (!found) {
        for (u32 index = store->guild_count; index > order_index; --index) {
            store->guild_order[index] = store->guild_order[index - 1];
        }
        store->guild_order[order_index] = static_cast<u16>(slot);
        ++store->guild_count;
        const u16 generation = next_generation(record.generation);
        reset_value<GuildRecord>(&record);
        record.generation = generation;
        record.id = input.id;
        record.occupied = true;
        if (slot == store->guild_free_hint)
            store->guild_free_hint = static_cast<u16>(slot + 1);
    } else if (replace_name) {
        store->name_wasted += record.name_bytes;
    }
    if (replace_name) {
        record.name_offset = name_offset;
        record.name_bytes = static_cast<u16>(input.name.bytes);
    }
    if (replace_icon) {
        record.icon_hash_bytes = store_asset_hash(record.icon_hash, input.icon_hash);
        record.icon_variant = input.icon_hash.bytes != 0 ? guild_icon_variant(input.icon_hash) : 0;
    }
    // Banners are presentation: a malformed hash is dropped, never the guild.
    if (input.banner_known)
        record.banner_hash_bytes =
            store_asset_hash(record.banner_hash, asset_hash_valid(input.banner_hash) ? input.banner_hash : Utf8View{});
    if (input.owner_id != 0) {
        record.owner_id = input.owner_id;
    }
    if (input.kind == GuildMutationKind::unavailable)
        chat_state_store_invalidate_thread_authority(store, input.id);
    record.unavailable = input.kind == GuildMutationKind::unavailable;
    if (record.unavailable)
        chat_state_store_recompute_guild_permissions(store, input.id);
    if (input.upload_features_known)
        record.upload_features = input.upload_features;
    if (input.nsfw_level_known) {
        record.nsfw_level = input.nsfw_level;
        record.nsfw_level_known = true;
    }
    const bool permissions_changed = input.settings && merge_guild_settings(&record.settings, *input.settings);
    saturating_increment(record.revision);
    ++store->guild_mutations;
    if (permissions_changed && found && !record.unavailable)
        chat_state_store_recompute_guild_permissions(store, input.id);
    const bool changed = chat_state_store_select_default(store);
    return ChatStateStoreResult{order_index, ChatStateStoreError::none, !found, changed};
}

bool chat_state_store_set_guild_settings(ChatStateStore* store, u64 guild_id,
                                            const GuildSettings& settings) noexcept {
    const auto* found = store ? chat_state_store_find_guild(*store, guild_id) : nullptr;
    if (!found || !guild_settings_valid(settings))
        return false;
    auto& record = store->guilds[found - store->guilds];
    const bool permissions_changed = merge_guild_settings(&record.settings, settings);
    saturating_increment(record.revision);
    ++store->guild_mutations;
    if (permissions_changed && !record.unavailable)
        chat_state_store_recompute_guild_permissions(store, guild_id);
    return true;
}

bool chat_state_store_set_self_timeout(ChatStateStore* store, u64 guild_id, u64 until_seconds) noexcept {
    GuildSettings settings{};
    settings.known = guild_settings_self_timeout;
    settings.self_communication_disabled_until = static_cast<u32>(minimum<u64>(until_seconds, 0xFFFFFFFFull));
    return chat_state_store_set_guild_settings(store, guild_id, settings);
}

ChatStateStoreError chat_state_store_preflight_guild_role_update(const ChatStateStore& store,
                                                                       const GuildInput& guild,
                                                                       u32 role_count,
                                                                       RoleInputRead read_role,
                                                                       const void* context) noexcept {
    NamePreflight budget{};
    if (!preflight_names(store, &budget) || guild.id == 0 || guild.nsfw_level > 3 ||
        (!guild.nsfw_level_known && guild.nsfw_level) || guild.kind != GuildMutationKind::update ||
        !valid_name(guild.name) || (guild.icon_known && !asset_hash_valid(guild.icon_hash)) ||
        !read_role || role_count == 0 || role_count > kGuildRoleMaximum)
        return ChatStateStoreError::invalid_input;
    bool found = false;
    const u32 guild_order = find_guild_order(store, guild.id, &found);
    const u32 slot = found ? store.guild_order[guild_order] : empty_guild_slot(store);
    if (slot >= store.guild_capacity || (!found && store.guild_count >= store.guild_capacity))
        return ChatStateStoreError::guild_capacity;

    const auto& resident_guild = store.guilds[slot];
    if (!preflight_replace_name(store, &budget, found ? resident_guild.name_offset : 0,
                                found ? resident_guild.name_bytes : u16{0}, guild.name))
        return ChatStateStoreError::name_capacity;

    u32 old_roles = 0;
    for (u32 index = 0; index < store.role_count; ++index) {
        const auto& role = store.roles[index];
        if (role.guild_id != guild.id)
            continue;
        ++old_roles;
        bool retained = false;
        for (u32 incoming = 0; incoming < role_count; ++incoming) {
            RoleInput input{};
            if (!read_role(context, incoming, &input))
                return ChatStateStoreError::invalid_input;
            if (input.id == role.id) {
                retained = true;
                break;
            }
        }
        if (!retained)
            budget.live -= role.name_bytes;
    }
    if (store.roles && role_count > store.role_capacity - (store.role_count - old_roles))
        return ChatStateStoreError::role_capacity;
    for (u32 index = 0; index < role_count; ++index) {
        RoleInput input{};
        if (!read_role(context, index, &input) || input.id == 0 || input.guild_id != guild.id ||
            !valid_name(input.name))
            return ChatStateStoreError::invalid_input;
        // Without permission backing, role writes are accepted as no-ops.
        if (!store.roles)
            continue;
        const auto* old = chat_state_store_find_role(store, guild.id, input.id);
        if (!preflight_replace_name(store, &budget, old ? old->name_offset : 0,
                                    old ? old->name_bytes : u16{0}, input.name))
            return ChatStateStoreError::name_capacity;
    }
    return back_name_budget(store, budget);
}

ChatStateStoreError chat_state_store_preflight_guild_create(
    const ChatStateStore& store, const GuildInput& guild, u32 role_count,
    RoleInputRead read_role, const void* role_context, u32 channel_count,
    ChannelInputRead read_channel, const void* channel_context) noexcept {
    NamePreflight budget{};
    if (!preflight_names(store, &budget) || guild.id == 0 ||
        guild.kind != GuildMutationKind::create || !valid_name(guild.name) ||
        (guild.icon_known && !asset_hash_valid(guild.icon_hash)) || !read_role || role_count == 0 ||
        role_count > kGuildRoleMaximum || (channel_count != 0 && !read_channel))
        return ChatStateStoreError::invalid_input;
    bool found = false;
    const u32 guild_order = find_guild_order(store, guild.id, &found);
    const u32 slot = found ? store.guild_order[guild_order] : empty_guild_slot(store);
    if (slot >= store.guild_capacity || (!found && store.guild_count >= store.guild_capacity))
        return ChatStateStoreError::guild_capacity;
    const auto& resident_guild = store.guilds[slot];
    if (!preflight_replace_name(store, &budget, found ? resident_guild.name_offset : 0,
                                found ? resident_guild.name_bytes : u16{0}, guild.name))
        return ChatStateStoreError::name_capacity;
    // Full guild creation removes every target channel and role after the guild
    // upsert, then recreates incoming roles and channels in packet order.
    u32 old_roles = 0, old_channels = 0;
    for (u32 index = 0; index < store.role_count; ++index) {
        const auto& role = store.roles[index];
        if (role.guild_id == guild.id) {
            ++old_roles;
            budget.live -= role.name_bytes;
        }
    }
    for (u32 order = 0; order < store.channel_count; ++order) {
        const auto& channel = store.channels[store.channel_order[order]];
        if (channel.guild_id == guild.id) {
            ++old_channels;
            budget.live -= channel.name_bytes;
        }
    }
    if (store.roles && role_count > store.role_capacity - (store.role_count - old_roles))
        return ChatStateStoreError::role_capacity;
    // Old channels go before new ones arrive, so the count peaks at its final value.
    if (channel_count > store.channel_capacity - (store.channel_count - old_channels) ||
        !back_channels(store, store.channel_count - old_channels + channel_count))
        return ChatStateStoreError::channel_capacity;
    for (u32 index = 0; index < role_count; ++index) {
        RoleInput input{};
        if (!read_role(role_context, index, &input) || input.id == 0 || input.guild_id != guild.id ||
            !valid_name(input.name))
            return ChatStateStoreError::invalid_input;
        if (store.roles && !preflight_replace_name(store, &budget, 0, 0, input.name))
            return ChatStateStoreError::name_capacity;
    }
    for (u32 index = 0; index < channel_count; ++index) {
        ChannelInput input{};
        if (!read_channel(channel_context, index, &input) || input.id == 0 || input.guild_id != guild.id ||
            channel_is_private(input.type) || !valid_name(input.name))
            return ChatStateStoreError::invalid_input;
        const auto* old = chat_state_store_find_channel(store, input.id);
        if (old && old->guild_id != guild.id)
            return ChatStateStoreError::invalid_input;
        if (!preflight_replace_name(store, &budget, 0, 0, input.name))
            return ChatStateStoreError::name_capacity;
    }
    return back_name_budget(store, budget);
}

static bool channel_input_invalid(const ChannelInput& input) noexcept {
    const bool thread = channel_is_thread(input.type);
    return input.id == 0 || (input.content_policy & ~kChannelContentAgeRestricted) ||
           (!input.content_policy_known && input.content_policy) ||
           (input.thread_listed_active && !input.thread_active_known) || (input.thread_active_known && !thread) ||
           (input.thread_join_seconds && (!thread || !input.thread_self_joined)) ||
           (channel_is_private(input.type) ? input.guild_id != 0 || input.parent_id != 0 : input.guild_id == 0) ||
           !valid_name(input.name);
}

ChatStateStoreError chat_state_store_preflight_channel(const ChatStateStore& store,
                                                             const ChannelInput& input) noexcept {
    if (!preflight_header(store) || channel_input_invalid(input))
        return ChatStateStoreError::invalid_input;
    bool found = false;
    u32 slot = 0;
    const auto admission = preflight_channel_slot(store, input.id, &found, &slot);
    if (admission != ChatStateStoreError::none)
        return admission;
    const auto& old = store.channels[slot];
    if (found && old.guild_id != input.guild_id)
        return ChatStateStoreError::invalid_input;
    NamePreflight target{store.name_used, 0};
    if (found && !preflight_live_name(store, &target, old.name_offset, old.name_bytes))
        return ChatStateStoreError::invalid_input;
    // Initialized model metadata is the contract. These paths cannot compact:
    // only the target name and free append tail matter, so unrelated live names
    // need no walk. A known empty private name is cleared by the writer.
    if (input.name.bytes == 0 ||
        (found && name_equals(store, old.name_offset, old.name_bytes, input.name)) ||
        input.name.bytes <= store.name_bank_bytes - store.name_used)
        return ChatStateStoreError::none;
    NamePreflight budget{};
    if (!preflight_names(store, &budget))
        return ChatStateStoreError::invalid_input;
    return preflight_replace_name(store, &budget, found ? old.name_offset : 0,
                                  found ? old.name_bytes : u16{0}, input.name)
               ? ChatStateStoreError::none
               : ChatStateStoreError::name_capacity;
}

// IDs of the batch seen so far, in an open-addressed table at most half full (zero marks a free slot;
// IDs are nonzero once admitted). Preflights run on the application thread only.
static u64 g_batch_ids[2 * kChatStateChannelBatchMaximum];

ChatStateStoreError chat_state_store_preflight_channel_batch(const ChatStateStore& store,
                                                                   u32 channel_count,
                                                                   ChannelInputRead read_channel,
                                                                   const void* context) noexcept {
    NamePreflight budget{};
    if (!read_channel || !preflight_names(store, &budget))
        return ChatStateStoreError::invalid_input;
    if (channel_count > store.channel_capacity || channel_count > kChatStateChannelBatchMaximum)
        return ChatStateStoreError::channel_capacity;
    u32 table = 2;
    while (table < 2 * channel_count)
        table *= 2;
    clear_bytes(g_batch_ids, table * static_cast<u32>(sizeof(u64)));
    u32 new_channels = 0;
    for (u32 index = 0; index < channel_count; ++index) {
        ChannelInput input{};
        if (!read_channel(context, index, &input))
            return ChatStateStoreError::invalid_input;
        const auto admission = chat_state_store_preflight_channel(store, input);
        if (admission != ChatStateStoreError::none)
            return admission;
        u32 at = static_cast<u32>((input.id * 0x9E3779B97F4A7C15ull) >> 40) & (table - 1);
        while (g_batch_ids[at] != 0 && g_batch_ids[at] != input.id)
            at = (at + 1) & (table - 1);
        if (g_batch_ids[at] != 0)
            return ChatStateStoreError::invalid_input;
        g_batch_ids[at] = input.id;
        const auto* old = chat_state_store_find_channel(store, input.id);
        if (!old && ++new_channels > store.channel_capacity - store.channel_count)
            return ChatStateStoreError::channel_capacity;
        if (!preflight_replace_name(store, &budget, old ? old->name_offset : 0,
                                    old ? old->name_bytes : u16{0}, input.name))
            return ChatStateStoreError::name_capacity;
    }
    if (!back_channels(store, store.channel_count + new_channels))
        return ChatStateStoreError::channel_capacity;
    return back_name_budget(store, budget);
}

ChatStateStoreResult chat_state_store_upsert_channel(ChatStateStore* store,
                                                           const ChannelInput& input) noexcept {
    const bool private_channel = channel_is_private(input.type);
    if (!store || !store->channels || channel_input_invalid(input)) {
        return state_error(store, ChatStateStoreError::invalid_input);
    }
    bool found = false;
    const u32 order_index = find_channel_order(*store, input.id, &found);
    if (!found && (store->channel_count >= store->channel_capacity ||
                   !back_channels(*store, store->channel_count + 1)))
        return state_error(store, ChatStateStoreError::channel_capacity);
    u32 slot = found ? store->channel_order[order_index] : empty_channel_slot(*store);
    if (!found && slot >= store->channel_capacity) {
        return state_error(store, ChatStateStoreError::channel_capacity);
    }
    ChannelRecord& record = store->channels[slot];
    if (found && record.guild_id != input.guild_id) {
        return state_error(store, ChatStateStoreError::invalid_input);
    }
    const ChannelActivity old = found ? channel_activity(*store, record) : ChannelActivity{};
    const bool replace_name =
        input.name.bytes != 0 &&
        (!found || !name_equals(*store, record.name_offset, record.name_bytes, input.name));
    u32 name_offset = record.name_offset;
    if (replace_name && !allocate_name(store, input.name, &name_offset)) {
        return state_error(store, ChatStateStoreError::name_capacity);
    }
    if (!found) {
        for (u32 index = store->channel_count; index > order_index; --index) {
            store->channel_order[index] = store->channel_order[index - 1];
        }
        store->channel_order[order_index] = static_cast<u16>(slot);
        ++store->channel_count;
        const u16 generation = next_generation(record.generation);
        reset_value<ChannelRecord>(&record);
        record.generation = generation;
        record.id = input.id;
        record.occupied = true;
        if (slot == store->channel_free_hint)
            store->channel_free_hint = static_cast<u16>(slot + 1);
        if (input.last_message_known) {
            record.last_message_id = input.last_message_id;
            record.last_read_message_id = input.id;
        }
    } else if (replace_name) {
        store->name_wasted += record.name_bytes;
    }
    if (replace_name) {
        record.name_offset = name_offset;
        record.name_bytes = static_cast<u16>(input.name.bytes);
    } else if (private_channel && input.name_known && input.name.bytes == 0 && record.name_bytes != 0) {
        store->name_wasted += record.name_bytes;
        record.name_offset = 0;
        record.name_bytes = 0;
    }
    if (found && (record.parent_id != input.parent_id || record.type != input.type)) {
        record.thread_self_joined_known = false;
        record.thread_archived_known = false;
        record.thread_locked_known = false;
        record.thread_active_known = false;
        record.thread_listing_synced = false;
        record.position = 0;
    }
    record.guild_id = input.guild_id;
    record.parent_id = input.parent_id;
    if (!channel_is_thread(input.type))
        record.position = input.position;
    else if (input.thread_join_seconds)
        record.thread_join_seconds = input.thread_join_seconds;
    record.flags = input.flags;
    record.type = input.type;
    if (input.rate_limit_known)
        record.slowmode_code = slowmode_encode(input.rate_limit_per_user);
    // Codes 1..4 (hour, day, three days, week) map to the record's two bits.
    if (input.default_archive_code)
        record.default_archive = static_cast<u8>(input.default_archive_code == 4   ? 3
                                                 : input.default_archive_code == 3 ? 0
                                                                                   : input.default_archive_code);
    if (input.thread_archived_known) {
        record.thread_archived = input.thread_archived;
        record.thread_archived_known = true;
    }
    if (input.thread_locked_known) {
        record.thread_locked = input.thread_locked;
        record.thread_locked_known = true;
    }
    if (input.thread_self_joined_known) {
        record.thread_self_joined = input.thread_self_joined;
        record.thread_self_joined_known = true;
        if (!input.thread_self_joined && channel_is_thread(record.type))
            record.thread_join_seconds = 0;
    }
    if (input.thread_active_known && channel_is_thread(record.type)) {
        record.thread_listed_active = input.thread_listed_active;
        record.thread_active_known = true;
    }
    if (input.content_policy_known) {
        record.content_policy = input.content_policy;
        record.content_policy_known = true;
    }
    if (private_channel) {
        // Private channels do not participate in guild role/overwrite resolution.
        record.effective_permissions = 0;
        record.permissions_resolved = true;
        record.actions = channel_actions(input.type, 0, true);
    } else if (channel_is_thread(record.type)) {
        record.actions = chat_state_store_thread_actions(*store, record);
    }
    if (input.member_list_id_known)
        record.member_list_id = input.member_list_id;
    if (found && input.last_message_known && record.last_message_id != input.last_message_id) {
        record.last_message_id = input.last_message_id;
        if (input.last_message_id == 0 && !record.read_state_known) {
            record.last_read_message_id = 0;
            record.mention_count = 0;
        }
    }
    // A READY read entry may precede its channel. Reconcile it before the
    // insertion contributes activity to the guild, avoiding a transient badge.
    if (!read_state_apply_pending(store, &record))
        return state_error(store, ChatStateStoreError::invalid_input);
    commit_channel_activity(store, record, old);
    saturating_increment(record.revision);
    ++store->channel_mutations;
    const bool changed = chat_state_store_select_default(store);
    return ChatStateStoreResult{order_index, ChatStateStoreError::none, !found, changed};
}

bool chat_state_store_remove_channel(ChatStateStore* store, u64 channel_id) noexcept {
    if (!store || channel_id == 0) {
        return false;
    }
    bool found = false;
    const u32 order_index = find_channel_order(*store, channel_id, &found);
    if (!found) {
        return false;
    }
    ChannelRecord& record = store->channels[store->channel_order[order_index]];
    if (!read_state_remove_channel(store, record))
        return false;
    const ChannelActivity old = channel_activity(*store, record);
    adjust_guild_activity(mutable_guild(store, record.guild_id), old.unread, old.mentions, false, 0);
    if (old.unread || old.mentions != 0)
        ++store->activity_mutations;
    chat_state_store_clear_channel_overwrites(store, channel_id);
    if (store->private_channels)
        private_channel_store_remove(store->private_channels, channel_id);
    store->name_wasted += record.name_bytes;
    const u16 generation = next_generation(record.generation);
    reset_value<ChannelRecord>(&record);
    record.generation = generation;
    store->channel_free_hint = minimum<u16>(store->channel_free_hint, store->channel_order[order_index]);
    for (u32 index = order_index + 1; index < store->channel_count; ++index) {
        store->channel_order[index - 1] = store->channel_order[index];
    }
    --store->channel_count;
    store->channel_order[store->channel_count] = 0xFFFF;
    ++store->channel_mutations;
    chat_state_store_select_default(store);
    return true;
}

bool chat_state_store_remove_guild(ChatStateStore* store, u64 guild_id, bool unavailable) noexcept {
    if (!store || guild_id == 0) {
        return false;
    }
    bool found = false;
    const u32 order_index = find_guild_order(*store, guild_id, &found);
    if (!found) {
        return false;
    }
    GuildRecord& record = store->guilds[store->guild_order[order_index]];
    if (unavailable) {
        chat_state_store_invalidate_thread_authority(store, guild_id);
        record.unavailable = true;
        chat_state_store_recompute_guild_permissions(store, guild_id);
        saturating_increment(record.revision);
        ++store->guild_mutations;
        chat_state_store_select_default(store);
        return true;
    }
    chat_state_store_remove_guild_channels(store, guild_id);
    chat_state_store_remove_guild_roles(store, guild_id);
    chat_state_store_remove_guild_emojis(store, guild_id);
    store->name_wasted += record.name_bytes;
    const u16 generation = next_generation(record.generation);
    reset_value<GuildRecord>(&record);
    record.generation = generation;
    store->guild_free_hint = minimum<u16>(store->guild_free_hint, store->guild_order[order_index]);
    for (u32 index = order_index + 1; index < store->guild_count; ++index) {
        store->guild_order[index - 1] = store->guild_order[index];
    }
    --store->guild_count;
    store->guild_order[store->guild_count] = 0xFFFF;
    ++store->guild_mutations;
    chat_state_store_select_default(store);
    return true;
}

void chat_state_store_remove_guild_channels(ChatStateStore* store, u64 guild_id) noexcept {
    if (!store || guild_id == 0) {
        return;
    }
    for (u32 index = store->channel_count; index != 0; --index) {
        const ChannelRecord& channel = store->channels[store->channel_order[index - 1]];
        if (channel.guild_id == guild_id) {
            chat_state_store_remove_channel(store, channel.id);
        }
    }
}

u32 chat_state_store_clear_private_channels(ChatStateStore* store) noexcept {
    if (!store || !store->channels || !store->channel_order)
        return 0;
    const u32 old_count = store->channel_count;
    u32 retained = 0;
    for (u32 index = 0; index < old_count; ++index) {
        const u16 slot = store->channel_order[index];
        auto& channel = store->channels[slot];
        if (!channel_is_private(channel.type)) {
            store->channel_order[retained++] = slot;
            continue;
        }
        // READY preflight has already backed the carry prefix, so this hook
        // cannot allocate midway through the compact private-channel removal.
        if (!read_state_remove_channel(store, channel))
            return 0;
        if (chat_state_store_channel_unread(*store, channel))
            ++store->activity_mutations;
        store->name_wasted += channel.name_bytes;
        const u16 generation = next_generation(channel.generation);
        reset_value(&channel);
        channel.generation = generation;
        store->channel_free_hint = minimum(store->channel_free_hint, slot);
        ++store->channel_mutations;
    }
    for (u32 index = retained; index < old_count; ++index)
        store->channel_order[index] = 0xFFFF;
    store->channel_count = retained;
    if (store->private_channels &&
        (store->private_channels->count || store->private_channels->recipient_count ||
         store->private_channels->nickname_count))
        private_channel_store_clear(store->private_channels);
    if (retained != old_count)
        chat_state_store_select_default(store);
    return old_count - retained;
}

void chat_state_store_clear_guilds(ChatStateStore* store) noexcept {
    if (!store) {
        return;
    }
    for (u32 index = store->guild_count; index != 0; --index) {
        const GuildRecord& guild = store->guilds[store->guild_order[index - 1]];
        chat_state_store_remove_guild(store, guild.id, false);
    }
}

const GuildRecord* chat_state_store_find_guild(const ChatStateStore& store,
                                                         u64 guild_id) noexcept {
    bool found = false;
    const u32 order = find_guild_order(store, guild_id, &found);
    return found ? &store.guilds[store.guild_order[order]] : nullptr;
}

const ChannelRecord* chat_state_store_find_channel(const ChatStateStore& store,
                                                             u64 channel_id) noexcept {
    bool found = false;
    const u32 order = find_channel_order(store, channel_id, &found);
    return found ? &store.channels[store.channel_order[order]] : nullptr;
}

const RoleRecord* chat_state_store_find_role(const ChatStateStore& store, u64 guild_id,
                                                       u64 role_id) noexcept {
    if (!store.roles || guild_id == 0 || role_id == 0) {
        return nullptr;
    }
    bool found = false;
    const u32 index = find_role_index(store, guild_id, role_id, &found);
    return found ? &store.roles[index] : nullptr;
}

Utf8View chat_state_store_guild_name(const ChatStateStore& store,
                                        const GuildRecord& record) noexcept {
    return stored_name(store, record.occupied, record.name_offset, record.name_bytes);
}

Utf8View chat_state_store_guild_icon_hash(const GuildRecord& record) noexcept {
    return record.occupied ? Utf8View{record.icon_hash, record.icon_hash_bytes} : Utf8View{};
}

Utf8View chat_state_store_guild_banner_hash(const GuildRecord& record) noexcept {
    return record.occupied ? Utf8View{record.banner_hash, record.banner_hash_bytes} : Utf8View{};
}

Utf8View chat_state_store_channel_name(const ChatStateStore& store,
                                          const ChannelRecord& record) noexcept {
    return stored_name(store, record.occupied, record.name_offset, record.name_bytes);
}

Utf8View chat_state_store_role_name(const ChatStateStore& store,
                                       const RoleRecord& record) noexcept {
    return stored_name(store, record.id != 0 && store.names, record.name_offset, record.name_bytes);
}

bool chat_state_store_upsert_role(ChatStateStore* store, const RoleInput& input) noexcept {
    if (!store || input.id == 0 || input.guild_id == 0 || !valid_name(input.name)) {
        return false;
    }
    if (!store->roles) {
        return true;
    }
    if (!chat_state_store_prepare_permissions(store, store->role_count, store->overwrite_count)) {
        ++store->rejected;
        return false;
    }
    bool found = false;
    const u32 index = find_role_index(*store, input.guild_id, input.id, &found);
    if (found) {
        RoleRecord& role = store->roles[index];
        const bool replace_name =
            input.name.bytes != 0 && !name_equals(*store, role.name_offset, role.name_bytes, input.name);
        u32 name_offset = role.name_offset;
        if (replace_name && !allocate_name(store, input.name, &name_offset)) {
            ++store->rejected;
            return false;
        }
        if (replace_name) {
            store->name_wasted += role.name_bytes;
            role.name_offset = name_offset;
            role.name_bytes = static_cast<u16>(input.name.bytes);
        }
    } else {
        if (store->role_count >= store->role_capacity ||
            !chat_state_store_prepare_permissions(store, store->role_count + 1, store->overwrite_count)) {
            ++store->rejected;
            return false;
        }
        u32 name_offset = 0;
        if (!allocate_name(store, input.name, &name_offset)) {
            ++store->rejected;
            return false;
        }
        // Keep the existing compact backing sorted: lookups need no extra index or cache.
        // Allocate the name before moving records so rejection leaves the order intact.
        for (u32 move = store->role_count; move > index; --move) {
            store->roles[move] = store->roles[move - 1];
        }
        ++store->role_count;
        RoleRecord& role = store->roles[index];
        reset_value<RoleRecord>(&role);
        role.id = input.id;
        role.guild_id = input.guild_id;
        role.name_offset = name_offset;
        role.name_bytes = static_cast<u16>(input.name.bytes);
    }
    RoleRecord& role = store->roles[index];
    role.permissions = input.permissions;
    role.position = input.position;
    role.color = input.color;
    role.hoist = input.hoist;
    role.managed = input.managed;
    role.mentionable = input.mentionable;
    role.has_icon = input.has_icon;
    role.in_prompt = input.in_prompt;
    role.linked = input.linked;
    role.secondary_color = input.secondary_color;
    role.tertiary_color = input.tertiary_color;
    return true;
}

// Voice (2) and stage (13) channels carry text chat too (text in voice); the
// channel's own View/Read History/Send permissions gate it like any channel.
bool channel_is_message_capable(u8 type) noexcept {
    return type == 0 || type == 1 || type == 2 || type == 3 || type == 5 || type == 10 || type == 11 ||
           type == 12 || type == 13;
}
bool chat_state_store_touch_private_channel(ChatStateStore* store, u64 channel_id) noexcept {
    auto* channel = mutable_channel(store, channel_id);
    if (!channel || !channel_is_private(channel->type))
        return false;
    saturating_increment(channel->revision);
    saturating_increment(store->channel_mutations);
    return true;
}

bool chat_state_store_select_default(ChatStateStore* store) noexcept {
    if (!store || store->selection_held) {
        return false;
    }
    const ChannelRecord* selected = nullptr;
    if (store->restore_channel_id) {
        selected = chat_state_store_find_channel(*store, store->restore_channel_id);
        if (selected && selected->guild_id == store->restore_guild_id &&
            selectable_channel(*store, *selected)) {
            store->restore_channel_id = store->restore_guild_id = store->restore_deadline_ms = 0;
            return set_selection(store, selected);
        }
        return set_selection(store, nullptr);
    }
    if (store->selected_channel_id != 0) {
        const ChannelRecord* resident =
            chat_state_store_find_channel(*store, store->selected_channel_id);
        if (resident && selectable_channel(*store, *resident) &&
            (!store->home_selected || channel_is_private(resident->type))) {
            selected = resident;
        }
    }
    if (!selected) {
        selected = best_channel(*store, 0, store->home_selected);
    }
    return set_selection(store, selected);
}

bool chat_state_store_finish_restore(ChatStateStore* store) noexcept {
    if (!store || !store->restore_channel_id)
        return false;
    const u64 guild_id = store->restore_guild_id;
    store->restore_channel_id = store->restore_guild_id = store->restore_deadline_ms = 0;
    const auto* preferred = guild_id ? best_channel(*store, guild_id) : nullptr;
    const bool changed =
        preferred ? set_selection(store, preferred) : chat_state_store_select_default(store);
    if (!changed)
        saturating_increment(store->selection_revision);
    return true;
}

bool chat_state_store_expire_restore(ChatStateStore* store, u64 now_ms) noexcept {
    return store && store->restore_deadline_ms && now_ms >= store->restore_deadline_ms
               ? chat_state_store_finish_restore(store)
               : false;
}

ChatStateSelectionResult chat_state_store_select_home(ChatStateStore* store) noexcept {
    if (!store)
        return {ChatStateSelectionError::invalid_input, false};
    saturating_increment(store->navigation_intent_revision);
    const bool was_restoring = store->restore_channel_id != 0;
    store->restore_channel_id = store->restore_guild_id = store->restore_deadline_ms = 0;
    const auto* selected = chat_state_store_find_channel(*store, store->selected_channel_id);
    if (!selected || !channel_is_private(selected->type) ||
        !selectable_channel(*store, *selected)) {
        selected = chat_state_store_find_channel(*store, store->last_private_channel_id);
    }
    if (!selected || !channel_is_private(selected->type) ||
        !selectable_channel(*store, *selected)) {
        selected = best_channel(*store, 0, true);
    }
    const bool scope_changed = !store->home_selected;
    store->home_selected = true;
    const bool channel_changed = set_selection(store, selected);
    if ((scope_changed || was_restoring) && !channel_changed)
        saturating_increment(store->selection_revision);
    return {ChatStateSelectionError::none, scope_changed || channel_changed || was_restoring};
}

ChatStateSelectionResult chat_state_store_select_guild(ChatStateStore* store, u64 guild_id,
                                                       u64 remembered_channel_id,
                                                       u64 remembered_parent_id) noexcept {
    if (!store || guild_id == 0) {
        return ChatStateSelectionResult{ChatStateSelectionError::invalid_input, false};
    }
    const GuildRecord* guild = chat_state_store_find_guild(*store, guild_id);
    if (!guild) {
        return ChatStateSelectionResult{ChatStateSelectionError::not_found, false};
    }
    if (guild->unavailable) {
        return ChatStateSelectionResult{ChatStateSelectionError::unavailable, false};
    }
    const ChannelRecord* channel = nullptr;
    const u64 hints[] = {remembered_channel_id, remembered_parent_id};
    for (const u64 hint : hints) {
        const ChannelRecord* remembered = hint ? chat_state_store_find_channel(*store, hint) : nullptr;
        if (remembered && remembered->guild_id == guild_id && selectable_channel(*store, *remembered)) {
            channel = remembered;
            break;
        }
    }
    if (!channel)
        channel = best_channel(*store, guild_id);
    if (!channel) {
        return ChatStateSelectionResult{ChatStateSelectionError::not_selectable, false};
    }
    store->restore_channel_id = store->restore_guild_id = store->restore_deadline_ms = 0;
    saturating_increment(store->navigation_intent_revision);
    return ChatStateSelectionResult{ChatStateSelectionError::none, set_selection(store, channel)};
}

ChatStateSelectionResult chat_state_store_select_pending_guild(ChatStateStore* store, u64 guild_id,
                                                               u64 remembered_channel_id) noexcept {
    if (!store || guild_id == 0)
        return {ChatStateSelectionError::invalid_input, false};
    if (!chat_state_store_find_guild(*store, guild_id))
        return {ChatStateSelectionError::not_found, false};
    saturating_increment(store->navigation_intent_revision);
    // Any channel ID marks the target; a missing one falls back to the
    // guild's default channel when READY's state completes.
    store->restore_channel_id = remembered_channel_id ? remembered_channel_id : guild_id;
    store->restore_guild_id = guild_id;
    store->restore_deadline_ms = 0;
    if (store->selected_guild_id == guild_id && !store->selected_channel_id && !store->home_selected)
        return {ChatStateSelectionError::none, false};
    read_state_leave_channel(store, store->selected_channel_id);
    store->selected_guild_id = guild_id;
    store->selected_channel_id = 0;
    store->home_selected = false;
    saturating_increment(store->selection_revision);
    return {ChatStateSelectionError::none, true};
}

ChatStateSelectionResult chat_state_store_select_channel(ChatStateStore* store,
                                                               u64 channel_id) noexcept {
    if (!store || channel_id == 0) {
        return ChatStateSelectionResult{ChatStateSelectionError::invalid_input, false};
    }
    const ChannelRecord* channel = chat_state_store_find_channel(*store, channel_id);
    if (!channel) {
        return ChatStateSelectionResult{ChatStateSelectionError::not_found, false};
    }
    const GuildRecord* guild = chat_state_store_find_guild(*store, channel->guild_id);
    if (!channel_is_private(channel->type) && (!guild || guild->unavailable)) {
        return ChatStateSelectionResult{ChatStateSelectionError::unavailable, false};
    }
    if (!selectable_channel(*store, *channel)) {
        return ChatStateSelectionResult{ChatStateSelectionError::not_selectable, false};
    }
    store->restore_channel_id = store->restore_guild_id = store->restore_deadline_ms = 0;
    saturating_increment(store->navigation_intent_revision);
    return ChatStateSelectionResult{ChatStateSelectionError::none, set_selection(store, channel)};
}

bool chat_state_store_channel_age_restricted(const ChatStateStore& store,
                                                const ChannelRecord& channel) noexcept {
    if (channel.content_policy & kChannelContentAgeRestricted)
        return true;
    const auto* guild = chat_state_store_find_guild(store, channel.guild_id);
    // nsfw_level 1 and 3 classify the whole server by age. This only feeds the
    // forward preflight; the server's REST response decides whether a forward
    // is accepted.
    if (guild && (guild->nsfw_level == 1 || guild->nsfw_level == 3))
        return true;
    if (channel.type != 10 && channel.type != 11 && channel.type != 12)
        return false;
    const auto* parent = chat_state_store_find_channel(store, channel.parent_id);
    return parent && parent->guild_id == channel.guild_id &&
           (parent->content_policy & kChannelContentAgeRestricted);
}

namespace {
enum class ThreadAuthorityField : u8 {
    membership,
    archived,
    locked,
    active
};

void finish_thread_authority_change(ChatStateStore* store, ChannelRecord& channel, ChannelActivity old) noexcept {
    channel.actions = chat_state_store_thread_actions(*store, channel);
    commit_channel_activity(store, channel, old);
    saturating_increment(channel.revision);
    ++store->channel_mutations;
}

// The occupied thread of the current account, or null.
ChannelRecord* self_thread(ChatStateStore* store, u64 thread_id, u64 self_user_id) noexcept {
    if (!store || !self_user_id || self_user_id != store->self_user_id)
        return nullptr;
    auto* channel = mutable_channel(store, thread_id);
    return channel && channel->occupied && channel_is_thread(channel->type) ? channel : nullptr;
}

bool set_thread_authority(ChatStateStore* store, u64 thread_id, u64 self_user_id, bool value, bool known,
                          ThreadAuthorityField field) noexcept {
    auto* found = self_thread(store, thread_id, self_user_id);
    if (!found)
        return false;
    auto& channel = *found;
    if (guild_unavailable(*store, channel.guild_id))
        return false;
    const bool old_known = field == ThreadAuthorityField::membership ? channel.thread_self_joined_known
                           : field == ThreadAuthorityField::archived ? channel.thread_archived_known
                           : field == ThreadAuthorityField::locked ? channel.thread_locked_known
                                                                   : channel.thread_active_known;
    const bool old_value = field == ThreadAuthorityField::membership ? channel.thread_self_joined
                           : field == ThreadAuthorityField::archived ? channel.thread_archived
                           : field == ThreadAuthorityField::locked ? channel.thread_locked
                                                                   : channel.thread_listed_active;
    if (old_known == known && (!known || old_value == value))
        return true;
    const ChannelActivity old = channel_activity(*store, channel);
    if (field == ThreadAuthorityField::membership) {
        channel.thread_self_joined_known = known;
        if (known) {
            channel.thread_self_joined = value;
            if (!value)
                channel.thread_join_seconds = 0;
        }
    } else if (field == ThreadAuthorityField::archived) {
        channel.thread_archived_known = known;
        if (known)
            channel.thread_archived = value;
    } else if (field == ThreadAuthorityField::locked) {
        channel.thread_locked_known = known;
        if (known)
            channel.thread_locked = value;
    } else {
        channel.thread_active_known = known;
        if (known)
            channel.thread_listed_active = value;
    }
    finish_thread_authority_change(store, channel, old);
    chat_state_store_select_default(store);
    return true;
}
} // namespace

bool chat_state_store_set_thread_self_membership(ChatStateStore* store, u64 thread_id,
                                                    u64 self_user_id, bool joined, bool known) noexcept {
    return set_thread_authority(store, thread_id, self_user_id, joined, known,
                                ThreadAuthorityField::membership);
}

bool chat_state_store_set_thread_join_time(ChatStateStore* store, u64 thread_id, u64 self_user_id,
                                              u32 seconds) noexcept {
    if (!seconds)
        return false;
    auto* channel = self_thread(store, thread_id, self_user_id);
    if (!channel || !channel->thread_self_joined_known || !channel->thread_self_joined)
        return false;
    if (channel->thread_join_seconds != seconds) {
        // The join time is the unread baseline of a thread without an ACK.
        const ChannelActivity old = channel_activity(*store, *channel);
        channel->thread_join_seconds = seconds;
        commit_channel_activity(store, *channel, old);
        saturating_increment(channel->revision);
        ++store->channel_mutations;
    }
    return true;
}

bool chat_state_store_set_thread_archived(ChatStateStore* store, u64 thread_id, u64 self_user_id,
                                             bool archived, bool known) noexcept {
    return set_thread_authority(store, thread_id, self_user_id, archived, known,
                                ThreadAuthorityField::archived);
}

bool chat_state_store_set_thread_locked(ChatStateStore* store, u64 thread_id, u64 self_user_id,
                                           bool locked, bool known) noexcept {
    return set_thread_authority(store, thread_id, self_user_id, locked, known,
                                ThreadAuthorityField::locked);
}

bool chat_state_store_set_thread_active(ChatStateStore* store, u64 thread_id, u64 self_user_id,
                                           bool active, bool known) noexcept {
    return set_thread_authority(store, thread_id, self_user_id, active, known,
                                ThreadAuthorityField::active);
}

bool chat_state_store_set_thread_listing_synced(ChatStateStore* store, u64 parent_id,
                                                   u64 self_user_id, bool synced) noexcept {
    if (!store || !self_user_id || self_user_id != store->self_user_id)
        return false;
    auto* found = mutable_channel(store, parent_id);
    if (!found || !found->occupied || !channel_is_thread_parent(found->type) ||
        guild_unavailable(*store, found->guild_id))
        return false;
    auto& parent = *found;
    if (parent.thread_listing_synced != synced) {
        parent.thread_listing_synced = synced;
        saturating_increment(parent.revision);
        ++store->channel_mutations;
    }
    return true;
}

void chat_state_store_invalidate_thread_authority(ChatStateStore* store, u64 guild_id,
                                                     u64 parent_id) noexcept {
    if (!store || !store->channels || !store->channel_order)
        return;
    bool changed = false;
    for (u32 order = 0; order < store->channel_count; ++order) {
        auto& channel = store->channels[store->channel_order[order]];
        if (!channel.occupied || (guild_id && channel.guild_id != guild_id))
            continue;
        if (channel_is_thread_parent(channel.type) && (!parent_id || channel.id == parent_id) &&
            channel.thread_listing_synced) {
            channel.thread_listing_synced = false;
            saturating_increment(channel.revision);
            ++store->channel_mutations;
        }
        if (!channel_is_thread(channel.type) || (parent_id && channel.parent_id != parent_id) ||
            (!channel.thread_self_joined_known && !channel.thread_archived_known &&
             !channel.thread_locked_known && !channel.thread_active_known))
            continue;
        const ChannelActivity old = channel_activity(*store, channel);
        channel.thread_self_joined_known = false;
        channel.thread_archived_known = false;
        channel.thread_locked_known = false;
        channel.thread_active_known = false;
        finish_thread_authority_change(store, channel, old);
        changed = true;
    }
    if (changed)
        chat_state_store_select_default(store);
}
} // namespace maicord
