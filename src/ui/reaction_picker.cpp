#include "maicord/ui/reaction_picker.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/emoji_catalog.hpp"

namespace maicord {
namespace {

inline constexpr Utf8View kReactionSeeds[] = {
    utf8_literal("hundred_points"),
    utf8_literal("hundred_points"),
    utf8_literal("thumbs_up"),
    utf8_literal("thumbs_up"),
    utf8_literal("thumbs_down"),
    utf8_literal("thumbs_down"),
    utf8_literal("red_heart"),
    utf8_literal("index_pointing_up"),
    utf8_literal("eyes"),
    utf8_literal("weary_face"),
    utf8_literal("grinning_squinting_face"),
    utf8_literal("check_mark_button"),
    utf8_literal("cross_mark"),
};
// Default frequently-used emoji for the composer picker.
inline constexpr Utf8View kMessageSeeds[] = {
    utf8_literal("thumbs_up"),      utf8_literal("eyes"),
    utf8_literal("grinning_squinting_face"), utf8_literal("watermelon"),
    utf8_literal("fork_and_knife"), utf8_literal("face_savoring_food"),
    utf8_literal("weary_face"),     utf8_literal("tired_face"),
    utf8_literal("pile_of_poo"),    utf8_literal("hundred_points"),
};

inline constexpr u16 kReactionPickerCustomResult = 0x8000u;

// Picker section headers.
inline constexpr Utf8View kSectionNames[] = {
    utf8_literal("Frequently Used"), utf8_literal("Favorites"),  utf8_literal("People"),
    utf8_literal("Nature"),          utf8_literal("Food"),       utf8_literal("Activities"),
    utf8_literal("Travel"),          utf8_literal("Objects"),    utf8_literal("Symbols"),
    utf8_literal("Flags"),           utf8_literal("Search results"),
};
inline constexpr usize kReactionPickerStorageBytes =
    sizeof(u16) * kReactionPickerResultCapacity +
    sizeof(ReactionPickerCustomEmoji) * kReactionPickerCustomEmojiCapacity +
    sizeof(ReactionPickerGuild) * kReactionPickerGuildCapacity + sizeof(MessageComposer) +
    sizeof(ReactionPickerSection) * kReactionPickerSectionCapacity + 128;

bool storage_valid(const ReactionPickerModel* picker) noexcept {
    return picker && picker->storage.base && picker->result_catalog_indices && picker->custom_emojis &&
           picker->guilds && picker->sections;
}

bool initialize_storage(ReactionPickerModel* picker, const ChatStateStore* state,
                        MessageComposer* borrowed_search) noexcept {
    *picker = ReactionPickerModel{};
    if (!arena_initialize(&picker->storage, kReactionPickerStorageBytes, 0, 4 * 1024)) {
        return false;
    }
    const u32 emoji_capacity =
        maximum(1u, minimum(kReactionPickerCustomEmojiCapacity, state ? state->emoji_count : 0));
    const u32 guild_capacity =
        maximum(1u, minimum(kReactionPickerGuildCapacity, state ? state->guild_count : 0));
    picker->search =
        borrowed_search ? borrowed_search : arena_allocate_zero<MessageComposer>(&picker->storage);
    picker->search_borrowed = borrowed_search != nullptr;
    if (!picker->search ||
        (!borrowed_search &&
         !message_composer_initialize(picker->search, {picker->query, kReactionPickerMaximumQueryBytes})))
        return false;
    if (!borrowed_search)
        message_composer_reset(picker->search, 1, false);
    picker->query_bytes = picker->search->bytes;
    picker->search_revision = picker->search->text_revision;
    picker->custom_emojis =
        arena_allocate_zero<ReactionPickerCustomEmoji>(&picker->storage, emoji_capacity);
    picker->guilds = arena_allocate_zero<ReactionPickerGuild>(&picker->storage, guild_capacity);
    picker->guild_count = 0;
    if (!picker->custom_emojis || !picker->guilds) {
        arena_release(&picker->storage);
        *picker = ReactionPickerModel{};
        return false;
    }
    return true;
}

bool push_result(ReactionPickerModel* picker, u32 catalog_index) noexcept {
    if (!storage_valid(picker) || picker->result_count >= picker->result_capacity ||
        catalog_index > 0xFFFFu) {
        return false;
    }
    picker->result_catalog_indices[picker->result_count++] = static_cast<u16>(catalog_index);
    return true;
}

Utf8View custom_name(const ReactionPickerCustomEmoji& emoji) noexcept {
    return emoji.id != 0 && emoji.name_bytes != 0 && emoji.name_bytes <= kCustomEmojiNameMaximumBytes
               ? Utf8View{emoji.name, emoji.name_bytes}
               : Utf8View{};
}

bool push_section(ReactionPickerModel* picker, u32 section) noexcept {
    if (!storage_valid(picker) || picker->section_count >= picker->guild_count + 10)
        return false;
    if (picker->section_count) {
        auto& previous = picker->sections[picker->section_count - 1];
        previous.count = picker->result_count - previous.first;
    }
    const u64 guild = section >= kReactionPickerSectionGuild &&
                              section - kReactionPickerSectionGuild < picker->guild_count
                          ? picker->guilds[section - kReactionPickerSectionGuild].id
                          : 0;
    const u32 local = guild ? kReactionPickerSectionGuild : section;
    const bool collapsed = picker->preferences && section != kReactionPickerSectionSearch &&
                           reaction_preferences_section_collapsed(*picker->preferences, local, guild);
    picker->sections[picker->section_count++] = {guild, local, picker->result_count, 0, collapsed};
    return true;
}
u32 query_limit(const ReactionPickerModel& picker) noexcept {
    return picker.search_borrowed ? kReactionPickerBorrowedQueryMaximumBytes
                                  : kReactionPickerMaximumQueryBytes;
}

Utf8View section_label(const ReactionPickerModel& picker, u32 section) noexcept {
    if (section >= picker.section_count)
        return {};
    const auto& source = picker.sections[section];
    if (source.guild_id)
        for (u32 i = 0; i < picker.guild_count; ++i)
            if (picker.guilds[i].id == source.guild_id)
                return {picker.guilds[i].name, picker.guilds[i].name_bytes};
    return kSectionNames[minimum(source.local, kReactionPickerSectionSearch)];
}
bool custom_before(const ReactionPickerCustomEmoji& left, const ReactionPickerCustomEmoji& right) noexcept {
    const Utf8View left_name = custom_name(left);
    const Utf8View right_name = custom_name(right);
    const u32 shared = minimum(left_name.bytes, right_name.bytes);
    for (u32 index = 0; index < shared; ++index) {
        const u8 left_value = ascii_lower(static_cast<u8>(left_name.data[index]));
        const u8 right_value = ascii_lower(static_cast<u8>(right_name.data[index]));
        if (left_value != right_value) {
            return left_value < right_value;
        }
    }
    return left_name.bytes != right_name.bytes ? left_name.bytes < right_name.bytes : left.id < right.id;
}

void sort_custom_emojis(ReactionPickerCustomEmoji* emojis, u32 count) noexcept {
    if (count <= 32) {
        for (u32 index = 1; index < count; ++index) {
            const auto value = emojis[index];
            u32 cursor = index;
            while (cursor && custom_before(value, emojis[cursor - 1])) {
                emojis[cursor] = emojis[cursor - 1];
                --cursor;
            }
            emojis[cursor] = value;
        }
        return;
    }
    u32 ordered = 1;
    while (ordered < count && !custom_before(emojis[ordered], emojis[ordered - 1]))
        ++ordered;
    if (ordered == count)
        return;

    // All emoji in this slice share one guild, so guild_index is borrowed as a
    // rank that keeps equal keys in input order, then restored below.
    const u16 guild_index = emojis[0].guild_index;
    for (u32 index = 0; index < count; ++index)
        emojis[index].guild_index = static_cast<u16>(index);
    heap_sort(emojis, count, [](const auto& left, const auto& right) noexcept {
        return custom_before(left, right) ||
               (left.id == right.id && !custom_before(right, left) &&
                left.guild_index < right.guild_index);
    });
    // No failure exit or callback occurs between rank assignment and restore.
    for (u32 index = 0; index < count; ++index)
        emojis[index].guild_index = guild_index;
}

bool snapshot_custom_emojis(ReactionPickerModel* picker, const ChatStateStore* state,
                            u64 guild_id) noexcept {
    if (!picker->storage.base || !picker->custom_emojis || !picker->guilds) {
        return false;
    }
    picker->custom_emoji_count = picker->guild_count = 0;
    if (!state || !state->emojis)
        return true;
    // Selected server first, followed by the account's server order.
    for (u32 pass = 0; pass <= state->guild_count; ++pass) {
        const auto* guild = pass == 0 ? chat_state_store_find_guild(*state, guild_id)
                                      : &state->guilds[state->guild_order[pass - 1]];
        if (!guild || guild->unavailable || (pass && guild->id == guild_id))
            continue;
        const u32 first = picker->custom_emoji_count;
        for (u32 index = 0; index < state->emoji_count; ++index) {
            const auto& source = state->emojis[index];
            if (source.guild_id != guild->id || !chat_state_store_emoji_usable(*state, source))
                continue;
            // Beyond the bound the snapshot keeps the earlier servers (the
            // selected one first) instead of failing the whole picker.
            if (picker->custom_emoji_count == kReactionPickerCustomEmojiCapacity ||
                picker->guild_count == kReactionPickerGuildCapacity)
                break;
            const auto name = chat_state_store_emoji_name(source);
            if (!name.bytes || name.bytes > kCustomEmojiNameMaximumBytes)
                return false;
            auto& target = picker->custom_emojis[picker->custom_emoji_count++];
            target = {};
            target.id = source.id;
            target.flags = source.flags;
            target.guild_index = static_cast<u16>(picker->guild_count);
            target.name_bytes = static_cast<u8>(name.bytes);
            copy_bytes(target.name, name.data, name.bytes);
        }
        if (first == picker->custom_emoji_count) {
            if (picker->custom_emoji_count == kReactionPickerCustomEmojiCapacity)
                break;
            continue;
        }
        auto& target = picker->guilds[picker->guild_count++];
        target = {};
        target.id = guild->id;
        target.icon_variant = guild->icon_variant;
        const auto hash = chat_state_store_guild_icon_hash(*guild);
        target.animated = hash.bytes > 2 && hash.data[0] == 'a' && hash.data[1] == '_';
        auto name = chat_state_store_guild_name(*state, *guild);
        if (name.bytes == 0 || name.bytes > sizeof(target.name))
            name = utf8_literal("Server emoji");
        target.name_bytes = static_cast<u16>(name.bytes);
        copy_bytes(target.name, name.data, name.bytes);
        sort_custom_emojis(picker->custom_emojis + first, picker->custom_emoji_count - first);
    }
    return true;
}

bool find_exact_name(Utf8View name, u32* catalog_index) noexcept {
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    for (u32 index = 0; index < catalog.count; ++index) {
        if (utf8_equal(catalog[index].name(), name)) {
            *catalog_index = index;
            return true;
        }
    }
    return false;
}

bool seed_preferences(ReactionPreferenceStore* preferences, ReactionPreferenceKind kind, u64 now_ms) noexcept {
    const auto* table = reaction_preferences_table(*preferences, kind);
    if (table && table->seeded) {
        return true;
    }
    if (!table || now_ms == 0) {
        return false;
    }
    if (table->usage_count == 0) {
        const bool reaction = kind == ReactionPreferenceKind::reaction;
        const Utf8View* seeds = reaction ? kReactionSeeds : kMessageSeeds;
        const u32 count = reaction ? static_cast<u32>(sizeof(kReactionSeeds) / sizeof(kReactionSeeds[0]))
                                   : static_cast<u32>(sizeof(kMessageSeeds) / sizeof(kMessageSeeds[0]));
        for (u32 index = 0; index < count; ++index) {
            u32 catalog_index = 0;
            // Seeds are local defaults only; they are written with later real use.
            if (!find_exact_name(seeds[index], &catalog_index) || catalog_index > 0xFFFEu ||
                !reaction_preferences_track_emoji(preferences, kind,
                                                  reaction_emoji_unicode(static_cast<u16>(catalog_index)),
                                                  now_ms, 1, false)
                     .ok()) {
                return false;
            }
        }
    }
    preferences->tables[static_cast<u32>(kind)].seeded = true;
    return true;
}

// Picker result value for a preference identity, or ~0u when this snapshot
// cannot show it (unknown Unicode or an unavailable custom emoji).
u32 preference_result(const ReactionPickerModel& picker, ReactionEmojiRef emoji, u32 catalog_count) noexcept {
    if (!emoji.custom())
        return emoji.catalog_index < catalog_count ? emoji.catalog_index : ~0u;
    for (u32 i = 0; i < picker.custom_emoji_count; ++i)
        if (picker.custom_emojis[i].id == emoji.custom_id)
            return kReactionPickerCustomResult | i;
    return ~0u;
}

ReactionPickerResult rebuild_results(ReactionPickerModel* picker) noexcept {
    if (!storage_valid(picker) || !picker->message_id || picker->query_bytes > query_limit(*picker))
        return {ReactionPickerError::invalid_input, false};
    picker->result_count = picker->section_count = 0;
    const auto catalog = unicode_emoji_catalog();
    if (catalog.count > kReactionPickerCatalogCapacity ||
        picker->custom_emoji_count > kReactionPickerCustomEmojiCapacity)
        return {ReactionPickerError::capacity, false};
    const Utf8View query = message_composer_text(*picker->search);
    if (!query.bytes) {
        if (picker->preferences) {
            const auto favorites = reaction_preferences_favorites(*picker->preferences);
            bool shown = false;
            for (u32 i = 0; i < favorites.count; ++i) {
                const u32 value = preference_result(*picker, favorites[i], catalog.count);
                if (value == ~0u)
                    continue;
                if ((!shown && !push_section(picker, kReactionPickerSectionFavorites)) ||
                    !push_result(picker, value))
                    return {ReactionPickerError::capacity, false};
                shown = true;
            }
        }
        if (!push_section(picker, kReactionPickerSectionFrequent))
            return {ReactionPickerError::capacity, false};
        if (picker->preferences) {
            const auto frequent = reaction_preferences_frequently(*picker->preferences, picker->preference_kind);
            for (u32 i = 0; i < frequent.count; ++i) {
                const u32 value = preference_result(*picker, frequent[i], catalog.count);
                if (value != ~0u && !push_result(picker, value))
                    return {ReactionPickerError::capacity, false};
            }
        } else {
            u8 included[kReactionPickerCatalogCapacity]{};
            for (const auto name : kReactionSeeds) {
                u32 index = 0;
                if (find_exact_name(name, &index) && !included[index]) {
                    if (!push_result(picker, index))
                        return {ReactionPickerError::capacity, false};
                    included[index] = 1;
                }
            }
        }
        for (u32 i = 0; i < picker->custom_emoji_count; ++i) {
            const u32 guild = picker->custom_emojis[i].guild_index;
            if (!i || guild != picker->custom_emojis[i - 1].guild_index) {
                if (!push_section(picker, kReactionPickerSectionGuild + guild))
                    return {ReactionPickerError::capacity, false};
                picker->guilds[guild].first_result = picker->result_count;
            }
            if (!push_result(picker, kReactionPickerCustomResult | i))
                return {ReactionPickerError::capacity, false};
        }
        for (u32 category = 0; category < kEmojiCategoryCount; ++category) {
            if (!push_section(picker, kReactionPickerSectionUnicode + category))
                return {ReactionPickerError::capacity, false};
            const auto range = emoji_catalog_category_range(static_cast<EmojiCategory>(category));
            for (u32 i = range.first; i < range.one_past_last; ++i)
                if (!push_result(picker, i))
                    return {ReactionPickerError::capacity, false};
        }
    } else {
        if (!push_section(picker, kReactionPickerSectionSearch))
            return {ReactionPickerError::capacity, false};
        for (u32 pass = 0; pass < 2; ++pass) {
            for (u32 i = 0; i < picker->custom_emoji_count; ++i) {
                const auto name = custom_name(picker->custom_emojis[i]);
                const bool prefix = emoji_catalog_name_starts_with(name, query);
                if ((pass ? !prefix && emoji_catalog_name_contains(name, query) : prefix) &&
                    !push_result(picker, kReactionPickerCustomResult | i))
                    return {ReactionPickerError::capacity, false};
            }
            for (u32 i = 0; i < catalog.count; ++i) {
                const bool prefix = emoji_catalog_name_starts_with(catalog[i].name(), query);
                if ((pass ? !prefix && emoji_catalog_name_contains(catalog[i].name(), query) : prefix) &&
                    !push_result(picker, i))
                    return {ReactionPickerError::capacity, false};
            }
        }
    }
    if (picker->section_count) {
        auto& last = picker->sections[picker->section_count - 1];
        last.count = picker->result_count - last.first;
    }
    saturating_increment(picker->revision);
    return {ReactionPickerError::none, true};
}

} // namespace

ReactionPickerResult reaction_picker_open(ReactionPickerModel* picker, u64 message_id,
                                          ReactionPreferenceStore* preferences, u64 now_ms,
                                          const ChatStateStore* state, u64 guild_id,
                                          MessageComposer* borrowed_search,
                                          ReactionPreferenceKind kind) noexcept {
    if (!picker || message_id == 0 || static_cast<u32>(kind) >= kReactionPreferenceKindCount ||
        (borrowed_search && (borrowed_search->bytes > kReactionPickerBorrowedQueryMaximumBytes ||
                             !utf8_validate(message_composer_text(*borrowed_search)).valid))) {
        return ReactionPickerResult{ReactionPickerError::invalid_input, false};
    }
    if (preferences && !seed_preferences(preferences, kind, now_ms)) {
        return ReactionPickerResult{ReactionPickerError::invalid_input, false};
    }
    reaction_picker_release(picker);
    if (!initialize_storage(picker, state, borrowed_search)) {
        reaction_picker_release(picker);
        return ReactionPickerResult{ReactionPickerError::capacity, false};
    }
    if (!snapshot_custom_emojis(picker, state, guild_id)) {
        reaction_picker_release(picker);
        return ReactionPickerResult{ReactionPickerError::capacity, false};
    }
    // Headings are descriptors only, never source slots. Allocate the full
    // upper bound of accepted sources for this snapshot.
    picker->result_capacity = kReactionPickerCatalogCapacity + picker->custom_emoji_count +
                              kReactionPreferenceFavoriteCapacity + kReactionPreferenceFrequentCapacity;
    picker->result_catalog_indices = arena_allocate_zero<u16>(&picker->storage, picker->result_capacity);
    picker->sections =
        arena_allocate_zero<ReactionPickerSection>(&picker->storage, picker->guild_count + 10);
    if (!storage_valid(picker)) {
        reaction_picker_release(picker);
        return {ReactionPickerError::capacity, false};
    }
    picker->preferences = preferences;
    picker->preference_kind = kind;
    picker->message_id = message_id;
    const ReactionPickerResult rebuilt = rebuild_results(picker);
    if (!rebuilt.ok()) {
        reaction_picker_release(picker);
    }
    return rebuilt;
}

void reaction_picker_release(ReactionPickerModel* picker) noexcept {
    if (!picker) {
        return;
    }
    if (picker->search && !picker->search_borrowed)
        message_composer_release(picker->search);
    arena_release(&picker->storage);
    *picker = ReactionPickerModel{};
}

ReactionPickerResult reaction_picker_refresh_search(ReactionPickerModel* picker) noexcept {
    if (!storage_valid(picker) || !picker->search)
        return {ReactionPickerError::invalid_input, false};
    if (picker->search->bytes > query_limit(*picker))
        return {ReactionPickerError::capacity, false};
    if (picker->search_revision == picker->search->text_revision)
        return {ReactionPickerError::none, false};
    picker->query_bytes = picker->search->bytes;
    picker->search_revision = picker->search->text_revision;
    return rebuild_results(picker);
}

ReactionPickerResult reaction_picker_refresh_preferences(ReactionPickerModel* picker) noexcept {
    if (!storage_valid(picker) || !picker->message_id || !picker->preferences)
        return {ReactionPickerError::invalid_input, false};
    return rebuild_results(picker);
}

ReactionPickerResult reaction_picker_toggle_section(ReactionPickerModel* picker, u32 section) noexcept {
    if (!storage_valid(picker) || picker->query_bytes || section >= picker->section_count ||
        picker->sections[section].local == kReactionPickerSectionSearch)
        return {ReactionPickerError::invalid_input, false};
    auto& target = picker->sections[section];
    if (picker->preferences &&
        !reaction_preferences_toggle_section(picker->preferences, target.local, target.guild_id).ok())
        return {ReactionPickerError::capacity, false};
    target.collapsed = !target.collapsed;
    saturating_increment(picker->revision);
    return {ReactionPickerError::none, true};
}

namespace {
bool resolve_result(const ReactionPickerModel& picker, u16 result, ReactionPickerSelection* selection,
                    Utf8View* label) noexcept {
    if (result == 0xFFFFu) {
        return false;
    }
    *selection = ReactionPickerSelection{};
    if ((result & kReactionPickerCustomResult) != 0) {
        const u32 index = result & ~kReactionPickerCustomResult;
        if (index >= picker.custom_emoji_count) {
            return false;
        }
        const ReactionPickerCustomEmoji& emoji = picker.custom_emojis[index];
        selection->emoji_id = emoji.id;
        selection->name = custom_name(emoji);
        selection->catalog_index = ~0u;
        selection->custom = true;
        selection->animated = (emoji.flags & custom_emoji_animated) != 0;
        if (label)
            *label = selection->name;
        return selection->emoji_id != 0 && selection->name.bytes != 0;
    }
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    if (result >= catalog.count) {
        return false;
    }
    selection->name = catalog[result].glyph();
    selection->catalog_index = result;
    if (label)
        *label = catalog[result].name();
    return true;
}
} // namespace

bool reaction_picker_result_at(const ReactionPickerModel& picker, u32 index,
                               ReactionPickerSelection* selection, Utf8View* label) noexcept {
    return storage_valid(&picker) && selection && index < picker.result_count &&
           resolve_result(picker, picker.result_catalog_indices[index], selection, label);
}
Utf8View reaction_picker_section_label(const ReactionPickerModel& picker, u32 section) noexcept {
    return storage_valid(&picker) ? section_label(picker, section) : Utf8View{};
}
Utf8View reaction_picker_result_guild_name(const ReactionPickerModel& picker, u32 index) noexcept {
    if (!storage_valid(&picker) || index >= picker.result_count)
        return {};
    const u16 result = picker.result_catalog_indices[index];
    if (result == 0xFFFFu || !(result & kReactionPickerCustomResult))
        return {};
    const u32 custom = result & ~kReactionPickerCustomResult;
    if (custom >= picker.custom_emoji_count)
        return {};
    const u32 guild = picker.custom_emojis[custom].guild_index;
    return guild < picker.guild_count ? Utf8View{picker.guilds[guild].name, picker.guilds[guild].name_bytes}
                                      : Utf8View{};
}

namespace {
bool settings_key_digits(Utf8View key) noexcept {
    if (!key.bytes || key.bytes > 20)
        return false;
    for (u32 i = 0; i < key.bytes; ++i)
        if (key.data[i] < '0' || key.data[i] > '9')
            return false;
    return true;
}

} // namespace

u32 reaction_emoji_settings_key(ReactionEmojiRef emoji, Slice<char> output) noexcept {
    if (!emoji.valid() || !output.data)
        return 0;
    if (emoji.custom()) {
        char reversed[20];
        u32 count = 0;
        for (u64 value = emoji.custom_id; value; value /= 10)
            reversed[count++] = static_cast<char>('0' + value % 10);
        if (count > output.count)
            return 0;
        for (u32 i = 0; i < count; ++i)
            output[i] = reversed[count - 1 - i];
        return count;
    }
    const auto catalog = unicode_emoji_catalog();
    if (emoji.catalog_index >= catalog.count)
        return 0;
    return emoji_shortcode_key(catalog[emoji.catalog_index].glyph(), output);
}

ReactionEmojiRef reaction_emoji_from_settings_key(Utf8View key) noexcept {
    constexpr ReactionEmojiRef none = reaction_emoji_unicode(kReactionPreferenceInvalidCatalogIndex);
    if (settings_key_digits(key)) {
        u64 id = 0;
        for (u32 i = 0; i < key.bytes; ++i) {
            const u64 digit = static_cast<u64>(key.data[i] - '0');
            if (id > (~u64{0} - digit) / 10)
                return none;
            id = id * 10 + digit;
        }
        // Emoji names such as "100" and "1234" are Unicode names, not snowflakes.
        if (id >= (1ull << 22))
            return reaction_emoji_custom(id);
    }
    // Any alias or catalog name, or the emoji sequence itself.
    const bool sequence = key.bytes && static_cast<u8>(key.data[0]) >= 0x80u;
    const u32 index = emoji_catalog_find_glyph(sequence ? key : emoji_shortcode_glyph(key));
    return index <= 0xFFFEu ? reaction_emoji_unicode(static_cast<u16>(index)) : none;
}

u32 reaction_emoji_scan_message(Utf8View content, Slice<ReactionEmojiRef> output) noexcept {
    u32 count = 0;
    if (!content.data || !output.data)
        return 0;
    const u32 limit = minimum(content.bytes, 4'000u);
    const auto add = [&](ReactionEmojiRef emoji) noexcept {
        if (!emoji.valid() || count == output.count)
            return;
        for (u32 i = 0; i < count; ++i)
            if (output[i] == emoji)
                return;
        output[count++] = emoji;
    };
    const auto pictographic = [](u32 c) noexcept {
        return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2300 && c <= 0x23FF) ||
               (c >= 0x2B00 && c <= 0x2BFF) || (c >= 0x2190 && c <= 0x21FF) || (c >= 0x25A0 && c <= 0x25FF) ||
               c == 0x00A9 || c == 0x00AE || c == 0x203C || c == 0x2049 || c == 0x2122 || c == 0x2139 ||
               c == 0x24C2 || c == 0x2934 || c == 0x2935 || c == 0x3030 || c == 0x303D || c == 0x3297 ||
               c == 0x3299;
    };
    const auto joins = [](u32 c) noexcept {
        return c == 0x200D || c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) ||
               (c >= 0xE0020 && c <= 0xE007F);
    };
    for (u32 at = 0; at < limit && count < output.count;) {
        const char c = content.data[at];
        if (c == '<') {
            // <:name:id> or <a:name:id>
            u32 cursor = at + 1;
            if (cursor < limit && content.data[cursor] == 'a')
                ++cursor;
            if (cursor < limit && content.data[cursor] == ':') {
                u32 name_end = cursor + 1;
                while (name_end < limit && name_end - cursor <= 33 && content.data[name_end] != ':' &&
                       content.data[name_end] != '>')
                    ++name_end;
                u32 id_end = name_end + 1;
                while (id_end < limit && content.data[id_end] >= '0' && content.data[id_end] <= '9')
                    ++id_end;
                if (name_end < limit && content.data[name_end] == ':' && name_end > cursor + 1 &&
                    id_end < limit && content.data[id_end] == '>' && id_end > name_end + 1) {
                    const auto emoji = reaction_emoji_from_settings_key({content.data + name_end + 1, id_end - name_end - 1});
                    if (emoji.custom())
                        add(emoji);
                    at = id_end + 1;
                    continue;
                }
            }
            ++at;
            continue;
        }
        const auto decoded = utf8_decode_one(content.data + at, limit - at);
        if (!decoded.valid) {
            ++at;
            continue;
        }
        const bool regional = decoded.codepoint >= 0x1F1E6 && decoded.codepoint <= 0x1F1FF;
        if (!pictographic(decoded.codepoint) && !(decoded.codepoint >= '0' && decoded.codepoint <= '9') &&
            decoded.codepoint != '#' && decoded.codepoint != '*') {
            at += decoded.width;
            continue;
        }
        // Grow one grapheme-like emoji cluster, then fall back to its first scalar.
        u32 end = at + decoded.width;
        if (regional && end < limit) {
            const auto next = utf8_decode_one(content.data + end, limit - end);
            if (next.valid && next.codepoint >= 0x1F1E6 && next.codepoint <= 0x1F1FF)
                end += next.width;
        }
        for (;;) {
            if (end >= limit || end - at > kUnicodeEmojiMaximumGlyphBytes)
                break;
            const auto next = utf8_decode_one(content.data + end, limit - end);
            if (!next.valid || !joins(next.codepoint))
                break;
            end += next.width;
            if (next.codepoint == 0x200D && end < limit) {
                const auto joined = utf8_decode_one(content.data + end, limit - end);
                if (!joined.valid)
                    break;
                end += joined.width;
            }
        }
        const bool keycap_base = decoded.codepoint < 0x80;
        u32 index = end - at > decoded.width || !keycap_base ? emoji_catalog_find_glyph({content.data + at, end - at}) : ~0u;
        if (index > 0xFFFEu && !keycap_base)
            index = emoji_catalog_find_glyph({content.data + at, decoded.width});
        if (index <= 0xFFFEu)
            add(reaction_emoji_unicode(static_cast<u16>(index)));
        at = index <= 0xFFFEu ? end : at + decoded.width;
    }
    return count;
}

} // namespace maicord
