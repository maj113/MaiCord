#include "../../../support/test_main_shared.hpp"

#include "../../../support/ui_scene_fixtures.hpp"
#include "../../../support/model_store_fixtures.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "../../../support/layout_validation.hpp"
#include "maicord/ui/chat/messages/author.hpp"
#include "maicord/ui/chat/messages/action_bar.hpp"
#include "maicord/app/chat/messages/reaction_viewer_controller.hpp"
#include "maicord/app/chat/messages/reaction_viewer_storage.hpp"
#include "maicord/ui/text/code_highlight.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/chat/messages/action_bar_presentation.hpp"
#include "maicord/ui/chat/messages/context_controller.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/ui/chat/messages/reaction_presentation.hpp"
#include "maicord/ui/chat/messages/rich_widgets.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/chat/messages/text_coordinator.hpp"
#include "maicord/ui/chat/messages/reaction_picker.hpp"
#include "maicord/ui/chat/messages/reaction_viewer_presentation.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"
#include "maicord/ui/shell/text_presentation.hpp"
#include "maicord/ui/text/text.hpp"
#include "maicord/app/net/rest_client.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
namespace {

MessageTextCommand g_message_text_commands[kMaximumMessageTextCommands];
char g_message_text_utf8[kMaximumMessageTextUtf8Bytes];
TextStyleSpan g_message_text_styles[kMaximumMessageTextStyleSpans];
MessageMarkdownSemantic g_message_text_semantics[kMaximumMessageTextSemantics];
MessageTextMeasurement g_message_text_measurements[kMaximumMessageTextMeasurements];
MessageTextSpoilerReveal g_message_text_spoiler_reveals[kMaximumMessageTextSpoilerReveals];
u32 g_message_text_byte_styles[kMessageTextByteStyleScratchCapacity];
UiTextMeasureRecord g_message_text_coordinator_records[kMaximumMessageTextMeasureCacheRecords];
MessageTextCommand g_tiny_message_text_commands[1];

bool markdown_text_equal(const MessageMarkdownResult& result, Utf8View expected) noexcept {
    return result.ok() &&
           bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(result.text.data), result.text.bytes},
                       ConstSlice<u8>{reinterpret_cast<const u8*>(expected.data), expected.bytes});
}

struct MarkdownSemanticFixture {
    bool reveal_spoiler;
};

Utf8View test_markdown_resolve(void*, MessageMarkdownEntityKind kind, u64 id) noexcept {
    if (kind == MessageMarkdownEntityKind::user && id == 1) {
        return utf8_literal("Alice");
    }
    if (kind == MessageMarkdownEntityKind::channel && id == 2) {
        return utf8_literal("general");
    }
    if (kind == MessageMarkdownEntityKind::role && id == 3) {
        return utf8_literal("Admin");
    }
    return Utf8View{};
}

TimestampFormatResult test_markdown_timestamp(void*, u64 unix_seconds, char style,
                                                     Slice<char> output) noexcept {
    constexpr Utf8View value = utf8_literal("soon");
    if (unix_seconds != 100 || style != 'R' || !output.data || output.count < value.bytes) {
        return TimestampFormatResult{};
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        output[index] = value.data[index];
    }
    return TimestampFormatResult{101, value.bytes, true};
}

bool test_markdown_spoiler(void* opaque, u32) noexcept {
    auto* fixture = static_cast<MarkdownSemanticFixture*>(opaque);
    return fixture && fixture->reveal_spoiler;
}

bool initialize_presentation(MessageTextPresentation* presentation, MessageTextCommand* commands,
                             u32 command_count) noexcept {
    return message_text_presentation_initialize(
        presentation, Slice<MessageTextCommand>{commands, command_count},
        Slice<char>{g_message_text_utf8, kMaximumMessageTextUtf8Bytes},
        Slice<TextStyleSpan>{g_message_text_styles, kMaximumMessageTextStyleSpans},
        Slice<MessageMarkdownSemantic>{g_message_text_semantics, kMaximumMessageTextSemantics},
        Slice<MessageTextMeasurement>{g_message_text_measurements, kMaximumMessageTextMeasurements},
        Slice<MessageTextSpoilerReveal>{g_message_text_spoiler_reveals, kMaximumMessageTextSpoilerReveals},
        Slice<u32>{g_message_text_byte_styles, kMessageTextByteStyleScratchCapacity});
}

} // namespace

void test_message_markdown() noexcept {
    char output[512]{};
    u32 styles[512]{};
    TextStyleSpan spans[64]{};
    const auto parse = [&](Utf8View input, u32 span_capacity = 64) noexcept {
        return message_markdown_parse(input, Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                      Slice<TextStyleSpan>{spans, span_capacity});
    };

    MessageMarkdownResult result = parse(utf8_literal("plain Καλημέρα 世界"));
    expect(markdown_text_equal(result, utf8_literal("plain Καλημέρα 世界")) && result.span_count == 0 &&
               result.transformations == 0,
           "message_markdown_plain_unicode");

    result = parse(utf8_literal("**bold *both* end**"));
    expect(markdown_text_equal(result, utf8_literal("bold both end")) && result.span_count == 3 &&
               spans[0].byte_offset == 0 && spans[0].byte_count == 5 && spans[0].flags == text_style_bold &&
               spans[1].byte_offset == 5 && spans[1].byte_count == 4 &&
               spans[1].flags == (text_style_bold | text_style_italic) && spans[2].byte_offset == 9 &&
               spans[2].byte_count == 4 && spans[2].flags == text_style_bold,
           "message_markdown_nested_bold_italic_partition");

    result = parse(utf8_literal("__***under bold italic***__ ~~strike~~ ||secret||"));
    expect(markdown_text_equal(result, utf8_literal("under bold italic strike secret")) &&
               result.span_count == 3 &&
               spans[0].flags == (text_style_bold | text_style_italic | text_style_underline) &&
               spans[1].flags == text_style_strikethrough && spans[2].flags == text_style_spoiler,
           "message_markdown_combined_underline_strike_spoiler");

    // Rules: **, __, ||, inline code and *** may open before a
    // space; a single * and ~~ need text right after their opener.
    result = parse(utf8_literal("*** spaced ***|** b **|__ u __|` c `|* i *|~~ s ~~"));
    expect(markdown_text_equal(result, utf8_literal(" spaced | b | u | c |* i *|~~ s ~~")) &&
               result.span_count == 4 && spans[0].byte_offset == 0 && spans[0].byte_count == 8 &&
               spans[0].flags == (text_style_bold | text_style_italic) && spans[1].flags == text_style_bold &&
               spans[2].flags == text_style_underline && spans[3].flags == text_style_monospace,
           "message_markdown_spaced_multi_char_markers_open");

    result = parse(utf8_literal("`**literal**` and ```cpp\nint x = 1;\n```"));
    expect(markdown_text_equal(result, utf8_literal("**literal** and int x = 1;\n")) &&
               result.code_blocks == 1 && result.syntax_tokens == 3 && !result.syntax_truncated &&
               styles[0] == text_style_monospace &&
               styles[16] == (text_style_monospace | text_style_code_block | text_style_syntax_type) &&
               styles[22] == (text_style_monospace | text_style_code_block | text_style_syntax_operator) &&
               styles[24] == (text_style_monospace | text_style_code_block | text_style_syntax_number),
           "message_markdown_code_suppresses_inner_and_hides_language");

    result = parse(utf8_literal("```\n\n1. step\n```"));
    expect(markdown_text_equal(result, utf8_literal("1. step\n")) && result.code_blocks == 1 &&
               result.span_count == 1 && spans[0].byte_offset == 0 && spans[0].byte_count == 8 &&
               spans[0].flags == (text_style_monospace | text_style_code_block),
           "message_markdown_fence_opens_on_its_own_line");

    result = parse(utf8_literal("- first\n  * nested\n1. ordered"));
    expect(markdown_text_equal(result, utf8_literal("  first\n    nested\n1. ordered")) &&
               result.list_items == 3 &&
               (styles[0] & (text_style_list | text_style_list_bullet)) ==
                   (text_style_list | text_style_list_bullet) &&
               (styles[8] & (text_style_list | text_style_list_bullet | kTextStyleListDepthMask)) ==
                   (text_style_list | text_style_list_bullet | text_style_list_depth_1) &&
               (styles[19] & text_style_list) != 0 && (styles[19] & text_style_list_bullet) == 0,
           "message_markdown_unordered_nested_and_ordered_lists");

    // Items with equal flags must not merge into one span: the renderer draws one bullet per span start.
    result = parse(utf8_literal("- hi\n- hi\n- hi"));
    expect(markdown_text_equal(result, utf8_literal("  hi\n  hi\n  hi")) && result.list_items == 3 &&
               result.span_count == 3 && spans[0].byte_offset == 0 && spans[0].byte_count == 5 &&
               spans[1].byte_offset == 5 && spans[1].byte_count == 5 && spans[2].byte_offset == 10 &&
               spans[2].byte_count == 4 &&
               spans[0].flags == spans[1].flags && spans[1].flags == spans[2].flags &&
               (spans[2].flags & text_style_list_bullet) != 0,
           "message_markdown_each_bullet_item_is_its_own_span");

    u32 limited_styles[32]{};
    const CodeHighlightResult limited = code_highlight(
        utf8_literal("int value = 42; // comment"), CodeLanguage::cpp, Slice<u32>{limited_styles, 32}, 2);
    struct LanguageAliasCase {
        Utf8View tag;
        CodeLanguage language;
    };
    const LanguageAliasCase language_aliases[] = {
        {utf8_literal("c"), CodeLanguage::c},
        {utf8_literal("C++"), CodeLanguage::cpp},
        {utf8_literal("csharp"), CodeLanguage::csharp},
        {utf8_literal("rust"), CodeLanguage::rust},
        {utf8_literal("jsx"), CodeLanguage::javascript},
        {utf8_literal("typescript"), CodeLanguage::typescript},
        {utf8_literal("java"), CodeLanguage::java},
        {utf8_literal("jsonc"), CodeLanguage::json},
        {utf8_literal("python"), CodeLanguage::python},
        {utf8_literal("ps1"), CodeLanguage::powershell},
        {utf8_literal("bash"), CodeLanguage::shell},
        {utf8_literal("hlsl"), CodeLanguage::hlsl},
        {utf8_literal("frag"), CodeLanguage::glsl},
        {utf8_literal("sql"), CodeLanguage::sql},
        {utf8_literal("xml"), CodeLanguage::markup},
        {utf8_literal("css"), CodeLanguage::css},
    };
    bool aliases_valid = true;
    for (u32 index = 0; index < sizeof(language_aliases) / sizeof(language_aliases[0]); ++index) {
        aliases_valid = aliases_valid && code_language_from_tag(language_aliases[index].tag) ==
                                             language_aliases[index].language;
    }
    expect(aliases_valid && code_language_from_tag(utf8_literal("unknown")) == CodeLanguage::none &&
               limited.token_count == 2 && limited.truncated &&
               (limited_styles[0] & text_style_syntax_type) != 0,
           "code_highlight_aliases_and_token_bound");

    u32 cpp_styles[64]{};
    const CodeHighlightResult cpp = code_highlight(utf8_literal("const char* s = \"hi\"; return 42; // c"),
                                                   CodeLanguage::cpp, Slice<u32>{cpp_styles, 64}, 48);
    u32 preprocessor_styles[32]{};
    const CodeHighlightResult preprocessor = code_highlight(
        utf8_literal("#define VALUE 42\n"), CodeLanguage::hlsl, Slice<u32>{preprocessor_styles, 32}, 48);
    expect(!cpp.truncated && cpp.token_count == 8 && (cpp_styles[0] & text_style_syntax_keyword) != 0 &&
               (cpp_styles[6] & text_style_syntax_type) != 0 &&
               (cpp_styles[10] & text_style_syntax_operator) != 0 &&
               (cpp_styles[16] & text_style_syntax_string) != 0 &&
               (cpp_styles[22] & text_style_syntax_keyword) != 0 &&
               (cpp_styles[29] & text_style_syntax_number) != 0 &&
               (cpp_styles[33] & text_style_syntax_comment) != 0 && preprocessor.token_count == 1 &&
               (preprocessor_styles[0] & text_style_syntax_preprocessor) != 0,
           "code_highlight_cpp_token_classes");

    result = parse(utf8_literal("# One\n## Two\n### Three\n-# Small\n> Quote\n>>> All\nLines"));
    expect(markdown_text_equal(result, utf8_literal("One\nTwo\nThree\nSmall\nQuote\nAll\nLines")) &&
               result.span_count == 5 && spans[0].flags == (text_style_heading_1 | text_style_bold) &&
               spans[1].flags == (text_style_heading_2 | text_style_bold) &&
               spans[2].flags == (text_style_heading_3 | text_style_bold) &&
               spans[3].flags == text_style_subtext && spans[4].flags == text_style_quote,
           "message_markdown_block_prefixes");

    result = parse(utf8_literal("[link](https://example.com) <@!123> <#456> <@&789> "
                                "<a:wave:111> </ping:222> @everyone @here"));
    expect(markdown_text_equal(result,
                               utf8_literal("link @123 #456 @789 \xEF\xBF\xBC /ping @everyone @here")) &&
               result.transformations == 8,
           "message_markdown_masked_link_and_mention_tags");

    MessageMarkdownSemantic semantics[16]{};
    MarkdownSemanticFixture semantic_fixture{};
    MessageMarkdownOptions options{&semantic_fixture,
                                   test_markdown_resolve,
                                   test_markdown_timestamp,
                                   test_markdown_spoiler,
                                   0,
                                   MessageMarkdownSemanticKind::none,
                                   false};
    result = message_markdown_parse(utf8_literal("<:still:111> <a:wave:222>"), Slice<char>{output, 512},
                                    Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    expect(markdown_text_equal(result, utf8_literal("\xEF\xBF\xBC \xEF\xBF\xBC")) &&
               result.semantic_count == 2 &&
               semantics[0].kind == MessageMarkdownSemanticKind::custom_emoji &&
               semantics[0].target == 111 && semantics[0].reserved == 0 && semantics[1].target == 222 &&
               semantics[1].reserved == 1 && (styles[0] & text_style_custom_emoji) != 0,
           "message_markdown_custom_emoji_inline_media_semantics");
    constexpr Utf8View semantic_source =
        utf8_literal("[site](https://e.test) <@1> <#2> <@&3> <t:100:R> ||secret||");
    result = message_markdown_parse(semantic_source, Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                    Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    expect(markdown_text_equal(result, utf8_literal("site @Alice #general @Admin soon secret")),
           "message_markdown_semantic_resolved_text");
    expect(result.semantic_count == 6 && result.resolved_entities == 3 && result.unresolved_entities == 0 &&
               result.timestamps == 1 && result.next_timestamp_update_unix_seconds == 101,
           "message_markdown_semantic_resolution_counts");
    expect(semantics[0].kind == MessageMarkdownSemanticKind::link &&
               semantics[1].kind == MessageMarkdownSemanticKind::user_mention && semantics[1].target == 1 &&
               semantics[2].target == 2 && semantics[3].target == 3 && semantics[4].target == 100 &&
               semantics[5].kind == MessageMarkdownSemanticKind::spoiler,
           "message_markdown_semantic_kinds_and_targets");
    expect(bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(semantic_source.data +
                                                                  semantics[0].source_offset),
                                      semantics[0].source_bytes},
                       byte_literal("https://e.test")),
           "message_markdown_semantic_source_range");
    const u32 masked_link_source_offset = semantics[0].source_offset;

    constexpr Utf8View message_link_source = utf8_literal("go https://discord.com/channels/1/2/3 now");
    result = message_markdown_parse(message_link_source, Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                    Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    MessageLink parsed_message_link{};
    expect(markdown_text_equal(result, utf8_literal("go #general \xE2\x80\xBA \xF0\x9F\x92\xAC now")) &&
               result.semantic_count == 1 &&
               semantics[0].kind == MessageMarkdownSemanticKind::message_link && semantics[0].target == 3 &&
               message_markdown_message_link(utf8_literal("https://discord.com/channels/1/2/3"),
                                             &parsed_message_link) &&
               parsed_message_link.guild_id == 1 && parsed_message_link.channel_id == 2 &&
               parsed_message_link.message_id == 3 && parsed_message_link.source_bytes == 34,
           "message_markdown_message_permalink_projects_and_retains_target");

    result = message_markdown_parse(utf8_literal("https://discord.com/channels/1/2/3."),
                                    Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                    Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    expect(markdown_text_equal(result, utf8_literal("#general \xE2\x80\xBA \xF0\x9F\x92\xAC.")) &&
               result.semantic_count == 1 && semantics[0].source_bytes == 34,
           "message_markdown_message_permalink_stops_before_sentence_punctuation");

    result = message_markdown_parse(utf8_literal("\xF0\x9F\x98\x80"), Slice<char>{output, 512},
                                    Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    bool jumbo_unicode = result.ok() && result.span_count != 0;
    for (u32 index = 0; index < result.span_count; ++index) {
        jumbo_unicode = jumbo_unicode && (spans[index].flags & text_style_jumbo_emoji) != 0;
    }
    result = message_markdown_parse(utf8_literal("<:party:111>"), Slice<char>{output, 512},
                                    Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    const bool jumbo_custom =
        result.ok() && result.span_count == 1 && (spans[0].flags & text_style_jumbo_emoji) != 0;
    result = message_markdown_parse(utf8_literal("\xF0\x9F\x98\x80 text"), Slice<char>{output, 512},
                                    Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    expect(jumbo_unicode, "message_markdown_jumbo_emoji_styles_unicode_only_message");
    expect(jumbo_custom, "message_markdown_jumbo_emoji_styles_custom_only_message");
    expect(result.ok() && (styles[0] & text_style_jumbo_emoji) == 0,
           "message_markdown_jumbo_emoji_rejects_mixed_text");

    char thirty_emoji[30 * 4]{};
    char thirty_one_emoji[31 * 4]{};
    constexpr char grinning_face[4]{static_cast<char>(0xF0), static_cast<char>(0x9F),
                                    static_cast<char>(0x98), static_cast<char>(0x80)};
    for (u32 index = 0; index < 31; ++index) {
        for (u32 byte = 0; byte < 4; ++byte) {
            thirty_one_emoji[index * 4 + byte] = grinning_face[byte];
            if (index < 30) {
                thirty_emoji[index * 4 + byte] = grinning_face[byte];
            }
        }
    }
    result = message_markdown_parse(Utf8View{thirty_emoji, sizeof(thirty_emoji)}, Slice<char>{output, 512},
                                    Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    const bool thirty_are_jumbo =
        result.ok() && result.span_count == 1 && (spans[0].flags & text_style_jumbo_emoji) != 0;
    result = message_markdown_parse(Utf8View{thirty_one_emoji, sizeof(thirty_one_emoji)},
                                    Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                    Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    expect(thirty_are_jumbo && result.ok() && (styles[0] & text_style_jumbo_emoji) == 0,
           "message_markdown_jumbo_emoji_threshold_is_less_than_31");

    semantic_fixture.reveal_spoiler = true;
    options.has_hover = true;
    options.hovered_kind = MessageMarkdownSemanticKind::link;
    options.hovered_source_offset = masked_link_source_offset;
    result = message_markdown_parse(semantic_source, Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                    Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 16});
    expect(result.ok() && (styles[0] & text_style_interactive_hover) != 0 &&
               (styles[0] & text_style_underline) != 0 &&
               (styles[result.text.bytes - 1] & text_style_spoiler) == 0 &&
               message_markdown_url_valid(utf8_literal("https://e.test")) &&
               !message_markdown_url_valid(utf8_literal("javascript:alert(1)")),
           "message_markdown_hover_reveal_and_url_policy");

    result = message_markdown_parse(semantic_source, Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                    Slice<TextStyleSpan>{spans, 64}, options,
                                    Slice<MessageMarkdownSemantic>{semantics, 1});
    expect(result.ok() && result.semantic_count == 1 && result.semantics_truncated,
           "message_markdown_semantic_capacity_degrades");

    result = parse(utf8_literal(R"(\*literal* **open)"));
    expect(markdown_text_equal(result, utf8_literal("*literal* **open")) && result.transformations == 1,
           "message_markdown_escape_and_unmatched_literal");

    // simple-markdown escape rule /^\\([^0-9A-Za-z\s])/ and the emoticon rule
    // /^(¯\\_\(ツ\)_\/¯)/ (both verified against the official client's rules).
    // The official /shrug sends the face unescaped; it renders verbatim.
    constexpr Utf8View shrug = utf8_literal("\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF");
    result = parse(shrug);
    expect(markdown_text_equal(result, shrug) && result.span_count == 0 && result.transformations == 0,
           "message_markdown_shrug_face_keeps_its_backslash");
    result = parse(utf8_literal("hi \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF there \xC2\xAF"));
    expect(markdown_text_equal(result, utf8_literal("hi \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF there \xC2\xAF")) &&
               result.span_count == 0,
           "message_markdown_shrug_face_mid_text");
    result = parse(utf8_literal("**\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF**"));
    expect(markdown_text_equal(result, shrug) && result.span_count == 1 && spans[0].byte_offset == 0 &&
               spans[0].byte_count == shrug.bytes && spans[0].flags == text_style_bold,
           "message_markdown_shrug_face_inside_formatting");
    // Only the exact face is the emoticon: a variant still escapes "\_".
    result = parse(utf8_literal("\xC2\xAF\\_(x)_/\xC2\xAF \\\xC2\xAF"));
    expect(markdown_text_equal(result, utf8_literal("\xC2\xAF_(x)_/\xC2\xAF \xC2\xAF")),
           "message_markdown_shrug_face_lookalike_still_escapes");
    // The face the client's shrug command produced before it stopped escaping: \\ is a backslash, \_ an underscore.
    result = parse(utf8_literal("\xC2\xAF\\\\\\_(\xE3\x83\x84)\\_/\xC2\xAF"));
    expect(markdown_text_equal(result, shrug) && result.span_count == 0,
           "message_markdown_escaped_shrug_face_renders_the_same");

    result = parse(utf8_literal(R"(a\\b \\*i* \\\*x* \\\\)"));
    expect(markdown_text_equal(result, utf8_literal(R"(a\b \i \*x* \\)")) && result.span_count == 1 &&
               spans[0].byte_offset == 5 && spans[0].byte_count == 1 && spans[0].flags == text_style_italic,
           "message_markdown_escaped_backslash_then_markers");
    // (Only ** and the single-marker italics honour escapes; ~~ and __ end at the next marker pair.)
    result = parse(utf8_literal(R"(**a\*b** _c\_d_)"));
    expect(markdown_text_equal(result, utf8_literal("a*b c_d")) && result.span_count == 2 &&
               spans[0].flags == text_style_bold && spans[0].byte_count == 3 &&
               spans[1].flags == text_style_italic && spans[1].byte_count == 3,
           "message_markdown_escapes_inside_formatting");
    result = parse(utf8_literal("`a\\*b` ```\nc\\_d\n``` `e\\`"));
    expect(markdown_text_equal(result, utf8_literal("a\\*b c\\_d\n e\\")) && result.span_count == 3 &&
               spans[0].flags == text_style_monospace && spans[0].byte_count == 4 &&
               spans[1].flags == (text_style_monospace | text_style_code_block) &&
               spans[2].flags == text_style_monospace && spans[2].byte_count == 2,
           "message_markdown_code_has_no_escapes_and_a_backslash_does_not_escape_its_close");
    result = parse(utf8_literal("https://e.test/a\\_b <https://e.test/c\\_d>"));
    expect(markdown_text_equal(result, utf8_literal("https://e.test/a\\_b https://e.test/c\\_d")),
           "message_markdown_urls_keep_their_backslashes");
    result = parse(utf8_literal("[x\\_y](https://e.test)"));
    expect(markdown_text_equal(result, utf8_literal("x_y")),
           "message_markdown_escape_inside_masked_link_text");
    result = parse(utf8_literal("end\\"));
    expect(markdown_text_equal(result, utf8_literal("end\\")) && result.transformations == 0,
           "message_markdown_trailing_backslash_is_literal");
    result = parse(utf8_literal("\\"));
    expect(markdown_text_equal(result, utf8_literal("\\")), "message_markdown_lone_backslash_is_literal");
    result = parse(utf8_literal("a\\\\"));
    expect(markdown_text_equal(result, utf8_literal("a\\")), "message_markdown_trailing_escaped_backslash");
    // [^0-9A-Za-z\s]: alphanumerics, ASCII whitespace and JavaScript's Unicode spaces stay literal; everything
    // else escapes, including non-ASCII letters, U+0085 and the other C0 controls.
    result = parse(utf8_literal("\\a\\Z\\5\\ \\\n\\\t\\\xC2\xA0\\\xE3\x80\x80"));
    expect(markdown_text_equal(result, utf8_literal("\\a\\Z\\5\\ \\\n\\\t\\\xC2\xA0\\\xE3\x80\x80")) &&
               result.transformations == 0,
           "message_markdown_escape_leaves_alphanumerics_and_whitespace");
    result = parse(utf8_literal("\\\xC3\xA9\\\xC2\x85\\\x01\\\x7F\\\xF0\x9F\x98\x80"));
    expect(markdown_text_equal(result, utf8_literal("\xC3\xA9\xC2\x85\x01\x7F\xF0\x9F\x98\x80")) &&
               result.transformations == 5,
           "message_markdown_escape_takes_any_other_character");

    const char invalid_utf8[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    expect(message_markdown_parse(Utf8View{invalid_utf8, 2}, Slice<char>{output, 512},
                                  Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64})
                       .error == MessageMarkdownError::invalid_utf8 &&
               message_markdown_parse(utf8_literal("too large"), Slice<char>{output, 2},
                                      Slice<u32>{styles, 512}, Slice<TextStyleSpan>{spans, 64})
                       .error == MessageMarkdownError::output_too_small,
           "message_markdown_utf8_and_output_bounds");

    // More than 32 nested closable openers (each marker recurs later; "["
    // without a "](url)" is plain text and never opens).
    char deep[80]{};
    constexpr char markers[4][3]{"**", "__", "~~", "||"};
    for (u32 index = 0; index < 37; ++index) {
        deep[index * 2] = markers[index % 4][0];
        deep[index * 2 + 1] = markers[index % 4][1];
    }
    deep[74] = 'x';
    expect(message_markdown_parse(Utf8View{deep, 75}, Slice<char>{output, 512}, Slice<u32>{styles, 512},
                                  Slice<TextStyleSpan>{spans, 64})
                       .error == MessageMarkdownError::delimiter_capacity &&
               parse(utf8_literal("**a** plain *b*"), 1).error == MessageMarkdownError::span_capacity,
           "message_markdown_delimiter_and_span_bounds");
}

void test_timestamp_format() noexcept {
    char output[64]{};
    const TimestampFormatResult past =
        format_timestamp_local(940, 'R', 1'000, Slice<char>{output, 64});
    expect(past.ok && past.bytes == 12 &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(output), past.bytes},
                           byte_literal("1 minute ago")) &&
               past.next_update_unix_seconds == 1'001,
           "timestamp_relative_past_and_refresh");
    const TimestampFormatResult future =
        format_timestamp_local(1'002, 'R', 1'000, Slice<char>{output, 64});
    expect(future.ok &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(output), future.bytes},
                           byte_literal("in 2 seconds")) &&
               future.next_update_unix_seconds == 1'001,
           "timestamp_relative_future");
    expect(!format_timestamp_local(1'000, 'R', 1'000, Slice<char>{output, 2}).ok,
           "timestamp_output_bound");
    // Calendar labels (search result headers): Today/Yesterday, else a dated label.
    const u64 now = 1'790'000'000; // mid-September 2026, away from DST changes
    const auto today = format_timestamp_calendar(now, now, Slice<char>{output, 64});
    const bool today_label = today.ok && today.bytes > 9 &&
                             bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(output), 9},
                                         byte_literal("Today at ")) &&
                             today.next_update_unix_seconds == now + 3'600;
    const auto older = format_timestamp_calendar(now - 30ull * 86'400, now, Slice<char>{output, 64});
    const bool dated = older.ok && older.bytes > 0 && older.next_update_unix_seconds == 0 &&
                       !bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(output), 5}, byte_literal("Today")) &&
                       !bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(output), 5}, byte_literal("Yeste"));
    const auto yesterday = format_timestamp_calendar(now - 86'400, now, Slice<char>{output, 64});
    const bool yesterday_label = yesterday.ok && yesterday.bytes > 13 &&
                                 bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(output), 13},
                                             byte_literal("Yesterday at "));
    expect(today_label && dated && yesterday_label &&
               !format_timestamp_calendar(now, now, Slice<char>{output, 4}).ok,
           "timestamp_calendar_today_yesterday_and_dated_labels");
}

void test_message_text_copy_projection() noexcept {
    const char display_storage[]{
        'A', ' ', static_cast<char>(0xEF), static_cast<char>(0xBF), static_cast<char>(0xBC), ' ', 'B'};
    constexpr Utf8View source = utf8_literal("A <:lol:44> B");
    const MessageMarkdownSemantic semantic{44, 2, 3, 2, 9, MessageMarkdownSemanticKind::custom_emoji, 0};
    MessageTextCommand command{};
    command.text = Utf8View{display_storage, sizeof(display_storage)};
    command.semantics = ConstSlice<MessageMarkdownSemantic>{&semantic, 1};
    char output[64]{};
    const MessageTextCopyProjectionResult whole = message_text_project_selection_for_copy(
        command, source, 0, command.text.bytes, Slice<char>{output, sizeof(output)});
    const bool whole_matches = whole.ok && whole.transformed && whole.bytes == source.bytes &&
                               utf8_equal(Utf8View{output, whole.bytes}, source);
    const MessageTextCopyProjectionResult emoji =
        message_text_project_selection_for_copy(command, source, 2, 5, Slice<char>{output, sizeof(output)});
    expect(whole_matches && emoji.ok && emoji.transformed &&
               utf8_equal(Utf8View{output, emoji.bytes}, utf8_literal("<:lol:44>")),
           "message_text_copy_restores_custom_emoji_wire_token");
}

void test_message_text_presentation() noexcept {
    MessageRecord records[2]{};
    u32 order[2]{};
    u8 source_text[512]{};
    MessageStore store{};
    u32 item_units[2]{};
    u64 fenwick[2]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    MessageSceneRowWidget rows[2]{};
    UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode layout[2 * kMessageSceneWidgetsPerRow + 1]{};
    const u32 default_units = ui_extent_units(kUiBootstrapMessageRowHeight);
    constexpr Utf8View source = utf8_literal("**Bold** [link](https://discord.com) ||secret||");
    const bool model_ready =
        message_store_initialize(&store, Slice<MessageRecord>{records, 2}, Slice<u32>{order, 2},
                                 Slice<u8>{source_text, 512}) &&
        message_store_upsert(&store, MessageInput{91'000, 7, 9, 1'700'000'000'000ull, 0, source, 0}).ok() &&
        ui_variable_list_initialize(&list, Slice<u32>{item_units, 2}, Slice<u64>{fenwick, 2}, 1,
                                    default_units) &&
        message_timeline_initialize(&timeline, &store, &list, default_units, ui_extent_units(320.0f),
                                    true) &&
        message_scene_initialize(&scene, &timeline, Slice<MessageSceneRowWidget>{rows, 2},
                                 Slice<UiWidget>{widgets, 2 * kMessageSceneWidgetsPerRow + 1},
                                 Slice<UiLayoutNode>{layout, 2 * kMessageSceneWidgetsPerRow + 1});
    MessageRichStore rich_store{};
    GatewayMessageAttachment rich_attachments[2]{};
    rich_attachments[0].id = 71;
    rich_attachments[0].filename = utf8_literal("first.txt");
    rich_attachments[0].content_type = utf8_literal("text/plain");
    rich_attachments[0].url = utf8_literal("https://example.com/first.txt");
    rich_attachments[1].id = 72;
    rich_attachments[1].filename = utf8_literal("second.txt");
    rich_attachments[1].content_type = utf8_literal("text/plain");
    GatewayMessageEmbed rich_embeds[2]{};
    rich_embeds[0].title = utf8_literal("Measured embed title");
    // The official client renders embed author names as plain text; mentions
    // resolve in descriptions and field values.
    rich_embeds[0].author_name = utf8_literal("Layout Bot");
    rich_embeds[0].description =
        utf8_literal("A wrapping embed description whose shaped height must move the next "
                     "rich region instead of overlapping it");
    rich_embeds[0].type = GatewayMessageEmbedType::rich;
    rich_embeds[0].url = utf8_literal("https://example.com/first");
    rich_embeds[0].field_count = 3;
    rich_embeds[1].title = utf8_literal("Second embed title");
    rich_embeds[1].description = utf8_literal("REST: 32ms");
    rich_embeds[1].type = GatewayMessageEmbedType::rich;
    GatewayMessageEmbedField rich_embed_fields[3]{};
    rich_embed_fields[0].name = utf8_literal("First inline field");
    rich_embed_fields[0].value = utf8_literal("A value that participates in retained measurement");
    rich_embed_fields[0].inline_field = true;
    rich_embed_fields[1].name = utf8_literal("Second inline field");
    rich_embed_fields[1].value = utf8_literal("A second value in the same grouped field row");
    rich_embed_fields[1].inline_field = true;
    rich_embed_fields[2].name = utf8_literal("Third inline field");
    rich_embed_fields[2].value = utf8_literal("<@&777> <#7> <@9>");
    rich_embed_fields[2].inline_field = true;
    GatewayMessageSticker rich_stickers[2]{};
    rich_stickers[0].id = 81;
    rich_stickers[0].name = utf8_literal("First sticker");
    rich_stickers[0].format_type = 2;
    rich_stickers[1].id = 82;
    rich_stickers[1].name = utf8_literal("Second sticker");
    rich_stickers[1].format_type = 1;
    GatewayMessageComponent rich_components[2]{};
    rich_components[0].content = utf8_literal("Component content");
    rich_components[0].label = utf8_literal("Component action");
    rich_components[0].type = 2;
    rich_components[0].parent = 0xFFFFu;
    rich_components[1].content = utf8_literal("Second component content");
    rich_components[1].label = utf8_literal("Second action");
    rich_components[1].type = 2;
    rich_components[1].parent = 0xFFFFu;
    GatewayMessageReaction rich_reactions[5]{};
    for (u32 index = 0; index < 5; ++index) {
        rich_reactions[index].emoji_id = (index & 1u) == 0 ? 44 + index : 0;
        rich_reactions[index].name = (index & 1u) == 0 ? utf8_literal("party") : utf8_literal("spark");
        rich_reactions[index].count = index + 1;
        rich_reactions[index].normal_count = index + 1;
        rich_reactions[index].me = index == 0;
    }
    GatewayMessageEvent rich_event{};
    rich_event.id = 91'000;
    rich_event.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message |
                        gateway_message_field_reactions | gateway_message_field_attachments |
                        gateway_message_field_embeds | gateway_message_field_sticker_items |
                        gateway_message_field_components;
    rich_event.reference.message_id = 90'999;
    rich_event.reference.channel_id = 7;
    rich_event.reference.author_id = 8;
    rich_event.reference.author_name = utf8_literal("Reply Author");
    rich_event.reference.content =
        utf8_literal("A reply to <@9> in <#7>\nwhose measured width must be ellipsized");
    rich_event.reference.state = GatewayMessageReferenceState::available;
    rich_event.attachments = ConstSlice<GatewayMessageAttachment>{rich_attachments, 2};
    rich_event.embeds = ConstSlice<GatewayMessageEmbed>{rich_embeds, 2};
    rich_event.embed_fields = ConstSlice<GatewayMessageEmbedField>{rich_embed_fields, 3};
    rich_event.stickers = ConstSlice<GatewayMessageSticker>{rich_stickers, 2};
    rich_event.components = ConstSlice<GatewayMessageComponent>{rich_components, 2};
    rich_event.reactions = ConstSlice<GatewayMessageReaction>{rich_reactions, 5};
    rich_event.rich_truncations = gateway_message_rich_truncated_text;
    const MessageStoreResult source_record = message_store_find(store, 91'000);
    const MessageRecord* source_message =
        source_record.ok() ? message_store_resolve(store, source_record.handle) : nullptr;
    const bool rich_ready =
        model_ready && source_message &&
        message_rich_store_initialize(
            &rich_store, Slice<MessageRichRecord>{g_message_scene_rich_records, 32},
            Slice<u16>{g_message_scene_rich_order, 32}, Slice<u8>{g_message_scene_rich_text, 16 * 1024}) &&
        message_timeline_bind_rich_store(&timeline, &rich_store) &&
        message_rich_store_apply(&rich_store, 91'000, source_message->revision, rich_event).ok();
    const UiRect viewport{0.0f, 0.0f, 720.0f, 320.0f};
    const MessageSceneResult arranged =
        rich_ready ? message_scene_update(&scene, viewport) : MessageSceneResult{};

    MessageTextPresentation presentation{};
    const bool initialized =
        initialize_presentation(&presentation, g_message_text_commands, kMaximumMessageTextCommands);
    ChatStateStore state{};
    GuildRecord state_guilds[1]{};
    u16 state_guild_order[1]{};
    ChannelRecord state_channels[1]{};
    u16 state_channel_order[1]{};
    RoleRecord state_roles[1]{};
    PermissionOverwriteRecord state_overwrites[1]{};
    u8 state_names[256]{};
    NavigationView navigation{};
    UserStore users{};
    UserRecord user_records[1]{};
    u32 user_order[1]{};
    u8 user_names[32]{};
    const bool user_ready =
        user_store_initialize(&users, Slice<UserRecord>{user_records, 1},
                                      Slice<u32>{user_order, 1}, Slice<u8>{user_names, 32}) &&
        user_store_upsert(&users, 9, utf8_literal("Layout Author")).ok();
    const bool state_ready =
        chat_state_store_initialize(&state, Slice<GuildRecord>{state_guilds, 1},
                                       Slice<u16>{state_guild_order, 1},
                                       Slice<ChannelRecord>{state_channels, 1},
                                       Slice<u16>{state_channel_order, 1}, Slice<u8>{state_names, 256}) &&
        chat_state_store_bind_permissions(
            &state, Slice<RoleRecord>{state_roles, 1},
            Slice<PermissionOverwriteRecord>{state_overwrites, 1}) &&
        chat_state_store_upsert_guild(
            &state, GuildInput{70, utf8_literal("Reply Guild"), GuildMutationKind::create, 9})
            .ok() &&
        chat_state_store_upsert_channel(
            &state, ChannelInput{7, 70, 0, utf8_literal("reply-channel"), 0, 0, 0})
            .ok() &&
        chat_state_store_upsert_role(
            &state, RoleInput{777, 70, 0, utf8_literal("Moderators"), 4, 0xC46BEFu, true, false});
    GuildMemberRecord reply_member_records[1]{};
    u32 reply_member_order[1]{};
    GuildMemberStore reply_members{};
    const bool reply_members_ready = guild_member_store_initialize(
        &reply_members, Slice<GuildMemberRecord>{reply_member_records, 1},
        Slice<u32>{reply_member_order, 1});
    if (reply_members_ready) {
        reply_member_records[0].guild_id = 70;
        reply_member_records[0].user_id = 8;
        reply_member_records[0].role_color = 0x123456u;
        reply_member_records[0].occupied = true;
        reply_member_order[0] = 0;
        reply_members.count = 1;
        reply_members.revision = 1;
    }
    const bool reply_members_bound =
        reply_members_ready && message_text_presentation_bind_guild_members(&presentation, &reply_members);
    const GuildMemberRecord* reply_member_lookup =
        guild_member_store_find(reply_members, 70, 8);
    const bool reply_member_lookup_ready =
        reply_member_lookup && reply_member_lookup->role_color == 0x123456u;
    navigation.selected_channel_id = 7;
    const MessageTextPresentationResult built =
        initialized && arranged.ok() && user_ready && state_ready && reply_members_bound
            ? message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                              kDefaultTheme, 1'700'000'100ull)
            : MessageTextPresentationResult{};

    const MessageSceneRowWidget* row = scene.count == 1 ? &scene.rows[0] : nullptr;
    bool precise_author_hits = false;
    for (u32 i = 0; row && i < presentation.command_count; ++i) {
        const auto& command = presentation.commands[i];
        if (command.owner.handle.slot != row->author.slot) continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(scene, command, state, users, nullptr, kDefaultTheme, &header)) continue;
        const float y = (header.name.rectangle.top + header.name.rectangle.bottom) * .5f;
        const auto name_hit = message_author_hit_test(scene, presentation, state, users, nullptr, kDefaultTheme,
                                                     header.name.rectangle.left + 1, y);
        const auto timestamp_hit = message_author_hit_test(scene, presentation, state, users, nullptr, kDefaultTheme,
                                                          header.timestamp.rectangle.left + 1, y);
        const auto blank_hit = message_author_hit_test(scene, presentation, state, users, nullptr, kDefaultTheme,
                                                      command.rectangle.right - 1, y);
        const auto* avatar = message_scene_layout_node(scene, row->avatar);
        const auto avatar_hit = avatar ? message_author_hit_test(scene, presentation, state, users, nullptr,
            kDefaultTheme, (avatar->rectangle.left + avatar->rectangle.right) * .5f,
            (avatar->rectangle.top + avatar->rectangle.bottom) * .5f) : MessageProfileHit{};
        precise_author_hits = name_hit.user_id != 0 && avatar_hit.user_id == name_hit.user_id &&
                              !timestamp_hit.user_id && !blank_hit.user_id;
        break;
    }
    expect(precise_author_hits, "message_profile_only_avatar_and_measured_name_hit_not_timestamp_or_blank_header");
    // Compact rows inline the name into the content flow; it stays a profile target.
    bool compact_name_hit = false;
    const bool compact_built = row && message_scene_set_display(&scene, {0, true}) &&
        message_scene_update(&scene, viewport).ok() &&
        message_text_presentation_build(&presentation, scene, state, &users, navigation, kDefaultTheme,
                                        1'700'000'100ull).ok();
    for (u32 i = 0; compact_built && i < presentation.command_count; ++i) {
        const auto& command = presentation.commands[i];
        if (command.owner.handle.slot != row->content.slot || command.source_id) continue;
        const float y = command.rectangle.top + 4;
        const auto name_hit = message_author_hit_test(scene, presentation, state, users, nullptr, kDefaultTheme,
                                                     command.rectangle.left + 1, y);
        const auto content_hit = message_author_hit_test(scene, presentation, state, users, nullptr, kDefaultTheme,
                                                        command.rectangle.right - 1, y);
        const auto below_hit = message_author_hit_test(scene, presentation, state, users, nullptr, kDefaultTheme,
                                                      command.rectangle.left + 1, command.rectangle.top + 40);
        compact_name_hit = name_hit.user_id == 9 && !content_hit.user_id && !below_hit.user_id &&
                           ui_rect_width(name_hit.anchor) > 0 &&
                           name_hit.anchor.right < command.rectangle.right;
        break;
    }
    expect(compact_name_hit, "message_profile_compact_inline_name_opens_profile_like_cozy_name");
    const bool cozy_restored = row && message_scene_set_display(&scene, {0, false}) &&
        message_scene_update(&scene, viewport).ok() &&
        message_text_presentation_build(&presentation, scene, state, &users, navigation, kDefaultTheme,
                                        1'700'000'100ull).ok();
    expect(cozy_restored, "message_profile_compact_fixture_restores_cozy_rows");
    UiWidgetHandle retained_rich_items[9]{};
    UiWidgetHandle retained_rich_parents[9]{};
    if (row) {
        retained_rich_items[0] =
            message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, 0);
        retained_rich_items[1] =
            message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, 1);
        retained_rich_items[2] = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::embed, 0);
        retained_rich_items[3] = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::embed, 1);
        retained_rich_items[4] = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::sticker, 0);
        retained_rich_items[5] = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::sticker, 1);
        retained_rich_items[6] =
            message_scene_rich_item_widget(*row, MessageSceneRichItemKind::component, 0);
        retained_rich_items[7] =
            message_scene_rich_item_widget(*row, MessageSceneRichItemKind::component, 1);
        retained_rich_items[8] = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::notice, 0);
        retained_rich_parents[0] = row->attachments;
        retained_rich_parents[1] = row->attachments;
        retained_rich_parents[2] = row->embeds;
        retained_rich_parents[3] = row->embeds;
        retained_rich_parents[4] = row->stickers;
        retained_rich_parents[5] = row->stickers;
        retained_rich_parents[6] = row->components;
        retained_rich_parents[7] = row->components;
        retained_rich_parents[8] = row->notice;
    }
    bool rich_item_hierarchy_owned = row && row->attachment_item_count == 2 && row->embed_item_count == 2 &&
                                     row->sticker_item_count == 2 && row->component_item_count == 2 &&
                                     row->notice_item_count == 1;
    for (u32 index = 0; index < 9 && rich_item_hierarchy_owned; ++index) {
        const UiWidget* item = ui_widget_manager_resolve(scene.widgets, retained_rich_items[index]);
        const UiLayoutNode* item_layout = message_scene_layout_node(scene, retained_rich_items[index]);
        rich_item_hierarchy_owned = item && item_layout &&
                                    ui_widget_handle_equal(
                                        ui_widget_manager_parent(scene.widgets, retained_rich_items[index]),
                                        retained_rich_parents[index]) &&
                                    message_scene_resolve(scene, retained_rich_items[index]) == row;
    }
    const UiLayoutNode* first_attachment_layout =
        row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::attachment, 0)
            : nullptr;
    const MessageSceneHit rich_item_hit =
        first_attachment_layout
            ? message_scene_hit_test(
                  scene,
                  (first_attachment_layout->rectangle.left + first_attachment_layout->rectangle.right) *
                      0.5f,
                  (first_attachment_layout->rectangle.top + first_attachment_layout->rectangle.bottom) *
                      0.5f)
            : MessageSceneHit{};
    const MessageTextCommand* content = nullptr;
    bool command_owners_valid = true;
    bool rich_commands_item_owned = true;
    bool command_clips_owned = row != nullptr;
    bool embed_entities_resolved = false;
    bool embed_role_semantic_retained = false;
    const UiLayoutNode* projected_row_layout =
        row ? message_scene_layout_node(scene, row->widget) : nullptr;
    const UiRect projected_row_clip =
        projected_row_layout ? ui_rect_intersection(scene.viewport, projected_row_layout->rectangle)
                             : UiRect{};
    for (u32 index = 0; index < presentation.command_count; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        command_owners_valid = command_owners_valid && command.owner.manager == &scene.widgets &&
                               ui_widget_manager_resolve(scene.widgets, command.owner.handle) != nullptr;
        command_clips_owned = command_clips_owned && ui_rect_has_area(command.clip) &&
                              command.clip.left >= projected_row_clip.left &&
                              command.clip.top >= projected_row_clip.top &&
                              command.clip.right <= projected_row_clip.right &&
                              command.clip.bottom <= projected_row_clip.bottom;
        if (command.measurement_kind == MessageTextMeasurementKind::embed) {
            const UiLayoutNode* owner_layout = message_scene_layout_node(scene, command.owner.handle);
            command_clips_owned = command_clips_owned && owner_layout &&
                                  command.clip.right <= owner_layout->content_rectangle.left + 520.0f;
            // <#7> is a channel chip: icon cell, margin and glue, then the name.
            if (utf8_equal(command.text, utf8_literal("@Moderators \xE2\x80\x83\xE2\x80\x85\xE2\x80\x8Dreply-channel "
                                                      "@Layout Author"))) {
                embed_entities_resolved = true;
                for (u32 semantic_index = 0; semantic_index < command.semantics.count; ++semantic_index) {
                    const MessageMarkdownSemantic& candidate = command.semantics[semantic_index];
                    embed_role_semantic_retained =
                        embed_role_semantic_retained ||
                        (candidate.kind == MessageMarkdownSemanticKind::role_mention &&
                         candidate.target == 777);
                }
            }
        }
        u32 first = 9;
        u32 end = 9;
        if (command.measurement_kind == MessageTextMeasurementKind::attachment) {
            first = 0;
            end = 2;
        } else if (command.measurement_kind == MessageTextMeasurementKind::embed) {
            first = 2;
            end = 4;
        } else if (command.measurement_kind == MessageTextMeasurementKind::component) {
            first = 6;
            end = 8;
        } else if (command.measurement_kind == MessageTextMeasurementKind::notice) {
            first = 8;
            end = 9;
        }
        if (first != 9 || command.measurement_kind == MessageTextMeasurementKind::notice) {
            bool exact_owner = false;
            for (u32 rich_index = first; rich_index < end; ++rich_index) {
                exact_owner =
                    exact_owner ||
                    (command.owner.handle.slot == retained_rich_items[rich_index].slot &&
                     command.owner.handle.generation == retained_rich_items[rich_index].generation);
            }
            rich_commands_item_owned = rich_commands_item_owned && exact_owner;
        }
        if (command.measurement_kind == MessageTextMeasurementKind::content) {
            content = &command;
        }
    }
    const bool content_owned = row && content && content->owner.handle.slot == row->content.slot &&
                               content->owner.handle.generation == row->content.generation &&
                               content->styles.count != 0 && content->semantics.count >= 2 &&
                               !utf8_equal(content->text, source);
    expect(model_ready && rich_ready && arranged.ok() && initialized && user_ready && reply_members_bound &&
               reply_member_lookup_ready && scene.count == 1,
           "message_text_projection_fixture_ready");
    expect(built.ok(), "message_text_projection_builds");
    expect(built.command_count >= 2 && built.measurement_count == 1,
           "message_text_projection_emits_commands_and_measurement");
    expect(command_owners_valid && content_owned,
           "message_text_projection_is_widget_owned_and_transformed");
    expect(command_clips_owned, "message_text_commands_are_row_and_embed_card_clipped");
    expect(embed_entities_resolved && embed_role_semantic_retained && state_roles[0].color == 0xC46BEFu,
           "message_embed_markdown_resolves_entities_and_retains_role_color_identity");
    expect(rich_item_hierarchy_owned && rich_commands_item_owned &&
               rich_item_hit.widget.slot == retained_rich_items[0].slot &&
               rich_item_hit.widget.generation == retained_rich_items[0].generation,
           "message_rich_items_own_hierarchy_text_and_hit_testing");
    // A sticker's tooltip and popout anchor to its 160 DIP square; the space
    // between and beside the squares misses. The stickers lie below this
    // viewport: lay the row out whole meanwhile.
    const float sticker_extent = kDefaultMessageStickerLayoutMetrics.extent;
    bool sticker_previews_hit = row && message_scene_update(&scene, {0.0f, 0.0f, 720.0f, 4000.0f}).ok();
    for (u32 index = 0; index < 2 && sticker_previews_hit; ++index) {
        const UiLayoutNode* node = message_scene_layout_node(scene, retained_rich_items[4 + index]);
        MessageStickerHit sticker{}, below{}, beside{};
        sticker_previews_hit =
            node && ui_rect_height(node->rectangle) == sticker_extent &&
            ui_rect_width(node->rectangle) > sticker_extent + 4 &&
            message_scene_sticker_hit(scene, node->rectangle.left + sticker_extent * .5f,
                                      node->rectangle.top + sticker_extent * .5f, &sticker) &&
            sticker.index == index && !sticker.snapshot && sticker.message_id == row->message_id &&
            ui_widget_handle_equal(sticker.widget, retained_rich_items[4 + index]) &&
            ui_rect_equal(sticker.preview, ui_rect_intersection({node->rectangle.left, node->rectangle.top,
                                                                 node->rectangle.left + sticker_extent,
                                                                 node->rectangle.top + sticker_extent},
                                                                node->clip_rectangle)) &&
            !message_scene_sticker_hit(scene, node->rectangle.left + 2, node->rectangle.bottom + 1, &below) &&
            !below.message_id &&
            !message_scene_sticker_hit(scene, node->rectangle.left + sticker_extent + 2,
                                       node->rectangle.top + sticker_extent * .5f, &beside) &&
            !beside.message_id;
    }
    // Discord's accessories: the squares stack 4 DIPs apart at the content's
    // start and the group ends in a 2 DIP padding.
    const UiLayoutNode* first_sticker = row ? message_scene_layout_node(scene, retained_rich_items[4]) : nullptr;
    const UiLayoutNode* second_sticker = row ? message_scene_layout_node(scene, retained_rich_items[5]) : nullptr;
    const UiLayoutNode* sticker_group = row ? message_scene_layout_node(scene, row->stickers) : nullptr;
    const UiLayoutNode* content_node = row ? message_scene_layout_node(scene, row->content) : nullptr;
    sticker_previews_hit = sticker_previews_hit && first_sticker && second_sticker && sticker_group && content_node &&
                           second_sticker->rectangle.top ==
                               first_sticker->rectangle.bottom + kDefaultMessageStickerLayoutMetrics.gap &&
                           second_sticker->rectangle.left == first_sticker->rectangle.left &&
                           first_sticker->rectangle.left == content_node->rectangle.left &&
                           ui_rect_height(sticker_group->rectangle) ==
                               kDefaultMessageStickerLayoutMetrics.group_height(2) &&
                           first_sticker->rectangle.top == sticker_group->rectangle.top;
    const UiLayoutNode* attachment_node =
        row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::attachment, 0) : nullptr;
    MessageStickerHit attachment_sticker{};
    sticker_previews_hit = sticker_previews_hit && attachment_node &&
                           !message_scene_sticker_hit(
                               scene, (attachment_node->rectangle.left + attachment_node->rectangle.right) * .5f,
                               (attachment_node->rectangle.top + attachment_node->rectangle.bottom) * .5f,
                               &attachment_sticker);
    expect(sticker_previews_hit && message_scene_update(&scene, viewport).ok(),
           "message_sticker_hit_is_its_160_dip_square_stacked_4_dips_apart_not_the_gaps_or_other_items");
    const auto* downloadable = ui_widget_manager_resolve(scene.widgets, retained_rich_items[0]);
    const auto* unavailable = ui_widget_manager_resolve(scene.widgets, retained_rich_items[1]);
    expect(downloadable && unavailable && (downloadable->flags & ui_widget_focusable) &&
        !(unavailable->flags & (ui_widget_focusable | ui_widget_hit_test)),
        "attachment_interaction_requires_download_url");

    struct CoordinatorFixture {
        u32 measure_attempts;
        u32 finalizations;
        bool fail_measure;
        bool fail_finalize;
    } coordinator_fixture{};
    const auto coordinator_measure =
        +[](void* opaque, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept -> bool {
        auto* fixture = static_cast<CoordinatorFixture*>(opaque);
        if (!fixture || !value || request.constraint.width <= 0.0f || request.constraint.height <= 0.0f) {
            return false;
        }
        ++fixture->measure_attempts;
        if (fixture->fail_measure) {
            return false;
        }
        const u32 columns = maximum<u32>(1, static_cast<u32>(request.constraint.width / 8.0f));
        const u32 lines = request.text.bytes == 0 ? 0 : (request.text.bytes + columns - 1u) / columns;
        const float height = static_cast<float>(lines) * 18.0f;
        *value = UiTextMeasureValue{
            UiSize{minimum(request.constraint.width, static_cast<float>(request.text.bytes) * 8.0f),
                   height},
            lines};
        return true;
    };
    const auto coordinator_finalize = +[](void* opaque, const Theme&) noexcept -> bool {
        auto* fixture = static_cast<CoordinatorFixture*>(opaque);
        if (!fixture) {
            return false;
        }
        ++fixture->finalizations;
        return !fixture->fail_finalize;
    };
    UiTextMeasureCache coordinator_cache{};
    const bool coordinator_cache_ready =
        ui_text_measure_cache_initialize(&coordinator_cache,
                                         Slice<UiTextMeasureRecord>{g_message_text_coordinator_records,
                                                                    kMaximumMessageTextMeasureCacheRecords},
                                         coordinator_measure, &coordinator_fixture);
    const MessageTextCoordinatorRequest coordinator_request{
        &scene,         &presentation,        &coordinator_cache,   &state,           &users, &navigation,
        &kDefaultTheme, coordinator_finalize, &coordinator_fixture, 1'700'000'100ull, 1};
    const MessageTextCoordinatorResult coordinated =
        coordinator_cache_ready ? message_text_coordinator_converge(coordinator_request)
                                : MessageTextCoordinatorResult{};
    const u32 first_measure_attempts = coordinator_fixture.measure_attempts;
    const MessageTextCoordinatorResult cached_coordinated =
        coordinated.ok() ? message_text_coordinator_converge(coordinator_request)
                         : MessageTextCoordinatorResult{};
    expect(coordinator_cache_ready && coordinated.ok() && coordinated.finalized &&
               coordinated.passes >= 1 && coordinated.passes <= kMaximumMessageTextConvergencePasses &&
               coordinator_fixture.finalizations == 2 && first_measure_attempts != 0,
           "message_text_coordinator_finalizes_only_stable_projection");
    const auto* compact_embed_row = message_scene_find_row(scene, 0);
    expect(coordinated.ok() && compact_embed_row && compact_embed_row->rich_item_layout.embed_widths[1] > 100 &&
        compact_embed_row->rich_item_layout.embed_widths[1] < 300,
        "compact_embed_width_is_measured_from_content");
    expect(cached_coordinated.ok() && cached_coordinated.finalized && cached_coordinated.passes == 1 &&
               coordinator_fixture.measure_attempts == first_measure_attempts,
           "message_text_coordinator_cached_steady_state_is_one_pass");

    MessageTextCoordinatorRequest invalid_coordinator = coordinator_request;
    invalid_coordinator.scene = nullptr;
    const u32 finalizations_before_invalid = coordinator_fixture.finalizations;
    const MessageTextCoordinatorResult invalid_coordinated =
        message_text_coordinator_converge(invalid_coordinator);
    expect(invalid_coordinated.error == MessageTextCoordinatorError::invalid_input &&
               !invalid_coordinated.finalized &&
               coordinator_fixture.finalizations == finalizations_before_invalid,
           "message_text_coordinator_rejects_invalid_request_before_finalization");

    ui_text_measure_cache_invalidate(&coordinator_cache);
    coordinator_fixture.fail_measure = true;
    const u32 finalizations_before_measure_failure = coordinator_fixture.finalizations;
    const MessageTextCoordinatorResult measure_failed =
        message_text_coordinator_converge(coordinator_request);
    expect(measure_failed.error == MessageTextCoordinatorError::measurement && !measure_failed.finalized &&
               coordinator_fixture.finalizations == finalizations_before_measure_failure,
           "message_text_coordinator_measure_failure_prevents_finalization");

    coordinator_fixture.fail_measure = false;
    ui_text_measure_cache_invalidate(&coordinator_cache);
    const MessageTextCoordinatorResult recovered = message_text_coordinator_converge(coordinator_request);
    coordinator_fixture.fail_finalize = true;
    const u32 finalizations_before_finalize_failure = coordinator_fixture.finalizations;
    const MessageTextCoordinatorResult finalize_failed =
        recovered.ok() ? message_text_coordinator_converge(coordinator_request)
                       : MessageTextCoordinatorResult{};
    expect(recovered.ok() && finalize_failed.error == MessageTextCoordinatorError::finalization &&
               !finalize_failed.finalized &&
               coordinator_fixture.finalizations == finalizations_before_finalize_failure + 1,
           "message_text_coordinator_classifies_finalization_failure");
    coordinator_fixture.fail_finalize = false;
    const MessageTextCoordinatorResult coordinator_restored =
        message_text_coordinator_converge(coordinator_request);
    expect(coordinator_restored.ok() && coordinator_restored.finalized,
           "message_text_coordinator_recovers_after_backend_failures");

    u32 reply_count = 0;
    bool reply_is_single_line_summary = false;
    bool reply_author_has_role_style = false;
    bool reply_author_has_expected_color = false;
    bool reply_has_normal_contrast = false;
    bool author_header_has_timestamp = false;
    for (u32 index = 0; index < presentation.command_count; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.measurement_kind == MessageTextMeasurementKind::reply) {
            ++reply_count;
            reply_has_normal_contrast = command.color.red == kDefaultTheme.text_primary.red &&
                command.color.green == kDefaultTheme.text_primary.green &&
                command.color.blue == kDefaultTheme.text_primary.blue &&
                command.color.alpha == kDefaultTheme.text_primary.alpha;
            reply_is_single_line_summary =
                reply_is_single_line_summary ||
                (utf8_equal(command.text,
                            utf8_literal("Reply Author A reply to @Layout Author in \xE2\x80\x83\xE2\x80\x85\xE2\x80\x8D"
                                         "reply-channel whose measured width must be ellipsized")) &&
                 ui_rect_height(command.rectangle) == message_scene_reply_line_height(scene) &&
                 command.wrapping == TextWrapping::no_wrap &&
                 command.overflow == TextOverflow::ellipsis && command.styles.count >= 3 &&
                 // The quoted <#7> chip keeps only its icon cell, never a hit target.
                 command.semantics.count == 1 &&
                 command.semantics[0].kind == MessageMarkdownSemanticKind::chip_icon);
            for (u32 style = 0; style < command.styles.count; ++style) {
                reply_author_has_role_style =
                    reply_author_has_role_style ||
                    (command.styles[style].flags & text_style_inline_role_color) != 0;
            }
            reply_author_has_expected_color =
                command.inline_role_color.red > 0.0f &&
                command.inline_role_color.green > command.inline_role_color.red &&
                command.inline_role_color.blue > command.inline_role_color.green &&
                command.inline_role_color.alpha == kMessageReplyAuthorOpacity;
        }
        if (row && command.measurement_kind == MessageTextMeasurementKind::none &&
            command.owner.handle.slot == row->author.slot &&
            command.owner.handle.generation == row->author.generation) {
            bool bold = false;
            bool timestamp = false;
            for (u32 style = 0; style < command.styles.count; ++style) {
                bold = bold || (command.styles[style].flags & text_style_bold) != 0;
                timestamp = timestamp || (command.styles[style].flags & text_style_subtext) != 0;
            }
            author_header_has_timestamp = bold && timestamp;
        }
    }
    expect(reply_count == 1 && reply_is_single_line_summary && reply_author_has_role_style &&
               reply_author_has_expected_color && reply_has_normal_contrast,
           "message_text_reply_reference_is_one_rich_ellipsized_summary_line");
    expect(author_header_has_timestamp, "message_text_group_header_includes_muted_timestamp");
    // Role colours are server data: the reply author keeps #123456 exactly
    // (muted only in opacity) under flat dark, flat light and both gradient theme types.
    Theme dark_gradient_theme = kDefaultTheme;
    dark_gradient_theme.background = background_theme_defaults();
    Theme light_gradient_theme = kLightTheme;
    light_gradient_theme.background = background_theme_defaults();
    const Theme* role_themes[]{&kDefaultTheme, &kLightTheme, &dark_gradient_theme, &light_gradient_theme};
    bool role_colors_exact = built.ok();
    for (const Theme* role_theme : role_themes) {
        bool found = false;
        if (role_colors_exact &&
            message_text_presentation_build(&presentation, scene, state, &users, navigation, *role_theme,
                                            1'700'000'100ull)
                .ok())
            for (u32 index = 0; index < presentation.command_count; ++index) {
                const auto& command = presentation.commands[index];
                if (command.measurement_kind != MessageTextMeasurementKind::reply)
                    continue;
                const auto near_byte = [](float value, u32 byte) noexcept {
                    const float delta = value - static_cast<float>(byte) / 255.0f;
                    return delta < 1e-5f && delta > -1e-5f;
                };
                found = near_byte(command.inline_role_color.red, 0x12) && near_byte(command.inline_role_color.green, 0x34) &&
                        near_byte(command.inline_role_color.blue, 0x56) &&
                        command.inline_role_color.alpha == kMessageReplyAuthorOpacity;
            }
        role_colors_exact = role_colors_exact && found;
    }
    expect(role_colors_exact && message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                                                kDefaultTheme, 1'700'000'100ull)
                                    .ok(),
           "message_text_role_colors_pass_through_every_theme_type_unchanged");

    const u64 saved_reply_scroll = timeline.scroll_units;
    const bool saved_reply_follow_bottom = timeline.follow_bottom;
    bool reply_scroll_changed = false;
    expect(message_scene_scroll_to_start(&scene, &reply_scroll_changed), "message_reply_hit_fixture_scrolls_to_preview");
    expect(message_scene_update(&scene, scene.viewport).ok(), "message_reply_hit_fixture_layouts_preview");
    const auto* reply_row = message_scene_find_row(scene, 0);
    const auto* reply_layout = reply_row ? message_scene_layout_node(scene, reply_row->reply) : nullptr;
    const float reply_x = reply_layout ? reply_layout->rectangle.left + 24.0f : 0;
    const float reply_y = reply_layout ? reply_layout->rectangle.top + 8.0f : 0;
    const auto reply_target = message_scene_reply_hit_test(scene, reply_x, reply_y);
    expect(reply_layout && reply_target.channel_id == 7 && reply_target.message_id == 90'999 &&
        !message_store_find(store, reply_target.message_id).ok(),
        "message_reply_hit_retains_unloaded_navigation_target");
    const UiRect saved_reply_viewport = scene.viewport;
    scene.viewport.top = reply_layout ? reply_layout->rectangle.bottom : scene.viewport.bottom;
    expect(message_scene_reply_hit_test(scene, reply_x, reply_y).message_id == 0,
        "message_reply_hit_rejects_virtualized_clipped_preview");
    scene.viewport = saved_reply_viewport;
    // Isolate hit testing from store revisions and retained-widget invalidation.
    auto* reply_record = const_cast<MessageRichRecord*>(message_rich_store_find(rich_store, 91'000));
    const auto saved_reply_reference = reply_record->reference;
    reply_record->reference.state = GatewayMessageReferenceState::unknown;
    reply_record->reference.channel_id = 0;
    const auto unknown_target = message_scene_reply_hit_test(scene, reply_x, reply_y);
    expect(unknown_target.message_id == 90'999 && unknown_target.channel_id == 7,
        "message_reply_unknown_reference_uses_id_and_owning_channel");
    reply_record->reference.state = GatewayMessageReferenceState::deleted;
    expect(message_scene_reply_hit_test(scene, reply_x, reply_y).message_id == 0,
        "message_reply_deleted_reference_is_not_navigable");
    reply_record->reference.state = GatewayMessageReferenceState::none;
    expect(message_scene_reply_hit_test(scene, reply_x, reply_y).message_id == 0,
        "message_reply_non_reference_header_is_not_navigable");
    reply_record->reference = saved_reply_reference;
    expect(message_timeline_set_scroll(&timeline, saved_reply_scroll, saved_reply_follow_bottom) &&
        message_scene_update(&scene, scene.viewport).ok(),
        "message_reply_hit_fixture_restored");

    GatewayMessageEvent emoji_reply_event{};
    emoji_reply_event.id = 91'000;
    emoji_reply_event.fields =
        gateway_message_field_message_reference | gateway_message_field_referenced_message;
    emoji_reply_event.reference = rich_event.reference;
    emoji_reply_event.reference.content = utf8_literal("\xF0\x9F\x98\x80");
    const bool emoji_reply_stored =
        message_rich_store_apply(&rich_store, 91'000, source_message->revision, emoji_reply_event).ok();
    const MessageTextPresentationResult emoji_reply_built =
        emoji_reply_stored ? message_text_presentation_build(&presentation, scene, state, &users,
                                                             navigation, kDefaultTheme, 1'700'000'100ull)
                           : MessageTextPresentationResult{};
    bool compact_reply_emoji_found = false;
    bool compact_reply_emoji_is_not_jumbo = true;
    TextSystem reply_text_system{};
    const bool reply_text_ready = text_system_initialize(&reply_text_system, L"Segoe UI", 14).ok();
    bool compact_reply_measures = reply_text_ready;
    for (u32 index = 0; emoji_reply_built.ok() && index < presentation.command_count; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.measurement_kind != MessageTextMeasurementKind::reply) {
            continue;
        }
        compact_reply_emoji_found =
            utf8_equal(command.text, utf8_literal("Reply Author \xF0\x9F\x98\x80"));
        compact_reply_measures = compact_reply_measures && text_measure_utf8(
            &reply_text_system, {command.text, command.styles, 400, 40,
            TextWrapping::no_wrap}).ok();
        for (u32 style = 0; style < command.styles.count; ++style) {
            compact_reply_emoji_is_not_jumbo = compact_reply_emoji_is_not_jumbo &&
                                               (command.styles[style].flags & text_style_jumbo_emoji) == 0;
        }
    }
    expect(emoji_reply_built.ok() && compact_reply_emoji_found && compact_reply_emoji_is_not_jumbo,
           "message_text_reply_preview_masks_jumbo_emoji_style");
    expect(compact_reply_emoji_found && compact_reply_measures, "message_text_emoji_reply_measures_with_native_backend");
    text_system_release(&reply_text_system);

    // Server nicknames name message authors and reply authors.
    const auto nickname_leads = [&](u64 user_id, MessageTextMeasurementKind kind) noexcept {
        reply_member_records[0].user_id = user_id;
        copy_bytes(reply_member_records[0].nickname, "Mod Nick", 8);
        reply_member_records[0].nickname_bytes = 8;
        ++reply_members.revision;
        bool found = false;
        const bool rebuilt = message_text_presentation_build(&presentation, scene, state, &users,
                                                             navigation, kDefaultTheme, 1'700'000'100ull)
                                 .ok();
        for (u32 index = 0; rebuilt && row && index < presentation.command_count; ++index) {
            const MessageTextCommand& command = presentation.commands[index];
            const bool author = kind == MessageTextMeasurementKind::none &&
                                command.owner.handle.slot == row->author.slot && command.styles.count;
            const bool measured = kind != MessageTextMeasurementKind::none && command.measurement_kind == kind;
            if ((author || measured) &&
                command.text.bytes >= 8 &&
                utf8_equal(Utf8View{command.text.data, 8}, utf8_literal("Mod Nick")))
                found = true;
        }
        reply_member_records[0].user_id = 8;
        reply_member_records[0].nickname_bytes = 0;
        ++reply_members.revision;
        return found;
    };
    expect(nickname_leads(9, MessageTextMeasurementKind::none) &&
               nickname_leads(8, MessageTextMeasurementKind::reply),
           "message_text_author_and_reply_headers_use_server_nicknames");

    // "Show time on reply previews": the quoted message's sent time follows its author in a subtext span.
    const auto reply_line = [&](bool timestamps, Utf8View* text, bool* time_span) noexcept {
        message_scene_set_display(&scene, {0, false, MessageSceneClock::meridiem, timestamps, true});
        *time_span = false;
        *text = {};
        if (!message_scene_update(&scene, scene.viewport).ok() ||
            !message_text_presentation_build(&presentation, scene, state, &users, navigation, kDefaultTheme,
                                             1'700'000'100ull)
                 .ok())
            return false;
        for (u32 index = 0; index < presentation.command_count; ++index) {
            const MessageTextCommand& command = presentation.commands[index];
            if (command.measurement_kind != MessageTextMeasurementKind::reply)
                continue;
            *text = command.text;
            for (u32 style = 0; style < command.styles.count; ++style)
                *time_span = *time_span || (command.styles[style].flags & text_style_subtext) != 0;
        }
        return text->bytes != 0;
    };
    Utf8View plain_reply{}, timed_reply{};
    bool plain_time = true, timed_time = false;
    const bool reply_lines = reply_line(false, &plain_reply, &plain_time);
    const u32 plain_bytes = plain_reply.bytes;
    // The text views alias the presentation's buffer: compare before the next build.
    const bool plain_exact = utf8_equal(plain_reply, utf8_literal("Reply Author \xF0\x9F\x98\x80"));
    const bool timed_lines = reply_line(true, &timed_reply, &timed_time);
    expect(reply_lines && timed_lines && plain_exact && !plain_time && timed_time && timed_reply.bytes > plain_bytes + 1 &&
               utf8_equal(Utf8View{timed_reply.data, 13}, utf8_literal("Reply Author ")) &&
               utf8_equal(Utf8View{timed_reply.data + timed_reply.bytes - 5, 5}, utf8_literal(" \xF0\x9F\x98\x80")),
           "message_text_reply_preview_shows_the_quoted_time_only_when_enabled");
    expect(reply_line(false, &plain_reply, &plain_time) && utf8_equal(plain_reply, utf8_literal("Reply Author \xF0\x9F\x98\x80")) &&
               !plain_time,
           "message_text_reply_preview_time_turns_off_again");

    u32 embed_command_index = ~0u;
    for (u32 index = 0; index < presentation.command_count; ++index) {
        if (embed_command_index == ~0u && presentation.commands[index].measurement_kind == MessageTextMeasurementKind::embed) {
            embed_command_index = index;
        }
    }
    if (embed_command_index != ~0u) {
        presentation.commands[embed_command_index].measured_height = 96.0f;
    }
    const MessageTextPresentationResult rich_flowed =
        embed_command_index != ~0u
            ? message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                              kDefaultTheme, 1'700'000'100ull)
            : MessageTextPresentationResult{};
    bool rich_measurement_changed = false;
    const bool rich_measurement_applied =
        rich_flowed.ok() &&
        message_text_presentation_apply_measurements(&scene, presentation, &rich_measurement_changed);
    const MessageSceneResult rich_reflowed =
        rich_measurement_applied ? message_scene_update(&scene, viewport) : MessageSceneResult{};
    const MessageSceneRowWidget* rich_row = rich_reflowed.ok() ? message_scene_find_row(scene, 0) : nullptr;
    const UiLayoutNode* embed_layout =
        rich_row ? message_scene_layout_node(scene, rich_row->embeds) : nullptr;
    const UiLayoutNode* component_layout =
        rich_row ? message_scene_layout_node(scene, rich_row->components) : nullptr;
    const UiLayoutNode* rich_attachment_items[2]{
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::attachment, 0)
            : nullptr,
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::attachment, 1)
            : nullptr};
    const UiLayoutNode* rich_embed_items[2]{
        rich_row ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::embed, 0)
                 : nullptr,
        rich_row ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::embed, 1)
                 : nullptr};
    const UiLayoutNode* rich_sticker_items[2]{
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::sticker, 0)
            : nullptr,
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::sticker, 1)
            : nullptr};
    const UiLayoutNode* rich_component_items[2]{
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::component, 0)
            : nullptr,
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::component, 1)
            : nullptr};
    const UiLayoutNode* rich_notice_item =
        rich_row
            ? message_scene_rich_item_layout_node(scene, *rich_row, MessageSceneRichItemKind::notice, 0)
            : nullptr;
    const MessageTextPresentationResult rich_settled =
        rich_reflowed.ok() ? message_text_presentation_build(&presentation, scene, state, &users,
                                                             navigation, kDefaultTheme, 1'700'000'100ull)
                           : MessageTextPresentationResult{};
    PaintStream sticker_preview_paint{};
    MessageMediaCommand sticker_preview_commands[16]{};
    MessageMediaCommandStream sticker_preview_stream{};
    MessagePresentationResult sticker_preview_result{};
    const bool sticker_preview_built = rich_settled.ok() &&
        paint_stream_reset(&sticker_preview_paint, viewport.right, viewport.bottom) &&
        message_media_command_stream_initialize(&sticker_preview_stream, {sticker_preview_commands, 16}) &&
        message_presentation_build(scene, kDefaultTheme, &sticker_preview_paint, &sticker_preview_stream,
                                  &sticker_preview_result);
    // Each sticker draws its 160 DIP square at its item's origin; no caption
    // text, and the measured item is the square.
    u32 sticker_squares = 0;
    bool sticker_squares_placed = sticker_preview_built;
    for (u32 media_index = 0; media_index < sticker_preview_stream.count; ++media_index) {
        const auto& preview = sticker_preview_commands[media_index];
        if (preview.kind != MessageMediaCommandKind::sticker)
            continue;
        const UiLayoutNode* item = rich_sticker_items[sticker_squares < 2 ? sticker_squares : 1];
        sticker_squares_placed = sticker_squares_placed && item &&
                                 ui_widget_handle_equal(preview.owner.handle, rich_row->rich_items[kMessageRichStickerItemOffset + sticker_squares]) &&
                                 ui_rect_equal(preview.rectangle, {item->rectangle.left, item->rectangle.top,
                                                                   item->rectangle.left + kDefaultMessageStickerLayoutMetrics.extent,
                                                                   item->rectangle.top + kDefaultMessageStickerLayoutMetrics.extent});
        ++sticker_squares;
    }
    for (u32 index = 0; rich_row && index < presentation.command_count; ++index)
        for (u32 sticker = 0; sticker < 2; ++sticker)
            sticker_squares_placed =
                sticker_squares_placed &&
                !ui_widget_handle_equal(presentation.commands[index].owner.handle,
                                        rich_row->rich_items[kMessageRichStickerItemOffset + sticker]);
    expect(sticker_squares_placed && sticker_squares == 2 && rich_sticker_items[0] && rich_sticker_items[1] &&
               ui_rect_height(rich_sticker_items[0]->rectangle) == kDefaultMessageStickerLayoutMetrics.extent &&
               ui_rect_height(rich_sticker_items[1]->rectangle) == kDefaultMessageStickerLayoutMetrics.extent,
           "message_stickers_draw_160_dip_squares_at_their_items_without_a_caption");
    paint_stream_release(&sticker_preview_paint);
    UiLayoutDiagnostic rich_diagnostics[32]{};
    const UiLayoutValidationResult rich_validation =
        rich_reflowed.ok() ? ui_layout_engine_validate(scene.widgets.layout,
                                                       Slice<UiLayoutDiagnostic>{rich_diagnostics, 32})
                           : UiLayoutValidationResult{};
    expect(rich_flowed.ok() && rich_measurement_applied && rich_measurement_changed && rich_reflowed.ok() &&
               rich_row && embed_layout && component_layout && rich_attachment_items[0] &&
               rich_attachment_items[1] && rich_embed_items[0] && rich_embed_items[1] &&
               rich_sticker_items[0] && rich_sticker_items[1] && rich_component_items[0] &&
               rich_component_items[1] && rich_notice_item &&
               rich_attachment_items[0]->rectangle.bottom <= rich_attachment_items[1]->rectangle.top &&
               rich_embed_items[0]->rectangle.bottom <= rich_embed_items[1]->rectangle.top &&
               rich_sticker_items[0]->rectangle.bottom <= rich_sticker_items[1]->rectangle.top &&
               rich_component_items[0]->rectangle.bottom <= rich_component_items[1]->rectangle.top &&
               ui_rect_height(rich_embed_items[0]->rectangle) ==
                   rich_row->rich_item_layout.heights[kMessageRichEmbedItemOffset] &&
               embed_layout->rectangle.bottom <= component_layout->rectangle.top && rich_settled.ok() &&
               presentation.rich.flow_overflow_regions == 0 && rich_validation.clean(),
           "message_text_rich_item_flow_reflows_every_retained_sibling_without_overlap");

    UiWidgetHandle retained_reaction_buttons[5]{};
    if (rich_row && rich_row->reaction_button_count == 5) {
        for (u32 index = 0; index < 5; ++index) {
            retained_reaction_buttons[index] = rich_row->reaction_buttons[index];
        }
    }
    const UiRect narrow_viewport{0.0f, 0.0f, 280.0f, 320.0f};
    const MessageSceneResult reaction_narrowed = message_scene_update(&scene, narrow_viewport);
    const MessageSceneRowWidget* narrow_row =
        reaction_narrowed.ok() ? message_scene_find_row(scene, 0) : nullptr;
    const MessageTextPresentationResult reaction_projected =
        narrow_row ? message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                                     kDefaultTheme, 1'700'000'100ull)
                   : MessageTextPresentationResult{};
    const UiLayoutNode* reaction_region =
        narrow_row ? message_scene_layout_node(scene, narrow_row->reactions) : nullptr;
    const UiLayoutNode* reaction_nodes[5]{};
    MessageReactionVisualItem reaction_visuals[5]{};
    UiLayoutContentBounds reaction_content[10]{};
    const MessageRichRecord* projected_rich = message_rich_store_find(rich_store, 91'000);
    u32 reaction_content_count = 0;
    bool reaction_identity_retained = narrow_row && narrow_row->reaction_button_count == 5;
    bool rich_item_identity_retained =
        narrow_row && narrow_row->attachment_item_count == 2 && narrow_row->embed_item_count == 2 &&
        narrow_row->sticker_item_count == 2 && narrow_row->component_item_count == 2 &&
        narrow_row->notice_item_count == 1;
    const MessageSceneRichItemKind retained_rich_kinds[9]{
        MessageSceneRichItemKind::attachment, MessageSceneRichItemKind::attachment,
        MessageSceneRichItemKind::embed,      MessageSceneRichItemKind::embed,
        MessageSceneRichItemKind::sticker,    MessageSceneRichItemKind::sticker,
        MessageSceneRichItemKind::component,  MessageSceneRichItemKind::component,
        MessageSceneRichItemKind::notice};
    const u32 retained_rich_indices[9]{0, 1, 0, 1, 0, 1, 0, 1, 0};
    for (u32 index = 0; index < 9 && rich_item_identity_retained; ++index) {
        const UiWidgetHandle current = message_scene_rich_item_widget(
            *narrow_row, retained_rich_kinds[index], retained_rich_indices[index]);
        rich_item_identity_retained = current.slot == retained_rich_items[index].slot &&
                                      current.generation == retained_rich_items[index].generation;
    }
    bool reaction_commands_owned = reaction_projected.ok();
    bool reaction_content_optically_lifted = reaction_projected.ok();
    for (u32 index = 0; index < 5 && narrow_row; ++index) {
        reaction_nodes[index] = message_scene_layout_node(scene, narrow_row->reaction_buttons[index]);
        reaction_identity_retained =
            reaction_identity_retained &&
            narrow_row->reaction_buttons[index].slot == retained_reaction_buttons[index].slot &&
            narrow_row->reaction_buttons[index].generation == retained_reaction_buttons[index].generation;
        const MessageTextCommand* reaction_command = nullptr;
        for (u32 command_index = 0; command_index < presentation.command_count; ++command_index) {
            const MessageTextCommand& candidate = presentation.commands[command_index];
            if (candidate.owner.handle.slot == narrow_row->reaction_buttons[index].slot &&
                candidate.owner.handle.generation == narrow_row->reaction_buttons[index].generation) {
                reaction_command = &candidate;
                break;
            }
        }
        const UiWidget* reaction_widget =
            ui_widget_manager_resolve(scene.widgets, narrow_row->reaction_buttons[index]);
        const bool visual_ready =
            projected_rich && message_reaction_presentation_resolve(scene, *narrow_row, *projected_rich,
                                                                    index, &reaction_visuals[index]);
        reaction_commands_owned = reaction_commands_owned && reaction_nodes[index] && reaction_command &&
                                  reaction_widget && visual_ready;
        reaction_commands_owned =
            reaction_commands_owned && reaction_command &&
            reaction_command->horizontal_alignment == UiTextAlignment::start &&
            reaction_command->vertical_alignment == UiTextAlignment::center &&
            reaction_command->rectangle.top >= reaction_visuals[index].control_rectangle.top &&
            reaction_command->rectangle.bottom <= reaction_visuals[index].control_rectangle.bottom;
        if (reaction_command && visual_ready) {
            const float control_center = (reaction_visuals[index].control_rectangle.top +
                                          reaction_visuals[index].control_rectangle.bottom) *
                                         0.5f;
            const float text_center =
                (reaction_command->rectangle.top + reaction_command->rectangle.bottom) * 0.5f;
            reaction_content_optically_lifted =
                reaction_content_optically_lifted && text_center == control_center - 1.0f;
            if (reaction_visuals[index].emoji_id != 0) {
                const float emoji_center = (reaction_visuals[index].emoji_rectangle.top +
                                            reaction_visuals[index].emoji_rectangle.bottom) *
                                           0.5f;
                reaction_content_optically_lifted =
                    reaction_content_optically_lifted && emoji_center == control_center - 1.0f;
            }
        }
        if (reaction_command && reaction_widget) {
            reaction_content[reaction_content_count++] =
                UiLayoutContentBounds{
                    ui_widget_manager_layout_handle(scene.widgets, narrow_row->reaction_buttons[index]),
                    reaction_command->rectangle,
                                      UiLayoutContentKind::text, ui_layout_content_bounds_collision};
            if (visual_ready && reaction_visuals[index].emoji_id != 0) {
                reaction_content[reaction_content_count++] =
                    UiLayoutContentBounds{
                        ui_widget_manager_layout_handle(scene.widgets, narrow_row->reaction_buttons[index]),
                        reaction_visuals[index].emoji_rectangle,
                                          UiLayoutContentKind::icon, ui_layout_content_bounds_collision};
            }
        }
    }
    const UiLayoutValidationResult reaction_structure_validation =
        reaction_narrowed.ok() ? ui_layout_engine_validate(scene.widgets.layout,
                                                           Slice<UiLayoutDiagnostic>{rich_diagnostics, 32})
                               : UiLayoutValidationResult{};
    const UiLayoutValidationResult reaction_content_validation =
        reaction_narrowed.ok()
            ? ui_layout_engine_validate_content_bounds(
                  scene.widgets.layout,
                  ConstSlice<UiLayoutContentBounds>{reaction_content, reaction_content_count},
                  Slice<UiLayoutDiagnostic>{rich_diagnostics, 32})
            : UiLayoutValidationResult{};
    const MessageSceneHit reaction_hit =
        reaction_nodes[0]
            ? message_scene_hit_test(
                  scene, (reaction_nodes[0]->rectangle.left + reaction_nodes[0]->rectangle.right) * 0.5f,
                  (reaction_nodes[0]->rectangle.top + reaction_nodes[0]->rectangle.bottom) * 0.5f)
            : MessageSceneHit{};
    u32 reaction_hit_index = ~0u;
    const bool reaction_hit_resolved =
        message_scene_reaction_index(scene, reaction_hit, &reaction_hit_index);
    char atomic_reaction_utf8[1]{'Q'};
    MessageReactionTextItem atomic_reaction_items[5]{};
    const MessageReactionPresentationResult atomic_reaction_failure =
        narrow_row && projected_rich
            ? message_reaction_presentation_build(scene, *narrow_row, rich_store, *projected_rich,
                                                  Slice<char>{atomic_reaction_utf8, 1},
                                                  Slice<MessageReactionTextItem>{atomic_reaction_items, 5})
            : MessageReactionPresentationResult{};
    expect(reaction_identity_retained && rich_item_identity_retained && reaction_commands_owned &&
               reaction_content_optically_lifted && reaction_content_count == 8 && reaction_region &&
               ui_rect_height(reaction_region->rectangle) == 52.0f && reaction_nodes[0] &&
               reaction_nodes[1] && reaction_nodes[2] && reaction_nodes[4] &&
               reaction_nodes[0]->rectangle.top == reaction_nodes[1]->rectangle.top &&
               reaction_nodes[2]->rectangle.top == reaction_nodes[0]->rectangle.top &&
               reaction_nodes[4]->rectangle.top == reaction_nodes[0]->rectangle.top + 28.0f &&
               ui_rect_width(reaction_nodes[0]->rectangle) < 80.0f &&
               reaction_visuals[0].emoji_rectangle.left ==
                   reaction_visuals[0].control_rectangle.left +
                       kDefaultMessageReactionLayoutMetrics.content_left_padding &&
               reaction_visuals[0].emoji_rectangle.right + kDefaultMessageReactionLayoutMetrics.emoji_gap ==
                   reaction_visuals[0].text_rectangle.left &&
               reaction_structure_validation.clean() && reaction_content_validation.clean() &&
               reaction_hit.widget.slot == narrow_row->reaction_buttons[0].slot && reaction_hit_resolved &&
               reaction_hit_index == 0,
           "message_retained_items_keep_identity_across_reaction_and_width_reflow");
    expect(atomic_reaction_failure.error == MessageReactionPresentationError::capacity &&
               atomic_reaction_failure.item_count == 0 && atomic_reaction_utf8[0] == 'Q' &&
               atomic_reaction_items[0].text.bytes == 0,
           "message_reaction_projection_capacity_failure_is_atomic");
    const UiRect transient_narrow_viewport{0.0f, 0.0f, 110.0f, 320.0f};
    const MessageSceneResult transient_narrowed = message_scene_update(&scene, transient_narrow_viewport);
    const MessageTextPresentationResult transient_narrow_projection =
        transient_narrowed.ok()
            ? message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                              kDefaultTheme, 1'700'000'100ull)
            : MessageTextPresentationResult{};
    expect(transient_narrowed.ok() && transient_narrow_projection.ok() && presentation.active &&
               presentation.last_error == MessageTextPresentationError::none,
           "message_text_transient_narrow_rich_projection_is_nonfatal");
    const MessageSceneResult reaction_restored = message_scene_update(&scene, viewport);
    const MessageTextPresentationResult reaction_restored_projection =
        reaction_restored.ok()
            ? message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                              kDefaultTheme, 1'700'000'100ull)
            : MessageTextPresentationResult{};
    const MessageMarkdownSemantic* link = nullptr;
    const MessageMarkdownSemantic* spoiler = nullptr;
    for (u32 index = 0; index < presentation.command_count; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.measurement_kind != MessageTextMeasurementKind::content) {
            continue;
        }
        for (u32 semantic = 0; semantic < command.semantics.count; ++semantic) {
            const MessageMarkdownSemantic& candidate = command.semantics[semantic];
            if (candidate.kind == MessageMarkdownSemanticKind::link) {
                link = &candidate;
            } else if (candidate.kind == MessageMarkdownSemanticKind::spoiler) {
                spoiler = &candidate;
            }
        }
    }
    expect(reaction_restored.ok() && reaction_restored_projection.ok(),
           "message_reaction_wrap_restores_wide_projection");

    GatewayMessageComponent application_components[2]{};
    application_components[0].type = 1;
    application_components[0].parent = 0xFFFFu;
    application_components[1].type = 2;
    application_components[1].parent = 0;
    application_components[1].style = 5;
    application_components[1].label = utf8_literal("Finish your submission");
    application_components[1].url = utf8_literal("https://example.com/finish");
    GatewayMessageEvent application_component_update{};
    application_component_update.id = 91'000;
    application_component_update.fields = gateway_message_field_components;
    application_component_update.components =
        ConstSlice<GatewayMessageComponent>{application_components, 2};
    const bool application_components_applied =
        source_message && message_rich_store_apply(&rich_store, 91'000, source_message->revision,
                                                   application_component_update)
                              .ok();
    const MessageSceneResult application_component_scene =
        application_components_applied ? message_scene_update(&scene, viewport) : MessageSceneResult{};
    const MessageTextPresentationResult application_component_projection =
        application_component_scene.ok()
            ? message_text_presentation_build(&presentation, scene, state, &users, navigation,
                                              kDefaultTheme, 1'700'000'100ull)
            : MessageTextPresentationResult{};
    expect(application_component_scene.ok() && application_component_projection.ok() &&
               presentation.active && presentation.rich.components == 1 &&
               presentation.rich.skipped_components == 1,
           "message_text_application_action_row_is_nonfatal");

    const MessageTextHit link_hit = link && row ? MessageTextHit{row->message_id,
                                                                 link->target,
                                                                 row->message_revision,
                                                                 link->source_offset,
                                                                 link->source_bytes,
                                                                 link->kind,
                                                                 true}
                                                : MessageTextHit{};
    const MessageTextHit spoiler_hit = spoiler && row ? MessageTextHit{row->message_id,
                                                                       spoiler->target,
                                                                       row->message_revision,
                                                                       spoiler->source_offset,
                                                                       spoiler->source_bytes,
                                                                       spoiler->kind,
                                                                       true}
                                                      : MessageTextHit{};
    bool hover_changed = false;
    bool reveal_changed = false;
    const Utf8View link_source = message_text_presentation_hit_source(store, nullptr, link_hit);
    const bool interactions_ready =
        utf8_equal(link_source, utf8_literal("https://discord.com")) &&
        message_text_presentation_set_hover(&presentation, link_hit, &hover_changed) &&
        message_text_presentation_toggle_spoiler(&presentation, store, nullptr, spoiler_hit, &reveal_changed);
    expect(link && spoiler, "message_text_projection_emits_semantics");
    expect(interactions_ready, "message_text_interactions_are_source_mapped");
    expect(hover_changed && reveal_changed && presentation.has_hover &&
               presentation.spoiler_reveal_count == 1,
           "message_text_interactions_are_ui_owned");

    if (presentation.measurement_count == 1) {
        presentation.measurements[0].content_height = 120.0f;
        presentation.measurements[0].content_present = true;
    }
    bool measurement_changed = false;
    const bool measurement_applied =
        message_text_presentation_apply_measurements(&scene, presentation, &measurement_changed);
    const MessageSceneResult reflowed =
        measurement_applied ? message_scene_update(&scene, viewport) : MessageSceneResult{};
    expect(measurement_applied && measurement_changed && reflowed.ok() &&
               ui_variable_list_item_units(list, 0) > default_units,
           "message_text_measurement_reflows_layout_engine");

    MessageTextPresentation tiny{};
    const bool tiny_initialized = initialize_presentation(&tiny, g_tiny_message_text_commands, 1);
    const MessageTextPresentationResult tiny_result =
        tiny_initialized ? message_text_presentation_build(&tiny, scene, state, &users, navigation,
                                                           kDefaultTheme, 1'700'000'100ull)
                         : MessageTextPresentationResult{};
    expect(tiny_initialized, "message_text_tiny_fixture_ready");
    expect(tiny_result.error == MessageTextPresentationError::capacity && tiny.command_count == 0 &&
               !tiny.active,
           "message_text_capacity_failure_is_atomic");

    const UiWidgetHandle saved_content = scene.rows[0].content;
    scene.rows[0].content = ui_widget_invalid_handle();
    const MessageTextPresentationResult layout_failure = message_text_presentation_build(
        &tiny, scene, state, &users, navigation, kDefaultTheme, 1'700'000'100ull);
    scene.rows[0].content = saved_content;
    expect(layout_failure.error == MessageTextPresentationError::layout && tiny.command_count == 0 &&
               !tiny.active && tiny.failure_message_id == 91'000 &&
               tiny.failure_source_index == 0 && tiny.failure_stage == MessageTextPresentationStage::row &&
               tiny.failure_operation == MessageTextPresentationOperation::resolve_layout,
           "message_text_layout_failure_is_atomic_and_classified");

    GatewayMessageEvent structural_rich_update{};
    structural_rich_update.id = 91'000;
    structural_rich_update.fields = gateway_message_field_components;
    structural_rich_update.components = ConstSlice<GatewayMessageComponent>{rich_components, 1};
    structural_rich_update.rich_truncations = gateway_message_rich_truncated_text;
    const bool structural_rich_applied =
        source_message &&
        message_rich_store_apply(&rich_store, 91'000, source_message->revision, structural_rich_update)
            .ok();
    const MessageSceneResult structural_rich_reflow =
        structural_rich_applied ? message_scene_update(&scene, viewport) : MessageSceneResult{};
    const MessageSceneRowWidget* structural_rich_row =
        structural_rich_reflow.ok() ? message_scene_find_row(scene, 0) : nullptr;
    expect(structural_rich_reflow.ok() && structural_rich_row &&
               structural_rich_row->component_item_count == 1 &&
               !ui_widget_manager_resolve(scene.widgets, retained_rich_items[7]) &&
               message_scene_rich_item_widget(*structural_rich_row, MessageSceneRichItemKind::component, 1)
                       .slot == kInvalidUiWidgetSlot,
           "message_rich_item_count_change_rematerializes_structure_and_expires_handles");

    const bool overscan_second_inserted =
        message_store_upsert(
            &store, MessageInput{92'000, 7, 9, 1'700'000'001'000ull, 0, utf8_literal("visible row"), 0})
            .ok() &&
        ui_variable_list_insert(&list, 1, 1, default_units);
    const u64 first_row_units = ui_variable_list_item_units(list, 0);
    const UiRect overscan_viewport{0.0f, 0.0f, 720.0f, 40.0f};
    const bool overscan_scrolled =
        overscan_second_inserted &&
        message_timeline_set_viewport(&timeline, ui_extent_units(ui_rect_height(overscan_viewport))) &&
        message_timeline_set_scroll(&timeline, first_row_units, false);
    const MessageSceneResult overscan_arranged =
        overscan_scrolled ? message_scene_update(&scene, overscan_viewport) : MessageSceneResult{};
    const MessageSceneRowWidget* overscan_rich_row =
        overscan_arranged.ok() ? message_scene_find_row(scene, 0) : nullptr;
    const UiLayoutNode* overscan_rich_layout =
        overscan_rich_row ? message_scene_layout_node(scene, overscan_rich_row->widget) : nullptr;
    const MessageTextPresentationResult overscan_projected =
        overscan_rich_layout ? message_text_presentation_build(&presentation, scene, state, &users,
                                                               navigation, kDefaultTheme, 1'700'000'100ull)
                             : MessageTextPresentationResult{};
    bool offscreen_row_measured = false;
    bool offscreen_commands_clipped = true;
    u32 offscreen_commands = 0;
    for (u32 index = 0; index < presentation.measurement_count; ++index) {
        offscreen_row_measured =
            offscreen_row_measured || presentation.measurements[index].source_index == 0;
    }
    for (u32 index = 0; index < presentation.command_count; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.measurement_index >= presentation.measurement_count ||
            presentation.measurements[command.measurement_index].source_index != 0) {
            continue;
        }
        ++offscreen_commands;
        offscreen_commands_clipped =
            offscreen_commands_clipped &&
            !ui_rect_has_area(ui_rect_intersection(command.clip, overscan_viewport));
    }
    expect(overscan_second_inserted && overscan_scrolled && overscan_arranged.ok() && overscan_rich_layout,
           "message_text_overscan_fixture_arranges");
    expect(overscan_rich_layout &&
               !ui_rect_has_area(ui_rect_intersection(overscan_rich_layout->rectangle, overscan_viewport)),
           "message_text_overscan_fixture_keeps_rich_row_offscreen");
    expect(overscan_projected.ok() && offscreen_row_measured && offscreen_commands != 0,
           "message_text_overscan_premeasures_rich_row");
    expect(offscreen_commands_clipped, "message_text_overscan_commands_remain_outside_viewport");

    const bool record_updated =
        message_store_upsert(
            &store, MessageInput{91'000, 7, 9, 1'700'000'000'000ull, 0, utf8_literal("revised"), 0})
            .ok();
    bool stale_changed = true;
    expect(
        record_updated && message_text_presentation_hit_source(store, nullptr, link_hit).bytes == 0 &&
            !message_text_presentation_toggle_spoiler(&presentation, store, nullptr, spoiler_hit, &stale_changed) &&
            !stale_changed,
        "message_text_stale_interaction_revision_is_rejected");
}

namespace {
// A measurer that follows the text size: a line is the glyph box of its largest run (1.3 em; Segoe UI's is 1.33), a
// character half an em, at the chat em for chat-face requests and at 16 for the interface's.
struct ChatTextSizeMeasure {
    float chat_em;
    u32 chat_requests, interface_requests;
};
bool chat_text_size_measure(void* opaque, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    auto* fixture = static_cast<ChatTextSizeMeasure*>(opaque);
    if (!fixture || !value || request.constraint.width <= 0.0f || request.constraint.height <= 0.0f)
        return false;
    const bool chat = ui_text_face_is_chat(request.face);
    ++(chat ? fixture->chat_requests : fixture->interface_requests);
    const float em = chat ? fixture->chat_em : 16.0f;
    float size = 0.0f;
    u32 covered = 0;
    for (u32 i = 0; i < request.styles.count; ++i) {
        const auto& span = request.styles[i];
        size = maximum(size, text_style_font_size(em, text_style_fold_size(span.flags, span.size)));
        covered += span.byte_count;
    }
    if (covered < request.text.bytes || !(size > 0.0f))
        size = maximum(size, em);
    const float line = size * 1.3f, glyph = size * 0.5f;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / glyph));
    const u32 lines =
        request.text.bytes == 0 ? 0u : request.no_wrap ? 1u : (request.text.bytes + columns - 1u) / columns;
    const float natural = static_cast<float>(request.text.bytes) * glyph;
    *value = UiTextMeasureValue{UiSize{minimum(request.constraint.width, natural), static_cast<float>(lines) * line},
                                lines};
    return true;
}
bool chat_text_size_finalize(void*, const Theme&) noexcept {
    return true;
}
} // namespace

// Accessibility's Text size in chat sizes message text only. At 12, 16 and 24 every line the scene lays out holds
// the glyph box of its text (the author line, the reply, the body, the embed), the author line is the chat size's
// own, and the reaction pill's fixed-box label stays at the interface's size.
void test_chat_text_size() noexcept {
    MessageRecord records[1]{};
    u32 order[1]{};
    u8 source_text[512]{};
    MessageStore store{};
    u32 item_units[1]{};
    u64 fenwick[1]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    MessageSceneRowWidget rows[1]{};
    UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode layout[kMessageSceneWidgetsPerRow + 1]{};
    const u32 default_units = ui_extent_units(kUiBootstrapMessageRowHeight);
    const bool model_ready =
        message_store_initialize(&store, Slice<MessageRecord>{records, 1}, Slice<u32>{order, 1},
                                 Slice<u8>{source_text, 512}) &&
        message_store_upsert(&store, MessageInput{91'000, 7, 9, 1'700'000'000'000ull, 0,
                                                  utf8_literal("**Bold** words that wrap across the row and `code`"),
                                                  0})
            .ok() &&
        ui_variable_list_initialize(&list, Slice<u32>{item_units, 1}, Slice<u64>{fenwick, 1}, 1, default_units) &&
        message_timeline_initialize(&timeline, &store, &list, default_units, ui_extent_units(320.0f), true) &&
        message_scene_initialize(&scene, &timeline, Slice<MessageSceneRowWidget>{rows, 1},
                                 Slice<UiWidget>{widgets, kMessageSceneWidgetsPerRow + 1},
                                 Slice<UiLayoutNode>{layout, kMessageSceneWidgetsPerRow + 1});
    GatewayMessageEmbed embeds[1]{};
    embeds[0].type = GatewayMessageEmbedType::rich;
    embeds[0].author_name = utf8_literal("Embed author");
    embeds[0].title = utf8_literal("Embed title");
    embeds[0].description = utf8_literal("An embed description long enough to wrap onto a second line");
    embeds[0].footer_text = utf8_literal("Embed footer");
    GatewayMessageReaction reactions[2]{};
    for (u32 index = 0; index < 2; ++index) {
        reactions[index].name = utf8_literal("spark");
        reactions[index].count = index + 1;
        reactions[index].normal_count = index + 1;
    }
    GatewayMessageEvent rich_event{};
    rich_event.id = 91'000;
    rich_event.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message |
                        gateway_message_field_reactions | gateway_message_field_embeds;
    rich_event.reference.message_id = 90'999;
    rich_event.reference.channel_id = 7;
    rich_event.reference.author_id = 8;
    rich_event.reference.author_name = utf8_literal("Reply Author");
    rich_event.reference.content = utf8_literal("The quoted message");
    rich_event.reference.state = GatewayMessageReferenceState::available;
    rich_event.embeds = ConstSlice<GatewayMessageEmbed>{embeds, 1};
    rich_event.reactions = ConstSlice<GatewayMessageReaction>{reactions, 2};
    MessageRichStore rich_store{};
    const MessageStoreResult source_record = message_store_find(store, 91'000);
    const MessageRecord* source_message =
        source_record.ok() ? message_store_resolve(store, source_record.handle) : nullptr;
    const bool rich_ready =
        model_ready && source_message &&
        message_rich_store_initialize(
            &rich_store, Slice<MessageRichRecord>{g_message_scene_rich_records, 32},
            Slice<u16>{g_message_scene_rich_order, 32}, Slice<u8>{g_message_scene_rich_text, 16 * 1024}) &&
        message_timeline_bind_rich_store(&timeline, &rich_store) &&
        message_rich_store_apply(&rich_store, 91'000, source_message->revision, rich_event).ok();
    MessageTextPresentation presentation{};
    const bool presentation_ready =
        initialize_presentation(&presentation, g_message_text_commands, kMaximumMessageTextCommands);
    ChatStateStore state{};
    GuildRecord state_guilds[1]{};
    u16 state_guild_order[1]{};
    ChannelRecord state_channels[1]{};
    u16 state_channel_order[1]{};
    RoleRecord state_roles[1]{};
    PermissionOverwriteRecord state_overwrites[1]{};
    u8 state_names[256]{};
    NavigationView navigation{};
    UserStore users{};
    UserRecord user_records[1]{};
    u32 user_order[1]{};
    u8 user_names[32]{};
    const bool state_ready =
        user_store_initialize(&users, Slice<UserRecord>{user_records, 1}, Slice<u32>{user_order, 1},
                              Slice<u8>{user_names, 32}) &&
        user_store_upsert(&users, 9, utf8_literal("Layout Author")).ok() &&
        chat_state_store_initialize(&state, Slice<GuildRecord>{state_guilds, 1}, Slice<u16>{state_guild_order, 1},
                                    Slice<ChannelRecord>{state_channels, 1}, Slice<u16>{state_channel_order, 1},
                                    Slice<u8>{state_names, 256}) &&
        chat_state_store_bind_permissions(&state, Slice<RoleRecord>{state_roles, 1},
                                          Slice<PermissionOverwriteRecord>{state_overwrites, 1}) &&
        chat_state_store_upsert_guild(
            &state, GuildInput{70, utf8_literal("Reply Guild"), GuildMutationKind::create, 9})
            .ok() &&
        chat_state_store_upsert_channel(&state, ChannelInput{7, 70, 0, utf8_literal("reply-channel"), 0, 0, 0}).ok();
    navigation.selected_channel_id = 7;
    ChatTextSizeMeasure fixture{};
    UiTextMeasureCache cache{};
    const bool cache_ready =
        ui_text_measure_cache_initialize(&cache,
                                         Slice<UiTextMeasureRecord>{g_message_text_coordinator_records,
                                                                    kMaximumMessageTextMeasureCacheRecords},
                                         chat_text_size_measure, &fixture);
    const MessageTextCoordinatorRequest request{&scene,         &presentation,        &cache, &state,
                                                &users,         &navigation,          &kDefaultTheme,
                                                chat_text_size_finalize, nullptr, 1'700'000'100ull, 1};
    const bool arranged = message_scene_update(&scene, UiRect{0.0f, 0.0f, 720.0f, 320.0f}).ok();
    expect(rich_ready && presentation_ready && state_ready && cache_ready && arranged,
           "chat_text_size_fixture_is_ready");
    if (!(rich_ready && presentation_ready && state_ready && cache_ready && arranged))
        return;

    constexpr float sizes[]{12.0f, 16.0f, 24.0f};
    float author_lines[3]{}, reaction_heights[3]{}, content_heights[3]{};
    bool fits_at[2][3]{};
    UiResolvedThemeStyles styles{};
    // Cozy rows, then compact ones (the name and the time join the text's flow).
    for (u32 mode = 0; mode < 2; ++mode) {
        if (mode && !message_scene_set_display(&scene, MessageSceneDisplay{0, true})) {
            expect(false, "chat_text_size_compact_display_applies");
            return;
        }
        for (u32 at = 0; at < 3; ++at) {
            // 16 is the theme's own size, which the publication stores as "follows the body size".
            UiThemeTypography typography{};
            typography.chat_size = sizes[at] == 16.0f ? 0.0f : sizes[at];
            const bool applied = ui_theme_styles_apply(&styles, typography, UiThemeMetrics{}).ok() &&
                                 message_scene_bind_theme_styles(&scene, &styles);
            fixture = ChatTextSizeMeasure{sizes[at], 0, 0};
            ui_text_measure_cache_invalidate(&cache);
            const MessageTextCoordinatorResult converged = applied ? message_text_coordinator_converge(request)
                                                                   : MessageTextCoordinatorResult{};
            bool fits = converged.ok() && converged.finalized && presentation.command_count != 0 && scene.count == 1;
            for (u32 i = 0; fits && i < presentation.command_count; ++i) {
                const MessageTextCommand& command = presentation.commands[i];
                // No glyph box taller than the line that holds it. (A reaction's label sits in its fixed 24-DIP pill
                // at the interface's size: the measurer's 1.3 em box is a little over that pill's inner 20.)
                fits = command.measurement_kind == MessageTextMeasurementKind::reaction ||
                       command.measured_height <= ui_rect_height(command.rectangle) + 0.01f;
                if (mode)
                    continue;
                if (command.measurement_kind == MessageTextMeasurementKind::reaction && !reaction_heights[at])
                    reaction_heights[at] = command.measured_height;
                if (command.measurement_kind == MessageTextMeasurementKind::content && !content_heights[at])
                    content_heights[at] = command.measured_height;
                if (command.measurement_kind == MessageTextMeasurementKind::none && !author_lines[at] &&
                    command.owner.handle.slot == scene.rows[0].author.slot)
                    author_lines[at] = ui_rect_height(command.rectangle);
            }
            fits_at[mode][at] = fits && fixture.chat_requests != 0 && fixture.interface_requests != 0;
        }
    }
    expect(fits_at[0][0], "chat_text_size_12_every_line_holds_its_glyph_box");
    expect(fits_at[0][1], "chat_text_size_16_every_line_holds_its_glyph_box");
    expect(fits_at[0][2], "chat_text_size_24_every_line_holds_its_glyph_box");
    expect(fits_at[1][0] && fits_at[1][1] && fits_at[1][2],
           "chat_text_size_compact_rows_hold_their_glyph_boxes_at_12_16_and_24");
    const auto equal = [](float a, float b) noexcept { return a - b < 0.01f && b - a < 0.01f; };
    expect(equal(author_lines[0], 12.0f * 1.3125f) && equal(author_lines[1], 16.0f * 1.3125f) &&
               equal(author_lines[2], 24.0f * 1.3125f),
           "chat_text_size_author_line_is_the_chat_sizes_own");
    expect(content_heights[0] > 0.0f && content_heights[0] < content_heights[1] &&
               content_heights[1] < content_heights[2],
           "chat_text_size_body_grows_with_the_setting");
    expect(reaction_heights[0] > 0.0f && equal(reaction_heights[0], reaction_heights[1]) &&
               equal(reaction_heights[1], reaction_heights[2]),
           "chat_text_size_reaction_label_stays_at_the_interface_size");
    // Which commands are chat text: the author line, replies, the body and embeds; reactions, file cards, components,
    // notices and the unread divider's label sit in fixed boxes and keep the interface's size.
    constexpr MessageTextMeasurementKind chat_kinds[]{MessageTextMeasurementKind::none, MessageTextMeasurementKind::reply,
                                                      MessageTextMeasurementKind::content,
                                                      MessageTextMeasurementKind::embed};
    constexpr MessageTextMeasurementKind box_kinds[]{MessageTextMeasurementKind::attachment,
                                                     MessageTextMeasurementKind::component,
                                                     MessageTextMeasurementKind::reaction,
                                                     MessageTextMeasurementKind::notice};
    MessageTextCommand sample{};
    bool faces = true;
    for (const auto kind : chat_kinds) {
        sample.measurement_kind = kind;
        faces = faces && ui_text_face_is_chat(message_text_command_face(sample));
    }
    for (const auto kind : box_kinds) {
        sample.measurement_kind = kind;
        faces = faces && !ui_text_face_is_chat(message_text_command_face(sample));
    }
    sample.measurement_kind = MessageTextMeasurementKind::none;
    sample.interface_text = true;
    faces = faces && !ui_text_face_is_chat(message_text_command_face(sample));
    expect(faces, "chat_text_size_commands_in_fixed_boxes_keep_the_interface_size");
}

void test_message_rich_widget_materializer() noexcept {
    UiWidget widgets[24]{};
    UiLayoutNode layouts[24]{};
    UiWidgetManager manager{};
    UiLayoutStyle root_style = ui_layout_style();
    root_style.flow = UiLayoutFlow::vertical;
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    root_style.horizontal_alignment = UiLayoutAlignment::stretch;
    const bool manager_ready = ui_widget_manager_initialize(&manager, Slice<UiWidget>{widgets, 24},
                                                            Slice<UiLayoutNode>{layouts, 24});
    const UiWidgetResult root =
        manager_ready
            ? ui_widget_manager_add(
                  &manager, ui_widget_invalid_handle(),
                  UiWidgetDesc{100, UiWidgetKind::container, ui_widget_enabled,
                               UiLayoutNodeSpec{utf8_literal("RichRoot"), root_style, UiSize{}, 100}})
            : UiWidgetResult{};

    MessageRichRecord rich{};
    MessageRichReaction fixture_reactions[2]{};
    MessageRichAttachment fixture_attachments[1]{};
    MessageRichEmbed fixture_embeds[2]{};
    MessageRichEmbedField fixture_fields[1]{};
    MessageRichSticker fixture_stickers[1]{};
    MessageRichComponent fixture_components[2]{};
    rich.reactions = fixture_reactions;
    rich.attachments = fixture_attachments;
    rich.embeds = fixture_embeds;
    rich.embed_fields = fixture_fields;
    rich.stickers = fixture_stickers;
    rich.components = fixture_components;
    rich.reaction_count = 2;
    rich.attachment_count = 1;
    rich.embed_count = 2;
    rich.sticker_count = 1;
    rich.component_count = 2;
    rich.truncations = gateway_message_rich_truncated_text;
    rich.embeds[0].url.bytes = 1;
    rich.components[0].label.bytes = 1;
    rich.components[0].custom_id.bytes = 1;
    rich.components[0].style = 1;

    rich.components[0].parent = 0xFFFFu;
    rich.components[0].type = 2;
    rich.components[1].parent = 0xFFFFu;
    rich.components[1].type = 10;

    MessageSceneRowWidget row{};
    row.rich_layout = MessageSceneRichLayout{42.0f, 130.0f, kDefaultMessageStickerLayoutMetrics.group_height(1), 92.0f, 24.0f, 22.0f};
    row.reaction_widths[0] = 44.0f;
    row.reaction_widths[1] = 76.0f;
    row.rich_item_layout.heights[kMessageRichAttachmentItemOffset] = 42.0f;
    row.rich_item_layout.heights[kMessageRichEmbedItemOffset] = 60.0f;
    row.rich_item_layout.heights[kMessageRichEmbedItemOffset + 1] = 70.0f;
    row.rich_item_layout.heights[kMessageRichStickerItemOffset] = kDefaultMessageStickerLayoutMetrics.extent;
    row.rich_item_layout.heights[kMessageRichComponentItemOffset] = 52.0f;
    row.rich_item_layout.heights[kMessageRichComponentItemOffset + 1] = 32.0f;
    row.rich_item_layout.component_widths[0] = 220.0f;
    row.rich_item_layout.heights[kMessageRichNoticeItemOffset] = 22.0f;
    const bool built =
        root.ok() && message_rich_widgets_build(&manager, root.handle, 200, &rich, &row) &&
        message_rich_widgets_apply(&manager, &rich, &row) &&
        ui_widget_manager_layout_rectangle(&manager, UiRect{0.0f, 0.0f, 400.0f, 520.0f}).ok();
    const UiWidgetHandle attachment =
        message_scene_rich_item_widget(row, MessageSceneRichItemKind::attachment, 0);
    const UiWidgetHandle first_embed =
        message_scene_rich_item_widget(row, MessageSceneRichItemKind::embed, 0);
    const UiWidgetHandle second_embed =
        message_scene_rich_item_widget(row, MessageSceneRichItemKind::embed, 1);
    const UiWidgetHandle first_component =
        message_scene_rich_item_widget(row, MessageSceneRichItemKind::component, 0);
    const UiWidget* attachment_widget = ui_widget_manager_resolve(manager, attachment);
    const UiWidget* first_embed_widget = ui_widget_manager_resolve(manager, first_embed);
    const UiWidget* second_embed_widget = ui_widget_manager_resolve(manager, second_embed);
    const UiWidget* first_component_widget = ui_widget_manager_resolve(manager, first_component);
    const UiLayoutNode* first_embed_layout = ui_widget_manager_layout_node(manager, first_embed);
    const UiLayoutNode* second_embed_layout = ui_widget_manager_layout_node(manager, second_embed);
    const UiLayoutNode* first_reaction_layout =
        ui_widget_manager_layout_node(manager, row.reaction_buttons[0]);
    const UiLayoutNode* second_reaction_layout =
        ui_widget_manager_layout_node(manager, row.reaction_buttons[1]);
    const MessageSceneHit embed_hit =
        first_embed_layout
            ? MessageSceneHit{0,
                              ui_widget_manager_hit_test(manager,
                                                         (first_embed_layout->rectangle.left +
                                                          first_embed_layout->rectangle.right) *
                                                             0.5f,
                                                         (first_embed_layout->rectangle.top +
                                                          first_embed_layout->rectangle.bottom) *
                                                             0.5f),
                              0}
            : MessageSceneHit{};
    expect(built && manager.count == 16 && message_rich_widgets_structure_matches(row, &rich),
           "message_rich_widget_materializer_builds_complete_bounded_subtree");
    expect(attachment_widget && ui_widget_manager_parent(manager, attachment).slot == row.attachments.slot &&
               first_embed_widget && ui_widget_manager_parent(manager, first_embed).slot == row.embeds.slot &&
               second_embed_widget && ui_widget_manager_parent(manager, second_embed).slot == row.embeds.slot &&
               first_component_widget &&
               ui_widget_manager_parent(manager, first_component).slot == row.components.slot,
           "message_rich_widget_materializer_assigns_exact_group_parents");
    expect(first_embed_widget && first_embed_widget->kind == UiWidgetKind::button &&
               (first_embed_widget->flags & ui_widget_hit_test) != 0 && second_embed_widget &&
               second_embed_widget->kind == UiWidgetKind::container &&
               (second_embed_widget->flags & ui_widget_hit_test) == 0 && first_component_widget &&
               first_component_widget->kind == UiWidgetKind::button &&
               embed_hit.widget.slot == first_embed.slot,
           "message_rich_widget_materializer_owns_interaction_and_hit_policy");
    expect(first_embed_layout && second_embed_layout &&
               first_embed_layout->rectangle.bottom <= second_embed_layout->rectangle.top,
           "message_rich_widget_materializer_vertical_items_do_not_overlap");
    expect(first_reaction_layout && second_reaction_layout &&
               ui_rect_width(first_reaction_layout->rectangle) == 44.0f &&
               ui_rect_width(second_reaction_layout->rectangle) == 76.0f,
           "message_reaction_widgets_use_measured_intrinsic_widths");

    MessageSceneRowWidget duplicate_row{};
    duplicate_row.attachments = UiWidgetHandle{23, 77};
    const u32 count_before_duplicate = manager.count;
    expect(!message_rich_widgets_build(&manager, root.handle, 200, &rich, &duplicate_row) &&
               manager.count == count_before_duplicate && duplicate_row.attachments.slot == 23 &&
               duplicate_row.attachments.generation == 77,
           "message_rich_widget_materializer_duplicate_preflight_is_atomic");

    UiWidget tiny_widgets[8]{};
    UiLayoutNode tiny_layouts[8]{};
    UiWidgetManager tiny_manager{};
    const bool tiny_ready = ui_widget_manager_initialize(&tiny_manager, Slice<UiWidget>{tiny_widgets, 8},
                                                         Slice<UiLayoutNode>{tiny_layouts, 8});
    const UiWidgetResult tiny_root =
        tiny_ready ? ui_widget_manager_add(&tiny_manager, ui_widget_invalid_handle(),
                                           UiWidgetDesc{300, UiWidgetKind::container, ui_widget_enabled,
                                                        UiLayoutNodeSpec{utf8_literal("TinyRichRoot"),
                                                                         root_style, UiSize{}, 300}})
                   : UiWidgetResult{};
    MessageSceneRowWidget tiny_row{};
    tiny_row.attachments = UiWidgetHandle{19, 88};
    expect(tiny_root.ok() &&
               !message_rich_widgets_build(&tiny_manager, tiny_root.handle, 400, &rich, &tiny_row) &&
               tiny_manager.count == 1 && tiny_manager.layout.count == 1 &&
               tiny_row.attachments.slot == 19 && tiny_row.attachments.generation == 88,
           "message_rich_widget_materializer_capacity_preflight_is_atomic");

    row.rich_item_layout.heights[kMessageRichEmbedItemOffset] = 90.0f;
    row.rich_layout.embeds = 160.0f;
    rich.components[0].custom_id.bytes = 0;
    const bool updated =
        message_rich_widgets_apply(&manager, &rich, &row) &&
        ui_widget_manager_layout_rectangle(&manager, UiRect{0.0f, 0.0f, 400.0f, 560.0f}).ok();
    first_embed_layout = ui_widget_manager_layout_node(manager, first_embed);
    second_embed_layout = ui_widget_manager_layout_node(manager, second_embed);
    first_component_widget = ui_widget_manager_resolve(manager, first_component);
    UiLayoutDiagnostic diagnostics[16]{};
    const UiLayoutValidationResult validation =
        ui_layout_engine_validate(manager.layout, Slice<UiLayoutDiagnostic>{diagnostics, 16});
    expect(updated && first_embed_layout && second_embed_layout &&
               ui_rect_height(first_embed_layout->rectangle) == 90.0f &&
               first_embed_layout->rectangle.bottom <= second_embed_layout->rectangle.top &&
               first_component_widget && first_component_widget->kind == UiWidgetKind::container &&
               (first_component_widget->flags & ui_widget_hit_test) == 0 && validation.clean(),
           "message_rich_widget_materializer_applies_measurements_and_interaction_updates");

    rich.component_count = 1;
    expect(!message_rich_widgets_structure_matches(row, &rich),
           "message_rich_widget_materializer_classifies_count_change_as_structural");
    message_rich_widgets_reset_handles(&row);
    expect(row.attachments.slot == kInvalidUiWidgetSlot &&
               row.rich_items[kMessageRichEmbedItemOffset].slot == kInvalidUiWidgetSlot &&
               row.reaction_button_count == 0 && row.component_item_count == 0,
           "message_rich_widget_materializer_resets_all_handles_and_counts");
}

namespace {

u32 shark_catalog_index() noexcept {
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    for (u32 index = 0; index < catalog.count; ++index)
        if (utf8_equal(catalog[index].name(), utf8_literal("shark")))
            return index;
    return ~0u;
}

} // namespace

void test_message_context_menu() noexcept {
    MessageRecord message{};
    message.id = 11;
    message.channel_id = 7;
    message.delivery = MessageDeliveryState::authoritative;

    ReactionPreferenceStore preferences{};
    const bool preferences_ready = reaction_preferences_initialize(&preferences);
    const u32 shark_index = shark_catalog_index();
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    const bool shark_tracked =
        preferences_ready && shark_index <= 0xFFFEu &&
        reaction_preferences_track_emoji(&preferences, ReactionPreferenceKind::reaction,
                                         reaction_emoji_unicode(static_cast<u16>(shark_index)), 1'000).ok();

    MessageContextMenuModel model{};
    const MessageContextMenuResult authoritative =
        message_context_menu_build(&model, message, true, true, false, &preferences);
    MessageContextQuickReaction quick{};
    const bool resolved = message_context_menu_resolve_quick_reaction(
        model, static_cast<u32>(MessageContextAction::quick_reaction_0), &quick);
    expect(authoritative.ok() && authoritative.count == 11 && shark_tracked && resolved &&
               quick.catalog_index == shark_index &&
               utf8_equal(quick.glyph, catalog[shark_index].glyph()) &&
               model.items[0].action == static_cast<u32>(MessageContextAction::quick_reaction_0) &&
               (model.items[0].flags & (popup_item_emoji | popup_item_quick_reaction)) ==
                   (popup_item_emoji | popup_item_quick_reaction) &&
               model.items[4].action == static_cast<u32>(MessageContextAction::add_reaction) &&
               model.items[5].action == static_cast<u32>(MessageContextAction::edit_message) &&
               model.items[6].action == static_cast<u32>(MessageContextAction::reply) &&
               model.items[7].action == static_cast<u32>(MessageContextAction::copy_text) &&
               model.items[8].action == static_cast<u32>(MessageContextAction::copy_message_link) &&
               model.items[9].action == static_cast<u32>(MessageContextAction::delete_message) &&
               model.items[10].action == static_cast<u32>(MessageContextAction::copy_id) &&
               (model.items[5].flags & popup_item_separator_before) != 0 &&
               (model.items[9].flags & popup_item_danger) != 0 &&
               (model.items[10].flags & popup_item_separator_before) != 0,
           "message_context_menu_frecency_and_verified_action_sections");

    const MessageContextMenuResult read_only =
        message_context_menu_build(&model, message, false, false, false, &preferences);
    expect(read_only.ok() && read_only.count == 9 &&
               model.items[5].action == static_cast<u32>(MessageContextAction::reply) &&
               model.items[8].action == static_cast<u32>(MessageContextAction::copy_id),
           "message_context_menu_permission_policy");
    const MessageContextMenuResult without_developer = message_context_menu_build(
        &model, message, false, false, false, &preferences, false, false, false, false, false, false, false);
    bool copy_id_hidden = without_developer.ok() && without_developer.count == 8;
    for (u32 i = 0; i < model.count; ++i)
        copy_id_hidden = copy_id_hidden && model.items[i].action != static_cast<u32>(MessageContextAction::copy_id);
    expect(copy_id_hidden, "message_context_menu_copy_id_requires_developer_mode");

    const MessageContextMenuResult with_reactions =
        message_context_menu_build(&model, message, false, false, true, &preferences);
    expect(with_reactions.ok() && with_reactions.count == 10 &&
               model.items[4].action == static_cast<u32>(MessageContextAction::add_reaction) &&
               model.items[5].action == static_cast<u32>(MessageContextAction::view_reactions) &&
               model.items[6].action == static_cast<u32>(MessageContextAction::reply),
           "message_context_menu_view_reactions_observed_order");

    const auto pin_menu = message_context_menu_build(&model, message, false, false, false,
        &preferences, false, true);
    expect(pin_menu.ok() && pin_menu.count == 10 &&
        model.items[6].action == static_cast<u32>(MessageContextAction::pin_message) &&
        utf8_equal(model.items[6].label, utf8_literal("Pin message")) &&
        ui_menu_row_icon(model.items[6].presentation) == UiIcon::pin &&
        (model.items[6].flags & popup_item_enabled) != 0,
        "message_context_menu_pin_authorized_without_edit_delete_or_send_authority");
    message.presentation_flags = message_presentation_mentions_current_user | message_presentation_pinned;
    const auto unpin_menu = message_context_menu_build(&model, message, false, false, false,
        &preferences, false, true);
    expect(unpin_menu.ok() && unpin_menu.count == 10 &&
        model.items[6].action == static_cast<u32>(MessageContextAction::unpin_message) &&
        utf8_equal(model.items[6].label, utf8_literal("Unpin message")),
        "message_context_menu_retained_pinned_state_selects_unpin");
    const auto pin_denied = message_context_menu_build(&model, message, true, true, false,
        &preferences, false, false);
    bool no_pin = pin_denied.ok() && pin_denied.count == 11;
    for (u32 index = 0; index < model.count; ++index)
        no_pin = no_pin && model.items[index].action != static_cast<u32>(MessageContextAction::pin_message) &&
            model.items[index].action != static_cast<u32>(MessageContextAction::unpin_message);
    expect(no_pin, "message_context_menu_edit_delete_authority_does_not_grant_pin");

    message.type = 7;
    const auto system_menu = message_context_menu_build(&model, message, false, false, false, &preferences);
    bool system_has_no_reply = system_menu.ok();
    for (u32 index = 0; index < model.count; ++index)
        system_has_no_reply = system_has_no_reply &&
            model.items[index].action != static_cast<u32>(MessageContextAction::reply);
    expect(system_has_no_reply, "message_context_menu_system_message_has_no_reply_action");
    message.type = 0;
    message.delivery = MessageDeliveryState::pending;
    const MessageContextMenuResult pending =
        message_context_menu_build(&model, message, true, true, false, &preferences, false, true);
    expect(pending.ok() && pending.count == 1 &&
               model.items[0].action == static_cast<u32>(MessageContextAction::copy_text),
           "message_context_menu_pending_policy");

    message.delivery = MessageDeliveryState::failed;
    const MessageContextMenuResult failed =
        message_context_menu_build(&model, message, true, true, false, &preferences, false, true);
    expect(failed.ok() && failed.count == 3 &&
               model.items[0].action == static_cast<u32>(MessageContextAction::retry_send) &&
               model.items[1].action == static_cast<u32>(MessageContextAction::copy_text) &&
               model.items[2].action == static_cast<u32>(MessageContextAction::discard_failed) &&
               (model.items[2].flags & popup_item_danger) != 0,
           "message_context_menu_failed_send_policy");
    message.presentation_flags = 0;

    model.items[0].action = 0xA5A5u;
    message.id = 0;
    const MessageContextMenuResult invalid =
        message_context_menu_build(&model, message, true, true, false, &preferences);
    expect(model.items[0].action == 0xA5A5u && invalid.error == MessageContextMenuError::invalid_input,
           "message_context_menu_transactional_errors");

    char link[kMessageContextMaximumLinkBytes]{};
    const u32 guild_link_bytes =
        message_context_format_link(1111222233334444555ull, 1212343456567878909ull, 1357913579246802468ull,
                                    Slice<char>{link, kMessageContextMaximumLinkBytes});
    const Utf8View expected_link = utf8_literal(
        "https://discord.com/channels/1111222233334444555/1212343456567878909/1357913579246802468");
    expect(guild_link_bytes == expected_link.bytes &&
               utf8_equal(Utf8View{link, guild_link_bytes}, expected_link),
           "message_context_copy_link_exact_route");
    const u32 dm_link_bytes =
        message_context_format_link(0, 7, 11, Slice<char>{link, kMessageContextMaximumLinkBytes});
    expect(dm_link_bytes == utf8_literal("https://discord.com/channels/@me/7/11").bytes &&
               utf8_equal(Utf8View{link, dm_link_bytes},
                          utf8_literal("https://discord.com/channels/@me/7/11")),
           "message_context_copy_link_exact_dm_route");

    message.id = 11;
    message.delivery = MessageDeliveryState::authoritative;
    PopupItem popup_items[kMessageContextMaximumItems]{};
    PopupRowWidget popup_rows[kMessageContextMaximumItems]{};
    UiWidget popup_widgets[2 + kMessageContextMaximumItems * kPopupWidgetsPerItem]{};
    UiLayoutNode popup_nodes[2 + kMessageContextMaximumItems * kPopupWidgetsPerItem]{};
    PopupScene popup{};
    MessageContextController controller{};
    const bool popup_ready = popup_scene_initialize(
        &popup, Slice<PopupItem>{popup_items, kMessageContextMaximumItems},
        Slice<PopupRowWidget>{popup_rows, kMessageContextMaximumItems},
        Slice<UiWidget>{popup_widgets, 2 + kMessageContextMaximumItems * kPopupWidgetsPerItem},
        Slice<UiLayoutNode>{popup_nodes, 2 + kMessageContextMaximumItems * kPopupWidgetsPerItem});
    const MessageContextControllerResult opened =
        popup_ready ? message_context_controller_open(&controller, &popup, message, true, true, false,
                                                      &preferences, UiRect{100.0f, 80.0f, 101.0f, 81.0f},
                                                      UiRect{0.0f, 0.0f, 640.0f, 520.0f})
                    : MessageContextControllerResult{};
    const UiLayoutNode* quick0 =
        opened.ok() ? popup_scene_layout_node(popup, popup.rows[0].widget) : nullptr;
    const UiLayoutNode* quick1 =
        opened.ok() ? popup_scene_layout_node(popup, popup.rows[1].widget) : nullptr;
    const UiLayoutNode* quick0_label =
        opened.ok() ? popup_scene_layout_node(popup, popup.rows[0].label) : nullptr;
    const UiLayoutNode* add = opened.ok() ? popup_scene_layout_node(popup, popup.rows[4].widget) : nullptr;
    UiTextCommand context_commands[kMessageContextMaximumItems]{};
    UiTextCommandStream context_text{};
    UiTextOutput context_text_output{&context_text, nullptr};
    const bool context_presented =
        ui_text_command_stream_initialize(
            &context_text, Slice<UiTextCommand>{context_commands, kMessageContextMaximumItems}) &&
        opened.ok() && popup_text_presentation_build_output(popup, kDefaultTheme, &context_text_output);
    expect(context_presented && quick0 && quick1 && quick0_label && add &&
               ui_rect_width(popup.rectangle) == kMessageContextPreferredWidth &&
               ui_rect_width(quick0->rectangle) == kPopupQuickReactionExtent &&
               quick1->rectangle.left - quick0->rectangle.left ==
                   kPopupQuickReactionExtent + kPopupQuickReactionGap &&
               quick1->rectangle.top == quick0->rectangle.top &&
               quick0_label->content_rectangle.left == quick0->content_rectangle.left &&
               quick0_label->content_rectangle.top == quick0->content_rectangle.top &&
               quick0_label->content_rectangle.right == quick0->content_rectangle.right &&
               quick0_label->content_rectangle.bottom == quick0->content_rectangle.bottom &&
               add->rectangle.top > quick0->rectangle.bottom &&
               context_commands[0].style_flags == text_style_picker_emoji &&
               context_commands[0].horizontal_alignment == UiTextAlignment::center,
           "message_context_controller_mixed_quick_strip_geometry");
}

void test_reaction_users_and_viewer() noexcept {
    u8 target[kReactionUsersMaximumTargetBytes]{};
    const ReactionUsersWriteResult shark_route = write_reaction_users_target(
        1212343456567878909ull, 1357913579246802468ull, 0, utf8_literal("🦈"),
        MessageReactionType::normal, 0, 100, Slice<u8>{target, sizeof(target)});
    const Utf8View expected_shark =
        utf8_literal("/api/v9/channels/1212343456567878909/messages/1357913579246802468/reactions/"
                     "%F0%9F%A6%88?limit=100&type=0");
    expect(shark_route.ok() && shark_route.bytes == expected_shark.bytes &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(target), shark_route.bytes},
                          expected_shark),
           "reaction_users_v10_route_preserves_observed_query_shape");

    const ReactionUsersWriteResult custom_route = write_reaction_users_target(
        7, 11, 123, utf8_literal("wave"), MessageReactionType::burst, 456, 100,
        Slice<u8>{target, sizeof(target)});
    expect(custom_route.ok() &&
               utf8_equal(
                   Utf8View{reinterpret_cast<const char*>(target), custom_route.bytes},
                   utf8_literal(
                       "/api/v9/channels/7/messages/11/reactions/wave:123?limit=100&after=456&type=1")),
           "reaction_users_custom_emoji_after_pagination_route");

    constexpr Utf8View response_body =
        utf8_literal("[{\"id\":\"1\",\"username\":\"wren042\",\"global_name\":\"Wren042\","
                     "\"avatar\":\"a_hash\",\"bot\":false,\"display_name_styles\":{\"font_id\":1},"
                     "\"clan\":null},{\"id\":\"2\",\"username\":\"claudia\","
                     "\"global_name\":null,\"avatar\":null,\"bot\":true,"
                     "\"primary_guild\":{\"tag\":\"APP\"}}]");
    u32 page_scratch[256]{};
    u32 object_scratch[128]{};
    ReactionUser decoded_users[4]{};
    char decoded_text[256]{};
    const ReactionUsersDecodeResult decoded = decode_reaction_users(
        response_body, Slice<u32>{page_scratch, 256}, Slice<u32>{object_scratch, 128}, 16,
        Slice<ReactionUser>{decoded_users, 4}, Slice<char>{decoded_text, 256});
    expect(decoded.ok() && decoded.user_count == 2 && decoded.text_bytes != 0 && decoded_users[0].id == 1 &&
               !decoded_users[0].bot &&
               utf8_equal(decoded_users[0].username, utf8_literal("wren042")) &&
               utf8_equal(decoded_users[0].display_name, utf8_literal("Wren042")) &&
               utf8_equal(decoded_users[0].avatar_hash, utf8_literal("a_hash")) &&
               decoded_users[1].id == 2 && decoded_users[1].bot &&
               utf8_equal(decoded_users[1].display_name, utf8_literal("claudia")) &&
               decoded_users[1].avatar_hash.bytes == 0,
           "reaction_users_stream_decode_rich_unknown_fields");
    char short_text[2]{};
    const ReactionUsersDecodeResult text_capacity = decode_reaction_users(
        response_body, Slice<u32>{page_scratch, 256}, Slice<u32>{object_scratch, 128}, 16,
        Slice<ReactionUser>{decoded_users, 4}, Slice<char>{short_text, 2});
    expect(text_capacity.error == ReactionUsersDecodeError::text_capacity,
           "reaction_users_decode_text_capacity_is_precise");
    const ReactionUsersDecodeResult user_capacity = decode_reaction_users(
        response_body, Slice<u32>{page_scratch, 256}, Slice<u32>{object_scratch, 128}, 16,
        Slice<ReactionUser>{decoded_users, 1}, Slice<char>{decoded_text, 256});
    expect(user_capacity.error == ReactionUsersDecodeError::user_capacity,
           "reaction_users_decode_user_capacity_is_bounded");

    MessageRichRecord rich{};
    u8 rich_text[64]{};
    const Utf8View shark = utf8_literal("🦈");
    const Utf8View heart = utf8_literal("❤");
    for (u32 index = 0; index < shark.bytes; ++index) {
        rich_text[index] = static_cast<u8>(shark.data[index]);
    }
    for (u32 index = 0; index < heart.bytes; ++index) {
        rich_text[shark.bytes + index] = static_cast<u8>(heart.data[index]);
    }
    MessageRichStore rich_store{};
    rich_store.text = rich_text;
    rich_store.text_capacity = sizeof(rich_text);
    rich_store.text_used = shark.bytes + heart.bytes;
    MessageRichReaction fixture_reactions[2]{};
    rich.reactions = fixture_reactions;
    rich.occupied = true;
    rich.message_id = 11;
    rich.reaction_count = 2;
    rich.reactions[0].name = MessageRichString{0, static_cast<u16>(shark.bytes), 0};
    rich.reactions[0].count = 6;
    rich.reactions[0].normal_count = 6;
    rich.reactions[1].name = MessageRichString{shark.bytes, static_cast<u16>(heart.bytes), 0};
    rich.reactions[1].count = 2;
    rich.reactions[1].normal_count = 2;

    ReactionViewerUser viewer_users[128]{};
    char viewer_text[4 * 1024]{};
    ReactionViewer viewer{};
    const bool viewer_ready =
        reaction_viewer_initialize(&viewer, Slice<ReactionViewerUser>{viewer_users, 128},
                                   Slice<char>{viewer_text, sizeof(viewer_text)});
    const ReactionViewerResult opened =
        viewer_ready
            ? reaction_viewer_open(&viewer, 5, 7, 11, rich_store, rich, UiRect{0.0f, 0.0f, 1600.0f, 900.0f})
            : ReactionViewerResult{};
    expect(opened.ok() && viewer.active && ui_rect_width(viewer.panel) == 480.0f &&
               ui_rect_height(viewer.panel) == 784.0f && ui_rect_height(viewer.header) == 48.0f &&
               reaction_viewer_user_rectangle(viewer, 0).right == 0.0f,
           "reaction_viewer_measured_retained_geometry");

    const ReactionUsersDecodeResult viewer_page = decode_reaction_users(
        response_body, Slice<u32>{page_scratch, 256}, Slice<u32>{object_scratch, 128}, 16,
        Slice<ReactionUser>{decoded_users, 4}, reaction_viewer_page_text_output(viewer));
    const ReactionViewerResult accepted =
        viewer_page.ok()
            ? reaction_viewer_accept_page(
                  &viewer, ConstSlice<ReactionUser>{decoded_users, viewer_page.user_count},
                  viewer_page.text_bytes)
            : ReactionViewerResult{};
    PaintStream paint{};
    paint_stream_reset(&paint, 1600.0f, 900.0f);
    UiTextCommand viewer_commands[96]{};
    UiTextCommandStream viewer_command_stream{};
    UiTextOutput viewer_command_stream_output{&viewer_command_stream, nullptr};
    const bool viewer_text_ready = ui_text_command_stream_initialize(
        &viewer_command_stream, Slice<UiTextCommand>{viewer_commands, 96});
    const bool paint_presented = reaction_viewer_presentation_build(viewer, kDefaultTheme, &paint);
    const bool text_presented = viewer_text_ready && reaction_viewer_text_presentation_build_output(
                                                         viewer, kDefaultTheme, &viewer_command_stream_output);
    const UiRect first_user = reaction_viewer_user_rectangle(viewer, 0);
    const ReactionViewerHit first_user_hit =
        reaction_viewer_hit_test(viewer, first_user.left + 2.0f, first_user.top + 2.0f);
    expect(accepted.ok() && viewer.user_count == 2 && viewer.complete && viewer.after_user_id == 2,
           "reaction_viewer_page_accept_contract");
    expect(utf8_equal(reaction_viewer_user_name(viewer, viewer.users[0]), utf8_literal("Wren042")),
           "reaction_viewer_direct_page_text_offsets");
    expect(paint_presented && text_presented, "reaction_viewer_retained_presentation_builds");
    expect(first_user_hit.kind == ReactionViewerHitKind::user && first_user_hit.index == 0,
           "reaction_viewer_user_hit_contract");

    // Both columns retain sub-row offsets and clip all presentation layers
    // to the body, independently of the fixed modal title/close controls.
    for (u32 i = 2; i < 30; ++i) viewer.users[i] = viewer.users[i % 2];
    viewer.user_count = 30;
    const u8 original_aggregate_count = viewer.aggregate_count;
    for (u32 i = original_aggregate_count; i < 20; ++i) viewer.aggregates[i] = viewer.aggregates[0];
    viewer.aggregate_count = 20;
    bool user_moved = false, aggregate_moved = false;
    const bool pixel_scrolled = reaction_viewer_set_scroll(&viewer, true, 31.5f, &user_moved) &&
        reaction_viewer_set_scroll(&viewer, false, 17.25f, &aggregate_moved);
    const auto partial_user = reaction_viewer_user_rectangle(viewer, 0);
    const auto partial_aggregate = reaction_viewer_aggregate_rectangle(viewer, 0);
    const auto header_hit = reaction_viewer_hit_test(viewer, partial_aggregate.left + 2,
        viewer.body.top - 1);
    expect(pixel_scrolled && user_moved && aggregate_moved && viewer.first_user == 0 &&
        viewer.first_aggregate == 0 && partial_user.top == first_user.top - 31.5f &&
        partial_aggregate.top == viewer.body.top - 17.25f && header_hit.kind == ReactionViewerHitKind::panel,
        "reaction_viewer_fractional_columns_and_header_hit_clipping");
    paint_stream_reset(&paint, 1600, 900);
    ui_text_command_stream_initialize(&viewer_command_stream, {viewer_commands, 96});
    bool clipped_columns = reaction_viewer_presentation_build(viewer, kDefaultTheme, &paint) &&
        reaction_viewer_text_presentation_build_output(viewer, kDefaultTheme, &viewer_command_stream_output);
    for (u32 i = 2; i < viewer_command_stream.count; ++i)
        clipped_columns = clipped_columns && ui_rect_equal(viewer_commands[i].clip, viewer.body);
    expect(clipped_columns && paint.clip_depth == 1, "reaction_viewer_fractional_presentation_shared_body_clip");
    reaction_viewer_set_scroll(&viewer, false, 39.5f, &aggregate_moved);
    reaction_viewer_set_scroll(&viewer, true, 49.5f, &user_moved);
    ui_text_command_stream_initialize(&viewer_command_stream, {viewer_commands, 96});
    bool gap_clipped = reaction_viewer_text_presentation_build_output(viewer, kDefaultTheme, &viewer_command_stream_output);
    for (u32 i = 0; i < viewer_command_stream.count; ++i)
        gap_clipped = gap_clipped && ui_rect_has_area(ui_rect_intersection(
            viewer_commands[i].rectangle, viewer_commands[i].clip));
    expect(gap_clipped, "reaction_viewer_skips_fully_clipped_labels_in_partial_row_gaps");

    ReactionUser controller_page_users[kReactionUsersMaximumPage]{};
    ReactionViewerController controller{};
    const bool controller_ready = reaction_viewer_controller_initialize(
        &controller, &viewer, Slice<ReactionUser>{controller_page_users, kReactionUsersMaximumPage});
    const ReactionViewerControllerResult controller_opened =
        controller_ready ? reaction_viewer_controller_open(&controller, 5, 7, 11, rich_store, rich,
                                                           UiRect{0.0f, 0.0f, 1600.0f, 900.0f})
                         : ReactionViewerControllerResult{};
    ReactionViewerQuery query{};
    const bool query_ready =
        controller_opened.ok() && reaction_viewer_controller_prepare_request(&controller, 10, &query);
    const ReactionViewerControllerResult marked =
        query_ready ? reaction_viewer_controller_mark_submitted(&controller, query.epoch)
                    : ReactionViewerControllerResult{};
    const u64 submitted_epoch = controller.request_epoch;
    const ReactionViewerControllerResult selected =
        marked.ok() ? reaction_viewer_controller_select(&controller, 1) : ReactionViewerControllerResult{};
    RestClientResponse response{};
    response.status_code = 200;
    const ReactionViewerControllerResult stale =
        selected.ok() ? reaction_viewer_controller_accept_response(&controller, response, ConstSlice<u8>{},
                                                                   Slice<u32>{page_scratch, 256},
                                                                   Slice<u32>{object_scratch, 128})
                      : ReactionViewerControllerResult{};
    expect(query_ready && marked.ok() && selected.ok() && selected.changed &&
               submitted_epoch != controller.epoch && stale.ok() && stale.stale &&
               controller.phase == ReactionViewerRequestPhase::required,
           "reaction_viewer_controller_discards_stale_epoch");

    ReactionViewerQuery current_query{};
    const bool current_ready = reaction_viewer_controller_prepare_request(&controller, 20, &current_query);
    const ReactionViewerControllerResult current_marked =
        current_ready ? reaction_viewer_controller_mark_submitted(&controller, current_query.epoch)
                      : ReactionViewerControllerResult{};
    const ReactionViewerControllerResult current_accepted =
        current_marked.ok()
            ? reaction_viewer_controller_accept_response(
                  &controller, response,
                  ConstSlice<u8>{reinterpret_cast<const u8*>(response_body.data), response_body.bytes},
                  Slice<u32>{page_scratch, 256}, Slice<u32>{object_scratch, 128})
            : ReactionViewerControllerResult{};
    viewer.complete = false;
    viewer.loading = false;
    viewer.failed = false;
    viewer.user_count = 100;
    viewer.expected_user_count = 200;
    viewer.first_user = 90;
    controller.phase = ReactionViewerRequestPhase::idle;
    const ReactionViewerControllerResult more = reaction_viewer_controller_request_more(&controller);
    expect(current_accepted.ok() && viewer.after_user_id == 2 && more.ok() && more.changed &&
               controller.phase == ReactionViewerRequestPhase::required,
           "reaction_viewer_controller_atomic_page_and_lazy_pagination");

    ReactionViewerQuery retry_query{};
    const bool retry_ready = reaction_viewer_controller_prepare_request(&controller, 30, &retry_query);
    const ReactionViewerControllerResult retry_marked =
        retry_ready ? reaction_viewer_controller_mark_submitted(&controller, retry_query.epoch)
                    : ReactionViewerControllerResult{};
    RestClientResponse limited{};
    limited.status_code = 429;
    limited.rate_limited = true;
    limited.retry_at_ms = GetTickCount64() + 10'000;
    const ReactionViewerControllerResult retry =
        retry_marked.ok()
            ? reaction_viewer_controller_accept_response(&controller, limited, ConstSlice<u8>{},
                                                         Slice<u32>{page_scratch, 256},
                                                         Slice<u32>{object_scratch, 128})
            : ReactionViewerControllerResult{};
    expect(retry.ok() && retry.retry_scheduled &&
               controller.phase == ReactionViewerRequestPhase::retry_wait &&
               reaction_viewer_controller_next_wake_at(controller) == limited.retry_at_ms,
           "reaction_viewer_controller_rate_limit_retry_contract");

    ReactionViewerStorage lazy_storage{};
    ReactionViewer lazy_viewer{};
    ReactionViewerController lazy_controller{};
    const bool lazy_ready =
        reaction_viewer_storage_materialize(&lazy_storage, &lazy_viewer, &lazy_controller);
    ReactionViewerUser* const lazy_users = lazy_viewer.users;
    expect(lazy_ready && lazy_storage.ready && lazy_controller.initialized &&
               lazy_storage.users.capacity == kReactionViewerUserCapacity &&
               lazy_storage.text.capacity == kReactionViewerTextCapacity &&
               lazy_storage.page_users.capacity == kReactionUsersMaximumPage &&
               reaction_viewer_storage_materialize(&lazy_storage, &lazy_viewer, &lazy_controller) &&
               lazy_viewer.users == lazy_users,
           "reaction_viewer_storage_materializes_once");
    reaction_viewer_storage_release(&lazy_storage, &lazy_viewer, &lazy_controller);
    expect(!lazy_storage.ready && !lazy_storage.users.arena.base && !lazy_viewer.users &&
               !lazy_controller.initialized,
           "reaction_viewer_storage_release");
}

void test_message_action_bar() noexcept {
    MessageRecord records[2]{};
    u32 order[2]{};
    u8 source_text[128]{};
    MessageStore store{};
    u32 item_units[2]{};
    u64 fenwick[2]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    MessageSceneRowWidget rows[2]{};
    UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode layout_nodes[2 * kMessageSceneWidgetsPerRow + 1]{};
    const u32 default_units = ui_extent_units(kUiBootstrapMessageRowHeight);
    const bool model_ready =
        message_store_initialize(&store, Slice<MessageRecord>{records, 2}, Slice<u32>{order, 2},
                                 Slice<u8>{source_text, 128}) &&
        message_store_upsert(&store,
                             MessageInput{71, 7, 9, 1'700'000'000'000ull, 0, utf8_literal("first"), 0})
            .ok() &&
        message_store_upsert(&store,
                             MessageInput{72, 7, 10, 1'700'000'500'000ull, 0, utf8_literal("second"), 0})
            .ok() &&
        ui_variable_list_initialize(&list, Slice<u32>{item_units, 2}, Slice<u64>{fenwick, 2}, 2,
                                    default_units) &&
        message_timeline_initialize(&timeline, &store, &list, default_units, ui_extent_units(360.0f),
                                    true) &&
        message_scene_initialize(&scene, &timeline, Slice<MessageSceneRowWidget>{rows, 2},
                                 Slice<UiWidget>{widgets, 2 * kMessageSceneWidgetsPerRow + 1},
                                 Slice<UiLayoutNode>{layout_nodes, 2 * kMessageSceneWidgetsPerRow + 1});
    const UiRect viewport{100.0f, 40.0f, 700.0f, 400.0f};
    const MessageSceneResult arranged =
        model_ready ? message_scene_update(&scene, viewport) : MessageSceneResult{};
    const MessageSceneRowWidget* row = arranged.ok() && scene.count == 2 ? &scene.rows[1] : nullptr;
    const UiLayoutNode* row_layout = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    const MessageStoreResult found = message_store_find(store, 72);
    const MessageRecord* message = found.ok() ? message_store_resolve(store, found.handle) : nullptr;

    ReactionPreferenceStore preferences{};
    const bool preferences_ready = reaction_preferences_initialize(&preferences);
    const u32 shark_index = shark_catalog_index();
    const bool preference_ready =
        preferences_ready && shark_index <= 0xFFFEu &&
        reaction_preferences_track_emoji(&preferences, ReactionPreferenceKind::reaction,
                                         reaction_emoji_unicode(static_cast<u16>(shark_index)), 2'000).ok();

    MessageActionBar bar{};
    const MessageSceneHit scene_hit =
        row ? MessageSceneHit{row->message_id, row->widget, row->source_index} : MessageSceneHit{};
    const MessageActionBarResult opened =
        message ? message_action_bar_open(&bar, scene, scene_hit, *message, &preferences)
                : MessageActionBarResult{};
    const u32 opened_revision = bar.revision;
    const MessageActionBarResult retained_open =
        message ? message_action_bar_open(&bar, scene, scene_hit, *message, &preferences)
                : MessageActionBarResult{};
    const bool retained_identity =
        retained_open.ok() && !retained_open.changed && bar.opens == 1 && opened_revision == bar.revision;
    const float item_y =
        opened.ok() ? (bar.items[0].rectangle.top + bar.items[0].rectangle.bottom) * 0.5f : 0.0f;
    const MessageActionBarHit quick_hit =
        opened.ok() ? message_action_bar_hit_test(
                          bar, (bar.items[0].rectangle.left + bar.items[0].rectangle.right) * 0.5f, item_y)
                    : MessageActionBarHit{};
    const MessageActionBarHit gap_hit =
        opened.ok() ? message_action_bar_hit_test(
                          bar, (bar.items[2].rectangle.right + bar.items[3].rectangle.left) * 0.5f, item_y)
                    : MessageActionBarHit{};
    const MessageActionBarPointerResult tracked =
        opened.ok() ? message_action_bar_track_pointer(
                          &bar, &scene, (bar.items[3].rectangle.left + bar.items[3].rectangle.right) * 0.5f,
                          item_y, &preferences)
                    : MessageActionBarPointerResult{};

    PaintStream paint{};
    paint_stream_reset(&paint, viewport.right, viewport.bottom);
    UiTextCommand text_commands[kMessageActionBarQuickReactionCount]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    const bool presented =
        opened.ok() &&
        ui_text_command_stream_initialize(
            &text, Slice<UiTextCommand>{text_commands, kMessageActionBarQuickReactionCount}) &&
        message_action_bar_presentation_build(bar, kDefaultTheme, &paint) &&
        message_action_bar_text_presentation_build_output(bar, kDefaultTheme, &text_output);
    u32 action_icons = 0;
    for (u32 i = 0; i < paint.count; ++i)
        action_icons += ui_icon_quad(paint.quads[i]) ? 1u : 0u;
    expect(opened.ok() && opened.changed && retained_identity && row_layout && bar.active &&
               bar.item_count == kMessageActionBarItemCount && ui_rect_width(bar.rectangle) == 180.0f &&
               ui_rect_height(bar.rectangle) == 32.0f &&
               bar.rectangle.right == row_layout->rectangle.right - kMessageActionBarRowRightInset &&
               ui_rect_width(bar.items[0].rectangle) == kMessageActionBarCellExtent &&
               bar.items[3].rectangle.left - bar.items[2].rectangle.right == kMessageActionBarSectionGap &&
               bar.items[0].catalog_index == shark_index &&
               quick_hit.action == MessageActionBarAction::quick_reaction_0 && quick_hit.message_id == 72 &&
               gap_hit.inside && gap_hit.action == MessageActionBarAction::none && tracked.ok() &&
               tracked.changed && tracked.action_bar.action == MessageActionBarAction::add_reaction &&
               bar.hovered_index == 3 && presented && action_icons >= 3 && text.count == 3 &&
               text.storage[0].style_flags == text_style_picker_emoji &&
               text.storage[0].horizontal_alignment == UiTextAlignment::center,
           "message_action_bar_verified_geometry_hit_and_presentation");

    Theme action_theme = kDefaultTheme;
    action_theme.background = background_theme_defaults();
    paint_stream_reset(&paint);
    const bool action_gradient = message_action_bar_presentation_build(bar, action_theme, &paint);
    expect(action_gradient && paint.count && paint.quads[0].border_width == -7 &&
               paint.quads[0].reserved_x == -1 && paint.quads[0].reserved_y > 0 &&
               paint.quads[0].left == bar.rectangle.left && paint.quads[0].right == bar.rectangle.right,
           "message_action_bar_uses_gradient_and_palette_border_with_hover");

    const UiRect narrow_viewport{100.0f, 40.0f, 620.0f, 400.0f};
    const MessageSceneResult rearranged = message_scene_update(&scene, narrow_viewport);
    bool synchronized_changed = false;
    const bool synchronized =
        rearranged.ok() && message_action_bar_synchronize(&bar, scene, &synchronized_changed);
    const MessageSceneRowWidget* synchronized_row = message_scene_find_row(&scene, 1);
    const UiLayoutNode* synchronized_layout =
        synchronized_row ? message_scene_layout_node(scene, synchronized_row->widget) : nullptr;
    bool closed_changed = false;
    const bool closed = message_action_bar_close(&bar, &closed_changed);
    MessageRecord pending = message ? *message : MessageRecord{};
    pending.delivery = MessageDeliveryState::pending;
    const MessageActionBarResult rejected =
        synchronized_row ? message_action_bar_open(&bar, scene,
                                                   MessageSceneHit{synchronized_row->message_id,
                                                                   synchronized_row->widget,
                                                                   synchronized_row->source_index},
                                                   pending, &preferences)
                         : MessageActionBarResult{};
    expect(preference_ready && synchronized && synchronized_changed && synchronized_layout &&
               bar.rectangle.right <= narrow_viewport.right && closed && closed_changed && !bar.active &&
               rejected.error == MessageActionBarError::unsupported_message,
           "message_action_bar_virtualization_sync_close_and_policy");

    const MessageSceneHit forward_hit{synchronized_row ? synchronized_row->message_id : 0,
        synchronized_row ? synchronized_row->widget : ui_widget_invalid_handle(),
        synchronized_row ? synchronized_row->source_index : 0};
    const bool forward_opened = message && message_action_bar_open(&bar, scene, forward_hit, *message,
        &preferences, MessageActionBarOptions{message_action_bar_forward}).ok();
    const auto arrow = bar.items[5].rectangle;
    const float arrow_x = (arrow.left + arrow.right) * .5f, arrow_y = (arrow.top + arrow.bottom) * .5f;
    paint_stream_reset(&paint);
    expect(forward_opened && bar.item_count == kMessageActionBarMaximumItems &&
        ui_rect_width(bar.rectangle) == 208.f && bar.items[5].action == MessageActionBarAction::forward &&
        bar.items[6].action == MessageActionBarAction::more &&
        message_action_bar_hit_test(bar, arrow_x, arrow_y).action == MessageActionBarAction::forward &&
        message_action_bar_presentation_build(bar, kDefaultTheme, &paint),
        "message_action_bar_forward_launcher_uses_bounded_native_arrow_before_more");
    bool forward_allowed = false;
    const auto revoked = message_action_bar_track_pointer(&bar, &scene, arrow_x, arrow_y, &preferences,
        +[](void* context, u64) noexcept -> u16 {
            return *static_cast<bool*>(context) ? u16{message_action_bar_forward} : u16{0};
        }, &forward_allowed);
    expect(revoked.ok() && revoked.changed && bar.item_count == kMessageActionBarItemCount &&
        revoked.action_bar.action != MessageActionBarAction::forward,
        "message_action_bar_revalidates_retained_hover_forward_authority_before_dispatch");
    MessageRecord poll_source = message ? *message : MessageRecord{};
    poll_source.presentation_flags |= message_presentation_has_poll;
    expect(message_action_bar_open(&bar, scene, forward_hit, poll_source, &preferences,
               MessageActionBarOptions{message_action_bar_forward}).ok() &&
        bar.item_count == kMessageActionBarItemCount,
        "message_action_bar_unsupported_source_cannot_gain_forward_from_caller_hint");
    message_action_bar_close(&bar);
}

void test_reaction_picker() noexcept {
    const auto section_of = [](const ReactionPickerModel& model, u32 local) noexcept {
        for (u32 i = 0; i < model.section_count; ++i)
            if (!model.sections[i].guild_id && model.sections[i].local == local)
                return i;
        return ~0u;
    };
    const auto catalog_at = [](const ReactionPickerModel& model, u32 index) noexcept {
        ReactionPickerSelection selection{};
        return reaction_picker_result_at(model, index, &selection) && !selection.custom ? selection.catalog_index
                                                                                         : ~0u;
    };
    constexpr u64 preference_now = 1'800'000'000'000ull;
    ReactionPreferenceStore seeded{};
    ReactionPickerModel picker{};
    const ReactionPickerResult opened = reaction_preferences_initialize(&seeded)
                                            ? reaction_picker_open(&picker, 77, &seeded, preference_now)
                                            : ReactionPickerResult{};
    const u32 frequent = section_of(picker, kReactionPickerSectionFrequent);
    const u32 first = frequent != ~0u ? catalog_at(picker, picker.sections[frequent].first) : ~0u;
    bool categories_ordered = picker.section_count == 1 + kEmojiCategoryCount;
    for (u32 i = 0; categories_ordered && i < kEmojiCategoryCount; ++i)
        categories_ordered = picker.sections[1 + i].local == kReactionPickerSectionUnicode + i &&
                             picker.sections[1 + i].first == picker.sections[i].first + picker.sections[i].count;
    expect(opened.ok() && frequent == 0 && picker.result_count > unicode_emoji_catalog().count &&
               categories_ordered && first < unicode_emoji_catalog().count &&
               unicode_emoji_catalog()[first].name().bytes == utf8_literal("hundred_points").bytes,
           "reaction_picker_frecency_seed_then_unicode_categories");

    ReactionPreferenceStore preferences{};
    ReactionPickerModel personalized{};
    const ReactionPickerResult personalized_opened =
        reaction_preferences_initialize(&preferences)
            ? reaction_picker_open(&personalized, 88, &preferences, preference_now)
            : ReactionPickerResult{};
    const u32 personalized_frequent = section_of(personalized, kReactionPickerSectionFrequent);
    const u32 hundred_index = personalized_frequent != ~0u
                                  ? catalog_at(personalized, personalized.sections[personalized_frequent].first)
                                  : ~0u;
    const u32 thumbs_index =
        personalized_frequent != ~0u
            ? catalog_at(personalized, personalized.sections[personalized_frequent].first + 1)
            : ~0u;
    const ReactionPreferenceUsage* hundred_usage = nullptr;
    for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index) {
        if (preferences.tables[0].usage[index].occupied && preferences.tables[0].usage[index].emoji.catalog_index == hundred_index) {
            hundred_usage = &preferences.tables[0].usage[index];
            break;
        }
    }
    expect(personalized_opened.ok() && preferences.tables[0].seeded && preferences.tables[0].usage_count == 10 &&
               preferences.tables[0].frequent_count == 10 && hundred_index != ~0u && thumbs_index != ~0u &&
               hundred_usage && hundred_usage->total_uses == 2 && hundred_usage->recent_count == 2 &&
               hundred_usage->score == 200 && hundred_usage->frecency == 360,
           "reaction_preferences_exact_seed_and_frecency");

    const ReactionPreferenceResult favorited =
        thumbs_index <= 0xFFFEu
            ? reaction_preferences_toggle_favorite_emoji(&preferences,
                                                         reaction_emoji_unicode(static_cast<u16>(thumbs_index)))
            : ReactionPreferenceResult{};
    const ReactionPickerResult favorite_refreshed =
        favorited.ok() ? reaction_picker_refresh_preferences(&personalized) : ReactionPickerResult{};
    const u32 favorite_first =
        personalized.section_count && personalized.sections[0].local == kReactionPickerSectionFavorites &&
                personalized.sections[0].count
            ? catalog_at(personalized, personalized.sections[0].first)
            : ~0u;
    expect(favorited.ok() && favorited.changed && favorited.favorite && favorite_refreshed.ok() &&
               favorite_first == thumbs_index &&
               reaction_preferences_is_favorite_emoji(preferences,
                                                      reaction_emoji_unicode(static_cast<u16>(thumbs_index))) &&
               personalized.sections[0].count == 1 &&
               personalized.sections[1].local == kReactionPickerSectionFrequent,
           "reaction_picker_alt_favorite_priority_precedes_frequently_used");

    ReactionPreferenceStore bounded{};
    const bool bounded_ready = reaction_preferences_initialize(&bounded);
    bool favorites_filled = bounded_ready;
    for (u32 index = 0; index < kReactionPreferenceFavoriteCapacity && favorites_filled; ++index) {
        favorites_filled =
            reaction_preferences_toggle_favorite_emoji(&bounded, reaction_emoji_unicode(static_cast<u16>(index))).ok();
    }
    const ReactionPreferenceResult favorite_overflow = reaction_preferences_toggle_favorite_emoji(
        &bounded, reaction_emoji_unicode(static_cast<u16>(kReactionPreferenceFavoriteCapacity)));
    ReactionPreferenceStore samples{};
    bool samples_ready = reaction_preferences_initialize(&samples);
    for (u32 use = 1; use <= 12 && samples_ready; ++use) {
        samples_ready = reaction_preferences_track_emoji(&samples, ReactionPreferenceKind::reaction,
                                                         reaction_emoji_unicode(7), preference_now + use, 1).ok();
    }
    const ReactionPreferenceUsage* retained = nullptr;
    for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index) {
        if (samples.tables[0].usage[index].occupied && samples.tables[0].usage[index].emoji.catalog_index == 7) {
            retained = &samples.tables[0].usage[index];
            break;
        }
    }
    expect(favorites_filled && bounded.favorite_count == 250 &&
               favorite_overflow.error == ReactionPreferenceError::capacity && samples_ready && retained &&
               retained->total_uses == 12 && retained->recent_count == 10 &&
               retained->recent_uses[0] == preference_now + 3 &&
               retained->recent_uses[9] == preference_now + 12,
           "reaction_preferences_exact_caps_and_recent_sample_window");

    ReactionPreferenceStore aging{};
    constexpr u64 day_ms = 24ull * 60ull * 60ull * 1'000ull;
    bool aging_ready = reaction_preferences_initialize(&aging);
    const u32 ages[] = {3, 15, 30, 45, 80, 81};
    for (u32 index = 0; index < 6 && aging_ready; ++index) {
        aging_ready = reaction_preferences_track_emoji(&aging, ReactionPreferenceKind::reaction,
                                                       reaction_emoji_unicode(static_cast<u16>(100 + index)),
                                                       preference_now - ages[index] * day_ms)
                          .ok();
    }
    // A use at preference_now recomputes every age against it.
    aging_ready = aging_ready && reaction_preferences_track_emoji(&aging, ReactionPreferenceKind::reaction,
                                                                  reaction_emoji_unicode(99), preference_now).ok();
    const u16 expected_scores[] = {100, 70, 50, 30, 10, 1};
    const u16 expected_frecencies[] = {280, 256, 240, 224, 208, 200};
    bool exact_aging = aging_ready;
    for (u32 expected = 0; expected < 6 && exact_aging; ++expected) {
        bool found_usage = false;
        for (u32 index = 0; index < kReactionPreferenceUsageCapacity; ++index) {
            const ReactionPreferenceUsage& usage = aging.tables[0].usage[index];
            if (usage.occupied && usage.emoji.catalog_index == 100 + expected) {
                found_usage = usage.score == expected_scores[expected] &&
                              usage.frecency == expected_frecencies[expected];
                break;
            }
        }
        exact_aging = found_usage;
    }
    expect(exact_aging, "reaction_preferences_exact_age_weight_buckets");

    reaction_picker_open(&picker, 77, &seeded, preference_now);
    const auto query = utf8_literal("thumbs_up");
    bool searched = picker.search && message_composer_restore(picker.search, 1, query, query.bytes, query.bytes) &&
                    reaction_picker_refresh_search(&picker).ok();
    const u32 match = catalog_at(picker, 0);
    expect(searched && picker.section_count == 1 && picker.sections[0].local == kReactionPickerSectionSearch &&
               match < unicode_emoji_catalog().count &&
               unicode_emoji_catalog()[match].name().bytes == query.bytes &&
               emoji_catalog_name_starts_with(unicode_emoji_catalog()[match].name(), query),
           "reaction_picker_prefix_search_ranks_exact_prefix_first");
    bool search_edit = picker.search && message_composer_enable_history(picker.search, 4096);
    const auto words = utf8_literal("sad face");
    search_edit = search_edit && message_composer_restore(picker.search, 1, words, words.bytes, words.bytes) &&
        reaction_picker_refresh_search(&picker).ok() && picker.query_bytes == words.bytes;
    const u32 phrase_results = picker.result_count;
    search_edit = search_edit &&
        message_composer_delete_word(picker.search, false) == MessageComposerResult::changed &&
        reaction_picker_refresh_search(&picker).ok() && picker.query_bytes == 4 &&
        picker.result_count >= phrase_results &&
        message_composer_undo(picker.search) == MessageComposerResult::changed &&
        reaction_picker_refresh_search(&picker).ok() && picker.result_count == phrase_results;
    const auto absent = utf8_literal("zzzzzzzzzzzzzzzzz");
    search_edit = search_edit && message_composer_restore(picker.search, 1, absent, absent.bytes, absent.bytes) &&
        reaction_picker_refresh_search(&picker).ok() && picker.result_count == 0;
    message_composer_clear(picker.search);
    search_edit = search_edit && reaction_picker_refresh_search(&picker).ok() &&
        picker.section_count == 1 + kEmojiCategoryCount && !picker.query_bytes;
    const auto unchanged = reaction_picker_refresh_search(&picker);
    expect(search_edit && unchanged.ok() && !unchanged.changed,
           "emoji_search_shared_word_edit_undo_empty_result_and_browse_restore");
    reaction_picker_release(&personalized);
    reaction_picker_release(&picker);
    expect(sizeof(ReactionPickerModel) < 512 && picker.storage.base == nullptr &&
               picker.results == nullptr,
           "reaction_picker_releases_lazy_working_set");
}

void test_emoji_category_rail() noexcept {
    ChatStateStore state{};
    GuildRecord guilds[8]{}; ChannelRecord channels[1]{};
    u16 guild_order[8]{}, channel_order[1]{}; u8 names[256]{};
    CustomEmojiRecord emojis[8]{}; u64 roles[1]{};
    bool valid = chat_state_store_initialize(&state, {guilds, 8}, {guild_order, 8},
        {channels, 1}, {channel_order, 1}, {names, 256});
    for (u32 i = 0; valid && i < 8; ++i) {
        valid = chat_state_store_upsert_guild(&state, {100 + i, utf8_literal("Studio"), GuildMutationKind::create,
            0, utf8_literal("abcdefabcdefabcdefabcdefabcdefab"), true}).ok();
        emojis[i].id = 500 + i; emojis[i].guild_id = 100 + i;
        emojis[i].flags = custom_emoji_available; emojis[i].name_bytes = 4;
        copy_bytes(emojis[i].name, "blob", 4);
    }
    state.emojis = emojis; state.emoji_count = 8; state.emoji_roles = roles;
    ReactionPickerModel picker{};
    ReactionPreferenceStore preferences{};
    valid = valid && reaction_preferences_initialize(&preferences) &&
            reaction_picker_open(&picker, 1, &preferences, 1000, &state, 103).ok();
    bool server_order = valid && picker.section_count == 1 + 8 + kEmojiCategoryCount &&
                        picker.sections[0].local == kReactionPickerSectionFrequent;
    for (u32 i = 0; server_order && i < 8; ++i)
        server_order = picker.sections[1 + i].local == kReactionPickerSectionGuild &&
                       picker.sections[1 + i].guild_id == picker.guilds[i].id &&
                       utf8_equal(reaction_picker_section_label(picker, 1 + i), utf8_literal("Studio"));
    expect(server_order && picker.guild_count == 8 && picker.custom_emoji_count == 8 &&
               picker.guilds[0].id == 103 && picker.guilds[0].icon_variant,
           "emoji_categories_list_every_server_after_frequently_used_starting_with_the_selected_one");
    ReactionPickerSelection selected_emoji{};
    const auto& seventh = picker.sections[1 + 7];
    expect(valid && seventh.count == 1 &&
               reaction_picker_result_at(picker, seventh.first, &selected_emoji) && selected_emoji.custom &&
               selected_emoji.emoji_id == 500 + (picker.guilds[7].id - 100) &&
               utf8_equal(selected_emoji.name, utf8_literal("blob")),
           "server_category_spans_its_own_named_emoji");
    reaction_picker_release(&picker);
}

} // namespace maicord
