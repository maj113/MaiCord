#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord {
struct MessageComposer;

// The emoji picker's data model. The expression picker's Emoji tab and the
// message reaction popout both present it through UiMediaPickerScene.
inline constexpr u32 kReactionPickerMaximumQueryBytes = 48;
inline constexpr u32 kReactionPickerBorrowedQueryMaximumBytes = 256;
inline constexpr u32 kReactionPickerCatalogCapacity = 3'780;
// Custom results index the chat state's emoji array, so the picker holds
// every emoji the store can.
inline constexpr u32 kReactionPickerCustomEmojiCapacity = 131'072;
inline constexpr u32 kReactionPickerGuildCapacity = 256;
inline constexpr u32 kReactionPickerResultCapacity =
    kReactionPickerCatalogCapacity + kReactionPickerCustomEmojiCapacity +
    kReactionPreferenceFavoriteCapacity + kReactionPreferenceFrequentCapacity;
inline constexpr u32 kReactionPickerSectionCapacity = kReactionPickerGuildCapacity + 10;
// ReactionPickerSection::local values. Unicode categories occupy
// kReactionPickerSectionUnicode + EmojiCategory.
inline constexpr u32 kReactionPickerSectionFrequent = 0;
inline constexpr u32 kReactionPickerSectionFavorites = 1;
inline constexpr u32 kReactionPickerSectionUnicode = 2;
inline constexpr u32 kReactionPickerSectionSearch = 10;
inline constexpr u32 kReactionPickerSectionGuild = 16;

enum class ReactionPickerError : u8 {
    none,
    invalid_input,
    invalid_character,
    capacity,
};

struct ReactionPickerGuild {
    u64 id;
    u32 icon_variant;
    u32 first_result;
    // The server's span of custom_order.
    u32 custom_first, custom_count;
    u16 name_bytes;
    bool animated;
    char name[176];
};

// One contiguous result span. Collapsing keeps the span; only browse
// sections (never search results) persist their collapse per account.
struct ReactionPickerSection {
    u64 guild_id;
    u32 local;
    u32 first, count;
    bool collapsed;
};

struct ReactionPickerSelection {
    u64 emoji_id;
    Utf8View name;
    u32 catalog_index;
    bool custom;
    bool animated;
};

struct ReactionPickerResult {
    ReactionPickerError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == ReactionPickerError::none;
    }
};

// Custom emoji are borrowed from the chat state by index: results and the
// per-server name order never copy names or IDs. A changed emoji array
// (emoji_mutations) leaves custom results unresolved until reopened.
struct ReactionPickerModel {
    MessageComposer* search;
    Arena storage;
    // Unicode catalog indices, or kReactionPickerCustomResult | emoji index.
    u32* results;
    // Usable custom emoji (chat state indices), per server in name order.
    u32* custom_order;
    const ChatStateStore* state;
    ReactionPickerGuild* guilds;
    ReactionPickerSection* sections;
    ReactionPreferenceStore* preferences;
    u64 message_id;
    u32 search_revision;
    u32 query_bytes;
    u32 result_count;
    u32 result_capacity;
    u32 custom_emoji_count;
    u32 guild_count;
    u32 section_count;
    u32 revision;
    u32 emoji_mutations;
    ReactionPreferenceKind preference_kind;
    bool search_borrowed;
    char query[kReactionPickerMaximumQueryBytes];
};

// message_id is the owner identity (a message, or the composer's channel).
ReactionPickerResult reaction_picker_open(ReactionPickerModel* picker, u64 message_id,
                                          ReactionPreferenceStore* preferences = nullptr, u64 now_ms = 0,
                                          const ChatStateStore* state = nullptr, u64 guild_id = 0,
                                          MessageComposer* borrowed_search = nullptr,
                                          ReactionPreferenceKind kind = ReactionPreferenceKind::reaction) noexcept;
void reaction_picker_release(ReactionPickerModel* picker) noexcept;
// Rebuilds the results once the search document's text revision changes.
ReactionPickerResult reaction_picker_refresh_search(ReactionPickerModel* picker) noexcept;
ReactionPickerResult reaction_picker_refresh_preferences(ReactionPickerModel* picker) noexcept;
ReactionPickerResult reaction_picker_toggle_section(ReactionPickerModel*, u32 section) noexcept;
constexpr ReactionEmojiRef reaction_picker_selection_emoji(const ReactionPickerSelection& selection) noexcept {
    return selection.custom ? reaction_emoji_custom(selection.emoji_id)
           : selection.catalog_index <= 0xFFFEu
               ? reaction_emoji_unicode(static_cast<u16>(selection.catalog_index))
               : reaction_emoji_unicode(kReactionPreferenceInvalidCatalogIndex);
}
// Logical results borrow the original catalog/custom snapshot. Label is optional.
bool reaction_picker_result_at(const ReactionPickerModel&, u32 logical_index, ReactionPickerSelection*,
                               Utf8View* label = nullptr) noexcept;
Utf8View reaction_picker_section_label(const ReactionPickerModel&, u32 section) noexcept;
// The server a custom emoji result comes from; empty for Unicode emoji.
Utf8View reaction_picker_result_guild_name(const ReactionPickerModel&, u32 logical_index) noexcept;
// Account settings keys: custom emoji IDs in decimal, Unicode emoji by
// emoji_shortcode_key() (keys for entries this client adds). Returns 0 when
// the emoji has no settings key. Parsing accepts any alias, catalog name or
// the raw emoji sequence.
u32 reaction_emoji_settings_key(ReactionEmojiRef, Slice<char> output) noexcept;
ReactionEmojiRef reaction_emoji_from_settings_key(Utf8View key) noexcept;
// Unique custom (<:name:id>) and Unicode emoji in message content, in order.
u32 reaction_emoji_scan_message(Utf8View content, Slice<ReactionEmojiRef> output) noexcept;

} // namespace maicord
