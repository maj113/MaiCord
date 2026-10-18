#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord { namespace {
constexpr auto tajikistan = utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF");
void diagnostic(const char* name, u32 value) noexcept {
    char text[96], digits[16]; u32 count = 0, used = 0;
    while (name[count] && count < 64) { text[count] = name[count]; ++count; }
    do { digits[used++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (used) text[count++] = digits[--used]; text[count++] = '\n';
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, count, &written, nullptr);
}
bool revealed(void*, u32) noexcept { return true; }
void scanner_cases(void (*check)(bool, const char*) noexcept) noexcept {
    u32 supported = 0;
    const auto catalog = unicode_emoji_catalog();
    for (u32 i = 0; i < catalog.count; ++i) {
        const auto& entry = catalog[i];
        const auto glyph = entry.glyph();
        const auto identity = regional_flag_sequence(glyph);
        if (!identity) continue;
        u32 cursor = 0; RegionalFlagRange range{};
        bool valid = regional_flag_next(glyph, &cursor, &range) && range.first_byte == 0 &&
            range.end_byte == glyph.bytes && range.sequence == identity && cursor == glyph.bytes;
        valid = valid && !regional_flag_next(glyph, &cursor, &range) && !range.sequence;
        check(valid, "flag scanner retains each full catalog RI/tag identity");
        ++supported;
    }
    check(supported == kRegionalFlagAssetCount, "flag scanner covers all 262 catalog identities");
    const auto odd = utf8_literal("\xF0\x9F\x87\xA6\xF0\x9F\x87\xA6\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF\xF0\x9F\x87\xA6");
    u32 cursor = 0; RegionalFlagRange range{};
    check(regional_flag_next(odd, &cursor, &range) && range.first_byte == 8 &&
        range.end_byte == 16 && range.sequence == 504 &&
        !regional_flag_next(odd, &cursor, &range) && cursor == odd.bytes,
        "flag scanner consumes unsupported AA pair and odd remainder without shifting TJ");
    const auto extension = utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF\xEF\xB8\x8F");
    cursor = 0;
    check(!regional_flag_next(extension, &cursor, &range) && cursor == extension.bytes,
        "flag scanner refuses a supported prefix with additional selector");
    const auto truncated_tag = utf8_literal("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7");
    cursor = 0;
    check(!regional_flag_next(truncated_tag, &cursor, &range) && cursor == truncated_tag.bytes,
        "flag scanner refuses subdivision without cancel tag");
    const char invalid[] = {static_cast<char>(0xff)}; cursor = 0;
    check(!regional_flag_next({invalid, 1}, &cursor, &range) && !cursor && !range.sequence,
        "flag scanner refuses malformed UTF8 without publishing range");
}
void markdown_cases(void (*check)(bool, const char*) noexcept) noexcept {
    char output[256]{}; u32 styles[256]{}, mapping[256]{};
    TextStyleSpan spans[32]{}; MessageMarkdownSemantic semantics[16]{};
    const auto input = utf8_literal("a **\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF** z");
    const auto result = message_markdown_parse(input, {output, 256}, {styles, 256}, {spans, 32},
        {}, {semantics, 16}, {mapping, 256});
    const auto expected = utf8_literal("a \xF0\x9F\x87\xB9\xF0\x9F\x87\xAF z");
    bool valid = result.ok() && utf8_equal(result.text, expected) && result.semantic_count == 1 &&
        semantics[0].kind == MessageMarkdownSemanticKind::regional_flag && semantics[0].target == 504 &&
        semantics[0].byte_offset == 2 && semantics[0].byte_count == 8 &&
        semantics[0].source_offset == 4 && semantics[0].source_bytes == 8;
    for (u32 i = 2; i < 10; ++i) valid = valid && mapping[i] == i + 2 &&
        (styles[i] & (text_style_bold | text_style_custom_emoji)) ==
            (text_style_bold | text_style_custom_emoji);
    check(valid, "flag markdown retains actual Unicode, source byte map and formatting");
    const auto code = utf8_literal("`\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF`");
    const auto code_result = message_markdown_parse(code, {output, 256}, {styles, 256}, {spans, 32}, {}, {semantics, 16});
    check(code_result.ok() && !code_result.semantic_count && !(styles[0] & text_style_custom_emoji),
        "flag markdown keeps literal code text out of media projection");
    const auto spoiler = utf8_literal("||\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF||");
    const auto hidden = message_markdown_parse(spoiler, {output, 256}, {styles, 256}, {spans, 32}, {}, {semantics, 16});
    check(hidden.ok() && hidden.semantic_count == 2 && (semantics[0].reserved & 2u) &&
        semantics[1].kind == MessageMarkdownSemanticKind::spoiler,
        "flag markdown preserves hidden spoiler action and suppresses artwork");
    MessageMarkdownOptions options{}; options.spoiler_revealed = revealed;
    const auto visible = message_markdown_parse(spoiler, {output, 256}, {styles, 256}, {spans, 32}, options, {semantics, 16});
    check(visible.ok() && !(semantics[0].reserved & 2u), "flag markdown revealed spoiler admits artwork");
    const auto full = message_markdown_parse(tajikistan, {output, 256}, {styles, 256}, {spans, 32}, {}, {});
    check(full.ok() && utf8_equal(full.text, tajikistan) && !(styles[0] & text_style_custom_emoji),
        "flag markdown capacity fallback retains visible Unicode glyphs");
}
void composer_cases(void (*check)(bool, const char*) noexcept) noexcept {
    MessageComposer composer{}; char source[128]{};
    MessageComposerScene scene{};
    bool valid = message_composer_initialize(&composer, {source, sizeof(source)}) &&
        message_composer_restore(&composer, 7, tajikistan, 4, 4) &&
        message_composer_scene_initialize_owned(&scene) &&
        message_composer_scene_update(&scene, &composer, {0, 0, 400, 56}, true).ok();
    valid = valid && utf8_equal(message_composer_scene_display_text(scene), tajikistan) &&
        scene.projection_span_count == 1 && scene.projection_spans[0].kind == MessageComposerProjectionKind::regional_flag &&
        scene.projection_spans[0].target == 504 && scene.display_caret == 4 && scene.display_anchor == 4;
    for (u32 at = 0; at <= 8; at += 4) valid = valid &&
        message_composer_scene_display_offset_from_model(scene, at) == at &&
        message_composer_scene_model_offset_from_display(scene, at) == at;
    check(valid, "flag composer true bytes and interior caret mappings remain unchanged");
    message_composer_scene_release(&scene);
    const auto query = utf8_literal(":flag_tajik");
    message_composer_restore(&composer, 7, query, query.bytes, query.bytes);
    ComposerAutocomplete suggestions{}; MemberListView members{}; UserStore users{};
    NavigationView navigation{}; ChatStateStore state{};
    valid = composer_autocomplete_build(&suggestions, composer, members, users, navigation, state);
    bool found = false;
    for (u32 i = 0; i < suggestions.count; ++i) {
        const auto& suggestion = suggestions.suggestions[i];
        if (suggestion.insertion_bytes != 9 || !bytes_equal(suggestion.insertion, tajikistan.data, 8)) continue;
        found = true;
        valid = valid && !suggestion.emoji_id && suggestion.insertion[8] == ' ' &&
            suggestion.label.data[0] == ':';
        PopupScene popup{}; PopupItem item{suggestion.label, 1,
            static_cast<u16>(popup_item_enabled | popup_item_emoji_preview), 0}; item.regional_flag = 504;
        UiWidget widgets[4]{}; UiLayoutNode layout[4]{};
        valid = valid && popup_scene_initialize_owned(&popup, 1, {widgets, 4}, {layout, 4}) &&
            popup_scene_open(&popup, PopupKind::autocomplete, 7, {&item, 1}, {0, 100, 400, 101},
                {0, 0, 400, 200}, 400).ok();
        PopupMediaCommand media{};
        valid = valid && popup_media_presentation_resolve(popup, 0, &media) == PopupMediaPresentationStatus::ready &&
            media.regional_flag == 504 && !media.media_id && ui_rect_has_area(media.rectangle);
        popup_scene_release(&popup);
    }
    check(valid && found, "flag autocomplete retains Unicode insertion and distinct native preview identity");
    composer_autocomplete_release(&suggestions); message_composer_release(&composer);
}
void composer_capacity_case(void (*check)(bool, const char*) noexcept) noexcept {
    char source[kTextStyleSpanMaximum * 8]{}; u32 used = 0;
    constexpr auto bold = utf8_literal("**a** ");
    // Retain 127 free styles for flag projection regardless of the configured
    // style capacity. The last of 128 flag atoms must remain ordinary Unicode.
    for (u32 i = 0; i < kTextStyleSpanMaximum - 255; ++i) {
        copy_bytes(source + used, bold.data, bold.bytes); used += bold.bytes;
    }
    for (u32 i = 0; i < 128; ++i) {
        copy_bytes(source + used, bold.data, bold.bytes); used += bold.bytes;
        copy_bytes(source + used, tajikistan.data, tajikistan.bytes); used += tajikistan.bytes;
        source[used++] = ' ';
    }
    MessageComposer composer{}; char model[sizeof(source)]{}; MessageComposerScene scene{};
    bool valid = message_composer_initialize(&composer, {model, sizeof(model)}) &&
        message_composer_restore(&composer, 7, {source, used}, used, used) &&
        message_composer_scene_initialize_owned(&scene) &&
        message_composer_scene_update(&scene, &composer, {0, 0, 400, 56}, true).ok();
    valid = valid && scene.display_style_count == kTextStyleSpanMaximum && scene.projection_span_count == 127 &&
        bytes_equal(scene.display + scene.display_bytes - 9, tajikistan.data, 8) &&
        scene.projection_spans[126].display_last < scene.display_bytes - 9;
    check(valid, "flag composer exhausted style capacity keeps final Unicode literal without media projection");
    message_composer_scene_release(&scene); message_composer_release(&composer);
}
void popup_identity_case(void (*check)(bool, const char*) noexcept) noexcept {
    PopupItem items[3]{{tajikistan, 1, static_cast<u16>(popup_item_enabled | popup_item_avatar), 0, 9001},
        {tajikistan, 2, static_cast<u16>(popup_item_enabled | popup_item_guild_icon), 0, 9002},
        {tajikistan, 3, static_cast<u16>(popup_item_enabled | popup_item_emoji), 0}};
    items[1].media_variant = 7;
    PopupScene popup{}; UiWidget widgets[8]{}; UiLayoutNode layout[8]{};
    bool valid = popup_scene_initialize_owned(&popup, 3, {widgets, 8}, {layout, 8}) &&
        popup_scene_open(&popup, PopupKind::autocomplete, 7, {items, 3}, {0, 160, 400, 161},
            {0, 0, 400, 300}, 400).ok();
    PopupMediaCommand media{};
    valid = valid && popup_media_presentation_resolve(popup, 0, &media) == PopupMediaPresentationStatus::ready &&
        media.avatar && !media.regional_flag && media.media_id == 9001;
    valid = valid && popup_media_presentation_resolve(popup, 1, &media) == PopupMediaPresentationStatus::ready &&
        media.guild_icon && !media.regional_flag && media.media_id == 9002;
    valid = valid && popup_media_presentation_resolve(popup, 2, &media) == PopupMediaPresentationStatus::ready &&
        media.regional_flag == 504 && !media.avatar && !media.guild_icon;
    UiTextCommand commands[8]{}; UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    valid = valid && ui_text_command_stream_initialize(&text, {commands, 8}) &&
        popup_text_presentation_build_output(popup, Theme{}, &text_output) && text.count == 1 &&
        utf8_equal(commands[0].text, tajikistan);
    check(valid, "flag-named users keep text, guild icons keep identity media and Unicode emoji cells use flags");
    popup_scene_release(&popup);
}
void geometry_cases(void (*check)(bool, const char*) noexcept) noexcept {
    TextSystem system{};
    const bool initialized = text_system_initialize(&system, L"Segoe UI", 16, L"en-us").ok();
    check(initialized, "flag geometry initializes configured text owner");
    if (!initialized) return;
    const auto catalog = unicode_emoji_catalog();
    u32 checked = 0;
    for (u32 i = 0; i < catalog.count; ++i) {
        const auto text = catalog[i].glyph();
        const u16 flag = regional_flag_sequence(text);
        if (flag != 504 && flag != 505 && flag != 679 && flag != 69) continue;
        TextStyleSpan style{0, text.bytes, text_style_custom_emoji};
        TextEditLayoutOwner owner{};
        TextEditMetric selections[kTextEditSelectionCapacity]{};
        owner.selections = selections; owner.selection_capacity = kTextEditSelectionCapacity;
        const TextEditLayoutInput input{text, text_edit_signature(text), 0, text.bytes / 2,
            0, 0, 16, 128, 128, text_edit_layout_wrap, {&style, 1}};
        bool valid = text_edit_layout_measure(&owner, input, system) && owner.active && owner.geometry &&
            text_edit_layout_line_count(owner) == 1;
        TextEditMetric metric{}; u32 count = 0;
        valid = valid && text_edit_layout_range_geometry(owner, 0, text.bytes / 2, {&metric, 1}, &count) &&
            count == 1 && metric.width > 15.99f && metric.width < 16.01f &&
            metric.source_first == 0 && metric.source_count == text.bytes / 2;
        check(valid, "flag native range/caret layout is one em-sized true RI/tag cluster");
        text_edit_layout_release(&owner); ++checked;
    }
    check(checked == 4, "flag native geometry exercises Tajikistan Tokelau Wales and Sark");
    const auto adjacent = utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF");
    TextStyleSpan style{0, adjacent.bytes, text_style_custom_emoji};
    TextEditLayoutOwner owner{};
    TextEditMetric selections[kTextEditSelectionCapacity]{};
    owner.selections = selections; owner.selection_capacity = kTextEditSelectionCapacity;
    const TextEditLayoutInput input{adjacent, text_edit_signature(adjacent), 0, 12,
        0, 0, 16, 128, 128, text_edit_layout_wrap, {&style, 1}};
    bool valid = text_edit_layout_measure(&owner, input, system) && owner.geometry &&
        text_edit_layout_line_count(owner) == 3;
    TextEditMetric metrics[3]{}; u32 count = 0;
    const bool range_ok = owner.active && text_edit_layout_range_geometry(owner, 0, 12, {metrics, 3}, &count);
    valid = valid && range_ok && count == 3 &&
        metrics[0].width > 15.99f && metrics[0].width < 16.01f &&
        metrics[1].width > 15.99f && metrics[1].width < 16.01f &&
        metrics[2].width > 15.99f && metrics[2].width < 16.01f &&
        metrics[0].top < metrics[1].top && metrics[1].top < metrics[2].top;
    if (!valid) {
        diagnostic("flag_geometry_lines=", text_edit_layout_line_count(owner));
        diagnostic("flag_geometry_ranges=", count);
        diagnostic("flag_geometry_width0_x64=", metrics[0].width > 0 ? static_cast<u32>(metrics[0].width * 64) : 0);
        diagnostic("flag_geometry_width1_x64=", metrics[1].width > 0 ? static_cast<u32>(metrics[1].width * 64) : 0);
        diagnostic("flag_geometry_width2_x64=", metrics[2].width > 0 ? static_cast<u32>(metrics[2].width * 64) : 0);
    }
    check(valid, "flag native adjacent RI sequences wrap as distinct em-sized complete atoms");
    text_edit_layout_release(&owner); text_system_release(&system);
}
// Pasted ANSI escapes stay in the draft; like the official editor the composer
// draws U+001B as nothing (no missing-glyph box, no fallback space) while the
// caret still steps over it as one position.
void escape_geometry_case(void (*check)(bool, const char*) noexcept) noexcept {
    TextSystem system{};
    if (!text_system_initialize(&system, L"Segoe UI", 16, L"en-us").ok()) {
        check(false, "composer escape geometry initializes configured text owner");
        return;
    }
    const auto escaped = utf8_literal("a\x1b[31mb\x1b[0m");
    const auto plain = utf8_literal("a[31mb[0m");
    // Cluster lefts per source position, unstyled and as a code block.
    float x[2][2][11]{};
    bool valid = true;
    for (u32 pass = 0; pass < 2 && valid; ++pass) {
        const auto text = pass ? plain : escaped;
        const TextStyleSpan style{0, text.bytes, text_style_monospace | text_style_code_block};
        for (u32 styled = 0; styled < 2 && valid; ++styled) {
            TextEditLayoutOwner owner{};
            TextEditMetric selections[kTextEditSelectionCapacity]{};
            owner.selections = selections; owner.selection_capacity = kTextEditSelectionCapacity;
            const TextEditLayoutInput input{text, text_edit_signature(text), 0, text.bytes, 0, 0, 400, 40, 400,
                text_edit_layout_wrap, {&style, styled}};
            valid = text_edit_layout_measure(&owner, input, system) && owner.active &&
                text_edit_layout_line_count(owner) == 1;
            if (!valid) diagnostic("escape_geometry_measure_geometry=", owner.geometry);
            for (u32 position = 0; valid && position < text.bytes; ++position) {
                TextEditMetric metric{};
                valid = text_edit_layout_position_geometry(owner, position, &metric) &&
                    metric.source_first == position && metric.source_count == 1;
                if (!valid) {
                    diagnostic("escape_geometry_position=", position);
                    diagnostic("escape_geometry_text_position=", metric.source_first);
                    diagnostic("escape_geometry_length=", metric.source_count);
                }
                x[pass][styled][position] = metric.left;
            }
            if (!valid) diagnostic("escape_geometry_failed_pass_styled=", pass * 2 + styled);
            text_edit_layout_release(&owner);
        }
    }
    // ESC adds no advance: "a" ESC "[31mb" ESC "[0m" lines up with "a[31mb[0m".
    for (u32 styled = 0; styled < 2 && valid; ++styled)
        for (u32 position = 0; valid && position < escaped.bytes; ++position) {
            const u32 mapped = position < 2 ? position : position < 8 ? position - 1 : position - 2;
            const float delta = x[0][styled][position] - x[1][styled][mapped];
            valid = delta > -.01f && delta < .01f;
            if (!valid) diagnostic("escape_geometry_mismatch_position=", styled * 16 + position);
        }
    check(valid, "composer draws ANSI escapes with no advance and keeps one caret stop each");
    text_system_release(&system);
}
} // namespace
void test_flag_surfaces(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(sizeof(MessageComposerProjectionSpan) == 40);
    scanner_cases(check); markdown_cases(check); composer_cases(check); composer_capacity_case(check); popup_identity_case(check);
    geometry_cases(check);
    escape_geometry_case(check);
}
} // namespace maicord
