#include "maicord/model/reaction_preferences.hpp"

namespace maicord {
namespace {

constexpr ReactionEmojiRef kNoEmoji = reaction_emoji_unicode(kReactionPreferenceInvalidCatalogIndex);

bool store_valid(const ReactionPreferenceStore& store) noexcept {
    for (const auto& table : store.tables)
        if (table.usage_count > kReactionPreferenceUsageCapacity ||
            table.frequent_count > kReactionPreferenceFrequentCapacity)
            return false;
    return store.favorite_count <= kReactionPreferenceFavoriteCapacity &&
           store.pending_use_count <= kReactionPreferencePendingUseCapacity &&
           store.favorite_edit_count <= kReactionPreferenceFavoriteEditCapacity && store.next_sequence != 0 &&
           store.collapsed_guild_count <= kReactionPreferenceCollapsedGuildCapacity &&
           (!store.collapsed_guild_count || (store.collapsed_guilds.arena.base &&
               store.collapsed_guilds.capacity >= store.collapsed_guild_count)) &&
           (!store.collapsed_guilds.arena.base ||
               (store.collapsed_guilds.reserved_capacity == kReactionPreferenceCollapsedGuildCapacity &&
                store.collapsed_guilds.element_bytes == sizeof(u64) && store.collapsed_guilds.alignment == alignof(u64)));
}

bool kind_valid(ReactionPreferenceKind kind) noexcept {
    return static_cast<u32>(kind) < kReactionPreferenceKindCount;
}

u32 find_usage(const ReactionPreferenceTable& table, ReactionEmojiRef emoji) noexcept {
    for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index)
        if (table.usage[index].occupied && table.usage[index].emoji == emoji)
            return index;
    return kReactionPreferenceUsageCapacity;
}

u32 find_empty(const ReactionPreferenceTable& table) noexcept {
    for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index)
        if (!table.usage[index].occupied)
            return index;
    return kReactionPreferenceUsageCapacity;
}

u16 age_weight(u64 now_ms, u64 timestamp_ms) noexcept {
    constexpr u64 day_ms = 24ull * 60ull * 60ull * 1'000ull;
    const u64 age_days = now_ms > timestamp_ms ? (now_ms - timestamp_ms) / day_ms : 0;
    if (age_days <= 3) {
        return 100;
    }
    if (age_days <= 15) {
        return 70;
    }
    if (age_days <= 30) {
        return 50;
    }
    if (age_days <= 45) {
        return 30;
    }
    return age_days <= 80 ? 10 : 1;
}

bool precedes(const ReactionPreferenceUsage& left, const ReactionPreferenceUsage& right) noexcept {
    return left.frecency > right.frecency ||
           (left.frecency == right.frecency && left.sequence < right.sequence);
}

u64 last_use(const ReactionPreferenceUsage& usage) noexcept {
    return usage.recent_count ? usage.recent_uses[usage.recent_count - 1] : 0;
}

// The official client serializes only the 100 most recently used entries, so eviction
// follows recency rather than score.
u32 eviction_candidate(const ReactionPreferenceTable& table) noexcept {
    u32 selected = kReactionPreferenceUsageCapacity;
    for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index) {
        const ReactionPreferenceUsage& usage = table.usage[index];
        if (!usage.occupied)
            continue;
        if (selected == kReactionPreferenceUsageCapacity) {
            selected = index;
            continue;
        }
        const auto& current = table.usage[selected];
        if (last_use(usage) < last_use(current) ||
            (last_use(usage) == last_use(current) && precedes(current, usage)))
            selected = index;
    }
    return selected;
}

void insert_recent(ReactionPreferenceUsage* usage, u64 timestamp_ms) noexcept {
    if (usage->recent_count < kReactionPreferenceRecentUseCapacity) {
        usage->recent_uses[usage->recent_count++] = timestamp_ms;
    } else {
        for (u32 index = 1; index < kReactionPreferenceRecentUseCapacity; ++index) {
            usage->recent_uses[index - 1] = usage->recent_uses[index];
        }
        usage->recent_uses[kReactionPreferenceRecentUseCapacity - 1] = timestamp_ms;
    }
    for (u32 index = usage->recent_count; index > 1; --index) {
        if (usage->recent_uses[index - 2] <= usage->recent_uses[index - 1]) {
            break;
        }
        const u64 temporary = usage->recent_uses[index - 2];
        usage->recent_uses[index - 2] = usage->recent_uses[index - 1];
        usage->recent_uses[index - 1] = temporary;
    }
}

void recompute_table(ReactionPreferenceTable& table, ReactionPreferenceKind kind, u64 now_ms) noexcept {
    u32 maximum_total = 0;
    for (const auto& usage : table.usage)
        if (usage.occupied)
            maximum_total = maximum(maximum_total, usage.total_uses);
    u16 ordered[kReactionPreferenceUsageCapacity]{};
    u32 ordered_count = 0;
    for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index) {
        ReactionPreferenceUsage& usage = table.usage[index];
        if (!usage.occupied)
            continue;
        u32 score = 0;
        for (u32 recent = 0; recent < usage.recent_count; ++recent)
            score += age_weight(now_ms, usage.recent_uses[recent]);
        usage.score = static_cast<u16>(minimum(score, 1'000u));
        if (kind == ReactionPreferenceKind::reaction) {
            // trunc(1000 * (total / max_total * 0.2 + score / 1000 * 0.8))
            const u64 numerator = 1'000ull * usage.total_uses + 4ull * usage.score * maximum_total;
            usage.frecency =
                maximum_total ? static_cast<u32>(minimum<u64>(1'000ull, numerator / (5ull * maximum_total))) : 0;
        } else {
            // ceil(total * score / samples)
            const u64 samples = maximum<u32>(1u, usage.recent_count);
            usage.frecency = static_cast<u32>(minimum<u64>(
                0x7FFFFFFFull, (static_cast<u64>(usage.total_uses) * usage.score + samples - 1) / samples));
        }
        u32 insertion = ordered_count;
        while (insertion != 0 && precedes(usage, table.usage[ordered[insertion - 1]])) {
            ordered[insertion] = ordered[insertion - 1];
            --insertion;
        }
        ordered[insertion] = static_cast<u16>(index);
        ++ordered_count;
    }
    table.frequent_count = minimum(ordered_count, kReactionPreferenceFrequentCapacity);
    for (u32 index = 0; index < kReactionPreferenceFrequentCapacity; ++index)
        table.frequently[index] = index < table.frequent_count ? table.usage[ordered[index]].emoji : kNoEmoji;
}

void clear_table(ReactionPreferenceTable& table) noexcept {
    table = ReactionPreferenceTable{};
    for (auto& entry : table.frequently)
        entry = kNoEmoji;
}

u32 next_sequence(ReactionPreferenceStore* store) noexcept {
    const u32 sequence = store->next_sequence++;
    if (store->next_sequence == 0)
        store->next_sequence = 1;
    return sequence;
}

ReactionPreferenceUsage* admit_usage(ReactionPreferenceStore* store, ReactionPreferenceTable& table,
                                     ReactionEmojiRef emoji) noexcept {
    u32 index = find_usage(table, emoji);
    if (index != kReactionPreferenceUsageCapacity)
        return &table.usage[index];
    if (table.usage_count == kReactionPreferenceUsageCapacity) {
        index = eviction_candidate(table);
    } else {
        index = find_empty(table);
        if (index != kReactionPreferenceUsageCapacity)
            ++table.usage_count;
    }
    if (index == kReactionPreferenceUsageCapacity)
        return nullptr;
    table.usage[index] = ReactionPreferenceUsage{};
    table.usage[index].emoji = emoji;
    table.usage[index].sequence = next_sequence(store);
    table.usage[index].occupied = true;
    return &table.usage[index];
}

bool favorite_index(const ReactionPreferenceStore& store, ReactionEmojiRef emoji, u32* found) noexcept {
    for (u32 index = 0; index < store.favorite_count; ++index)
        if (store.favorites[index] == emoji) {
            *found = index;
            return true;
        }
    return false;
}

// Applies one edit to the visible favorite list; false only on capacity.
bool apply_favorite(ReactionPreferenceStore* store, ReactionEmojiRef emoji, bool favorite) noexcept {
    u32 index = 0;
    const bool present = favorite_index(*store, emoji, &index);
    if (favorite == present)
        return true;
    if (!favorite) {
        for (u32 next = index + 1; next < store->favorite_count; ++next)
            store->favorites[next - 1] = store->favorites[next];
        store->favorites[--store->favorite_count] = kNoEmoji;
        return true;
    }
    if (store->favorite_count == kReactionPreferenceFavoriteCapacity)
        return false;
    store->favorites[store->favorite_count++] = emoji;
    return true;
}

void push_pending(ReactionPreferenceStore* store, ReactionPreferenceKind kind, ReactionEmojiRef emoji,
                  u64 timestamp_ms) noexcept {
    if (store->pending_use_count == kReactionPreferencePendingUseCapacity) {
        // The oldest unsent use survives locally; only its replay is lost.
        for (u32 index = 1; index < store->pending_use_count; ++index)
            store->pending_uses[index - 1] = store->pending_uses[index];
        --store->pending_use_count;
    }
    store->pending_uses[store->pending_use_count++] = {emoji, timestamp_ms, kind};
}

bool track_one(ReactionPreferenceStore* store, ReactionPreferenceKind kind, ReactionEmojiRef emoji,
               u64 timestamp_ms, u32 uses) noexcept {
    ReactionPreferenceUsage* usage = admit_usage(store, store->tables[static_cast<u32>(kind)], emoji);
    if (!usage)
        return false;
    saturating_accumulate(usage->total_uses, uses);
    insert_recent(usage, timestamp_ms);
    return true;
}

} // namespace

bool reaction_preferences_initialize(ReactionPreferenceStore* store) noexcept {
    if (!store || store->collapsed_guilds.arena.base) {
        return false;
    }
    *store = ReactionPreferenceStore{};
    store->next_sequence = 1;
    for (auto& table : store->tables)
        clear_table(table);
    for (auto& favorite : store->favorites)
        favorite = kNoEmoji;
    return true;
}

void reaction_preferences_release(ReactionPreferenceStore* store) noexcept {
    if (!store) return;
    stable_storage_release(&store->collapsed_guilds);
    *store = ReactionPreferenceStore{};
}

bool reaction_preferences_recompute(ReactionPreferenceStore* store, u64 now_ms) noexcept {
    if (!store || !store_valid(*store) || now_ms == 0) {
        return false;
    }
    for (u32 kind = 0; kind < kReactionPreferenceKindCount; ++kind)
        recompute_table(store->tables[kind], static_cast<ReactionPreferenceKind>(kind), now_ms);
    return true;
}

ReactionPreferenceResult reaction_preferences_track_emoji(ReactionPreferenceStore* store,
                                                          ReactionPreferenceKind kind, ReactionEmojiRef emoji,
                                                          u64 timestamp_ms, u32 uses, bool pending) noexcept {
    if (!store || !store_valid(*store) || !kind_valid(kind) || !emoji.valid() || timestamp_ms == 0 ||
        uses == 0) {
        return ReactionPreferenceResult{ReactionPreferenceError::invalid_input, false, false};
    }
    if (!track_one(store, kind, emoji, timestamp_ms, uses))
        return ReactionPreferenceResult{ReactionPreferenceError::capacity, false, false};
    recompute_table(store->tables[static_cast<u32>(kind)], kind, timestamp_ms);
    if (pending) {
        for (u32 use = 0; use < minimum(uses, kReactionPreferenceRecentUseCapacity); ++use)
            push_pending(store, kind, emoji, timestamp_ms);
        saturating_increment(store->unsynced_revision);
    }
    saturating_increment(store->revision);
    return ReactionPreferenceResult{ReactionPreferenceError::none, true,
                                    reaction_preferences_is_favorite_emoji(*store, emoji)};
}

ReactionPreferenceResult reaction_preferences_track(ReactionPreferenceStore* store, u16 catalog_index,
                                                    u64 timestamp_ms, u32 uses) noexcept {
    return reaction_preferences_track_emoji(store, ReactionPreferenceKind::reaction,
                                            reaction_emoji_unicode(catalog_index), timestamp_ms, uses);
}

ReactionPreferenceResult reaction_preferences_toggle_favorite_emoji(ReactionPreferenceStore* store,
                                                                    ReactionEmojiRef emoji) noexcept {
    if (!store || !store_valid(*store) || !emoji.valid()) {
        return ReactionPreferenceResult{ReactionPreferenceError::invalid_input, false, false};
    }
    u32 index = 0;
    const bool favorite = !favorite_index(*store, emoji, &index);
    if ((favorite && store->favorite_count == kReactionPreferenceFavoriteCapacity) ||
        store->favorite_edit_count == kReactionPreferenceFavoriteEditCapacity)
        return ReactionPreferenceResult{ReactionPreferenceError::capacity, false, !favorite};
    apply_favorite(store, emoji, favorite);
    store->favorite_edits[store->favorite_edit_count++] = {emoji, favorite};
    saturating_increment(store->unsynced_revision);
    saturating_increment(store->revision);
    return ReactionPreferenceResult{ReactionPreferenceError::none, true, favorite};
}

ReactionPreferenceResult reaction_preferences_toggle_favorite(ReactionPreferenceStore* store,
                                                              u16 catalog_index) noexcept {
    return reaction_preferences_toggle_favorite_emoji(store, reaction_emoji_unicode(catalog_index));
}

bool reaction_preferences_is_favorite_emoji(const ReactionPreferenceStore& store,
                                            ReactionEmojiRef emoji) noexcept {
    u32 index = 0;
    return store_valid(store) && emoji.valid() && favorite_index(store, emoji, &index);
}

bool reaction_preferences_is_favorite(const ReactionPreferenceStore& store, u16 catalog_index) noexcept {
    return reaction_preferences_is_favorite_emoji(store, reaction_emoji_unicode(catalog_index));
}

ConstSlice<ReactionEmojiRef> reaction_preferences_favorites(const ReactionPreferenceStore& store) noexcept {
    return store_valid(store) ? ConstSlice<ReactionEmojiRef>{store.favorites, store.favorite_count}
                              : ConstSlice<ReactionEmojiRef>{};
}

ConstSlice<ReactionEmojiRef> reaction_preferences_frequently(const ReactionPreferenceStore& store,
                                                             ReactionPreferenceKind kind) noexcept {
    if (!store_valid(store) || !kind_valid(kind))
        return {};
    const auto& table = store.tables[static_cast<u32>(kind)];
    return {table.frequently, table.frequent_count};
}

const ReactionPreferenceTable* reaction_preferences_table(const ReactionPreferenceStore& store,
                                                          ReactionPreferenceKind kind) noexcept {
    return store_valid(store) && kind_valid(kind) ? &store.tables[static_cast<u32>(kind)] : nullptr;
}

bool reaction_preferences_apply_remote(ReactionPreferenceStore* store, ConstSlice<ReactionEmojiRef> favorites,
                                       ConstSlice<ReactionPreferenceRemoteUsage> reaction,
                                       ConstSlice<ReactionPreferenceRemoteUsage> message, u64 now_ms) noexcept {
    if (!store || !store_valid(*store) || !now_ms || (favorites.count && !favorites.data) ||
        (reaction.count && !reaction.data) || (message.count && !message.data))
        return false;
    store->favorite_count = 0;
    for (auto& favorite : store->favorites)
        favorite = kNoEmoji;
    for (u32 i = 0; i < favorites.count && store->favorite_count < kReactionPreferenceFavoriteCapacity; ++i) {
        u32 existing = 0;
        if (favorites[i].valid() && !favorite_index(*store, favorites[i], &existing))
            store->favorites[store->favorite_count++] = favorites[i];
    }
    for (u32 i = 0; i < store->favorite_edit_count; ++i)
        apply_favorite(store, store->favorite_edits[i].emoji, store->favorite_edits[i].favorite);
    const ConstSlice<ReactionPreferenceRemoteUsage> remote[]{reaction, message};
    for (u32 kind = 0; kind < kReactionPreferenceKindCount; ++kind) {
        auto& table = store->tables[kind];
        clear_table(table);
        for (u32 i = 0; i < remote[kind].count; ++i) {
            const auto& source = remote[kind][i];
            if (!source.emoji.valid() || source.recent_count > kReactionPreferenceRecentUseCapacity ||
                find_usage(table, source.emoji) != kReactionPreferenceUsageCapacity)
                continue;
            ReactionPreferenceUsage* usage = admit_usage(store, table, source.emoji);
            if (!usage)
                break;
            usage->total_uses = source.total_uses;
            for (u32 sample = 0; sample < source.recent_count; ++sample)
                if (source.recent_uses[sample])
                    insert_recent(usage, source.recent_uses[sample]);
        }
    }
    for (u32 i = 0; i < store->pending_use_count; ++i) {
        const auto& use = store->pending_uses[i];
        if (kind_valid(use.kind) && use.emoji.valid())
            track_one(store, use.kind, use.emoji, use.timestamp_ms, 1);
    }
    for (u32 kind = 0; kind < kReactionPreferenceKindCount; ++kind) {
        recompute_table(store->tables[kind], static_cast<ReactionPreferenceKind>(kind), now_ms);
        // An empty authoritative table is seeded again by its picker.
        store->tables[kind].seeded = store->tables[kind].usage_count != 0;
    }
    saturating_increment(store->revision);
    return true;
}

void reaction_preferences_commit(ReactionPreferenceStore* store, u32 favorite_edits, u32 pending_uses) noexcept {
    if (!store || !store_valid(*store))
        return;
    favorite_edits = minimum(favorite_edits, store->favorite_edit_count);
    pending_uses = minimum(pending_uses, store->pending_use_count);
    for (u32 i = favorite_edits; i < store->favorite_edit_count; ++i)
        store->favorite_edits[i - favorite_edits] = store->favorite_edits[i];
    store->favorite_edit_count -= favorite_edits;
    for (u32 i = pending_uses; i < store->pending_use_count; ++i)
        store->pending_uses[i - pending_uses] = store->pending_uses[i];
    store->pending_use_count -= pending_uses;
}

bool reaction_preferences_section_collapsed(const ReactionPreferenceStore& store,
                                            u32 section, u64 guild_id) noexcept {
    if (!store_valid(store) || (!guild_id && section >= 10)) return false;
    if (!guild_id) return (store.collapsed_sections & (1u << section)) != 0;
    const auto* guilds = stable_storage_data<u64>(store.collapsed_guilds);
    for (u32 i = 0; i < store.collapsed_guild_count; ++i)
        if (guilds[i] == guild_id) return true;
    return false;
}

ReactionPreferenceResult reaction_preferences_toggle_section(ReactionPreferenceStore* store,
                                                              u32 section, u64 guild_id) noexcept {
    if (!store || !store_valid(*store) || (!guild_id && section >= 10))
        return {ReactionPreferenceError::invalid_input, false, false};
    if (!guild_id) store->collapsed_sections ^= static_cast<u16>(1u << section);
    else {
        auto* guilds = stable_storage_data<u64>(store->collapsed_guilds);
        u32 found = 0;
        while (found < store->collapsed_guild_count && guilds[found] != guild_id) ++found;
        if (found < store->collapsed_guild_count) {
            for (u32 i = found + 1; i < store->collapsed_guild_count; ++i)
                guilds[i - 1] = guilds[i];
            guilds[--store->collapsed_guild_count] = 0;
            if (!store->collapsed_guild_count) stable_storage_release(&store->collapsed_guilds);
        } else {
            if (store->collapsed_guild_count == kReactionPreferenceCollapsedGuildCapacity)
                return {ReactionPreferenceError::capacity, false, false};
            const bool creating = !store->collapsed_guilds.arena.base;
            if ((creating && !stable_storage_initialize(&store->collapsed_guilds,
                    kReactionPreferenceCollapsedGuildCapacity, 0, sizeof(u64), alignof(u64), true)) ||
                !stable_storage_reserve(&store->collapsed_guilds, store->collapsed_guild_count + 1)) {
                if (creating) stable_storage_release(&store->collapsed_guilds);
                return {ReactionPreferenceError::capacity, false, false};
            }
            stable_storage_data<u64>(store->collapsed_guilds)[store->collapsed_guild_count++] = guild_id;
        }
    }
    saturating_increment(store->revision);
    return {ReactionPreferenceError::none, true, false};
}

} // namespace maicord
