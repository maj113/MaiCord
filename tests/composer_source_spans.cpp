#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/code_highlight.hpp"

namespace maicord {
namespace {

bool exact(Utf8View input, ConstSlice<TextStyleSpan> expected) noexcept {
    TextStyleSpan actual[256]{};
    const u32 count = message_markdown_source_styles(input, {actual, 256});
    if (count != expected.count) return false;
    for (u32 i = 0; i < count; ++i)
        if (actual[i].byte_offset != expected[i].byte_offset || actual[i].byte_count != expected[i].byte_count ||
            actual[i].flags != expected[i].flags) return false;
    return true;
}

void expand_highlight(void* context, u32 begin, u32 end, u32 flags) noexcept {
    auto* styles = static_cast<u32*>(context);
    for (u32 i = begin; i < end; ++i) styles[i] |= flags;
}

} // namespace

void test_composer_source_spans(void (*check)(bool, const char*) noexcept) noexcept {
    const TextStyleSpan nested[]{{2, 5, text_style_bold}, {8, 4, text_style_bold | text_style_italic},
        {13, 4, text_style_bold}};
    check(exact(utf8_literal("**bold *both* end**"), {nested, 3}),
        "composer source spans preserve exact nested delimiter gaps");
    const TextStyleSpan escaped[]{{2, 2, text_style_bold}, {5, 2, text_style_bold}};
    check(exact(utf8_literal("**a \\*b**"), {escaped, 2}),
        "composer source spans leave escape slash plain inside bold");
    const TextStyleSpan masked[]{{3, 1, text_style_bold | text_style_link}};
    check(exact(utf8_literal("[**x**](https://a)"), {masked, 1}),
        "composer source spans combine masked label styles without styling URL");
    const TextStyleSpan slash[]{{2, 11, text_style_bold | text_style_mention}};
    check(exact(utf8_literal("**</ping:222>**"), {slash, 1}),
        "composer source spans keep a command mention tag whole like other entity tags");
    const TextStyleSpan empty_fence[]{{2, 5, text_style_bold}, {14, 2, text_style_bold}};
    check(exact(utf8_literal("```cpp\n```"), {}) &&
        exact(utf8_literal("**a ```cpp\n``` b**"), {empty_fence, 2}),
        "composer source spans preserve empty language fence opening marker");
    constexpr u32 code = text_style_monospace | text_style_code_block;
    const TextStyleSpan highlighted[]{{7, 3, code | text_style_syntax_type}, {10, 2, code},
        {12, 1, code | text_style_syntax_operator}, {13, 1, code | text_style_syntax_number}, {14, 2, code}};
    check(exact(utf8_literal("```cpp\nint x=1;\n```"), {highlighted, 5}),
        "composer source spans stream code lexer tokens and plain gaps exactly");
    const TextStyleSpan bare_fence[]{{5, 5, code}};
    check(exact(utf8_literal("```\n\nbody\n```"), {bare_fence, 1}),
        "composer source spans style a fence opened on its own line from its first code line");

    char siblings[257 * 3]{};
    for (u32 i = 0; i < 257; ++i) { siblings[3 * i] = '*'; siblings[3 * i + 1] = 'x'; siblings[3 * i + 2] = '_'; }
    TextStyleSpan spans[256]{};
    const u32 count = message_markdown_source_styles({siblings, sizeof(siblings)}, {spans, 256});
    bool prefix = count == 256;
    for (u32 i = 0; i < count; ++i)
        prefix = prefix && spans[i].byte_offset == 3 * i + 1 && spans[i].byte_count == 1 && spans[i].flags == text_style_italic;
    check(prefix,
        "composer source spans truncate source prefix while transformed run fits");
    char separated[257 * 4]{};
    for (u32 i = 0; i < 257; ++i) {
        separated[4 * i] = '*'; separated[4 * i + 1] = 'x'; separated[4 * i + 2] = '_'; separated[4 * i + 3] = ' ';
    }
    check(message_markdown_source_styles({separated, sizeof(separated)}, {spans, 256}) == 0,
        "composer source spans reject transformed run overflow after prefix saturation");

    char depth[40]{};
    for (u32 i = 0; i < 33; ++i) depth[i] = '[';
    check(message_markdown_source_styles({depth, 33}, {spans, 256}) == 0,
        "composer source spans preserve delimiter depth error");

    const char invalid_utf8[]{static_cast<char>(0xC0), static_cast<char>(0xAF)};
    check(message_markdown_source_styles({invalid_utf8, 2}, {spans, 256}) == 0,
        "composer source spans reject invalid UTF8 before emitting");

    char fences[128]{};
    u32 used = 0;
    for (u32 block = 0; block < 2; ++block) {
        const auto opening = utf8_literal("```cpp\n");
        for (u32 i = 0; i < opening.bytes; ++i) fences[used++] = opening.data[i];
        for (u32 i = 0; i < 25; ++i) { fences[used++] = '1'; fences[used++] = ' '; }
        for (u32 i = 0; i < 3; ++i) fences[used++] = '`';
        if (block == 0) fences[used++] = '\n';
    }
    const u32 fence_count = message_markdown_source_styles({fences, used}, {spans, 256});
    u32 number_runs = 0;
    bool shared_budget = true;
    for (u32 i = 0; i < fence_count; ++i) {
        if (spans[i].flags & text_style_syntax_number) {
            ++number_runs;
            shared_budget = shared_budget && spans[i].byte_count == 1;
        }
    }
    check(shared_budget && number_runs == 48,
        "composer source spans share 48 syntax tokens across fenced blocks");

    const auto token_source = utf8_literal("const x = 42; // comment\n\"string\" <tag a='b'> -- comment\n# command");
    bool lexer_equal = true;
    constexpr u32 token_capacities[]{0, 1, 47, 48};
    for (u32 language = static_cast<u32>(CodeLanguage::c); language <= static_cast<u32>(CodeLanguage::css); ++language) {
        for (const u32 capacity : token_capacities) {
            u32 byte_styles[128]{}, range_styles[128]{};
            const auto a = code_highlight(token_source, static_cast<CodeLanguage>(language), {byte_styles, 128}, capacity);
            const auto b = code_highlight_ranges(token_source, static_cast<CodeLanguage>(language), expand_highlight, range_styles, capacity);
            lexer_equal = lexer_equal && a.token_count == b.token_count && a.styled_bytes == b.styled_bytes && a.truncated == b.truncated;
            for (u32 i = 0; i < token_source.bytes; ++i) lexer_equal = lexer_equal && byte_styles[i] == range_styles[i];
        }
    }
    check(lexer_equal, "composer range lexer preserves byte adapter styles and token admission");
}

} // namespace maicord
