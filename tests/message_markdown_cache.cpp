#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
namespace {
constexpr u32 kCount = 24;
constexpr u32 kWidgets = kCount * kMessageSceneWidgetsPerRow + 1;
constexpr u64 kNow = 1'700'000'000ull;

struct Fixture {
    MessageRecord records[kCount];
    u32 order[kCount];
    u8 text[8192];
    MessageRichRecord rich_records[kCount];
    u16 rich_order[kCount];
    u8 rich_text[1024];
    u32 heights[kCount];
    u64 fenwick[kCount];
    UiWidget widget_storage[kWidgets];
    UiLayoutNode nodes[kWidgets];
    MessageSceneRowWidget rows[kCount];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MessageTextCommand commands[kMaximumMessageTextCommands];
    char utf8[kMaximumMessageTextUtf8Bytes];
    TextStyleSpan styles[kMaximumMessageTextStyleSpans];
    MessageMarkdownSemantic semantics[kMaximumMessageTextSemantics];
    MessageTextMeasurement measurements[kMaximumMessageTextMeasurements];
    MessageTextSpoilerReveal reveals[8];
    u32 scratch[kMessageTextByteStyleScratchCapacity];
    MessageMarkdownCacheEntry entries[kMessageMarkdownCacheSlots];
    alignas(8) u8 arena[kMessageMarkdownCacheArenaBytes];
    UserRecord users[2];
    u32 user_order[2];
    u8 user_names[128];
    GuildRecord guilds[1];
    u16 guild_order[1];
    ChannelRecord channels[1];
    u16 channel_order[1];
    u8 names[256];
};
Fixture g_fixture;

// Code, mentions, spoilers and a relative timestamp: everything the parser
// highlights or resolves through a callback.
Utf8View shape(u32 index) noexcept {
    switch (index % 6) {
    case 0: return utf8_literal("**bold** and _italic_ with `code` and a [link](https://example.com)");
    case 1: return utf8_literal("```cpp\nint main() { return 0; } // highlighted\n```");
    case 2: return utf8_literal("hello <@9> in <#7>, see ||the spoiler|| here");
    case 3: return utf8_literal("> quoted line\nplain line with ~~strike~~");
    case 4: return utf8_literal("posted <t:1700000060:R> by <@9>");
    default: return utf8_literal("# Heading\n- one\n- two");
    }
}

// Distinct sources per message (identical text shares one cache entry).
char g_content[kCount][160];
Utf8View content(u32 index) noexcept {
    const Utf8View base = shape(index);
    char* out = g_content[index];
    u32 bytes = 0;
    for (; bytes < base.bytes; ++bytes)
        out[bytes] = base.data[bytes];
    out[bytes++] = ' ';
    out[bytes++] = static_cast<char>('a' + index / 10);
    out[bytes++] = static_cast<char>('0' + index % 10);
    return Utf8View{out, bytes};
}

bool contains(Utf8View text, Utf8View needle) noexcept {
    for (u32 i = 0; needle.bytes && i + needle.bytes <= text.bytes; ++i)
        if (utf8_equal(Utf8View{text.data + i, needle.bytes}, needle))
            return true;
    return false;
}

bool resolves_entities(const MessageTextCommand& command) noexcept {
    for (u32 s = 0; s < command.semantics.count; ++s)
        if (command.semantics[s].kind == MessageMarkdownSemanticKind::user_mention ||
            command.semantics[s].kind == MessageMarkdownSemanticKind::channel_mention)
            return true;
    return false;
}

u64 text_signature(const MessageTextPresentation& presentation) noexcept {
    u64 hash = hash64_mix(kHash64Seed, presentation.command_count);
    for (u32 i = 0; i < presentation.command_count; ++i) {
        const auto& command = presentation.commands[i];
        hash = hash64_counted_bytes(hash, command.text.data, command.text.bytes);
        hash = hash64_counted_bytes(hash, command.styles.data,
                                    command.styles.count * static_cast<u32>(sizeof(TextStyleSpan)));
        hash = hash64_counted_bytes(hash, command.semantics.data,
                                    command.semantics.count * static_cast<u32>(sizeof(MessageMarkdownSemantic)));
    }
    return hash;
}
} // namespace

void test_message_markdown_cache(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture& f = g_fixture;
    bool ready =
        message_store_initialize(&f.store, {f.records, kCount}, {f.order, kCount}, {f.text, sizeof(f.text)}) &&
        message_rich_store_initialize(&f.rich, {f.rich_records, kCount}, {f.rich_order, kCount},
                                      {f.rich_text, sizeof(f.rich_text)});
    for (u32 i = 0; ready && i < kCount; ++i)
        ready = message_store_upsert(&f.store, {100 + i, 7, i % 2 ? 9u : 8u, (kNow - 3'600 + i * 600) * 1'000, 0,
                                                content(i), 0})
                    .ok();
    ready = ready &&
            ui_variable_list_initialize(&f.list, {f.heights, kCount}, {f.fenwick, kCount}, kCount,
                                        ui_extent_units(57)) &&
            message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(240),
                                        false) &&
            message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
            message_scene_initialize(&f.scene, &f.timeline, {f.rows, kCount}, {f.widget_storage, kWidgets},
                                     {f.nodes, kWidgets});
    const UiRect viewport{0, 0, 740, 240};
    ready = ready && message_timeline_set_scroll(&f.timeline, 0, false) && message_scene_update(&f.scene, viewport).ok();

    UserStore users{};
    ChatStateStore state{};
    NavigationView navigation{};
    navigation.selected_channel_id = 7;
    ready = ready &&
            user_store_initialize(&users, {f.users, 2}, {f.user_order, 2}, {f.user_names, sizeof(f.user_names)}) &&
            user_store_upsert(&users, 8, utf8_literal("Author")).ok() &&
            user_store_upsert(&users, 9, utf8_literal("Mentioned")).ok() &&
            chat_state_store_initialize(&state, {f.guilds, 1}, {f.guild_order, 1}, {f.channels, 1},
                                        {f.channel_order, 1}, {f.names, sizeof(f.names)}) &&
            chat_state_store_upsert_guild(&state, GuildInput{70, utf8_literal("Guild"), GuildMutationKind::create, 8})
                .ok() &&
            chat_state_store_upsert_channel(&state, ChannelInput{7, 70, 0, utf8_literal("general"), 0, 0, 0}).ok();

    MessageTextPresentation presentation{}, reference{};
    MessageMarkdownCache cache{};
    const auto initialize = [&](MessageTextPresentation* target) noexcept {
        return message_text_presentation_initialize(
            target, {f.commands, kMaximumMessageTextCommands}, {f.utf8, kMaximumMessageTextUtf8Bytes},
            {f.styles, kMaximumMessageTextStyleSpans}, {f.semantics, kMaximumMessageTextSemantics},
            {f.measurements, kMaximumMessageTextMeasurements}, {f.reveals, 8},
            {f.scratch, kMessageTextByteStyleScratchCapacity});
    };
    const auto build = [&](MessageTextPresentation* target, u64 now) noexcept {
        return message_text_presentation_build(target, f.scene, state, &users, navigation, kDefaultTheme, now).ok();
    };
    ready = ready && initialize(&presentation) &&
            message_markdown_cache_initialize(&cache, {f.entries, kMessageMarkdownCacheSlots},
                                              {f.arena, sizeof(f.arena)}) &&
            message_text_presentation_bind_markdown_cache(&presentation, &cache);
    const bool first = ready && build(&presentation, kNow);
    const u32 cold_parses = presentation.markdown_parses;
    const u64 cold_text = text_signature(presentation);
    const u64 cold_projection = presentation.projection_signature;
    check(first && cold_parses > 0 && presentation.markdown_cache_hits == 0 && cache.inserts == cold_parses,
          "markdown_cache_cold_build_parses_each_visible_source_once_and_retains_it");

    // A rebuild with unchanged inputs (a relayout or a scroll-only frame)
    // lays out retained parses: no parser run, identical commands.
    const bool warm = first && build(&presentation, kNow);
    check(warm && presentation.markdown_parses == cold_parses && presentation.markdown_cache_hits == cold_parses &&
              text_signature(presentation) == cold_text && presentation.projection_signature == cold_projection,
          "markdown_cache_unchanged_rebuild_runs_no_parser_and_projects_identical_text");
    const bool uncached = warm && initialize(&reference) && build(&reference, kNow);
    check(uncached && text_signature(reference) == cold_text && reference.projection_signature == cold_projection,
          "markdown_cache_hit_output_equals_an_uncached_parse");

    // Scroll through history and back, twice: the second pass meets only
    // sources the first pass already parsed.
    const u64 total = f.list.total_units;
    const auto scroll_pass = [&]() noexcept {
        bool ok = warm;
        for (u32 step = 0; ok && step <= 8; ++step) {
            const u64 units = total * step / 8;
            ok = message_timeline_set_scroll(&f.timeline, units, false) &&
                 message_scene_update(&f.scene, viewport).ok() && build(&presentation, kNow);
        }
        for (u32 step = 8; ok && step-- > 0;) {
            ok = message_timeline_set_scroll(&f.timeline, total * step / 8, false) &&
                 message_scene_update(&f.scene, viewport).ok() && build(&presentation, kNow);
        }
        return ok;
    };
    const bool scrolled = scroll_pass();
    const u32 scrolled_parses = presentation.markdown_parses;
    const bool rescrolled = scrolled && scroll_pass();
    check(rescrolled && scrolled_parses > cold_parses && presentation.markdown_parses == scrolled_parses &&
              cache.inserts == scrolled_parses,
          "markdown_cache_scroll_over_seen_history_runs_zero_parses");

    // An edit changes one source: only that message parses again.
    u32 before = presentation.markdown_parses;
    const auto* head = message_scene_find_row(f.scene, 0);
    const MessageRecord* edited_record = head ? message_store_resolve(f.store, head->message) : nullptr;
    const u64 edited_id = edited_record ? edited_record->id : 0;
    const bool edited = edited_record &&
                        message_store_upsert(&f.store, {edited_id, 7, 8, edited_record->timestamp_ms, kNow * 1'000,
                                                        utf8_literal("**edited** text"), 0})
                            .ok() &&
                        message_scene_update(&f.scene, viewport).ok() && build(&presentation, kNow);
    check(edited && presentation.markdown_parses == before + 1,
          "markdown_cache_edit_reparses_only_the_edited_message");

    // Renaming a mentioned user reparses the sources that resolved entities.
    u32 entity_sources = 0;
    for (u32 i = 0; i < presentation.command_count; ++i)
        entity_sources += resolves_entities(presentation.commands[i]) ? 1u : 0u;
    before = presentation.markdown_parses;
    const bool renamed = edited && user_store_upsert(&users, 9, utf8_literal("Renamed")).ok() &&
                         build(&presentation, kNow);
    bool rename_projected = false;
    for (u32 i = 0; renamed && i < presentation.command_count; ++i)
        rename_projected |= contains(presentation.commands[i].text, utf8_literal("@Renamed"));
    check(renamed && entity_sources > 0 && rename_projected &&
              presentation.markdown_parses - before == entity_sources,
          "markdown_cache_entity_rename_reparses_resolving_sources_and_projects_the_new_name");

    // Revealing a spoiler reparses that message; a relative timestamp parses
    // again once its displayed text expires.
    MessageTextHit spoiler{};
    for (u32 i = 0; renamed && !spoiler.valid && i < presentation.command_count; ++i) {
        const auto& command = presentation.commands[i];
        for (u32 s = 0; s < command.semantics.count; ++s)
            if (command.semantics[s].kind == MessageMarkdownSemanticKind::spoiler) {
                spoiler = {command.message_id, 0, command.message_revision, command.semantics[s].source_offset,
                           command.semantics[s].source_bytes, MessageMarkdownSemanticKind::spoiler, true};
                break;
            }
    }
    before = presentation.markdown_parses;
    bool toggled = false;
    const bool revealed = spoiler.valid &&
                          message_text_presentation_toggle_spoiler(&presentation, f.store, spoiler, &toggled) &&
                          toggled && build(&presentation, kNow);
    check(revealed && presentation.markdown_parses == before + 1,
          "markdown_cache_spoiler_reveal_reparses_only_its_message");
    const u64 expiry = presentation.next_timestamp_update_unix_seconds;
    before = presentation.markdown_parses;
    const bool expired = revealed && expiry > kNow && build(&presentation, expiry);
    check(expired && presentation.markdown_parses > before,
          "markdown_cache_relative_timestamp_reparses_when_its_text_expires");

    before = presentation.markdown_parses;
    message_markdown_cache_clear(&cache);
    const bool cleared = expired && build(&presentation, expiry);
    check(cleared && presentation.markdown_parses > before && cache.inserts > 0,
          "markdown_cache_clear_reparses_every_visible_source");
    message_scene_release(&f.scene);
}

} // namespace maicord
