#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Emoji favorites and frecency inside the FRECENCY_AND_FAVORITES settings
// proto (settings-proto/2): favorite_emojis = 5 (repeated string emojis = 1),
// emoji_frecency = 6 and emoji_reaction_frecency = 13 (map<string,
// FrecencyItem> emojis = 1). Keys are custom emoji IDs or a name of a Unicode
// emoji; keys written by other clients are always sent back unchanged.
inline constexpr u32 kEmojiPreferencesMaximumGroupBytes = 128 * 1024;
inline constexpr u32 kEmojiPreferencesMaximumKeyBytes = 96;
inline constexpr u32 kEmojiPreferencesMaximumFavorites = 250;
inline constexpr u32 kEmojiPreferencesMaximumFrecencyEntries = 100;
inline constexpr u32 kEmojiPreferencesRecentUses = 10;
inline constexpr u32 kEmojiPreferencesMaximumPatchBytes =
    ((3 * (kEmojiPreferencesMaximumGroupBytes + 8) + 2) / 3) * 4 + 64;

enum class EmojiPreferencesError : u8 {
    none,
    input,
    protobuf,
    utf8,
    capacity,
    envelope,
};

struct EmojiPreferencesSnapshot {
    // Concatenated occurrences of each group in merge order; borrowed from
    // the caller's owned output. sticker_frecency (field 4, map<string,
    // FrecencyItem> stickers = 1 keyed by sticker ID) is read only and empty
    // when malformed; it never makes the decode fail.
    ConstSlice<u8> favorites, frecency, reaction_frecency, sticker_frecency;
    u32 data_version;
    bool versions_present;
};

struct EmojiPreferencesResult {
    u32 group_bytes;
    EmojiPreferencesError error;
    constexpr bool ok() const noexcept {
        return error == EmojiPreferencesError::none;
    }
};

struct EmojiFrecencyItem {
    u64 recent_uses[kEmojiPreferencesRecentUses]; // ascending, newest retained
    u32 total_uses;
    i32 frecency;
    i32 score;
    u8 recent_count;
};

// Validates the whole proto. A null output measures; otherwise output must
// hold result.group_bytes and must not overlap the input.
EmojiPreferencesResult emoji_preferences_decode(ConstSlice<u8> proto, Slice<u8> output,
                                                EmojiPreferencesSnapshot* snapshot) noexcept;
using EmojiFavoriteVisitor = bool (*)(void* context, Utf8View key) noexcept;
using EmojiFrecencyVisitor = bool (*)(void* context, Utf8View key, const EmojiFrecencyItem& item) noexcept;
// Visit in wire order; for map duplicates the last entry is authoritative.
EmojiPreferencesError emoji_preferences_visit_favorites(ConstSlice<u8> group, EmojiFavoriteVisitor,
                                                        void* context) noexcept;
EmojiPreferencesError emoji_preferences_visit_frecency(ConstSlice<u8> group, EmojiFrecencyVisitor,
                                                       void* context) noexcept;

struct EmojiFavoriteEdit {
    Utf8View key;
    bool favorite;
};
struct EmojiFrecencyEntry {
    Utf8View key;
    EmojiFrecencyItem item;
    // Local uses changed the item since the observed snapshot.
    bool updated;
};
// Maps a key to an opaque emoji identity, or 0 when it cannot be resolved.
// Observed keys match edits and entries by identity (by exact key when 0), so
// "slight_smile" from another client and this client's key for the same
// emoji are one entry, kept under the observed key.
using EmojiPreferenceIdentity = u64 (*)(void* context, Utf8View key) noexcept;
struct EmojiPreferencesPatch {
    // Group 5 is written only when edits exist. Observed favorites keep their
    // bytes and order; a removal drops every observed key of that emoji, and
    // an add appends its key only when no observed key names the emoji.
    ConstSlice<u8> observed_favorites;
    ConstSlice<EmojiFavoriteEdit> favorite_edits;
    // Groups 6 and 13 keep observed entries in wire order and byte for byte,
    // except: the last observed entry matching an updated entry gets the new
    // value under its observed key; a resolvable observed key with no entry
    // (evicted locally) is dropped; unresolvable keys and earlier duplicates
    // are kept while the 100-entry limit allows. Updated entries the group
    // lacks are appended with their own key. Unknown fields are preserved.
    ConstSlice<u8> observed_frecency, observed_reaction_frecency;
    ConstSlice<EmojiFrecencyEntry> frecency, reaction_frecency;
    EmojiPreferenceIdentity identity;
    void* identity_context;
    u32 required_data_version;
    bool write_frecency;
};
struct EmojiPreferencesWriteResult {
    u32 bytes, proto_bytes, favorite_count;
    EmojiPreferencesError error;
    constexpr bool ok() const noexcept {
        return error == EmojiPreferencesError::none;
    }
};
// Writes {"settings":"<base64>","required_data_version":N}. Scratch holds the
// raw proto (kEmojiPreferencesMaximumGroupBytes * 3 + 32 always suffices).
// Adding past 250 favorites fails with capacity; outputs may change on failure.
EmojiPreferencesWriteResult emoji_preferences_write_patch(const EmojiPreferencesPatch&, Slice<u8> scratch,
                                                          Slice<u8> json) noexcept;
// Accepts only that envelope with root groups 5, 6 and/or 13.
EmojiPreferencesError emoji_preferences_validate_patch_json(Utf8View) noexcept;
} // namespace maicord
