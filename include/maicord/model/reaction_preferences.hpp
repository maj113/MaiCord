#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/stable_storage.hpp"

namespace maicord {

inline constexpr u32 kReactionPreferenceUsageCapacity = 100;
inline constexpr u32 kReactionPreferenceRecentUseCapacity = 10;
inline constexpr u32 kReactionPreferenceFrequentCapacity = 42;
inline constexpr u32 kReactionPreferenceFavoriteCapacity = 250;
inline constexpr u16 kReactionPreferenceInvalidCatalogIndex = 0xFFFFu;
inline constexpr u32 kReactionPreferenceCollapsedGuildCapacity = 256;
inline constexpr u32 kReactionPreferencePendingUseCapacity = 128;
inline constexpr u32 kReactionPreferenceFavoriteEditCapacity = kReactionPreferenceFavoriteCapacity;

enum class ReactionPreferenceError : u8 {
    none,
    invalid_input,
    capacity,
};

// Reaction-picker and composer frecency live in separate settings
// groups (emoji_reaction_frecency and emoji_frecency) with different scoring.
enum class ReactionPreferenceKind : u8 {
    reaction,
    message,
};
inline constexpr u32 kReactionPreferenceKindCount = 2;

// Exactly one identity: a custom emoji snowflake or a Unicode catalog index.
struct ReactionEmojiRef {
    u64 custom_id;
    u16 catalog_index;

    constexpr bool custom() const noexcept {
        return custom_id != 0;
    }
    constexpr bool valid() const noexcept {
        return (custom_id != 0) != (catalog_index != kReactionPreferenceInvalidCatalogIndex);
    }
    friend constexpr bool operator==(ReactionEmojiRef a, ReactionEmojiRef b) noexcept {
        return a.custom_id == b.custom_id && a.catalog_index == b.catalog_index;
    }
};

constexpr ReactionEmojiRef reaction_emoji_unicode(u16 catalog_index) noexcept {
    return {0, catalog_index};
}
constexpr ReactionEmojiRef reaction_emoji_custom(u64 id) noexcept {
    return {id, kReactionPreferenceInvalidCatalogIndex};
}

struct ReactionPreferenceUsage {
    ReactionEmojiRef emoji;
    u64 recent_uses[kReactionPreferenceRecentUseCapacity];
    u32 total_uses;
    u32 sequence;
    u32 frecency;
    u16 score;
    u8 recent_count;
    bool occupied;
};

struct ReactionPreferenceTable {
    ReactionPreferenceUsage usage[kReactionPreferenceUsageCapacity];
    ReactionEmojiRef frequently[kReactionPreferenceFrequentCapacity];
    u32 usage_count;
    u32 frequent_count;
    bool seeded;
};

// A local use not yet confirmed by a settings write; replayed over each
// authoritative snapshot.
struct ReactionPreferencePendingUse {
    ReactionEmojiRef emoji;
    u64 timestamp_ms;
    ReactionPreferenceKind kind;
};

struct ReactionPreferenceFavoriteEdit {
    ReactionEmojiRef emoji;
    bool favorite;
};

// A decoded remote frecency entry; the recent uses are the newest samples.
struct ReactionPreferenceRemoteUsage {
    ReactionEmojiRef emoji;
    u64 recent_uses[kReactionPreferenceRecentUseCapacity];
    u32 total_uses;
    u8 recent_count;
};

struct ReactionPreferenceStore {
    // Session-owned, account-scoped section state. Zero until the first guild
    // collapse. No pointer may escape this owner.
    StableStorage collapsed_guilds;
    u32 collapsed_guild_count;
    u16 collapsed_sections; // frequent, favorites, then eight Unicode categories
    ReactionPreferenceTable tables[kReactionPreferenceKindCount];
    ReactionEmojiRef favorites[kReactionPreferenceFavoriteCapacity];
    ReactionPreferencePendingUse pending_uses[kReactionPreferencePendingUseCapacity];
    ReactionPreferenceFavoriteEdit favorite_edits[kReactionPreferenceFavoriteEditCapacity];
    u32 favorite_count;
    u32 pending_use_count;
    u32 favorite_edit_count;
    u32 next_sequence;
    u32 revision;
    // Revision of local changes still unsent (uses or favorite edits).
    u32 unsynced_revision;
};

struct ReactionPreferenceResult {
    ReactionPreferenceError error;
    bool changed;
    bool favorite;

    constexpr bool ok() const noexcept {
        return error == ReactionPreferenceError::none;
    }
};

// Zero initialize; a live guild bank must be released before reset.
bool reaction_preferences_initialize(ReactionPreferenceStore* store) noexcept;
void reaction_preferences_release(ReactionPreferenceStore* store) noexcept;
// Records one use and queues it for settings sync unless `pending` is false
// (seeds). Unicode overloads track the reaction table.
ReactionPreferenceResult reaction_preferences_track_emoji(ReactionPreferenceStore* store,
                                                          ReactionPreferenceKind kind, ReactionEmojiRef emoji,
                                                          u64 timestamp_ms, u32 uses = 1,
                                                          bool pending = true) noexcept;
ReactionPreferenceResult reaction_preferences_track(
    ReactionPreferenceStore* store, u16 catalog_index, u64 timestamp_ms,
    u32 uses = 1) noexcept;
ReactionPreferenceResult reaction_preferences_toggle_favorite_emoji(ReactionPreferenceStore* store,
                                                                    ReactionEmojiRef emoji) noexcept;
ReactionPreferenceResult reaction_preferences_toggle_favorite(
    ReactionPreferenceStore* store, u16 catalog_index) noexcept;
bool reaction_preferences_is_favorite_emoji(const ReactionPreferenceStore& store,
                                            ReactionEmojiRef emoji) noexcept;
bool reaction_preferences_is_favorite(const ReactionPreferenceStore& store,
                                      u16 catalog_index) noexcept;
ConstSlice<ReactionEmojiRef> reaction_preferences_favorites(
    const ReactionPreferenceStore& store) noexcept;
ConstSlice<ReactionEmojiRef> reaction_preferences_frequently(
    const ReactionPreferenceStore& store,
    ReactionPreferenceKind kind = ReactionPreferenceKind::reaction) noexcept;
const ReactionPreferenceTable* reaction_preferences_table(const ReactionPreferenceStore& store,
                                                          ReactionPreferenceKind kind) noexcept;
bool reaction_preferences_recompute(ReactionPreferenceStore* store,
                                    u64 now_ms) noexcept;
// Replaces favorites and both tables with an authoritative snapshot, then
// replays still-unsent favorite edits and uses on top of it.
bool reaction_preferences_apply_remote(ReactionPreferenceStore* store, ConstSlice<ReactionEmojiRef> favorites,
                                       ConstSlice<ReactionPreferenceRemoteUsage> reaction,
                                       ConstSlice<ReactionPreferenceRemoteUsage> message, u64 now_ms) noexcept;
// Drops the oldest favorite edits and pending uses after a write that
// carried them was confirmed.
void reaction_preferences_commit(ReactionPreferenceStore* store, u32 favorite_edits, u32 pending_uses) noexcept;
bool reaction_preferences_section_collapsed(const ReactionPreferenceStore& store,
                                            u32 section, u64 guild_id = 0) noexcept;
ReactionPreferenceResult reaction_preferences_toggle_section(ReactionPreferenceStore* store,
                                                              u32 section, u64 guild_id = 0) noexcept;

} // namespace maicord
