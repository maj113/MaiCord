#include "embed_parity_internal.hpp"
#include "maicord/text/layout/visibility.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/ui/chat/messages/author.hpp"
#include "maicord/ui/text/ansi_text.hpp"
#include "maicord/ui/chat/messages/interaction.hpp"
#include "maicord/ui/chat/messages/system_text.hpp"
#include "maicord/protocol/messages/interaction_data.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/text/layout/paragraph_hits.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/sys/dwrite.hpp"
#include "../../../support/test_output.hpp"
#include "../../../support/gateway_message_scratch.hpp"

namespace maicord {
namespace embed_parity_detail {

TextSystem text_system;
TextGlyph glyphs[1024];
TextRun runs[256];
u16 clusters[1024];

} // namespace embed_parity_detail

namespace {

TextHitRegion hit_regions[64];

} // namespace

namespace embed_parity_detail {

TextDecoration decorations[64];

bool simple_script_covered[sizeof(kSimpleScripts) / sizeof(kSimpleScripts[0])];

} // namespace embed_parity_detail

namespace {

// Whether any installed family (its regular face) maps the scalar, as an
// independent DirectWrite oracle for the native fallback.
void probe_system_coverage() noexcept {
    IDWriteFactory* factory = nullptr;
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&factory))) ||
        FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection) {
        com_release(factory);
        return;
    }
    const UINT32 families = collection->GetFontFamilyCount();
    for (UINT32 f = 0; f < families; ++f) {
        IDWriteFontFamily* family = nullptr;
        IDWriteFont* font = nullptr;
        if (SUCCEEDED(collection->GetFontFamily(f, &family)) &&
            SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, &font)))
            for (u32 i = 0; i < sizeof(kSimpleScripts) / sizeof(kSimpleScripts[0]); ++i) {
                BOOL exists = FALSE;
                if (!simple_script_covered[i] && SUCCEEDED(font->HasCharacter(kSimpleScripts[i], &exists)))
                    simple_script_covered[i] = exists != FALSE;
            }
        if (font)
            font->Release();
        if (family)
            family->Release();
    }
    collection->Release();
    factory->Release();
}

} // namespace

namespace embed_parity_detail {

bool close_to(float a, float b) noexcept {
    return a > b - .01f && a < b + .01f;
}
bool ends_with(Utf8View text, Utf8View part) noexcept {
    return part.bytes <= text.bytes && bytes_equal(text.data + text.bytes - part.bytes, part.data, part.bytes);
}

u32 encode(u32 cp, char* out) noexcept {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
        out[2] = static_cast<char>(0x80 | (cp & 63));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 63));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 63));
    out[3] = static_cast<char>(0x80 | (cp & 63));
    return 4;
}

Shaped shape_styled(Utf8View text, ConstSlice<TextStyleSpan> styles, float width) noexcept {
    ShapedText output{};
    Shaped result{};
    if (!shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}))
        return result;
    result.ok = text_shape_utf8(&text_system, {text, styles, width, 4096}, &output).ok();
    for (u32 i = 0; i < output.glyph_count; ++i) {
        result.notdef += output.glyphs[i].index == 0 ? 1u : 0u;
        result.advance += output.glyphs[i].advance;
    }
    result.lines = output.line_count;
    result.width = output.width;
    result.height = output.height;
    shaped_text_reset(&output);
    return result;
}

} // namespace embed_parity_detail

namespace {

// Every Default_Ignorable code point is invisible in the official client (the
// browser hides it). FE0F after a plain letter, U+2065, Hangul fillers, tags and
// variation selectors once drew tofu or took width.
void test_default_ignorables(Check check) noexcept {
    const u32 hidden[]{0x034F, 0x115F, 0x1160, 0x17B4, 0x17B5, 0x180B, 0x180E, 0x200B, 0x200C, 0x200D,
                       0x2062, 0x2064, 0x2065, 0x3164, 0xFE0E, 0xFE0F, 0xFFA0, 0xFFF0, 0xE0001, 0xE0020};
    const Shaped plain = shape_styled(utf8_literal("abcd"));
    const Shaped line = shape_styled(utf8_literal("Hello"));
    bool invisible = plain.ok && line.ok && plain.notdef == 0;
    for (u32 cp : hidden) {
        for (u32 form = 0; form < 2; ++form) {
            // "ab XX cd" mid-line and "XX Hello" at a line start (embed lines).
            char text[32]{};
            u32 n = 0;
            if (!form) {
                text[n++] = 'a';
                text[n++] = 'b';
            }
            n += encode(cp, text + n);
            n += encode(cp, text + n);
            const char* tail = form ? "Hello" : "cd";
            for (u32 i = 0; tail[i]; ++i)
                text[n++] = tail[i];
            const Shaped s = shape_styled(Utf8View{text, n});
            const float expected = form ? line.width : plain.width;
            invisible = invisible && s.ok && s.notdef == 0 && s.width > expected - 0.5f && s.width < expected + 0.5f;
        }
    }
    check(invisible, "default_ignorables_shape_without_tofu_or_advance");
    const Shaped braille = shape_styled(utf8_literal("\xE2\xA0\x80Hi"));
    check(braille.ok && braille.notdef == 0 && braille.width > shape_styled(utf8_literal("Hi")).width,
          "braille_blank_is_a_visible_blank_not_tofu");
    // Emoji presentation sequences keep their selector.
    const Shaped heart = shape_styled(utf8_literal("\xE2\x9D\xA4\xEF\xB8\x8F"));
    check(heart.ok && heart.notdef == 0 && heart.width > 8.0f, "emoji_variation_sequence_still_renders");

    char out[32]{};
    check(text_hide_default_ignorables(utf8_literal("\xE2\x9D\xA4\xEF\xB8\x8F ok"), {out, 32}) == 0 &&
              text_hide_default_ignorables(utf8_literal("\xE1\x84\x80\xE1\x85\xA1"), {out, 32}) == 0 &&
              text_hide_default_ignorables(utf8_literal("\xE4\xB8\x80\xF3\xA0\x84\x80"), {out, 32}) == 0 &&
              text_hide_default_ignorables(utf8_literal("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5"
                                                        "\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF"), {out, 32}) == 0,
          "consumed_selectors_jamo_and_flag_tags_stay_for_shaping");
    const u32 bytes = text_hide_default_ignorables(utf8_literal("b\xEF\xB8\x8F\xF3\xA0\x84\x80!"), {out, 32});
    check(bytes == 9 && bytes_equal(out, "b\xE2\x80\x8C\xCD\x8F\xCD\x8F!", 9),
          "orphan_selectors_become_same_length_invisible_scalars");
}

// No missing glyph, no advance and no measured width.
bool draws_nothing(Utf8View text) noexcept {
    const Shaped g = shape_styled(text);
    const auto m = text_measure_utf8(&text_system, {text, {}, 1000, 1000});
    return g.ok && g.notdef == 0 && close_to(g.width, 0) && close_to(g.advance, 0) && m.ok() && close_to(m.width, 0);
}
// "[#ff0000,#000000]" in tag characters: hidden profile colours in a bio.
constexpr u32 kHiddenColors[]{0xE005B, 0xE0023, 0xE0066, 0xE0066, 0xE0030, 0xE0030, 0xE0030, 0xE0030, 0xE002C,
                              0xE0023, 0xE0030, 0xE0030, 0xE0030, 0xE0030, 0xE0030, 0xE0030, 0xE005D};

u32 append(char* out, u32 n, const char* ascii) noexcept {
    for (u32 i = 0; ascii[i]; ++i)
        out[n++] = ascii[i];
    return n;
}
u32 append_hidden_colors(char* out, u32 n) noexcept {
    for (u32 cp : kHiddenColors)
        n += encode(cp, out + n);
    return n;
}

// Default-ignorables without a base draw nothing and take no width: alone,
// leading, on lines of their own and at wrapped line ends. They once failed the
// native item, whose fallback drew one missing glyph.
void test_baseless_ignorables(Check check) noexcept {
    char text[256];
    u32 n = append_hidden_colors(text, 0);
    const auto measured = text_measure_utf8(&text_system, {{text, n}, {}, 1000, 1000});
    check(n == 68 && measured.utf16_units == 34 && draws_nothing({text, n}), "hidden_tag_bio_draws_nothing");

    const float hello = shape_styled(utf8_literal("Hello")).width;
    n = append(text, append_hidden_colors(text, append(text, 0, "hi ")), " x");
    const Shaped inside = shape_styled({text, n});
    const Shaped plain = shape_styled(utf8_literal("hi  x"));
    n = append_hidden_colors(text, append(text, 0, "Hello\n"));
    const Shaped after_break = shape_styled({text, n});
    check(inside.ok && inside.notdef == 0 && close_to(inside.width, plain.width) && after_break.ok &&
              after_break.notdef == 0 && after_break.lines == 2 && close_to(after_break.width, hello),
          "hidden_tags_inside_text_take_no_width");

    // Lines holding only ignorables, and ignorables ending wrapped lines.
    n = append(text, append_hidden_colors(text, 0), "\n\xCD\x8F\n\xE2\x80\x8C\xE2\x81\xA0");
    const Shaped only = shape_styled({text, n});
    const float narrow = hello + 4;
    const Shaped wrapped_plain = shape_styled(utf8_literal("Hello world"), {}, narrow);
    n = append_hidden_colors(text, append(text, append_hidden_colors(text, append(text, 0, "Hello")), " world"));
    const Shaped wrapped = shape_styled({text, n}, {}, narrow);
    check(only.ok && only.notdef == 0 && only.lines == 3 && close_to(only.width, 0) && wrapped_plain.lines == 2 &&
              wrapped.ok && wrapped.notdef == 0 && wrapped.lines == 2 && close_to(wrapped.width, wrapped_plain.width),
          "ignorable_only_lines_and_wrapped_line_ends_take_no_width");

    // Each ignorable alone, doubled, on two lines and leading a word.
    const u32 ignorables[]{0x00AD, 0x034F, 0x061C, 0x115F, 0x180E, 0x200B, 0x200C, 0x200D, 0x200E, 0x2060,
                           0x2064, 0x3164, 0xFE00, 0xFE0F, 0xFEFF, 0xE0001, 0xE0020, 0xE007F, 0xE0100};
    bool alone = true, leading = true;
    for (u32 cp : ignorables) {
        n = encode(cp, text);
        alone = alone && draws_nothing({text, n});
        n += encode(cp, text + n);
        alone = alone && draws_nothing({text, n});
        n = append(text, encode(cp, text), "\n");
        n += encode(cp, text + n);
        alone = alone && draws_nothing({text, n});
        n = append(text, encode(cp, text), "Hello");
        const Shaped word = shape_styled({text, n});
        leading = leading && word.ok && word.notdef == 0 && close_to(word.width, hello);
    }
    check(alone, "each_ignorable_alone_draws_nothing");
    check(leading, "each_ignorable_leading_a_word_takes_no_width");

    // Subdivision flags keep their tags; hidden tags beside a flag add nothing.
    const auto england = utf8_literal("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5"
                                      "\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF");
    const Shaped flag = shape_styled(england);
    n = append_hidden_colors(text, 0);
    copy_bytes(text + n, england.data, england.bytes);
    n = append_hidden_colors(text, n + england.bytes);
    const Shaped flanked = shape_styled({text, n});
    check(flag.ok && flag.notdef == 0 && flag.width > 8 && flanked.ok && flanked.notdef == 0 &&
              close_to(flanked.width, flag.width),
          "subdivision_flag_keeps_tags_beside_hidden_tags");

    // Carets over hidden text keep the source UTF-16 offsets: "hi " + tags +
    // " x" has the carets of "hi  x", the space and tags forming one cluster.
    NativeParagraphHitCluster hit_clusters[2][64];
    NativeParagraphHitLine hit_lines[2][4];
    float advances[2][256];
    SfntFontLineParts parts[2][2];
    NativeParagraphHits geometry[2]{};
    const auto hits = [&](u32 i, Utf8View source) noexcept {
        const TextShapingView shaping(source);
        NativeParagraphHitStorage storage{{hit_clusters[i], 64}, {hit_lines[i], 4}, {advances[i], 256}, &geometry[i],
                                          {parts[i], 2}};
        TextMeasureResult measure{};
        return shaping.text.bytes == source.bytes &&
               native_text_candidate_hits(text_system, {shaping.text, {}, 1000, 1000, TextWrapping::wrap}, storage, &measure) &&
               measure.ok() && geometry[i].source_units == measure.utf16_units;
    };
    const auto caret = [&](u32 i, u32 unit) noexcept {
        NativeParagraphHitMetric metric{};
        float x = -1;
        return native_paragraph_hit_position(geometry[i], unit, false, &metric, &x) == SfntStatus::ok ? x : -1.0f;
    };
    n = append(text, append_hidden_colors(text, append(text, 0, "hi ")), " x");
    bool carets = hits(0, {text, n}) && hits(1, utf8_literal("hi  x")) && geometry[0].source_units == 39;
    for (u32 unit = 0; carets && unit <= 39; ++unit) {
        if (unit > 3 && unit < 37 && (unit & 1) == 0)
            continue; // inside a surrogate pair
        const u32 plain_unit = unit < 3 ? unit : unit < 37 ? 2 : unit - 34;
        carets = close_to(caret(0, unit), caret(1, plain_unit));
    }
    NativeParagraphHitPoint end{};
    carets = carets && native_paragraph_hit_point(geometry[0], 1000, 0, &end) == SfntStatus::ok &&
             end.source_position == 39;
    check(carets, "carets_over_hidden_tags_keep_source_offsets");
}

} // namespace

namespace embed_parity_detail {

MessageMarkdownResult parse(Utf8View source, char* output, u32* styles, TextStyleSpan* spans,
                            MessageMarkdownSemantic* semantics, MessageMarkdownOptions options) noexcept {
    return message_markdown_parse(source, Slice<char>{output, 1024}, Slice<u32>{styles, 1024},
                                  Slice<TextStyleSpan>{spans, 128}, options,
                                  Slice<MessageMarkdownSemantic>{semantics, 32});
}

} // namespace embed_parity_detail

namespace {

u32 style_at(const MessageMarkdownResult& result, const TextStyleSpan* spans, u32 byte) noexcept {
    for (u32 i = 0; i < result.span_count; ++i)
        if (byte >= spans[i].byte_offset && byte < spans[i].byte_offset + spans[i].byte_count)
            return spans[i].flags;
    return 0;
}

Utf8View semantic_source(Utf8View source, const MessageMarkdownSemantic& semantic) noexcept {
    return {source.data + semantic.source_offset, semantic.source_bytes};
}

void test_markdown(Check check) noexcept {
    static char output[1024];
    static u32 styles[1024];
    static TextStyleSpan spans[128];
    static MessageMarkdownSemantic semantics[32];
    // Escapes: a backslash escapes any ASCII punctuation or non-ASCII
    // character, never a letter, digit or space.
    auto result = parse(utf8_literal(R"([GOOD NEWS\!] Comet\.\.\. NOW\! \a \1 \_x\_ \\)"), output, styles, spans,
                        semantics);
    check(result.ok() && utf8_equal(result.text, utf8_literal(R"([GOOD NEWS!] Comet... NOW! \a \1 _x_ \)")),
          "markdown_escape_matches_official_rule");
    // A "[" that never becomes a masked link must not hold the delimiter
    // stack: the quote and bold that follow still apply.
    const Utf8View quoted = utf8_literal("[GOOD NEWS\\!]\n> quoted **line**\n>no space\n  > indented\nplain");
    result = parse(quoted, output, styles, spans, semantics);
    const u32 quote_line = 13; // "quoted"
    check(result.ok() && utf8_equal(result.text, utf8_literal("[GOOD NEWS!]\nquoted line\n>no space\nindented\nplain")) &&
              (style_at(result, spans, quote_line) & text_style_quote) &&
              (style_at(result, spans, quote_line + 7) & (text_style_quote | text_style_bold)) ==
                  (text_style_quote | text_style_bold) &&
              !(style_at(result, spans, 25) & text_style_quote) && (style_at(result, spans, 35) & text_style_quote) &&
              !(style_at(result, spans, result.text.bytes - 1) & text_style_quote),
          "markdown_blockquote_after_stray_bracket_and_with_indent");
    result = parse(utf8_literal(">>> first\n> second\n# big"), output, styles, spans, semantics);
    check(result.ok() && utf8_equal(result.text, utf8_literal("first\n> second\nbig")) &&
              (style_at(result, spans, 7) & text_style_quote) &&
              (style_at(result, spans, result.text.bytes - 1) & (text_style_quote | text_style_heading_1)) ==
                  (text_style_quote | text_style_heading_1),
          "markdown_multiline_quote_is_not_nested_and_keeps_block_rules");
    // Bare URLs, <url> and masked links (with <target>, nested parentheses
    // and unclosed emphasis in the text) are link semantics.
    const Utf8View links = utf8_literal("see https://e.test/a_b_c. (https://w.test/F_(x)) **https://b.test** "
                                        "<https://s.test/q> [a_b](<https://m.test/p>) https://x.test)");
    result = parse(links, output, styles, spans, semantics);
    const Utf8View expected_links[]{utf8_literal("https://e.test/a_b_c"), utf8_literal("https://w.test/F_(x)"),
                                    utf8_literal("https://b.test"), utf8_literal("https://s.test/q"),
                                    utf8_literal("https://m.test/p"), utf8_literal("https://x.test")};
    bool linked = result.ok() && result.semantic_count == 6;
    for (u32 i = 0; linked && i < 6; ++i)
        linked = semantics[i].kind == MessageMarkdownSemanticKind::link &&
                 utf8_equal(semantic_source(links, semantics[i]), expected_links[i]);
    check(linked && utf8_equal(result.text,
                                    utf8_literal("see https://e.test/a_b_c. (https://w.test/F_(x)) https://b.test "
                                                 "https://s.test/q a_b https://x.test)")),
          "markdown_autolinks_trim_punctuation_and_balance_parentheses");
    check(linked && (style_at(result, spans, 5) & text_style_link) && !(style_at(result, spans, 20) & text_style_italic) &&
              (style_at(result, spans, semantics[2].byte_offset) & (text_style_bold | text_style_link)) ==
                  (text_style_bold | text_style_link),
          "markdown_autolink_bytes_never_start_formatting");
    MessageMarkdownOptions title{};
    title.no_links = title.no_headings = title.no_lists = true;
    result = parse(utf8_literal("[t](https://e.test) https://e.test # x"), output, styles, spans, semantics, title);
    check(result.ok() && result.semantic_count == 0 &&
              utf8_equal(result.text, utf8_literal("[t](https://e.test) https://e.test # x")),
          "markdown_embed_title_rules_drop_links_and_headings");
    const Utf8View permalink = utf8_literal("https://canary.discord.com/channels/@me/2/3");
    result = parse(permalink, output, styles, spans, semantics);
    check(result.ok() && result.semantic_count == 1 &&
              semantics[0].kind == MessageMarkdownSemanticKind::message_link && semantics[0].target == 3 &&
              semantics[0].source_bytes == permalink.bytes,
          "markdown_message_links_cover_every_service_host");
}

void test_link_hits(Check check) noexcept {
    // A link wrapped over several lines inside quote and bold styling yields
    // one hit region per line, all for the link's semantic.
    const Utf8View text = utf8_literal("> **see https://example.com/a/very/long/path/that/wraps/over/lines now**");
    const TextStyleSpan styles[]{{0, 8, text_style_quote | text_style_bold},
                                 {8, 58, text_style_quote | text_style_bold | text_style_link},
                                 {66, text.bytes - 66, text_style_quote | text_style_bold}};
    const TextHitSpan hits[]{{8, 58, 0, 0}};
    ShapedText output{};
    bool ok = shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}, {hit_regions, 64});
    ok = ok && text_shape_utf8(&text_system, {text, {styles, 3}, 160, 1000, TextWrapping::wrap, TextOverflow::clip, {hits, 1}}, &output)
                   .ok();
    u32 regions = 0;
    float first_top = 0, last_top = 0;
    for (u32 i = 0; ok && i < output.hit_region_count; ++i) {
        const TextHitRegion& region = output.hit_regions[i];
        ok = ok && region.semantic_index == 0 && region.right > region.left && region.bottom > region.top;
        first_top = regions ? first_top : region.top;
        last_top = region.top;
        ++regions;
    }
    check(ok && regions >= 2 && last_top > first_top, "wrapped_link_is_hittable_on_every_line");
    shaped_text_reset(&output);
}

void test_calendar(Check check) noexcept {
    constexpr u64 stamp = 1'788'552'480; // 2026-09-04T20:08:00Z
    char text[128]{};
    const auto at = [&](u64 when, u64 now) noexcept {
        const auto formatted = format_timestamp_calendar(when, now, {text, sizeof(text)});
        return formatted.ok ? Utf8View{text, formatted.bytes} : Utf8View{};
    };
    check(utf8_starts_with(at(stamp, stamp + 86'400), utf8_literal("Yesterday at ")) &&
              utf8_starts_with(at(stamp, stamp), utf8_literal("Today at ")) &&
              utf8_starts_with(at(stamp + 86'400, stamp), utf8_literal("Tomorrow at ")) &&
              !test_contains(at(stamp + 5 * 86'400, stamp), utf8_literal(":")) &&
              test_contains(at(stamp, stamp + 10 * 86'400), utf8_literal(":")),
          "embed_footer_calendar_today_yesterday_tomorrow_and_dates");
}

} // namespace

namespace embed_parity_detail {

Fixture fixture;

} // namespace embed_parity_detail

namespace {

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* output) noexcept {
    auto result = text_measure_utf8(&text_system, {request.text, request.styles, request.constraint.width,
                                    request.constraint.height,
                                    request.no_wrap ? TextWrapping::no_wrap : TextWrapping::wrap});
    *output = {{result.width_including_trailing_whitespace, result.height}, result.line_count};
    return result.ok();
}

} // namespace

namespace embed_parity_detail {

bool finish(void*, const Theme&) noexcept {
    return true;
}

// Decoded views remain valid until the next payload; cases consume them before
// replacing the single shared scene. Share the largest existing scratch limits.
bool decode_message(Utf8View payload, GatewayMessageEvent* message) noexcept {
    static u32 structurals[4096];
    static char packed_text[32768];
    static u64 ids[256];
    static GatewayMessageComponent components[8];
    static GatewayMessageEmbed embeds[10];
    static GatewayMessageEmbedField fields[8];
    static GatewayMessageAttachment attachments[4];
    static GatewayMessageSticker stickers[3];
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {components, 8};
    scratch.embeds = {embeds, 10};
    scratch.embed_fields = {fields, 8};
    scratch.attachments = {attachments, 4};
    scratch.stickers = {stickers, 3};
    return gateway_decode_envelope(payload, {structurals, 4096}, 32, &envelope).ok() &&
           gateway_client_route_dispatch(envelope, {structurals, 4096}, 32,
               {packed_text, sizeof(packed_text)}, {ids, 256}, &routed, &packed, gateway_test_message_scratch()).ok() &&
           gateway_client_event_as_message(routed, packed, scratch, message);
}

bool initialize_fixture(const GatewayMessageEvent& message, float height) noexcept {
    auto& f = fixture;
    return message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
           message_reconcile_create(&f.store, message).ok() &&
           message_rich_store_initialize(&f.rich, {f.rich_record, 2}, {f.rich_order, 2},
                                         {f.rich_text, sizeof(f.rich_text)}) &&
           message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
           ui_variable_list_initialize(&f.list, {f.heights, 2}, {f.fenwick, 2}, 1, ui_extent_units(57)) &&
           message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57),
                                       ui_extent_units(height), false) &&
           message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
           message_scene_initialize_owned(&f.scene, &f.timeline, 2, 2, {f.widget, kWidgets}, {f.node, kWidgets}) &&
           message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
               {f.styles, 1024}, {f.semantics, 256}, {f.measurements, 4}, {f.spoilers, 4},
               {f.byte_styles, 32768}) &&
           ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 512}, measure, nullptr);
}

const MessageTextCommand* command(const MessageTextPresentation& p, u16 id, u32 nth) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].source_id == id && !nth--)
            return &p.commands[i];
    return nullptr;
}

Utf8View link_target(const Fixture& f, const MessageTextCommand* c, u32 nth) noexcept {
    for (u32 i = 0; c && i < c->semantics.count; ++i) {
        const auto& s = c->semantics[i];
        if (s.kind != MessageMarkdownSemanticKind::link || nth--)
            continue;
        return message_text_presentation_hit_source(
            f.store, &f.rich, {c->message_id, s.target, c->message_revision, s.source_offset, s.source_bytes, s.kind, true});
    }
    return {};
}
bool has_style(const MessageTextCommand* c, Utf8View part, u32 flags) noexcept {
    if (!c)
        return false;
    u32 at = 0;
    while (at + part.bytes <= c->text.bytes && !bytes_equal(c->text.data + at, part.data, part.bytes))
        ++at;
    if (at + part.bytes > c->text.bytes)
        return false;
    for (u32 i = 0; i < c->styles.count; ++i)
        if (at >= c->styles[i].byte_offset && at < c->styles[i].byte_offset + c->styles[i].byte_count)
            return (c->styles[i].flags & flags) == flags;
    return false;
}
// The TextSize at the first occurrence of `part`, or 0xFF when unstyled.
u8 size_at(const MessageTextCommand* c, Utf8View part) noexcept {
    if (!c)
        return 0xFF;
    u32 at = 0;
    while (at + part.bytes <= c->text.bytes && !bytes_equal(c->text.data + at, part.data, part.bytes))
        ++at;
    for (u32 i = 0; at + part.bytes <= c->text.bytes && i < c->styles.count; ++i)
        if (at >= c->styles[i].byte_offset && at < c->styles[i].byte_offset + c->styles[i].byte_count)
            return c->styles[i].size;
    return 0xFF;
}
// Every byte is covered by a span of `size` (the embed text variants).
bool sized_throughout(const MessageTextCommand* c, u8 size) noexcept {
    u32 end = 0;
    for (u32 i = 0; c && i < c->styles.count; ++i) {
        if (c->styles[i].byte_offset != end || c->styles[i].size != size)
            return false;
        end += c->styles[i].byte_count;
    }
    return c && c->text.bytes && end == c->text.bytes;
}
// The em of every glyph run when `c` is shaped as painted.
float shaped_em(const MessageTextCommand* c, float* height) noexcept {
    ShapedText output{};
    float em = -1;
    if (c && shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}) &&
        text_shape_utf8(&text_system, {c->text, c->styles, ui_rect_width(c->rectangle), 4096}, &output)
            .ok()) {
        for (u32 i = 0; i < output.run_count; ++i)
            if (output.runs[i].glyph_count)
                em = em < 0 || em == output.runs[i].em_size ? output.runs[i].em_size : 0;
        if (height)
            *height = output.height;
    }
    shaped_text_reset(&output);
    return em;
}

} // namespace embed_parity_detail

Utf8View c_text(const char* value) noexcept {
    u32 bytes = 0;
    while (value[bytes])
        ++bytes;
    return {value, bytes};
}
// One MESSAGE_CREATE through the client's dispatch pack/unpack, both stores and
// a converged text pass. Returns message 1's row; the caller releases the scene.
const MessageSceneRowWidget* converge_message(Utf8View payload,
                                              GatewayMessageEvent* decoded = nullptr) noexcept {
    auto& f = fixture;
    GatewayMessageEvent message{};
    bool ok = decode_message(payload, &message);
    if (decoded)
        *decoded = message;
    ok = ok && initialize_fixture(message);
    if (!ok)
        return nullptr;
    static ChatStateStore state{};
    static NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, 1'788'552'480, 1};
    ok = message_scene_update(&f.scene, {0, 0, 900, 1000}).ok() && message_text_coordinator_converge(request).ok();
    const auto* row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    if (!row)
        message_scene_release(&f.scene);
    return row;
}
const MessageTextCommand* owned_command(const MessageTextPresentation& p, UiWidgetHandle owner) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].owner.handle.slot == owner.slot && p.commands[i].owner.handle.generation == owner.generation)
            return &p.commands[i];
    return nullptr;
}
struct ReplyCase {
    const char* referenced; // JSON members after the referenced message's author
    const char* expected;   // the whole preview line
    bool italic;            // the placeholder after "Alice " is italic
    MessageReplyIcon icon;
};

// Reply previews: the text under the inline reply rules on one line, otherwise
// the poll question or an italic placeholder, plus the trailing icon.
void test_reply_previews(Check check) noexcept {
    auto& f = fixture;
    const ReplyCase cases[]{
        {R"("content":"```ansi\n\u001b[2;31mWelcome\u001b[0m to \u001b[38;2;255;0;0mSample\u001b[0m's\nPalette\n```")",
         "Alice Welcome to Sample's Palette", false, MessageReplyIcon::none},
        {R"("content":"> quoted\n# Big\n- item","type":0)", "Alice > quoted Big \xE2\x80\xA2 item", false,
         MessageReplyIcon::none},
        {R"("content":"","attachments":[{"id":"3","filename":"a.png","size":5,"url":"https://cdn.discordapp.com/a.png","proxy_url":"https://media.discordapp.net/a.png"}])",
         "Alice Click to see attachment", true, MessageReplyIcon::image},
        {R"("content":"look","attachments":[{"id":"3","filename":"a.png","size":5,"url":"https://cdn.discordapp.com/a.png","proxy_url":"https://media.discordapp.net/a.png"}])",
         "Alice look", false, MessageReplyIcon::image},
        {R"("content":"","sticker_items":[{"id":"4","name":"wave","format_type":1}])",
         "Alice Click to see sticker", true, MessageReplyIcon::sticker},
        {R"("content":"","flags":8192,"attachments":[{"id":"3","filename":"voice-message.ogg","size":5,"url":"https://cdn.discordapp.com/v.ogg","proxy_url":"https://media.discordapp.net/v.ogg","duration_secs":1.5,"waveform":"AAAA"}])",
         "Alice Click to see voice message", true, MessageReplyIcon::voice},
        {R"("content":"","interaction_metadata":{"id":"6","type":2})", "Alice Click to see command", true,
         MessageReplyIcon::command},
        {R"("content":"","flags":32768)", "Alice Click to see message", true, MessageReplyIcon::none},
        {R"("content":"","poll":{"question":{"text":"Lunch?"},"answers":[{"answer_id":1,"poll_media":{"text":"Pizza"}}],"expiry":"2026-10-06T00:00:00Z","allow_multiselect":false,"layout_type":1})",
         "Alice Lunch?", false, MessageReplyIcon::none},
        {R"("content":"","type":6)", "Alice Message pinned.", true, MessageReplyIcon::none},
        {R"("content":"https://media.tenor.com/x.gif","embeds":[{"type":"gifv","url":"https://media.tenor.com/x.gif","video":{"url":"https://media.tenor.com/x.mp4","width":4,"height":4}}])",
         "Alice Click to see attachment", true, MessageReplyIcon::image},
    };
    static char payload[4096];
    u32 passed = 0, failures_logged = 0;
    for (const auto& c : cases) {
        u32 at = 0;
        const auto append = [&](const char* text) noexcept {
            while (*text && at + 1 < sizeof(payload))
                payload[at++] = *text++;
        };
        append(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"reply","timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},)");
        append(c.referenced);
        append("}}}");
        const auto* row = converge_message({payload, at});
        const auto* line = row ? owned_command(f.presentation, row->reply) : nullptr;
        bool ok = line && utf8_equal(line->text, c_text(c.expected));
        bool code_block = false, colored = false, italic = false, monospace = false;
        for (u32 i = 0; ok && i < line->styles.count; ++i) {
            const auto& span = line->styles[i];
            code_block |= (span.flags & text_style_code_block) != 0;
            colored |= ansi_span_foreground(span) != kAnsiColorNone || ansi_span_background(span) != kAnsiColorNone;
            italic |= (span.flags & text_style_italic) != 0 && span.byte_offset == 6;
            monospace |= (span.flags & text_style_monospace) != 0;
        }
        const auto* rich = ok ? message_rich_store_find(f.rich, 1) : nullptr;
        const bool icon_ok =
            rich && message_rich_reply_icon(rich->reference.content_flags, rich->reference.message_type,
                                            !rich->reference.content.bytes,
                                            (rich->reference.content_flags & gateway_reference_poll_question) != 0) ==
                        c.icon &&
            (c.icon == MessageReplyIcon::none) == (row->rich_item_layout.reply_width == 0.0f);
        ok = ok && !code_block && !colored && italic == c.italic && icon_ok &&
             (&c != &cases[0] || monospace);
        if (!ok && failures_logged++ < 3 && line)
            check(false, c.expected);
        passed += ok ? 1u : 0u;
        if (row)
            message_scene_release(&f.scene);
    }
    check(passed == sizeof(cases) / sizeof(cases[0]),
          "reply preview: ANSI fences become one uncoloured inline code run, quotes stay literal, headings and "
          "lists inline, and empty replies read Click to see attachment/sticker/voice message/command/message, "
          "the poll question or Message pinned. with the official client's trailing icon");

    // A deleted original: "Original message was deleted" in italics, no author.
    const auto deleted = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"reply","timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":null}})");
    const auto* row = converge_message(deleted);
    const auto* line = row ? owned_command(f.presentation, row->reply) : nullptr;
    check(line && utf8_equal(line->text, utf8_literal("Original message was deleted")) && line->styles.count == 1 &&
              line->styles[0].flags == text_style_italic,
          "reply preview: a deleted original reads \"Original message was deleted\" in italics");
    if (row)
        message_scene_release(&f.scene);

    // A long truecolor ANSI message still closes its fence inside the bounded
    // preview source, so no raw escape or ``` reaches the line.
    static char long_payload[16384];
    u32 at = 0;
    const auto put = [&](const char* text) noexcept {
        while (*text && at + 1 < sizeof(long_payload))
            long_payload[at++] = *text++;
    };
    put(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"reply","timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},"content":"```ansi\n)");
    for (u32 i = 0; i < 400; ++i)
        put(R"(\u001b[38;2;255;0;0mW)");
    put(R"(\n```"}}})");
    row = converge_message({long_payload, at});
    line = row ? owned_command(f.presentation, row->reply) : nullptr;
    bool clean = line && utf8_starts_with(line->text, utf8_literal("Alice WWWW")) && !test_contains(line->text, utf8_literal("`")) &&
                 !test_contains(line->text, utf8_literal("\x1b")) && !test_contains(line->text, utf8_literal("[38;"));
    check(clean, "reply preview: a long truecolor ```ansi message shows its text without escapes or fences");
    if (row)
        message_scene_release(&f.scene);
}

// A reply that pings its quoted author names them "@Author"; a quoted join
// notice or app subscription reads as its system line without an author name.
void test_reply_author_and_system_quotes(Check check) noexcept {
    auto& f = fixture;
    const auto pinged = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"reply","mentions":[{"id":"3","username":"other"},{"id":"8","username":"alice"}],"timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},"content":"hello"}}})");
    const auto* row = converge_message(pinged);
    const auto* line = row ? owned_command(f.presentation, row->reply) : nullptr;
    const auto* rich = row ? message_rich_store_find(f.rich, 1) : nullptr;
    check(line && utf8_equal(line->text, utf8_literal("@Alice hello")) && line->styles.count >= 1 &&
              line->styles[0].byte_offset == 0 && line->styles[0].byte_count == 6 &&
              (line->styles[0].flags & text_style_bold) && (line->styles[0].flags & text_style_inline_role_color) && rich &&
              (rich->reference.content_flags & gateway_reference_mentions_author),
          "reply preview: a reply whose mentions list the quoted author names them @Author in the muted name span");
    if (line) {
        const float cy = (line->rectangle.top + line->rectangle.bottom) * .5f;
        const auto avatar_hit = message_reply_author_hit_test(f.scene, f.presentation, &f.cache,
                                                              line->rectangle.left - kMessageReplyTextInset + 8, cy);
        const auto name_hit = message_reply_author_hit_test(f.scene, f.presentation, &f.cache,
                                                            line->rectangle.left + 4, cy);
        const auto text_hit = message_reply_author_hit_test(f.scene, f.presentation, &f.cache,
                                                            line->rectangle.right - 8, cy);
        check(avatar_hit.target == MessageInteractionTarget::avatar && avatar_hit.user_id == 8 &&
                  name_hit.target == MessageInteractionTarget::name && name_hit.user_id == 8 &&
                  text_hit.target == MessageInteractionTarget::none,
              "reply preview: the quoted author's avatar and name open their profile, the rest of the line is not theirs");
    }
    if (row)
        message_scene_release(&f.scene);
    const auto silent = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"reply","mentions":[{"id":"3","username":"other"}],"timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},"content":"hello"}}})");
    row = converge_message(silent);
    line = row ? owned_command(f.presentation, row->reply) : nullptr;
    check(line && utf8_equal(line->text, utf8_literal("Alice hello")),
          "reply preview: a reply without the quoted author in its mentions (ping off) keeps the bare name");
    if (row)
        message_scene_release(&f.scene);

    struct SystemCase {
        const char* referenced;
        u16 type;
        MessageReplyLeading leading;
        const char* question; // the poll result's poll name
    };
    const SystemCase system_cases[]{
        {R"("type":7,"content":"")", message_type_user_join, MessageReplyLeading::join_arrow, ""},
        {R"("type":32,"content":"")", message_type_guild_application_premium, MessageReplyLeading::ticket, ""},
        {R"("type":46,"content":"","embeds":[{"type":"poll_result","fields":[{"name":"poll_question_text","value":"Lunch?"},{"name":"total_votes","value":"3"}]}])",
         message_type_poll_result, MessageReplyLeading::poll, "Lunch?"},
        {R"("type":25,"content":"")", message_type_role_subscription_purchase, MessageReplyLeading::join_arrow, ""},
    };
    u32 passed = 0;
    for (const auto& c : system_cases) {
        static char payload[2048];
        u32 at = 0;
        const auto append = [&](const char* text) noexcept {
            while (*text && at + 1 < sizeof(payload))
                payload[at++] = *text++;
        };
        append(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"welcome","timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},)");
        append(c.referenced);
        append("}}}");
        row = converge_message({payload, at});
        line = row ? owned_command(f.presentation, row->reply) : nullptr;
        char storage[kMessageSystemTextMaximumBytes];
        MessageSystemText expected{};
        MessageSystemTextInput input{};
        input.message_id = 5;
        input.author = utf8_literal("Alice");
        input.type = c.type;
        input.poll_question = c_text(c.question);
        rich = row ? message_rich_store_find(f.rich, 1) : nullptr;
        const bool ok = line && rich && message_system_text_build(input, {storage, sizeof(storage)}, &expected) &&
                        utf8_equal(line->text, expected.text) &&
                        line->styles.count == (c.leading == MessageReplyLeading::poll ? 1u : 0u) &&
                        message_rich_reply_leading(rich->reference) == c.leading &&
                        row->rich_item_layout.reply_width == 0.0f;
        if (!ok)
            check(false, c.referenced);
        passed += ok ? 1u : 0u;
        if (row)
            message_scene_release(&f.scene);
    }
    check(passed == 4, "reply preview: a quoted join notice, role subscription, app subscription or closed poll shows its "
                       "system line without an author name, leads with its icon and has no trailing icon");

    // A quoted forward is flagged (its preview leads with the forward icon).
    const auto forward_quote = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"reply","timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},"content":"fwd","message_reference":{"type":1,"message_id":"4","channel_id":"6"}}}})");
    row = converge_message(forward_quote);
    rich = row ? message_rich_store_find(f.rich, 1) : nullptr;
    check(rich && (rich->reference.quote_flags & gateway_quote_forwarded) && !(rich->reference.quote_flags & gateway_quote_media_mention),
          "reply preview: a quote that is itself a forward is flagged for the forward icon");
    line = row ? owned_command(f.presentation, row->reply) : nullptr;
    check(line && utf8_equal(line->text, utf8_literal("Alice \xE2\x80\x83\xE2\x80\x85\xE2\x80\x8D" "fwd")) &&
              line->styles.count >= 1 && line->styles[0].byte_count == 5 && line->semantics.count == 1 &&
              line->semantics[0].kind == MessageMarkdownSemanticKind::chip_icon &&
              line->semantics[0].target == static_cast<u64>(MessageMarkdownChipIcon::forward) &&
              line->semantics[0].byte_offset == 6 && line->semantics[0].byte_count == 3,
          "reply preview: a quoted forward leads its text with the forward icon cell");
    if (row)
        message_scene_release(&f.scene);

    // A reply that points into a clip quotes the clip's title and the moment.
    const auto clip = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"","media_mention":{"attachment_id":"3","timestamp":3723000},"timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},"content":"","attachments":[{"id":"2","filename":"x.png","size":5,"url":"https://cdn.discordapp.com/x.png","proxy_url":"https://media.discordapp.net/x.png"},{"id":"3","filename":"c.mp4","title":"Epic clip","flags":1,"size":5,"url":"https://cdn.discordapp.com/c.mp4","proxy_url":"https://media.discordapp.net/c.mp4"}]}}})");
    row = converge_message(clip);
    line = row ? owned_command(f.presentation, row->reply) : nullptr;
    rich = row ? message_rich_store_find(f.rich, 1) : nullptr;
    check(line && rich && utf8_equal(line->text, utf8_literal("Alice Epic clip @1:02:03")) && line->styles.count == 3 &&
              (line->styles[1].flags & text_style_bold) && line->styles[2].flags == text_style_link &&
              (rich->reference.quote_flags & gateway_quote_media_mention) &&
              (rich->reference.quote_flags & gateway_quote_media_clip) && row->rich_item_layout.reply_width == 0.0f,
          "reply preview: a clip mention reads the clip's title (bold) and the moment (link) with no trailing icon");
    if (row)
        message_scene_release(&f.scene);
    const auto untitled = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":19,"author":{"id":"9","username":"Poster"},"content":"","media_mention":{"attachment_id":"3","timestamp":83000},"timestamp":"2026-09-05T10:00:00Z","message_reference":{"message_id":"5","channel_id":"7"},"referenced_message":{"id":"5","channel_id":"7","author":{"id":"8","username":"alice","global_name":"Alice"},"content":"hi"}}})");
    row = converge_message(untitled);
    line = row ? owned_command(f.presentation, row->reply) : nullptr;
    check(line && utf8_equal(line->text, utf8_literal("Alice Untitled @1:23")),
          "reply preview: a clip mention without a known title reads Untitled");
    if (row)
        message_scene_release(&f.scene);
}

// "<user> used <command>": the line, its layout (muted name, "used", a chip for
// a slash command and plain link text for any other command) and its hits.
void test_interaction_headers(Check check) noexcept {
    auto& f = fixture;
    static UserStore users{};
    const auto slash = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":20,"author":{"id":"9","username":"Poster","bot":true},"content":"","timestamp":"2026-09-05T10:00:00Z","interaction":{"id":"6","type":2,"name":"blueprint file","user":{"id":"8","username":"alice","global_name":"Alice"}}}})");
    const auto* row = converge_message(slash);
    const auto* line = row ? owned_command(f.presentation, row->reply) : nullptr;
    check(line && utf8_equal(line->text, utf8_literal("Alice used blueprint file")) && line->styles.count == 2 &&
              line->styles[0].byte_count == 5 && (line->styles[0].flags & text_style_inline_role_color) &&
              line->styles[1].byte_offset == 11 && line->styles[1].byte_count == 14 &&
              line->styles[1].flags == text_style_mention,
          "interaction header: a slash command reads \"<user> used <command>\" without a leading slash, the command a chip span");
    MessageInteractionLayout layout{};
    const bool laid = line && message_interaction_layout(f.scene, *line, users, &f.cache, kDefaultTheme, &layout);
    check(laid && utf8_equal(layout.name.text, utf8_literal("Alice")) && utf8_equal(layout.used.text, utf8_literal("used")) &&
              utf8_equal(layout.command.text, utf8_literal("blueprint file")) && !layout.tag.label.bytes &&
              layout.name.rectangle.right < layout.used.rectangle.left &&
              layout.used.rectangle.right < layout.chip.left && ui_rect_has_area(layout.chip) &&
              layout.chip.left <= layout.chip_icon.left && layout.chip_icon.right < layout.command.rectangle.left &&
              layout.command.rectangle.right <= layout.chip.right && layout.user_id == 8 &&
              close_to(layout.name.color.alpha, kMessageReplyAuthorOpacity) && layout.used.color.alpha == 1.0f,
          "interaction header: the name is muted, \"used\" is not, and the command is a chip with its icon before the name");
    if (laid) {
        const auto at = [&](UiRect r) noexcept {
            return message_interaction_hit_test(f.scene, f.presentation, users, &f.cache, kDefaultTheme,
                                                (r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
        };
        const float cy = (line->rectangle.top + line->rectangle.bottom) * .5f;
        const UiRect avatar{line->rectangle.left - kMessageReplyTextInset, cy - 4, line->rectangle.left - kMessageReplyTextInset + 16,
                            cy + 4};
        const auto avatar_hit = at(avatar), name_hit = at(layout.name.rectangle), chip_hit = at(layout.chip),
                   gap_hit = at({layout.name.rectangle.right + 1, cy - 2, layout.name.rectangle.right + 3, cy + 2});
        check(avatar_hit.target == MessageInteractionTarget::avatar && avatar_hit.user_id == 8 &&
                  name_hit.target == MessageInteractionTarget::name && name_hit.user_id == 8 &&
                  chip_hit.target == MessageInteractionTarget::command && gap_hit.target == MessageInteractionTarget::none,
              "interaction header: the avatar and name open the user, the chip is its own target and the gap between is none");
    }
    if (row)
        message_scene_release(&f.scene);

    // A component response (type 0 with a component interaction) names the command as plain link text.
    const auto component = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","type":0,"author":{"id":"9","username":"Poster","bot":true},"content":"","timestamp":"2026-09-05T10:00:00Z","interaction":{"id":"6","type":3,"name":"vote","user":{"id":"8","username":"alice"}}}})");
    row = converge_message(component);
    line = row ? owned_command(f.presentation, row->reply) : nullptr;
    MessageInteractionLayout plain{};
    check(line && utf8_equal(line->text, utf8_literal("alice used vote")) && line->styles.count == 2 &&
              line->styles[1].flags == text_style_link &&
              message_interaction_layout(f.scene, *line, users, &f.cache, kDefaultTheme, &plain) &&
              !ui_rect_has_area(plain.chip) && utf8_equal(plain.command.text, utf8_literal("vote")) &&
              plain.used.rectangle.right < plain.command.rectangle.left,
          "interaction header: a command that is not a slash command is plain link-coloured text, not a chip");
    if (row)
        message_scene_release(&f.scene);
}

Utf8View interaction_data_name(void*, InteractionDataEntity entity, u64 id) noexcept {
    if (entity == InteractionDataEntity::user && id == 8)
        return utf8_literal("Alice");
    if (entity == InteractionDataEntity::channel && id == 5)
        return utf8_literal("general");
    if (entity == InteractionDataEntity::role && id == 4)
        return utf8_literal("Mods");
    return {};
}

// The command card's request (its target and rate-limit route are the request
// metadata) and its text: the root command, sub commands as words, options
// with their entity names, one line.
void test_interaction_data(Check check) noexcept {
    u8 target[kInteractionDataTargetBytes];
    const u32 bytes = interaction_data_write_target(1234567890123ull, 98765432109876ull, {target, sizeof(target)});
    const Utf8View expected = utf8_literal("/api/v9/channels/1234567890123/messages/98765432109876/interaction-data");
    const RestRouteKey route = interaction_data_route(1234567890123ull);
    u8 small[16];
    check(bytes == expected.bytes && bytes_equal(target, expected.data, bytes) && rest_route_valid(route) &&
              route.method == RestMethod::get && route.major == RestMajorParameter::channel &&
              route.major_key == 1234567890123ull && route.route_id == kInteractionDataRouteId &&
              interaction_data_write_target(0, 9, {target, sizeof(target)}) == 0 &&
              interaction_data_write_target(9, 0, {target, sizeof(target)}) == 0 &&
              interaction_data_write_target(9, 9, {small, sizeof(small)}) == 0,
          "interaction data request: a bodyless GET of the message's interaction-data, one route per channel; zero ids and short buffers are refused");
    const InteractionDataNames names{nullptr, interaction_data_name};
    u32 scratch[kInteractionDataScratchStructurals];
    char out[256];
    const auto text = [&](const char* body, u32 capacity = sizeof(out)) noexcept {
        const u32 n = interaction_data_text(c_text(body), names, {scratch, kInteractionDataScratchStructurals},
                                            {out, capacity});
        return Utf8View{out, n};
    };
    const auto full = text(R"({"id":"6","type":2,"name":"blueprint file","application_command":{"id":"1","name":"blueprint"},"options":[{"name":"file","type":1,"options":[{"name":"user","type":6,"value":"8"},{"name":"where","type":7,"value":"5"},{"name":"who","type":8,"value":"4"},{"name":"text","type":3,"value":"a\nb \u00e9"},{"name":"n","type":4,"value":12},{"name":"flag","type":5,"value":true},{"name":"img","type":11,"value":"99"}]}]})");
    check(utf8_equal(full, utf8_literal("/blueprint file user: @Alice where: #general who: @Mods text: a b \xC3\xA9 n: 12 flag: true img: [attachment]")),
          "interaction data text: the command, sub command words, entity names, one line");
    check(utf8_equal(text(R"({"name":"x y","application_command":{"name":"x","name_localized":"ex"},"options":[{"name":"user","type":6,"value":"777"}]})"),
                     utf8_literal("/ex user: 777")) &&
              utf8_equal(text(R"({"name":"solo"})"), utf8_literal("/solo")) && text("[]").bytes == 0 &&
              text(R"({"id":"1"})").bytes == 0,
          "interaction data text: the localized root name wins, unknown entities show their id, a body without a command reads nothing");
    const auto cut = text(R"({"name":"cmd","options":[{"name":"message","type":3,"value":"abcdefghijklmnopqrstuvwxyz"}]})", 24);
    check(cut.bytes <= 24 && cut.bytes > 3 && utf8_equal({cut.data + cut.bytes - 3, 3}, utf8_literal("\xE2\x80\xA6")) &&
              utf8_starts_with(cut, utf8_literal("/cmd message")),
          "interaction data text: a line longer than the card is cut after a whole piece and ends with an ellipsis");
}

// Shapes a presentation command as the renderer does and checks each glyph's
// ```ansi colour tag, and the underline/line-through segment under it,
// against the foreground of its span (the block colour without one).
struct AnsiPaint {
    bool ok;
    u32 underlined, struck, wrong;
};
AnsiPaint ansi_paint(const MessageTextCommand* command) noexcept {
    AnsiPaint result{};
    static float pen_left[1024], pen_right[1024], pen_baseline[1024];
    ShapedText output{};
    if (!command || !shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}))
        return result;
    const ConstSlice<TextStyleSpan> styles = command->styles;
    const Utf8View text = command->text;
    if (!text_shape_utf8(&text_system, {text, styles, maximum(1.0f, ui_rect_width(command->rectangle)), 4000}, &output)
             .ok() ||
        !ansi_text_tag_glyphs(output, text, styles)) {
        shaped_text_reset(&output);
        return result;
    }
    for (u32 run = 0; run < output.run_count; ++run) {
        const TextRun& r = output.runs[run];
        float x = r.baseline_x;
        for (u32 g = r.first_glyph; g < r.first_glyph + r.glyph_count && g < output.glyph_count; ++g) {
            pen_left[g] = x;
            pen_right[g] = x + output.glyphs[g].advance;
            pen_baseline[g] = r.baseline_y;
            x += output.glyphs[g].advance;
        }
    }
    const auto line_under = [&](TextDecorationKind kind, float centre, float baseline) noexcept -> const TextDecoration* {
        const bool under = kind == TextDecorationKind::underline;
        for (u32 i = 0; i < output.decoration_count; ++i) {
            const TextDecoration& d = output.decorations[i];
            if (d.kind == kind && d.left <= centre && centre < d.right &&
                (under ? d.top >= baseline - 1.0f && d.top <= baseline + 8.0f
                       : d.top >= baseline - 12.0f && d.top < baseline))
                return &d;
        }
        return nullptr;
    };
    u32 unit = 0;
    for (u32 at = 0; at < text.bytes && unit < output.cluster_count;) {
        const Utf8DecodeResult cp = utf8_decode_one(text.data + at, text.bytes - at);
        const u32 width = cp.valid && cp.width ? cp.width : 1u;
        const u32 units = cp.valid && cp.codepoint > 0xFFFFu ? 2u : 1u;
        const u32 glyph = output.clusters[unit];
        const TextStyleSpan* span = nullptr;
        for (u32 i = 0; i < styles.count && !span; ++i)
            if (styles[i].byte_offset <= at && at < styles[i].byte_offset + styles[i].byte_count)
                span = &styles[i];
        if (cp.valid && cp.codepoint > ' ' && glyph < output.glyph_count) {
            const u16 foreground = span ? ansi_span_foreground(*span) : u16{kAnsiColorNone};
            const float centre = (pen_left[glyph] + pen_right[glyph]) * 0.5f;
            const TextDecoration* under = line_under(TextDecorationKind::underline, centre, pen_baseline[glyph]);
            const TextDecoration* through =
                line_under(TextDecorationKind::strikethrough, centre, pen_baseline[glyph]);
            const bool underlined = span && (span->flags & text_style_underline);
            const bool struck = span && (span->flags & text_style_strikethrough);
            result.underlined += underlined;
            result.struck += struck;
            result.wrong += output.glyphs[glyph].reserved != foreground;
            result.wrong += underlined && (!under || ansi_decoration_line_color(*under) != foreground);
            result.wrong += struck && (!through || ansi_decoration_line_color(*through) != foreground);
        }
        at += width;
        unit += units;
    }
    shaped_text_reset(&output);
    result.ok = true;
    return result;
}
const MessageTextCommand* command_containing(const MessageTextPresentation& p, Utf8View part) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (test_contains(p.commands[i].text, part))
            return &p.commands[i];
    return nullptr;
}

// A colored-text generator page's component embed: a components embed whose
// container holds markdown text with a ```ansi block. It renders the title
// link and the coloured code block, never an empty card.
void test_component_embed(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://colors.example.com/ansi-text-preview/","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"components","url":"https://colors.example.com/ansi-text-preview/","components":[{"type":17,"accent_color":5793266,"components":[{"type":10,"content":"### [Example ANSI Colored Text Preview Tool](https://colors.example.com/ansi-text-preview/)\n```ansi\nSmall tool to make \u001b[48;2;59;61;78m\u001b[38;2;255;0;0mr\u001b[38;2;255;255;0ma\u001b[0m \u001b[48;2;88;101;242m\u001b[38;2;255;255;255mglowing\u001b[0m text \u001b[2;4;2m\u001b[38;2;244;141;100mlike that\u001b[0m!\n```"}]}]}]}})");
    GatewayMessageEvent message{};
    const auto* row = converge_message(payload, &message);
    const auto* rich = row ? message_rich_store_find(f.rich, 1) : nullptr;
    const auto* embed = rich && rich->embed_count == 1 ? &rich->embeds[0] : nullptr;
    check(embed && embed->type == GatewayMessageEmbedType::components && embed->has_color &&
              embed->color == 5793266 && embed->description.bytes &&
              message_rich_embed_plan(f.rich, *embed).markdown,
          "component embed: the container's text display and accent colour decode into the embed card");
    const MessageTextCommand *title = nullptr, *code = nullptr;
    for (u32 i = 0; row && i < f.presentation.command_count; ++i) {
        const auto& c = f.presentation.commands[i];
        if (test_contains(c.text, utf8_literal("Example ANSI Colored Text Preview Tool")))
            title = &c;
        if (test_contains(c.text, utf8_literal("Small tool to make")))
            code = &c;
    }
    bool colored = false, background = false, underline = false, block = false;
    for (u32 i = 0; code && i < code->styles.count; ++i) {
        const auto& span = code->styles[i];
        colored |= ansi_span_foreground(span) != kAnsiColorNone;
        background |= ansi_span_background(span) != kAnsiColorNone;
        underline |= (span.flags & text_style_underline) != 0;
        block |= (span.flags & text_style_code_block) != 0;
    }
    const float height = row ? row->rich_item_layout.heights[kMessageSceneEmbedItemOffset] : 0.0f;
    check(title && code && has_style(title, utf8_literal("Example"), text_style_link) && colored && background &&
              underline && block && !test_contains(code->text, utf8_literal("\x1b")) &&
              !test_contains(code->text, utf8_literal("```")) && height > 52.0f,
          "component embed: the heading link and the ```ansi block render with colours, background and "
          "underline inside a non-empty card");
    // "\e[2;4;2m\e[38;2;...mlike that" opens <u> before the colour span; the
    // underline still draws orange under every letter, the first included.
    const AnsiPaint like = ansi_paint(code);
    check(like.ok && like.underlined == 8 && like.struck == 0 && like.wrong == 0,
          "component embed: each glyph and its underline segment take the run's foreground (first letter's "
          "segment orange like its glyph)");
    if (row)
        message_scene_release(&f.scene);

    // Lines opened before their colour, and lines over recoloured text, draw
    // in each run's foreground in every text path sharing the parse.
    const auto lined = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"x","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"components","components":[{"type":17,"components":[{"type":10,"content":"```ansi\nAnsi \u001b[33;4mlined \u001b[34mblue\u001b[0m \u001b[2;4m\u001b[9m\u001b[32mTESTINGTESTING\u001b[0m\n```"}]}]}]}})");
    row = converge_message(lined);
    const AnsiPaint component = ansi_paint(row ? command_containing(f.presentation, utf8_literal("Ansi lined")) : nullptr);
    check(component.ok && component.underlined == 23 && component.struck == 14 && component.wrong == 0,
          "component text ansi: underline and line-through take each run's foreground, opened before it or not");
    if (row)
        message_scene_release(&f.scene);
    const auto paths = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"```ansi\n\u001b[2;4m\u001b[9m\u001b[32mCONTENTLINES\u001b[0m\n```","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"rich","title":"t","description":"```ansi\n\u001b[2;4m\u001b[9m\u001b[32mDESCRIBED\u001b[0m\n```","fields":[{"name":"f","value":"```ansi\n\u001b[2;4m\u001b[9m\u001b[31mFIELDVALUE\u001b[0m\n```"}]}]}})");
    row = converge_message(paths);
    const AnsiPaint content = ansi_paint(row ? command_containing(f.presentation, utf8_literal("CONTENTLINES")) : nullptr);
    const AnsiPaint described = ansi_paint(row ? command_containing(f.presentation, utf8_literal("DESCRIBED")) : nullptr);
    const AnsiPaint field = ansi_paint(row ? command_containing(f.presentation, utf8_literal("FIELDVALUE")) : nullptr);
    check(content.ok && content.underlined == 12 && content.struck == 12 && content.wrong == 0 && described.ok &&
              described.underlined == 9 && described.struck == 9 && described.wrong == 0 && field.ok &&
              field.underlined == 10 && field.struck == 10 && field.wrong == 0,
          "message, embed description and field ansi: both lines take the run's foreground");
    if (row)
        message_scene_release(&f.scene);

    // Any embed carrying a components tree renders its text, whatever its
    // type string, instead of an empty card.
    const auto future = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"x","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"rich","components":[{"type":17,"components":[{"type":9,"components":[{"type":10,"content":"**Card** text"}],"accessory":{"type":11,"media":{"url":"https://example.com/t.png","proxy_url":"https://images-ext-1.discordapp.net/external/t/https/example.com/t.png","width":80,"height":80}}},{"type":14},{"type":10,"content":"second"}]}]}]}})");
    row = converge_message(future);
    rich = row ? message_rich_store_find(f.rich, 1) : nullptr;
    const auto* card = rich && rich->embed_count == 1 ? &rich->embeds[0] : nullptr;
    const Utf8View text = card ? message_rich_store_string(f.rich, card->description) : Utf8View{};
    const MessageTextCommand* body = nullptr;
    for (u32 i = 0; row && i < f.presentation.command_count; ++i)
        if (test_contains(f.presentation.commands[i].text, utf8_literal("Card text")))
            body = &f.presentation.commands[i];
    check(card && utf8_equal(text, utf8_literal("**Card** text\n\nsecond")) && card->thumbnail.url.bytes &&
              body && has_style(body, utf8_literal("Card"), text_style_bold),
          "component embed: sections, separators and thumbnail accessories map to the card's text and thumbnail");
    if (row)
        message_scene_release(&f.scene);
}

// Fenced code blocks are framed blocks in embeds and message bodies: each is
// its own command inside the box insets, without its trailing newline, and
// paint draws the box from that command, so the measured card and the drawing
// agree. The text layer keeps the ANSI colours but no code background of its own.
struct CodeBlockCase {
    const MessageSceneRowWidget* row;
    const MessageTextCommand *title, *code;
    UiRect card, box;
};
CodeBlockCase code_block_case(Utf8View payload, Utf8View title) noexcept {
    auto& f = fixture;
    CodeBlockCase c{converge_message(payload), nullptr, nullptr, {}, {}};
    if (!c.row)
        return c;
    c.title = title.bytes ? command_containing(f.presentation, title) : nullptr;
    c.code = command_containing(f.presentation, utf8_literal("Simple app"));
    c.box = c.code ? message_text_code_block_box(*c.code) : UiRect{};
    if (const UiLayoutNode* item =
            message_scene_rich_item_layout_node(f.scene, *c.row, MessageSceneRichItemKind::embed, 0)) {
        const float width = c.row->rich_item_layout.embed_widths[0];
        c.card = {item->rectangle.left, item->rectangle.top,
                  minimum(item->rectangle.right, item->rectangle.left + (width > 0 ? width : kMessageEmbedMaximumWidth)),
                  item->rectangle.top + c.row->rich_item_layout.heights[kMessageSceneEmbedItemOffset] -
                      kMessageEmbedItemGap};
    }
    return c;
}
bool framed_throughout(const MessageTextCommand* c) noexcept {
    bool framed = c && c->styles.count && message_text_command_code_block(*c) && c->text.bytes &&
                  c->text.data[c->text.bytes - 1] != '\n';
    for (u32 i = 0; framed && i < c->styles.count; ++i)
        framed = text_span_framed_block(c->styles[i]) && (c->styles[i].flags & text_style_code_block);
    return framed;
}

void test_code_block_embeds(Check check) noexcept {
    auto& f = fixture;
    // The colored-text tool's components embed: a heading link, then a
    // ```ansi block with bold, a background and an underline.
    const auto components = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://colors.example.com/","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"components","url":"https://colors.example.com/","components":[{"type":17,"accent_color":5793266,"components":[{"type":10,"content":"### [Example Colored Text Tool](https://colors.example.com/)\n```ansi\nSimple app to make \u001b[1;31mcolored\u001b[0m \u001b[45;37mtext\u001b[0m \u001b[4;34mlike this\u001b[0m!\n```"}]}]}]}})");
    CodeBlockCase c = code_block_case(components, utf8_literal("Example Colored Text Tool"));
    check(c.title && c.code && framed_throughout(c.code) && ui_rect_has_area(c.card) &&
              has_style(c.code, utf8_literal("colored"), text_style_bold | text_style_monospace) &&
              close_to(c.title->rectangle.top - c.card.top, kMessageEmbedContainerInset) &&
              close_to(c.box.top, c.title->rectangle.top + c.title->measured_height + kMessageCodeBlockMargin) &&
              close_to(c.box.left, c.card.left + kMessageEmbedContentInset) &&
              close_to(c.box.right, c.card.right - kMessageEmbedEndInset) &&
              close_to(c.box.bottom - c.box.top, c.code->measured_height + 2 * kMessageCodeBlockInset) &&
              close_to(c.card.bottom - c.box.bottom, kMessageEmbedContainerInset),
          "code block embed: the measured card is the heading, the full-width code box and equal 16-DIP padding "
          "above and below");
    ShapedText out{};
    u32 block_backgrounds = 0, ansi_backgrounds = 0, underlines = 0, lines = 0;
    if (c.code && shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}, {decorations, 64}) &&
        text_shape_utf8(&text_system, {c.code->text, c.code->styles, ui_rect_width(c.code->rectangle), 4096}, &out)
            .ok()) {
        lines = out.line_count;
        for (u32 i = 0; i < out.decoration_count; ++i) {
            const TextDecoration& d = out.decorations[i];
            if (d.kind == TextDecorationKind::background)
                ++(ansi_decoration_background(d) ? ansi_backgrounds : block_backgrounds);
            underlines += d.kind == TextDecorationKind::underline;
        }
    }
    shaped_text_reset(&out);
    check(lines == 1 && block_backgrounds == 0 && ansi_backgrounds == 1 && underlines == 1,
          "code block embed: one line (no trailing empty line); the text layer draws the ANSI background and "
          "underline but no code background under the framed box");
    // Per embed: the card, its accent and the code box, in the row's one batch
    // (the box replaces the text layer's code background instance).
    paint_stream_reset(&f.paint, 900, 1000);
    MessageMediaCommandStream media{};
    MessagePresentationResult painted{};
    bool paint_ok = c.row && message_media_command_stream_initialize(&media, {f.media, 32}) &&
                    message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &painted, &f.presentation);
    u32 cards = 0, accents = 0, boxes = 0;
    const Color code_fill = kDefaultTheme.tokens.background.secondary;
    for (u32 i = 0; paint_ok && i < f.paint.count; ++i) {
        const PaintQuad& q = f.paint.quads[i];
        const bool card = close_to(q.left, c.card.left) && close_to(q.top, c.card.top) &&
                          close_to(q.bottom, c.card.bottom);
        cards += card && close_to(q.right, c.card.right) && q.border_width >= 0;
        accents += card && q.border_width == kPaintEdgeStripMode && q.right - q.left == kMessageEmbedAccentWidth &&
                   q.radius == kDefaultTheme.scale.radius_sm && q.reserved_x == c.card.right - c.card.left;
        boxes += close_to(q.left, c.box.left) && close_to(q.top, c.box.top) && close_to(q.right, c.box.right) &&
                 close_to(q.bottom, c.box.bottom) && q.radius == kDefaultTheme.scale.radius_sm &&
                 q.border_width == kMessageCodeBlockBorder && q.fill.red == code_fill.red &&
                 q.fill.alpha == code_fill.alpha;
    }
    check(paint_ok && cards == 1 && accents == 1 && boxes == 1 && f.paint.batch_count == 1,
          "code block embed: paint draws the card, its corner-following accent and the code box in one batch");
    paint_stream_release(&f.paint);
    if (c.row)
        message_scene_release(&f.scene);

    // A rich embed: the title links to embed.url in bold at 16 DIPs; the
    // grid pads 2 DIPs plus the block's 8-DIP margin above and 16 below.
    const auto rich = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"x","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"rich","title":"Example Colored Text Tool","url":"https://colors.example.com/","color":5793266,"description":"```ansi\nSimple app to make \u001b[1;31mcolored\u001b[0m text!\n```"}]}})");
    c = code_block_case(rich, utf8_literal("Example Colored Text Tool"));
    check(c.title && has_style(c.title, utf8_literal("Example"), text_style_bold | text_style_link) &&
              size_at(c.title, utf8_literal("Example")) == text_size_md && shaped_em(c.title) == 16.0f &&
              utf8_equal(link_target(f, c.title), utf8_literal("https://colors.example.com/")),
          "title link embed: the title is a bold 16-DIP link to embed.url");
    check(c.code && framed_throughout(c.code) &&
              close_to(c.title->rectangle.top - c.card.top, kMessageEmbedTopInset + kMessageEmbedBlockGap) &&
              close_to(c.box.top, c.title->rectangle.top + c.title->measured_height + kMessageEmbedBlockGap) &&
              close_to(c.box.right, c.card.right - kMessageEmbedEndInset) &&
              close_to(c.card.bottom - c.box.bottom, kMessageEmbedBottomInset),
          "code block embed: a rich embed's description block is framed full-width, 8 DIPs below the title, "
          "and the card ends 16 DIPs below the box");
    if (c.row)
        message_scene_release(&f.scene);

    // Message bodies share the blocks: at most 90% of the markup, the text
    // after a block starts at its box, and a leading block's box counts.
    const auto body = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"before **bold** and _italic_\nsecond plain line\n```ansi\nSimple app to make \u001b[31mcolored\u001b[0m\n```\nafter","timestamp":"2026-09-05T10:00:00Z"}})");
    c = code_block_case(body, {});
    const MessageTextCommand* before = command_containing(f.presentation, utf8_literal("before"));
    const MessageTextCommand* after = command_containing(f.presentation, utf8_literal("after"));
    u32 before_flags = 0;
    if (before)
        for (u32 i = 0; i < before->styles.count; ++i)
            before_flags |= before->styles[i].flags;
    check(before && utf8_equal(before->text, utf8_literal("before bold and italic\nsecond plain line")) &&
              (before_flags & text_style_bold) && (before_flags & text_style_italic) &&
              after && utf8_equal(after->text, utf8_literal("after")),
          "message code block: styled plain lines merge up to the next code boundary");
    const float markup = before ? ui_rect_width(before->rectangle) : 0.0f;
    check(c.row && before && after && framed_throughout(c.code) &&
              close_to(c.box.left, before->rectangle.left) && close_to(c.box.right - c.box.left, markup * .9f) &&
              close_to(c.box.top, before->rectangle.top + before->measured_height + kMessageCodeBlockMargin) &&
              close_to(after->rectangle.top, c.box.bottom) &&
              close_to(f.presentation.measurements[0].content_height,
                       after->rectangle.top + after->measured_height - before->rectangle.top),
          "message code block: framed at 90% width between the lines around it, inside the measured body");
    if (c.row)
        message_scene_release(&f.scene);
    const auto leading = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"```\nSimple app\nsecond line\n```","timestamp":"2026-09-05T10:00:00Z"}})");
    c = code_block_case(leading, {});
    const UiLayoutNode* content = c.row ? message_scene_layout_node(f.scene, c.row->content) : nullptr;
    check(content && framed_throughout(c.code) && close_to(c.box.top, content->content_rectangle.top) &&
              close_to(f.presentation.measurements[0].content_height, c.box.bottom - c.box.top),
          "message code block: a leading block's box starts the body and its height is the body's");
    if (c.row)
        message_scene_release(&f.scene);
}

// Only a row's author widget carries the author header. Poll labels and card
// text are unmeasured styled runs too; while a popup hides the selected poll
// scene they paint from the message pass and must keep their own style.
void test_author_header_isolation(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"x","timestamp":"2026-09-05T10:00:00Z"}})");
    const auto* row = converge_message(payload);
    ChatStateStore state{};
    static UserStore users{};
    MessageAuthorLayout header{};
    char label[] = "Couldn't update the poll";
    const TextStyleSpan subtext{0, sizeof(label) - 1, text_style_fold_size(text_style_none, text_size_sm)};
    MessageTextCommand command{};
    command.text = {label, sizeof(label) - 1};
    command.styles = {&subtext, 1};
    command.message_id = 1;
    command.rectangle = {0, 0, 300, 20};
    command.clip = {0, 0, 900, 1000};
    command.measurement_kind = MessageTextMeasurementKind::none;
    bool isolated = row != nullptr;
    if (row) {
        command.owner = {&f.scene.widgets, row->author};
        const bool author = message_author_layout(f.scene, command, state, users, &f.cache, kDefaultTheme, &header);
        command.owner = {&f.scene.widgets, row->poll};
        const bool poll = message_author_layout(f.scene, command, state, users, &f.cache, kDefaultTheme, &header);
        command.owner = {&f.scene.widgets, row->content};
        const bool content = message_author_layout(f.scene, command, state, users, &f.cache, kDefaultTheme, &header);
        command.owner = {&f.scene.widgets, row->author};
        command.measurement_slot = 1;
        const bool slot = message_author_layout(f.scene, command, state, users, &f.cache, kDefaultTheme, &header);
        isolated = author && !poll && !content && !slot;
        message_scene_release(&f.scene);
    }
    check(isolated, "author header: only the author widget's first run becomes the bold name; poll and card "
                    "runs keep their style while a popup is open");
}

void test_embed_parity(Check check) noexcept {
    if (!text_system_initialize(&text_system, L"Segoe UI", 16).ok()) {
        check(false, "embed_parity_text_system");
        return;
    }
    // The client's policy (main.cpp); the bare shaping engine keeps DirectWrite parity.
    const bool previous = text_browser_invisibles();
    text_set_browser_invisibles(true);
    test_default_ignorables(check);
    test_baseless_ignorables(check);
    test_markdown(check);
    test_link_hits(check);
    test_calendar(check);
    test_thumbnail_rules(check);
    test_embed_gallery(check);
    test_fxtwitter_embed(check);
    test_text_sizes(check);
    test_quote_blocks(check);
    probe_system_coverage();
    test_native_text_never_fails(check);
    test_vxtwitter_embeds(check);
    test_reply_previews(check);
    test_reply_author_and_system_quotes(check);
    test_interaction_headers(check);
    test_interaction_data(check);
    test_component_embed(check);
    test_code_block_embeds(check);
    test_author_header_isolation(check);
    text_set_browser_invisibles(previous);
    text_system_release(&text_system);
}

} // namespace maicord
