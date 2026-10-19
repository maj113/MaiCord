#include "maicord/ui/reaction_picker.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
void measurement(const char* name, usize value) noexcept {
    test_write_metric(name, value);
}
u32 find_section(const ReactionPickerModel& picker, u32 local, u64 guild = 0) noexcept {
    for (u32 i = 0; i < picker.section_count; ++i)
        if (picker.sections[i].local == local && picker.sections[i].guild_id == guild)
            return i;
    return ~0u;
}
bool search(ReactionPickerModel* picker, Utf8View text) noexcept {
    return picker->search && message_composer_restore(picker->search, 1, text, text.bytes, text.bytes) &&
           reaction_picker_refresh_search(picker).ok();
}
} // namespace
// The emoji picker model shared by the expression picker's Emoji tab and the
// reaction popout: section spans, preferences and capacity.
void test_reaction_sections(void (*check)(bool, const char*) noexcept) noexcept {
    measurement("MEASURE reaction_picker_owner_bytes=", sizeof(ReactionPickerModel));
    measurement("MEASURE reaction_section_descriptor_bytes=", sizeof(ReactionPickerSection));
    measurement("MEASURE reaction_preference_owner_bytes=", sizeof(ReactionPreferenceStore));

    ReactionPreferenceStore preferences{};
    ReactionPickerModel picker{};
    bool valid = reaction_preferences_initialize(&preferences) &&
                 reaction_preferences_track_emoji(&preferences, ReactionPreferenceKind::reaction,
                                                  reaction_emoji_unicode(4), 1000).ok() &&
                 reaction_preferences_toggle_favorite_emoji(&preferences, reaction_emoji_unicode(4)).ok() &&
                 reaction_picker_open(&picker, 77, &preferences, 1000).ok();
    const u32 favorite = find_section(picker, kReactionPickerSectionFavorites),
              recent = find_section(picker, kReactionPickerSectionFrequent);
    measurement("MEASURE reaction_small_allocation_bytes=", picker.storage.offset);
    measurement("MEASURE reaction_small_committed_bytes=", picker.storage.committed);
    measurement("MEASURE reaction_small_reserved_bytes=", picker.storage.reserved);
    measurement("MEASURE reaction_small_source_capacity=", picker.result_capacity);
    check(valid && favorite == 0 && recent == 1 && picker.sections[favorite].count == 1 &&
              picker.sections[recent].count == 1 &&
              picker.result_count == unicode_emoji_catalog().count + 2 &&
              picker.result_catalog_indices[picker.sections[favorite].first] == 4 &&
              picker.result_catalog_indices[picker.sections[recent].first] == 4,
          "reaction sections: favorites, reaction frequency and Unicode each retain their own occurrences");
    bool contiguous = valid;
    for (u32 i = 0; contiguous && i < picker.section_count; ++i)
        contiguous = picker.sections[i].first == (i ? picker.sections[i - 1].first + picker.sections[i - 1].count : 0);
    check(contiguous && picker.sections[picker.section_count - 1].first +
                                picker.sections[picker.section_count - 1].count ==
                            picker.result_count,
          "reaction sections: spans are contiguous and cover every result exactly once");
    const u32 source_count = picker.result_count, source_revision = picker.revision;
    const usize allocation_before_toggle = picker.storage.offset,
                committed_before_toggle = picker.storage.committed;
    const u32 commits_before_toggle = picker.storage.commit_count;
    valid = valid && reaction_picker_toggle_section(&picker, favorite).ok();
    check(valid && picker.result_count == source_count && picker.sections[favorite].collapsed &&
              picker.sections[favorite].count == 1 && picker.revision > source_revision,
          "reaction sections: collapse keeps the complete source span without rebuilding the catalog");
    check(picker.storage.offset == allocation_before_toggle &&
              picker.storage.committed == committed_before_toggle &&
              picker.storage.commit_count == commits_before_toggle,
          "reaction sections: toggle performs no arena allocation or page commitment");
    reaction_picker_release(&picker);
    valid = valid && reaction_picker_open(&picker, 77, &preferences, 1000).ok();
    check(valid && picker.sections[find_section(picker, kReactionPickerSectionFavorites)].collapsed,
          "reaction sections: account collapse state survives picker release and reopen");
    valid = valid && search(&picker, utf8_literal("s"));
    check(valid && picker.section_count == 1 && picker.sections[0].local == kReactionPickerSectionSearch &&
              !picker.sections[0].collapsed && !reaction_picker_toggle_section(&picker, 0).ok(),
          "reaction sections: search ignores browse collapse and presents one combined result span");
    valid = valid && search(&picker, {});
    check(valid && picker.sections[find_section(picker, kReactionPickerSectionFavorites)].collapsed,
          "reaction sections: clearing search restores saved browse collapse");
    reaction_picker_release(&picker);

    static GuildRecord guilds[kReactionPickerGuildCapacity]{};
    static u16 guild_order[kReactionPickerGuildCapacity]{};
    static ChannelRecord channels[1]{};
    static u16 channel_order[1]{};
    static u8 names[8192]{};
    // 16 per server across all 256 servers; the overflow case reuses it.
    constexpr u32 kFixtureEmoji = kReactionPickerGuildCapacity * 16;
    static CustomEmojiRecord emojis[kReactionPickerCustomEmojiCapacity + 129]{};
    u64 roles[1]{};
    ChatStateStore state{};
    valid = chat_state_store_initialize(&state, {guilds, kReactionPickerGuildCapacity},
                                           {guild_order, kReactionPickerGuildCapacity}, {channels, 1},
                                           {channel_order, 1}, {names, 8192}) &&
            reaction_preferences_initialize(&preferences);
    for (u32 i = 0; valid && i < kReactionPickerGuildCapacity; ++i)
        valid = chat_state_store_upsert_guild(
                    &state, {1000 + i, utf8_literal("Guild"), GuildMutationKind::create, 0})
                    .ok();
    for (u32 i = 0; i < kFixtureEmoji; ++i) {
        emojis[i] = {};
        emojis[i].id = 10000 + i;
        emojis[i].guild_id = 1000 + i / 16;
        emojis[i].flags = custom_emoji_available;
        emojis[i].name_bytes = 4;
        copy_bytes(emojis[i].name, "blob", 4);
    }
    state.emojis = emojis;
    state.emoji_count = kFixtureEmoji;
    state.emoji_roles = roles;
    for (u32 i = 0; valid && i < kReactionPreferenceFavoriteCapacity; ++i)
        valid = reaction_preferences_toggle_favorite_emoji(&preferences, reaction_emoji_unicode(static_cast<u16>(i))).ok();
    for (u32 i = 0; valid && i < kReactionPreferenceFrequentCapacity; ++i)
        valid = reaction_preferences_track_emoji(&preferences, ReactionPreferenceKind::reaction,
                                                 reaction_emoji_unicode(static_cast<u16>(i)), 1000).ok();
    valid = valid && reaction_picker_open(&picker, 88, &preferences, 1000, &state, 1007).ok();
    measurement("MEASURE reaction_maximum_allocation_bytes=", picker.storage.offset);
    measurement("MEASURE reaction_maximum_committed_bytes=", picker.storage.committed);
    measurement("MEASURE reaction_maximum_reserved_bytes=", picker.storage.reserved);
    measurement("MEASURE reaction_maximum_source_capacity=", picker.result_capacity);
    check(valid && picker.guild_count == 256 && picker.custom_emoji_count == 4096 &&
              picker.section_count == 266 && picker.result_count == 3780 + 4096 + 250 + 42 &&
              picker.result_capacity == 3780 + 4096 + 250 + 42,
          "reaction sections: full 256 guilds, 4096 custom, Unicode, favorites and frequent admitted "
          "without capacity reduction");
    const u32 guild_section = find_section(picker, kReactionPickerSectionGuild, 1007);
    check(valid && guild_section == 2 && picker.sections[guild_section].count == 16,
          "reaction sections: the selected server's emoji follow Favorites and Frequently Used");
    valid = valid && reaction_picker_toggle_section(&picker, guild_section).ok();
    reaction_picker_release(&picker);
    valid = valid && reaction_picker_open(&picker, 88, &preferences, 1000, &state, 1008).ok();
    check(valid && picker.sections[find_section(picker, kReactionPickerSectionGuild, 1007)].collapsed &&
              !picker.sections[find_section(picker, kReactionPickerSectionGuild, 1008)].collapsed,
          "reaction sections: guild collapse follows 64-bit identity across selected-guild reordering");
    reaction_picker_release(&picker);
    char borrowed_text[kReactionPickerBorrowedQueryMaximumBytes]{};
    MessageComposer borrowed{};
    const bool borrowed_ready =
        message_composer_initialize(&borrowed, {borrowed_text, sizeof(borrowed_text)}) &&
        message_composer_restore(&borrowed, 88, utf8_literal("blob"), 4, 4) &&
        reaction_picker_open(&picker, 88, &preferences, 1000, &state, 1008, &borrowed).ok();
    ReactionPickerSelection logical{};
    Utf8View logical_label{};
    check(borrowed_ready && picker.search == &borrowed && picker.search_borrowed &&
              picker.result_count == kFixtureEmoji &&
              reaction_picker_result_at(picker, picker.result_count - 1, &logical, &logical_label) &&
              logical.custom && logical.emoji_id && utf8_equal(logical_label, utf8_literal("blob")) &&
              !reaction_picker_result_at(picker, picker.result_count, &logical),
          "reaction adapter: complete logical custom results borrow the original snapshot and reject offrange");
    char long_query[60];
    for (char& byte : long_query)
        byte = 'x';
    check(borrowed_ready && message_composer_restore(&borrowed, 88, {long_query, 60}, 60, 60) &&
              reaction_picker_refresh_search(&picker).ok() && picker.query_bytes == 60 &&
              !picker.result_count,
          "reaction adapter: shared native edit has explicit 256 byte query bound beyond the owned 48 byte one");
    const auto before_release = message_composer_text(borrowed);
    reaction_picker_release(&picker);
    check(borrowed.bytes == 60 && message_composer_text(borrowed).data == before_release.data &&
              bytes_equal(message_composer_text(borrowed).data, long_query, 60),
          "reaction adapter: model retirement never frees or resets externally owned edit document");
    message_composer_release(&borrowed);
    // More usable server emoji than the picker holds (a large server list):
    // the picker still opens, keeps the selected server first and truncates
    // the last servers instead of failing (the Emoji tab used to close).
    const u32 overflow = kReactionPickerCustomEmojiCapacity + 129;
    for (u32 i = 0; i < overflow; ++i) {
        emojis[i] = {};
        emojis[i].id = 100000 + i;
        emojis[i].guild_id = 1000 + i / 129;
        emojis[i].flags = custom_emoji_available;
        emojis[i].name_bytes = 4;
        copy_bytes(emojis[i].name, "wave", 4);
    }
    state.emoji_count = overflow;
    valid = reaction_picker_open(&picker, 89, &preferences, 1000, &state, 1200).ok();
    const u32 selected = find_section(picker, kReactionPickerSectionGuild, 1200);
    check(valid && picker.custom_emoji_count == kReactionPickerCustomEmojiCapacity &&
              picker.guild_count == kReactionPickerCustomEmojiCapacity / 129 + 1 && selected == 2 &&
              picker.sections[selected].count == 129 &&
              find_section(picker, kReactionPickerSectionGuild, 1000 + overflow / 129) == ~0u,
          "reaction sections: more server emoji than capacity truncate the last servers, never fail open");
    reaction_picker_release(&picker);
    reaction_preferences_release(&preferences);
}
} // namespace maicord
