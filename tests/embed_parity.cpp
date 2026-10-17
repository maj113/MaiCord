// Official-client parity for embeds and the text they share with messages:
// default-ignorable code points render invisible, markdown escapes, quotes and
// links follow the official client's rules, and an fxtwitter-style embed
// decodes, lays out and paints the same way (author/footer icons, footer
// timestamp, video without a corner thumbnail).
#include "maicord/ui/text.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/message_component_media.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/ui/ansi_text.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include <dwrite.h> // Test-only coverage oracle; the client has no DirectWrite.

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

TextSystem text_system;
wchar_t utf16[1024];
TextGlyph glyphs[1024];
TextRun runs[256];
u16 clusters[1024];
TextHitRegion hit_regions[64];
TextDecoration decorations[64];
// Simple scripts the native shaper draws from a covering system face: Bamum,
// Bamum Supplement, Yi, Vai, Tifinagh, Canadian Syllabics, N'Ko, Ol Chiki, Lisu.
constexpr u32 kSimpleScripts[]{0xA6A0, 0x169E7, 0xA000, 0xA564, 0x2D63, 0x1401, 0x07CA, 0x1C5A, 0xA4D0};
bool simple_script_covered[sizeof(kSimpleScripts) / sizeof(kSimpleScripts[0])];
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

bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && (a.bytes == 0 || bytes_equal(a.data, b.data, a.bytes));
}
bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i = 0; part.bytes <= text.bytes && i <= text.bytes - part.bytes; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes))
            return true;
    return false;
}
bool starts_with(Utf8View text, Utf8View part) noexcept {
    return part.bytes <= text.bytes && bytes_equal(text.data, part.data, part.bytes);
}
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

struct Shape {
    bool ok;
    u32 notdef;
    float width;
};
Shape shape(Utf8View text) noexcept {
    ShapedText output{};
    Shape result{};
    if (!shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}))
        return result;
    result.ok = text_shape_utf8_styled(&text_system, text, {}, 1000, 1000, {utf16, 1024}, &output).ok();
    for (u32 i = 0; i < output.glyph_count; ++i)
        result.notdef += output.glyphs[i].index == 0 ? 1u : 0u;
    result.width = output.width;
    shaped_text_reset(&output);
    return result;
}

// Every Default_Ignorable code point is invisible in the official client (the
// browser hides it). FE0F after a plain letter, U+2065, Hangul fillers, tags and
// variation selectors once drew tofu or took width.
void test_default_ignorables(Check check) noexcept {
    const u32 hidden[]{0x00AD, 0x034F, 0x061C, 0x115F, 0x1160, 0x17B4, 0x17B5, 0x180B, 0x180E, 0x200B,
                       0x200C, 0x200D, 0x200E, 0x2060, 0x2062, 0x2063, 0x2064, 0x2065, 0x206A, 0x3164,
                       0xFE0E, 0xFE0F, 0xFEFF, 0xFFA0, 0xFFF0, 0xE0001, 0xE0020, 0xE0100, 0x1D173};
    const Shape plain = shape(utf8_literal("abcd"));
    const Shape line = shape(utf8_literal("Hello"));
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
            const Shape s = shape(Utf8View{text, n});
            const float expected = form ? line.width : plain.width;
            invisible = invisible && s.ok && s.notdef == 0 && s.width > expected - 0.5f && s.width < expected + 0.5f;
        }
    }
    check(invisible, "default_ignorables_shape_without_tofu_or_advance");
    const Shape braille = shape(utf8_literal("\xE2\xA0\x80Hi"));
    check(braille.ok && braille.notdef == 0 && braille.width > shape(utf8_literal("Hi")).width,
          "braille_blank_is_a_visible_blank_not_tofu");
    // Emoji presentation sequences keep their selector.
    const Shape heart = shape(utf8_literal("\xE2\x9D\xA4\xEF\xB8\x8F"));
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

MessageMarkdownResult parse(Utf8View source, char* output, u32* styles, TextStyleSpan* spans,
                            MessageMarkdownSemantic* semantics, MessageMarkdownOptions options = {}) noexcept {
    return message_markdown_parse(source, Slice<char>{output, 1024}, Slice<u32>{styles, 1024},
                                  Slice<TextStyleSpan>{spans, 128}, options,
                                  Slice<MessageMarkdownSemantic>{semantics, 32});
}

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
    check(result.ok() && same(result.text, utf8_literal(R"([GOOD NEWS!] Comet... NOW! \a \1 _x_ \)")),
          "markdown_escape_matches_official_rule");
    // A "[" that never becomes a masked link must not hold the delimiter
    // stack: the quote and bold that follow still apply.
    const Utf8View quoted = utf8_literal("[GOOD NEWS\\!]\n> quoted **line**\n>no space\n  > indented\nplain");
    result = parse(quoted, output, styles, spans, semantics);
    const u32 quote_line = 13; // "quoted"
    check(result.ok() && same(result.text, utf8_literal("[GOOD NEWS!]\nquoted line\n>no space\nindented\nplain")) &&
              (style_at(result, spans, quote_line) & text_style_quote) &&
              (style_at(result, spans, quote_line + 7) & (text_style_quote | text_style_bold)) ==
                  (text_style_quote | text_style_bold) &&
              !(style_at(result, spans, 25) & text_style_quote) && (style_at(result, spans, 35) & text_style_quote) &&
              !(style_at(result, spans, result.text.bytes - 1) & text_style_quote),
          "markdown_blockquote_after_stray_bracket_and_with_indent");
    result = parse(utf8_literal(">>> first\n> second\n# big"), output, styles, spans, semantics);
    check(result.ok() && same(result.text, utf8_literal("first\n> second\nbig")) &&
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
                 same(semantic_source(links, semantics[i]), expected_links[i]);
    check(linked && same(result.text,
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
              same(result.text, utf8_literal("[t](https://e.test) https://e.test # x")),
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
    bool ok = shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
              shaped_text_bind_hit_regions(&output, {hit_regions, 64}) &&
              shaped_text_bind_decorations(&output, {decorations, 64});
    ok = ok && text_shape_utf8_interactive(&text_system, text, {styles, 3}, {hits, 1}, 160, 1000, {utf16, 1024},
                                           &output)
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
    check(starts_with(at(stamp, stamp + 86'400), utf8_literal("Yesterday at ")) &&
              starts_with(at(stamp, stamp), utf8_literal("Today at ")) &&
              starts_with(at(stamp + 86'400, stamp), utf8_literal("Tomorrow at ")) &&
              !contains(at(stamp + 5 * 86'400, stamp), utf8_literal(":")) &&
              contains(at(stamp, stamp + 10 * 86'400), utf8_literal(":")),
          "embed_footer_calendar_today_yesterday_tomorrow_and_dates");
}

constexpr u32 kWidgets = 2 * kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord record[2];
    u32 order[2];
    u8 text[2048];
    MessageRichRecord rich_record[2];
    u16 rich_order[2];
    u8 rich_text[32768];
    u32 heights[2];
    u64 fenwick[2];
    UiWidget widget[kWidgets];
    UiLayoutNode node[kWidgets];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MessageTextCommand commands[256];
    char utf8[32768];
    TextStyleSpan styles[1024];
    MessageMarkdownSemantic semantics[256];
    MessageTextMeasurement measurements[4];
    MessageTextSpoilerReveal spoilers[4];
    u32 byte_styles[32768];
    MessageTextPresentation presentation;
    UiTextMeasureRecord cache_records[512];
    UiTextMeasureCache cache;
    PaintStream paint;
    MessageMediaCommand media[32];
};
Fixture fixture;

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* output) noexcept {
    auto result = text_measure_utf8_styled(&text_system, request.text, request.styles, request.constraint.width,
                                           request.constraint.height, {utf16, 1024},
                                           request.no_wrap ? TextWrapping::no_wrap : TextWrapping::wrap);
    *output = {{result.width_including_trailing_whitespace, result.height}, 0, result.line_count};
    return result.ok();
}
bool finish(void*, const Theme&) noexcept {
    return true;
}

const MessageTextCommand* command(const MessageTextPresentation& p, u16 id, u32 nth = 0) noexcept {
    for (u32 i = 0; i < p.command_count; ++i)
        if (p.commands[i].source_id == id && !nth--)
            return &p.commands[i];
    return nullptr;
}
Utf8View link_target(const Fixture& f, const MessageTextCommand* c, u32 nth = 0) noexcept {
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
float shaped_em(const MessageTextCommand* c, float* height = nullptr) noexcept {
    ShapedText output{};
    float em = -1;
    if (c && shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
        shaped_text_bind_decorations(&output, {decorations, 64}) &&
        text_shape_utf8_styled(&text_system, c->text, c->styles, ui_rect_width(c->rectangle), 4096, {utf16, 1024},
                               &output)
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

// An fxtwitter-style link embed, decoded from the Gateway JSON, through the
// text pass and paint: escapes and quotes in the description, invisible
// separators, author/footer icons through the media proxy, the footer
// timestamp, and the video shown without a corner thumbnail.
void test_fxtwitter_embed(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://x.com/quill_example/status/1","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"rich","url":"https://x.com/quill_example/status/1","color":6765239,"description":"[GOOD NEWS\\!] ⁣⁣Я Comet\\.\\.\\.\n\n> quoted tweet line\n> second line\n\n️️PREVIEW\\.\\.\\. IT SHIPS NOW\\! https://x.com/a_b_c","author":{"name":"quill (…) (@quill_example)","url":"https://x.com/quill_example","icon_url":"https://pbs.twimg.com/profile_images/1/a.jpg","proxy_icon_url":"https://images-ext-1.discordapp.net/external/abc/https/pbs.twimg.com/profile_images/1/a.jpg"},"footer":{"text":"EmbedX","icon_url":"https://embed.example.com/icon.png","proxy_icon_url":"https://images-ext-1.discordapp.net/external/def/https/embed.example.com/icon.png"},"timestamp":"2026-09-04T20:08:00.000000+00:00","provider":{"name":"EmbedX","url":"https://embed.example.com"},"thumbnail":{"url":"https://pbs.twimg.com/media/b.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/ghi/https/pbs.twimg.com/media/b.jpg","width":1280,"height":720,"flags":0},"video":{"url":"https://video.twimg.com/c.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/jkl/https/video.twimg.com/c.mp4","width":1280,"height":720,"flags":0}}]}})json");
    static u32 structurals[2048];
    static char packed_text[16384];
    static u64 ids[128];
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageComponent components[8]{};
    GatewayMessageEmbed embeds[1]{};
    GatewayMessageEmbedField fields[4]{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {components, 8};
    scratch.embeds = {embeds, 1};
    scratch.embed_fields = {fields, 4};
    GatewayMessageEvent message{};
    bool ok = gateway_decode_envelope(payload, {structurals, 2048}, 32, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {structurals, 2048}, 32, {packed_text, sizeof(packed_text)},
                                            {ids, 128}, &routed, &packed)
                  .ok() &&
              gateway_client_event_as_message(routed, packed, scratch, &message) && message.embeds.count == 1;
    const GatewayMessageEmbed& decoded = message.embeds[0];
    check(ok && same(decoded.author_url, utf8_literal("https://x.com/quill_example")) &&
              starts_with(decoded.author_icon, utf8_literal("https://images-ext-1.discordapp.net/external/abc/")) &&
              starts_with(decoded.footer_icon, utf8_literal("https://images-ext-1.discordapp.net/external/def/")) &&
              same(decoded.provider_name, utf8_literal("EmbedX")) &&
              same(decoded.provider_url, utf8_literal("https://embed.example.com")) &&
              decoded.timestamp_ms == 1'788'552'480'000ull && decoded.video.width == 1280 &&
              starts_with(decoded.video.proxy_url, utf8_literal("https://images-ext-1.discordapp.net/")),
          "embed_decodes_author_footer_provider_timestamp_and_video_through_dispatch");

    ok = ok && message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
         message_reconcile_create(&f.store, message).ok() &&
         message_rich_store_initialize(&f.rich, {f.rich_record, 2}, {f.rich_order, 2},
                                       {f.rich_text, sizeof(f.rich_text)}) &&
         message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, 2}, {f.fenwick, 2}, 1, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000),
                                     false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, 2, 2, {f.widget, kWidgets}, {f.node, kWidgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 1024}, {f.semantics, 256}, {f.measurements, 4},
                                              {f.spoilers, 4}, {f.byte_styles, 32768}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 512}, measure, nullptr);
    const MessageRichRecord* rich = ok ? message_rich_store_find(f.rich, 1) : nullptr;
    const MessageRichEmbed* embed = rich && rich->embed_count == 1 ? &rich->embeds[0] : nullptr;
    MessageRichEmbedMediaSelection selected{};
    check(embed && !message_rich_embed_plan(f.rich, *embed).corner_thumbnail &&
              message_rich_embed_media_select(f.rich, *embed, &selected) &&
              selected.resource == MediaEmbedResource::video && selected.format == MediaAttachmentFormat::mp4,
          "video_embed_uses_thumbnail_only_as_poster");

    ChatStateStore state{};
    NavigationView navigation{};
    constexpr u64 now = 1'788'552'480 + 86'400; // the next day: "Yesterday at"
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, now, 1};
    ok = ok && embed && message_scene_update(&f.scene, {0, 0, 900, 1000}).ok() &&
         message_text_coordinator_converge(request).ok();
    const MessageSceneRowWidget* row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    check(ok && row, "fxtwitter_embed_converges");
    if (!row) {
        message_scene_release(&f.scene);
        return;
    }
    const auto& p = f.presentation;
    const auto* provider = command(p, 0x8008);
    const auto* author = command(p, 0x8002);
    const auto* description = command(p, 0x8004);
    const auto* footer = command(p, 0x8005);
    check(provider && same(provider->text, utf8_literal("EmbedX")) &&
              same(link_target(f, provider), utf8_literal("https://embed.example.com")) &&
              author && same(link_target(f, author), utf8_literal("https://x.com/quill_example")) &&
              has_style(author, utf8_literal("quill"), text_style_bold),
          "embed_provider_and_author_link_to_their_urls");
    // The quote is its own block between the two plain runs.
    const auto* quote = command(p, 0x8004, 1);
    const auto* tail = command(p, 0x8004, 2);
    check(description && quote && tail && !command(p, 0x8004, 3) &&
              starts_with(description->text, utf8_literal("[GOOD NEWS!] ")) &&
              ends_with(description->text, utf8_literal(" Comet...\n")) &&
              same(quote->text, utf8_literal("quoted tweet line\nsecond line")) &&
              starts_with(tail->text, utf8_literal("\n")) &&
              contains(tail->text, utf8_literal("PREVIEW... IT SHIPS NOW! ")) &&
              !contains(tail->text, utf8_literal("\\")) &&
              has_style(quote, utf8_literal("quoted tweet line"), text_style_quote) &&
              has_style(quote, utf8_literal("second"), text_style_quote) &&
              !has_style(tail, utf8_literal("PREVIEW"), text_style_quote) &&
              same(link_target(f, tail), utf8_literal("https://x.com/a_b_c")),
          "embed_description_escapes_quotes_and_links");
    check(footer && starts_with(footer->text, utf8_literal("EmbedX \xE2\x80\xA2 Yesterday at ")) &&
              has_style(footer, utf8_literal("EmbedX"), text_style_subtext),
          "embed_footer_shows_text_bullet_and_calendar_timestamp");
    check(description && shape(description->text).notdef == 0, "embed_description_invisible_separators_draw_nothing");
    // The official client's embed type scale: provider/footer text-xs (12), author and
    // description text-sm (14), each run shaped at that em.
    check(sized_throughout(provider, text_size_xs) && sized_throughout(author, text_size_sm) &&
              sized_throughout(description, text_size_sm) && sized_throughout(quote, text_size_sm) &&
              sized_throughout(tail, text_size_sm) && sized_throughout(footer, text_size_xs) &&
              shaped_em(footer) == 12 && shaped_em(provider) == 12 && shaped_em(author) == 14 &&
              shaped_em(quote) == 14,
          "embed_text_uses_reference_sizes_per_part");
    // The description quote is indented past its spine inside the card text.
    check(quote && description && quote->rectangle.left == description->rectangle.left + 16 &&
              quote->rectangle.top >= description->rectangle.top + description->measured_height + 4 &&
              tail && tail->rectangle.top >= quote->rectangle.top + quote->measured_height + 4,
          "embed_description_quote_is_an_indented_block");

    // Geometry: icons left of the author/footer text, video below the text,
    // footer last, no corner thumbnail; images keyed by proxied URL.
    const UiLayoutNode* item = message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::embed, 0);
    const MessageRichItemLayout& layout = row->rich_item_layout;
    UiRect author_icon{}, footer_icon{};
    const bool icons = item &&
                       message_rich_embed_author_icon_rect(f.rich, *embed, layout, 0, item->content_rectangle,
                                                           &author_icon) &&
                       message_rich_embed_footer_icon_rect(f.rich, *embed, layout, 0, item->content_rectangle,
                                                           &footer_icon);
    check(icons && ui_rect_width(author_icon) == 24 && ui_rect_width(footer_icon) == 20 &&
              author_icon.left == item->content_rectangle.left + 16 && author && footer &&
              author->rectangle.left == author_icon.right + 8 && footer->rectangle.left == footer_icon.right + 8 &&
              layout.embed_media_widths[0] == 400 && layout.embed_media_heights[0] == 225 &&
              layout.embed_media_tops[0] > layout.embed_author_tops[0] + 24 &&
              layout.embed_footer_tops[0] >= layout.embed_media_tops[0] + 225 + 8,
          "embed_author_media_footer_geometry");
    paint_stream_reset(&f.paint, 900, 1000);
    MessageMediaCommandStream media{};
    MessagePresentationResult painted{};
    bool paint_ok = message_media_command_stream_initialize(&media, {f.media, 32}) &&
                    message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &painted);
    u32 videos = 0, thumbnails = 0, round_icons = 0;
    const Utf8View author_url = message_rich_embed_author_icon(f.rich, *embed);
    const Utf8View footer_url = message_rich_embed_footer_icon(f.rich, *embed);
    for (u32 i = 0; paint_ok && i < media.count; ++i) {
        const MessageMediaCommand& c = media.storage[i];
        if (c.kind == MessageMediaCommandKind::embed) {
            videos += c.embed_resource == static_cast<u8>(MediaEmbedResource::video) &&
                              c.rectangle.left == item->content_rectangle.left + 16
                          ? 1u
                          : 0u;
            thumbnails += c.embed_resource == static_cast<u8>(MediaEmbedResource::thumbnail) ? 1u : 0u;
        } else if (c.kind == MessageMediaCommandKind::component) {
            const bool author_match =
                c.id == message_component_media_key(hash64_bytes(kHash64Seed, author_url.data, author_url.bytes),
                                                    MediaAttachmentFormat::jpeg)
                            .id &&
                c.rectangle.left == author_icon.left && c.rectangle.top == author_icon.top && c.tile_radius == 12;
            const bool footer_match =
                c.id == message_component_media_key(hash64_bytes(kHash64Seed, footer_url.data, footer_url.bytes),
                                                    MediaAttachmentFormat::png)
                            .id &&
                c.rectangle.left == footer_icon.left && c.rectangle.top == footer_icon.top && c.tile_radius == 10;
            round_icons += author_match || footer_match ? 1u : 0u;
        }
    }
    check(paint_ok && videos == 1 && thumbnails == 0 && round_icons == 2,
          "embed_paints_video_and_round_icons_without_corner_thumbnail");
    paint_stream_release(&f.paint);
    message_scene_release(&f.scene);
}

// The official client's thumbnail rules per embed type.
void test_thumbnail_rules(Check check) noexcept {
    static MessageRichRecord records[1];
    static u16 order[1];
    static u8 text[4096];
    MessageRichStore store{};
    GatewayMessageEmbed embeds[1]{};
    embeds[0].title = utf8_literal("Title");
    embeds[0].thumbnail.url = utf8_literal("https://e.test/t.png");
    embeds[0].thumbnail.width = embeds[0].thumbnail.height = 128;
    GatewayMessageEvent event{};
    event.id = 5;
    event.fields = gateway_message_field_embeds;
    event.embeds = {embeds, 1};
    const auto plan = [&](GatewayMessageEmbedType type, bool image, bool video,
                          MessageRichEmbedMediaSelection* media) noexcept {
        embeds[0].type = type;
        embeds[0].image = {};
        embeds[0].video = {};
        if (image) {
            embeds[0].image.url = utf8_literal("https://e.test/i.png");
            embeds[0].image.width = embeds[0].image.height = 64;
        }
        if (video) {
            embeds[0].video.url = utf8_literal("https://e.test/v.mp4");
            embeds[0].video.width = embeds[0].video.height = 64;
        }
        if (!message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)}) ||
            !message_rich_store_apply(&store, 5, 1, event).ok())
            return MessageRichEmbedPlan{};
        const MessageRichRecord* rich = message_rich_store_find(store, 5);
        *media = {};
        if (rich)
            message_rich_embed_media_select(store, rich->embeds[0], media);
        return rich ? message_rich_embed_plan(store, rich->embeds[0]) : MessageRichEmbedPlan{};
    };
    MessageRichEmbedMediaSelection media{};
    const bool rich_corner = plan(GatewayMessageEmbedType::rich, true, false, &media).corner_thumbnail &&
                             media.resource == MediaEmbedResource::image;
    const bool rich_video = !plan(GatewayMessageEmbedType::rich, false, true, &media).corner_thumbnail &&
                            media.resource == MediaEmbedResource::video;
    const bool link_corner = plan(GatewayMessageEmbedType::link, false, false, &media).corner_thumbnail;
    const bool link_media = !plan(GatewayMessageEmbedType::link, true, false, &media).corner_thumbnail;
    const MessageRichEmbedPlan article = plan(GatewayMessageEmbedType::article, false, false, &media);
    const bool article_large = !article.corner_thumbnail && media.resource == MediaEmbedResource::thumbnail;
    const MessageRichEmbedPlan video = plan(GatewayMessageEmbedType::video, false, true, &media);
    check(rich_corner && rich_video && link_corner && link_media && article_large && !video.corner_thumbnail &&
              !video.description && !video.markdown && media.resource == MediaEmbedResource::video,
          "embed_thumbnail_image_video_selection_per_type");
}
// As in the official client, embeds sharing a url fold into the first, whose
// media becomes a gallery of their images (a video wins over the images).
void test_embed_gallery(Check check) noexcept {
    static MessageRichRecord records[1];
    static u16 order[1];
    static u8 text[4096];
    MessageRichStore store{};
    GatewayMessageEmbed embeds[5]{};
    const char* images[]{"https://e.test/1.jpg", "https://e.test/2.jpg", "https://e.test/3.jpg",
                         "https://e.test/4.jpg", "https://e.test/5.jpg"};
    const auto reset = [&](u32 count) noexcept {
        for (u32 i = 0; i < 5; ++i) {
            embeds[i] = {};
            embeds[i].type = GatewayMessageEmbedType::rich;
            embeds[i].url = utf8_literal("https://x.test/status/2");
            if (i < count) {
                embeds[i].image.url = {images[i], 20};
                embeds[i].image.width = 1200;
                embeds[i].image.height = 900;
            }
        }
        embeds[0].description = utf8_literal("post");
    };
    const auto apply = [&](u32 count) noexcept -> const MessageRichRecord* {
        GatewayMessageEvent event{};
        event.id = 5;
        event.fields = gateway_message_field_embeds;
        event.embeds = {embeds, count};
        return message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)}) &&
                       message_rich_store_apply(&store, 5, 1, event).ok()
                   ? message_rich_store_find(store, 5)
                   : nullptr;
    };
    const UiRect block{0, 0, 400, 300};
    const auto same_rect = [](UiRect a, UiRect b) noexcept {
        return close_to(a.left, b.left) && close_to(a.top, b.top) && close_to(a.right, b.right) &&
               close_to(a.bottom, b.bottom);
    };
    bool merges = true;
    for (u32 count = 1; count <= 4; ++count) {
        reset(count);
        const MessageRichRecord* rich = apply(count);
        MessageRichEmbedGallery gallery{};
        const bool shown = rich && message_rich_embed_gallery(store, *rich, 0, &gallery);
        merges = merges && rich && shown == (count > 1) && gallery.count == (count > 1 ? count : 0u);
        for (u32 i = 0; rich && i < count; ++i)
            merges = merges && message_rich_embed_merge_host(store, *rich, i) == 0 &&
                     (count == 1 || gallery.members[i] == i);
        const MessageRichItemLayout layout =
            rich ? message_rich_estimate_item_layout(&store, rich, 900) : MessageRichItemLayout{};
        merges = merges && layout.embed_media_heights[0] == 300 && layout.embed_media_widths[0] == 400;
        for (u32 i = 1; i < count; ++i)
            merges = merges && layout.heights[kMessageRichEmbedItemOffset + i] == 0;
    }
    // Two: side by side; three: one left, two stacked right; four: 2x2.
    merges = merges && same_rect(message_rich_embed_gallery_tile(block, 2, 0), {0, 0, 198, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 2, 1), {202, 0, 400, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 3, 0), {0, 0, 198, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 3, 1), {202, 0, 400, 148}) &&
             same_rect(message_rich_embed_gallery_tile(block, 3, 2), {202, 152, 400, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 4, 2), {0, 152, 198, 300}) &&
             same_rect(message_rich_embed_gallery_tile(block, 4, 3), {202, 152, 400, 300}) &&
             !ui_rect_has_area(message_rich_embed_gallery_tile(block, 5, 4));
    // One merged image under an imageless card fills the first column.
    reset(2);
    embeds[0].image = {};
    const MessageRichRecord* rich = apply(2);
    MessageRichEmbedGallery gallery{};
    MessageRichEmbedGalleryPlace place{};
    merges = merges && rich && message_rich_embed_gallery(store, *rich, 0, &gallery) && gallery.count == 1 &&
             gallery.members[0] == 1 && message_rich_embed_gallery_place(store, *rich, 1, &place) &&
             place.host == 0 && place.ordinal == 0 &&
             same_rect(message_rich_embed_gallery_tile(block, 1, 0), {0, 0, 198, 300}) &&
             message_rich_embed_gallery_width(store, rich->embeds[0], 1000) == 488;
    // A portrait first image narrower than 300 keeps the 520-wide card.
    reset(2);
    embeds[0].image.width = 900;
    embeds[0].image.height = 1200;
    rich = apply(2);
    merges = merges && rich && message_rich_embed_gallery_width(store, rich->embeds[0], 1000) == 488 &&
             message_rich_embed_gallery_width(store, rich->embeds[0], 300) == 300;
    check(merges, "embed_gallery_merges_one_to_four_images");

    // Different urls never merge; an image-less same-url embed is dropped.
    reset(3);
    embeds[1].url = utf8_literal("https://x.test/status/3");
    embeds[2].image = {};
    embeds[2].title = utf8_literal("dropped");
    rich = apply(3);
    const bool separate = rich && message_rich_embed_merge_host(store, *rich, 1) == 1 &&
                          message_rich_embed_merge_host(store, *rich, 2) == 0 &&
                          !message_rich_embed_gallery(store, *rich, 0, &gallery) &&
                          !message_rich_embed_gallery(store, *rich, 1, &gallery);
    check(separate, "embed_gallery_keeps_other_urls_separate");

    // A video (or gifv) card plays its video: the merged images are dropped.
    reset(3);
    embeds[0].video.url = utf8_literal("https://e.test/v.mp4");
    embeds[0].video.width = 1280;
    embeds[0].video.height = 720;
    rich = apply(3);
    MessageRichEmbedMediaSelection media{};
    const MessageRichItemLayout video_layout =
        rich ? message_rich_estimate_item_layout(&store, rich, 900) : MessageRichItemLayout{};
    bool video = rich && !message_rich_embed_gallery(store, *rich, 0, &gallery) &&
                 message_rich_embed_merge_host(store, *rich, 2) == 0 &&
                 !message_rich_embed_gallery_place(store, *rich, 1, &place) &&
                 message_rich_embed_media_select(store, rich->embeds[0], &media) &&
                 media.resource == MediaEmbedResource::video &&
                 video_layout.heights[kMessageRichEmbedItemOffset + 1] == 0 &&
                 video_layout.embed_media_heights[0] == 225;
    // A merged video embed adds nothing; the images still form the gallery.
    reset(2);
    embeds[2].video.url = utf8_literal("https://e.test/w.mp4");
    rich = apply(3);
    video = video && rich && message_rich_embed_gallery(store, *rich, 0, &gallery) && gallery.count == 2 &&
            message_rich_embed_merge_host(store, *rich, 2) == 0;
    check(video, "embed_gallery_video_wins_over_merged_images");
}

// Markup headings and subtext take the official client's rem sizes as explicit
// span sizes (24/20/16/14 DIP); a base size covers the plain text too. Each sized
// run shapes at its own em while unsized runs keep the system em.
void test_text_sizes(Check check) noexcept {
    static char output[1024];
    static u32 styles[1024];
    static TextStyleSpan spans[128];
    static MessageMarkdownSemantic semantics[32];
    MessageMarkdownOptions sized{};
    sized.text_sizes = true;
    auto size = [&](const MessageMarkdownResult& result, u32 byte) noexcept -> u8 {
        for (u32 i = 0; i < result.span_count; ++i)
            if (byte >= spans[i].byte_offset && byte < spans[i].byte_offset + spans[i].byte_count)
                return spans[i].size;
        return 0xFF;
    };
    auto result = parse(utf8_literal("# a\n## b\n### c\n-# d\ne **f**"), output, styles, spans, semantics, sized);
    // Display: "a\nb\nc\nd\ne f"
    check(result.ok() && size(result, 0) == text_size_xl && size(result, 2) == text_size_lg &&
              size(result, 4) == text_size_md && size(result, 6) == text_size_sm && size(result, 8) == 0xFF &&
              size(result, 10) == text_size_inherit,
          "markdown_headings_and_subtext_take_reference_rem_sizes");
    sized.base_size = text_size_sm;
    result = parse(utf8_literal("plain **bold** # not a heading"), output, styles, spans, semantics, sized);
    u32 covered = 0;
    bool all = result.ok();
    for (u32 i = 0; all && i < result.span_count; ++i) {
        all = spans[i].byte_offset == covered && spans[i].size == text_size_sm;
        covered += spans[i].byte_count;
    }
    check(all && covered == result.text.bytes && result.span_count == 3 && spans[0].flags == 0 &&
              spans[1].flags == text_style_bold,
          "markdown_base_size_covers_plain_text_with_size_only_spans");
    result = parse(utf8_literal("# a"), output, styles, spans, semantics);
    check(result.ok() && size(result, 0) == text_size_inherit, "markdown_without_text_sizes_keeps_flag_sizes");

    const auto em = [&](Utf8View text, TextStyleSpan span, float* width = nullptr) noexcept {
        ShapedText out{};
        float value = -1;
        if (shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
            shaped_text_bind_decorations(&out, {decorations, 64}) &&
            text_shape_utf8_styled(&text_system, text, {&span, 1}, 1000, 1000, {utf16, 1024}, &out).ok() &&
            out.run_count != 0) {
            value = out.runs[0].em_size;
            if (width)
                *width = out.width;
        }
        shaped_text_reset(&out);
        return value;
    };
    const Utf8View hello = utf8_literal("Hello");
    float body = 0, xl = 0, xs = 0;
    check(em(hello, {0, 5, text_style_bold}, &body) == 16 && em(hello, {0, 5, text_style_bold, text_size_xl}, &xl) == 24 &&
              em(hello, {0, 5, 0, text_size_xs}, &xs) == 12 && em(hello, {0, 5, text_style_heading_3, text_size_md}) == 16 &&
              em(hello, {0, 5, text_style_heading_3}) == 16 * 1.1f &&
              em(hello, {0, 5, text_style_monospace, text_size_sm}) == 14 && xl > body * 1.3f &&
              xs < body * .85f,
          "sized_spans_shape_at_their_own_em");
    // Sized spans fold into the native engine's flags and keep their em.
    TextSystem native = text_system;
    bool native_sizes = native.native_text && native.native_format;
    const TextStyleSpan native_spans[]{{0, 5, text_style_bold, text_size_xl}, {0, 5, 0, text_size_sm},
                                       {0, 5, text_style_heading_3, text_size_md}, {0, 5, text_style_bold, text_size_xs}};
    const float native_ems[]{24, 14, 16, 12};
    for (u32 i = 0; native_sizes && i < 4; ++i) {
        ShapedText out{};
        native_sizes = shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
                       text_shape_utf8_styled(&native, hello, {&native_spans[i], 1}, 1000, 1000, {utf16, 1024}, &out)
                           .ok() &&
                       out.run_count && out.runs[0].native_font && out.runs[0].em_size == native_ems[i] &&
                       text_measure_utf8_styled(&native, hello, {&native_spans[i], 1}, 1000, 1000, {utf16, 1024})
                           .ok();
        shaped_text_reset(&out);
    }
    check(native_sizes, "sized_spans_shape_in_the_native_engine");
    TextStyleSpan invalid{0, 5, 0, text_size_count};
    check(em(hello, invalid) < 0 && em(hello, {0, 5, 0, 0}) < 0, "size_only_spans_need_a_valid_size");
    const auto measured = [&](TextStyleSpan span) noexcept {
        return text_measure_utf8_styled(&text_system, hello, {&span, 1}, 1000, 1000, {utf16, 1024}).height;
    };
    check(measured({0, 5, 0, text_size_xl}) > measured({0, 5, text_style_bold}) &&
              measured({0, 5, 0, text_size_xs}) < measured({0, 5, text_style_bold}),
          "sized_spans_measure_their_line_height");
}

// Quotes are blocks as in the official client: the body splits into plain and quoted
// runs, the quote text starts 16 DIP in (4-DIP spine + 12-DIP padding) and
// wraps at min(width - 4, 90%) - 20, every wrapped line keeps the indent, one
// spine spans the block and 4-DIP margins separate the blocks. Links inside
// keep their source, headings and subtext keep their sizes.
void test_quote_blocks(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"# Title\nintro\n> quoted **bold** text long enough to wrap across several lines of this narrow pane, with https://q.test/x inside\n> second\n-# after","timestamp":"2026-09-05T10:00:00Z"}})json");
    static u32 structurals[2048];
    static char packed_text[16384];
    static u64 ids[128];
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageComponent components[8]{};
    GatewayMessageEmbed embeds[1]{};
    GatewayMessageEmbedField fields[4]{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {components, 8};
    scratch.embeds = {embeds, 1};
    scratch.embed_fields = {fields, 4};
    GatewayMessageEvent message{};
    bool ok = gateway_decode_envelope(payload, {structurals, 2048}, 32, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {structurals, 2048}, 32, {packed_text, sizeof(packed_text)},
                                            {ids, 128}, &routed, &packed)
                  .ok() &&
              gateway_client_event_as_message(routed, packed, scratch, &message);
    ok = ok && message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
         message_reconcile_create(&f.store, message).ok() &&
         message_rich_store_initialize(&f.rich, {f.rich_record, 2}, {f.rich_order, 2},
                                       {f.rich_text, sizeof(f.rich_text)}) &&
         message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, 2}, {f.fenwick, 2}, 1, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000),
                                     false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, 2, 2, {f.widget, kWidgets}, {f.node, kWidgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 1024}, {f.semantics, 256}, {f.measurements, 4},
                                              {f.spoilers, 4}, {f.byte_styles, 32768}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 512}, measure, nullptr);
    ChatStateStore state{};
    NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, 1'788'552'480, 1};
    ok = ok && message_scene_update(&f.scene, {0, 0, 520, 1000}).ok() && message_text_coordinator_converge(request).ok();
    const auto& p = f.presentation;
    const MessageTextCommand* body[4]{};
    u32 count = 0;
    for (u32 i = 0; ok && i < p.command_count; ++i)
        if (p.commands[i].measurement_kind == MessageTextMeasurementKind::content && count < 4)
            body[count++] = &p.commands[i];
    check(ok && count == 3 && same(body[0]->text, utf8_literal("Title\nintro")) &&
              starts_with(body[1]->text, utf8_literal("quoted bold text")) &&
              ends_with(body[1]->text, utf8_literal("inside\nsecond")) && same(body[2]->text, utf8_literal("after")),
          "quote_splits_the_body_into_blocks");
    if (count != 3) {
        message_scene_release(&f.scene);
        return;
    }
    const float width = ui_rect_width(body[0]->rectangle);
    const float quote_width = (width - 4 < width * .9f ? width - 4 : width * .9f) - 20;
    check(body[1]->rectangle.left == body[0]->rectangle.left + 16 && body[2]->rectangle.left == body[0]->rectangle.left &&
              ui_rect_width(body[1]->rectangle) > quote_width - .01f &&
              ui_rect_width(body[1]->rectangle) < quote_width + .01f &&
              close_to(body[1]->rectangle.top, body[0]->rectangle.top + body[0]->measured_height + 4) &&
              close_to(body[2]->rectangle.top, body[1]->rectangle.top + body[1]->measured_height + 4) &&
              close_to(p.measurements[0].content_height,
                   body[0]->measured_height + body[1]->measured_height + body[2]->measured_height + 8),
          "quote_block_is_indented_narrowed_and_stacked_with_margins");
    check(has_style(body[1], utf8_literal("bold"), text_style_bold | text_style_quote) &&
              has_style(body[1], utf8_literal("second"), text_style_quote) &&
              !has_style(body[0], utf8_literal("intro"), text_style_quote) &&
              same(link_target(f, body[1]), utf8_literal("https://q.test/x")) &&
              size_at(body[0], utf8_literal("Title")) == text_size_xl &&
              size_at(body[2], utf8_literal("after")) == text_size_sm &&
              has_style(body[2], utf8_literal("after"), text_style_subtext),
          "quote_keeps_nested_formatting_links_and_heading_sizes");
    // Wrapped quote lines all start at the indented origin; one 4-DIP spine
    // spans every line, 16 DIP left of the text.
    ShapedText out{};
    bool shaped = shaped_text_initialize(&out, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) &&
                  shaped_text_bind_decorations(&out, {decorations, 64}) &&
                  text_shape_utf8_styled(&text_system, body[1]->text, body[1]->styles, ui_rect_width(body[1]->rectangle),
                                         4096, {utf16, 1024}, &out)
                      .ok();
    u32 spines = 0;
    TextDecoration spine{};
    for (u32 i = 0; shaped && i < out.decoration_count; ++i)
        if (out.decorations[i].kind == TextDecorationKind::quote_bar) {
            spine = out.decorations[i];
            ++spines;
        }
    float left_most = 1e9f;
    u32 line_starts = 0;
    float previous_y = -1;
    for (u32 i = 0; shaped && i < out.run_count; ++i) {
        if (out.runs[i].baseline_y != previous_y) {
            left_most = out.runs[i].baseline_x < left_most ? out.runs[i].baseline_x : left_most;
            previous_y = out.runs[i].baseline_y;
            ++line_starts;
        }
    }
    check(shaped && out.line_count >= 3 && line_starts >= 3 && left_most >= 0 && spines == 1 && spine.left == -16 &&
              spine.right == -12 && spine.top == 0 && spine.bottom > out.height - .5f &&
              spine.bottom < out.height + .5f,
          "quote_wrapped_lines_keep_the_indent_under_one_spine");
    shaped_text_reset(&out);
    message_scene_release(&f.scene);
}
} // namespace

// The client shapes with the native engine alone (main.cpp). A text item it
// rejected used to fail the frame's whole content text: an fxtwitter embed
// author name with a Bamum Supplement scalar (U+169E7) or a description with
// U+2063 blanked the message list behind the shell fallback, and the full
// rebuild re-ran every 1.6 s, locking the UI up. These run on that engine.

u64 ticks() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
}
double elapsed_ms(u64 start) noexcept {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(ticks() - start) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

struct Shaped {
    bool ok;
    u32 notdef;
    float width;
    float height;
};
Shaped shape_styled(Utf8View text, ConstSlice<TextStyleSpan> styles, float width = 1000) noexcept {
    ShapedText output{};
    Shaped result{};
    if (!shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) ||
        !shaped_text_bind_decorations(&output, {decorations, 64}))
        return result;
    result.ok = text_shape_utf8_styled(&text_system, text, styles, width, 4096, {utf16, 1024}, &output).ok();
    for (u32 i = 0; i < output.glyph_count; ++i)
        result.notdef += output.glyphs[i].index == 0 ? 1u : 0u;
    result.width = output.width;
    result.height = output.height;
    shaped_text_reset(&output);
    return result;
}

// Every scalar shapes and measures, plain and styled, in a line of text.
void test_native_text_never_fails(Check check) noexcept {
    bool shaped = true, measured = true;
    for (u32 cp = 1; cp < 0xE01F0; cp += cp < 0x80 ? 1u : 3u) {
        if (cp >= 0xD800 && cp < 0xF900)
            cp = 0xF900; // surrogates and the private-use area
        if (cp >= 0x20000 && cp < 0x2FFF0)
            cp = 0x2FFF0;
        if (cp >= 0x30000 && cp < 0xE0000)
            cp = 0xE0000;
        char text[16];
        u32 n = 0;
        text[n++] = 'a';
        text[n++] = 'b';
        text[n++] = ' ';
        n += encode(cp, text + n);
        text[n++] = ' ';
        text[n++] = 'c';
        const TextStyleSpan bold{0, n, text_style_bold, text_size_sm};
        shaped = shaped && shape_styled({text, n}, {}).ok;
        measured = measured && text_measure_utf8_styled(&text_system, {text, n}, {&bold, 1}, 1000, 1000,
                                                        {utf16, 1024}).ok();
    }
    check(shaped, "native_every_scalar_shapes_without_failing_the_item");
    check(measured, "native_every_scalar_measures_without_failing_the_item");

    // Names shaped like the ones that failed, other display-name decorations, and the
    // fxtwitter statistics line; measurement and drawing agree.
    const Utf8View names[]{
        utf8_literal("rowan \xF0\x96\xA7\xA7 (@rowan_example)"),
        utf8_literal("\xE1\xA1\xA3\xF0\x90\xAD\xA9 \xEA\x92\xB0\xE1\x90\xA2. .\xE1\x90\xA2\xEA\x92\xB1 \xE2\x8B\x86\xCB\x9A\xE2\x9C\xBF (@sample)"),
        utf8_literal("\xEA\x95\xA4 \xE2\xB5\xA3 \xF0\x90\x99\x9A \xEA\xA6\xBF \xE1\xA5\xAB\xE1\xAD\xA1 \xF0\x9D\x93\xAA\xF0\x9D\x93\xB7 (@x)"),
        utf8_literal("\xF0\x9F\x92\xAC 412   \xF0\x9F\x94\x81 377   \xE2\x9D\xA4\xEF\xB8\x8F 21.4K   \xF0\x9F\x91\x81\xEF\xB8\x8F 1.08M"),
        utf8_literal("ctl\x1B[0m bell\x07 \xC2\x90 soft\xC2\xADhyphen \xE2\x80\x8Frtl\xE2\x80\x8E \xEF\xBB\xBF \xF0\x9D\x85\xB3"),
    };
    bool agree = true;
    for (const Utf8View name : names) {
        const TextStyleSpan span{0, name.bytes, text_style_bold, text_size_sm};
        const auto m = text_measure_utf8_styled(&text_system, name, {&span, 1}, 400, 4096, {utf16, 1024});
        const Shaped s = shape_styled(name, {&span, 1}, 400);
        agree = agree && m.ok() && s.ok && close_to(m.width, s.width) && m.width > 20;
    }
    check(agree, "native_decorated_names_measure_and_draw_alike");

    // A scalar no native item can express draws the missing glyph, not a
    // blank item; the placeholders are unassigned in the Unicode tables.
    const float ab = shape_styled(utf8_literal("ab"), {}).width;
    const Shaped javanese = shape_styled(utf8_literal("a\xEA\xA6\x84" "b"), {});
    check(javanese.ok && javanese.notdef == 1 && javanese.width > ab, "native_unsupported_scalar_draws_one_missing_glyph");
    // Simple scripts draw real glyphs from a covering system face (else one
    // missing glyph each), alone and between Latin letters, measured alike.
    bool simple = true;
    for (u32 i = 0; i < sizeof(kSimpleScripts) / sizeof(kSimpleScripts[0]); ++i) {
        char text[16];
        u32 n = 0;
        text[n++] = 'a';
        n += encode(kSimpleScripts[i], text + n);
        n += encode(kSimpleScripts[i], text + n);
        text[n++] = 'b';
        const Shaped s = shape_styled({text, n}, {});
        const auto m = text_measure_utf8_styled(&text_system, {text, n}, {}, 1000, 1000, {utf16, 1024});
        simple = simple && s.ok && m.ok() && s.notdef == (simple_script_covered[i] ? 0u : 2u) && s.width > ab &&
                 close_to(m.width, s.width);
    }
    check(simple, "native_simple_scripts_draw_covering_system_glyphs");
    // Right-to-left text with an unmatched bracket keeps its letters; only the
    // bracket becomes a mirrored lookalike.
    const Shaped hebrew = shape_styled(utf8_literal("abc \xD7\x90\xD7\x91 ("), {});
    check(hebrew.ok && hebrew.notdef == 0 && hebrew.width > shape_styled(utf8_literal("abc"), {}).width,
          "native_unmatched_bracket_in_rtl_text_keeps_its_letters");
    check(unicode_properties(0x378)->general_category() == UnicodeGeneralCategory::cn &&
              unicode_properties(0x2B74)->general_category() == UnicodeGeneralCategory::cn &&
              unicode_properties(0x2FFFD)->general_category() == UnicodeGeneralCategory::cn,
          "native_missing_glyph_placeholders_are_unassigned");

    // Invisible format controls take no width natively, mid-line and at a
    // line start (fxtwitter prefixes lines with U+2063).
    const u32 hidden[]{0x0090, 0x00AD, 0x061C, 0x200E, 0x200F, 0x2060, 0x2063, 0x206A, 0xFEFF,
                       0xFFF9, 0x1BCA0, 0x1D173, 0xE0002, 0xE0100};
    const float plain = shape_styled(utf8_literal("abcd"), {}).width;
    const float line = shape_styled(utf8_literal("Hello"), {}).width;
    bool invisible = plain > 0 && line > 0;
    for (u32 cp : hidden) {
        for (u32 form = 0; form < 2; ++form) {
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
            const Shaped s = shape_styled({text, n}, {});
            const float expected = form ? line : plain;
            invisible = invisible && s.ok && s.notdef == 0 && close_to(s.width, expected);
        }
    }
    check(invisible, "native_invisible_format_controls_take_no_width");

    // A failing cluster is narrowed to itself, whatever the item's length: a
    // Cyrillic fxtwitter quote (lines led by U+FE00 U+FE00, emoji, CJK, escaped
    // markdown) stays real glyphs, plain and with an unshapable scalar
    // (Javanese) appended; only that cluster becomes the missing glyph.
    static const char tweet[] =
        "\xD0\x91\xD0\xB8\xD0\xB1\xD0\xBB\xD0\xB8\xD0\xBE\xD1\x82\xD0\xB5\xD0\xBA\xD0\xB0 \xD0\xA1\xD0\xB5\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBD\xD0\xB0\xD1\x8F \xD0\x97\xD0\xB2\xD0\xB5\xD0\xB7\xD0\xB4\xD0\xB0 \xD0\xB4\xD0\xBB\xD1\x8F GitHub\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD1\x88\xD0\xBD\xD0\xBE \xD1\x81\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xBB\xD0\xB0 \xD1\x82\xD1\x8B\xD1\x81\xD1\x8F\xD1\x87\xD0\xB8\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD0\xBA\xD0\xBD\xD0\xB8\xD0\xB3 \xD1\x81\xD1\x82\xD0\xB0\xD1\x80\xD0\xB8\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB9 \xD0\xBF\xD0\xBE\xD1\x8D\xD0\xB7\xD0\xB8\xD0\xB8 \\[\xD0\x90\xD0\x90\xD0\x90\\] \n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD1\x80\xD1\x83\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB5\xD0\xB9 \xD0\xB8 \xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xBB\xD0\xB0 \xD1\x82\xD1\x8B\xD1\x81\xD1\x8F\xD1\x87\xD0\xB8 \n"
        "\xEF\xB8\x80\xEF\xB8\x80TLS cipher suites\n\xEF\xB8\x80\xEF\xB8\x80\xD0\xB2\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xB5 \xD1\x81 make \xD0\xB8 cmake\n"
        "\xEF\xB8\x80\xEF\xB8\x80Quiet, quiet, \\[GOOD NEWS!\\]\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD1\x8C \xD0\xB2\xD1\x81\xD1\x91 \xD0\xB8\xD0\xB4\xD1\x91\xD1\x82 \xD1\x81\xD0\xB2\xD0\xBE\xD0\xB8\xD0\xBC \xD0\xBF\xD1\x83\xD1\x82\xD1\x91\xD0\xBC\n"
        "\xEF\xB8\x80\xEF\xB8\x80\\-3^41 sample inputs \xF0\x9F\x9B\x8E\xEF\xB8\x8F\xF0\x9F\x94\xA8\xE2\x98\xA0\xEF\xB8\x8F\xE2\x98\xA3\xEF\xB8\x8F\n"
        "\xEF\xB8\x80\xEF\xB8\x80\xD0\xA2\xD1\x8B Vega, \xD0\xAF Comet...\n\n**[\xD0\xA6\xD0\xB8\xD1\x82\xD0\xB8\xD1\x80\xD1\x83\xD1\x8F](https://x.com/a/status/1)** "
        "\xE2\x81\xA3\xE2\x81\xA3\n> \\-\\-\\-\n> 2030: \xE6\x98\xA5\xE7\xA7\x8B \xD0\x9F\xD0\xA0\xD0\x90\xD0\x97\xD0\x94\xD0\x9D\xD0\x98\xD0\x9A TRULY BEGAN..\n"
        "**\xE2\x9D\xA4\xEF\xB8\x8F 27\xE2\x80\x82\xF0\x9F\x91\x81\xEF\xB8\x8F 358\xE2\x80\x82**";
    char forced[sizeof(tweet) + 8];
    copy_bytes(forced, tweet, sizeof(tweet) - 1);
    copy_bytes(forced + sizeof(tweet) - 1, " \xEA\xA6\x84", 4);
    // The quote's opening lines alone (no emoji or CJK): each line break is
    // followed by U+FE00 U+FE00, hidden as zero-width non-joiners that open the
    // Cyrillic run; that cut used to fail the item and box every letter.
    u32 lines = 0, opening = 0;
    while (opening < sizeof(tweet) - 1 && lines < 6)
        lines += tweet[opening++] == '\n' ? 1u : 0u;
    const Utf8View forms[]{{tweet, static_cast<u32>(sizeof(tweet) - 1)},
                           {tweet, opening},
                           {forced, static_cast<u32>(sizeof(tweet) + 3)}};
    bool cyrillic = true;
    for (u32 form = 0; form < 3; ++form) {
        const Utf8View text = forms[form];
        const TextStyleSpan span{0, text.bytes, 0, text_size_sm};
        const auto m = text_measure_utf8_styled(&text_system, text, {&span, 1}, 520, 1'000'000, {utf16, 1024});
        const Shaped s = shape_styled(text, {&span, 1}, 520);
        cyrillic = cyrillic && m.ok() && s.ok && s.notdef <= (form == 2 ? 1u : 0u) && m.width > 100 &&
                   close_to(m.height, s.height);
    }
    check(cyrillic, "native_failing_cluster_keeps_cyrillic_glyphs_in_a_long_item");
    // Pathological text stays bounded: every scalar distinct
    // (Yi syllables) with invisible separators, as one 3 KB embed field.
    static char yi[2800];
    u32 bytes = 0;
    for (u32 cp = 0xA000; bytes + 6 < sizeof(yi) && cp < 0xA48D; ++cp) {
        bytes += encode(cp, yi + bytes);
        if (!(cp & 7))
            bytes += encode(0x2063, yi + bytes);
    }
    const TextStyleSpan span{0, bytes, 0, text_size_sm};
    const u64 start = ticks();
    const auto m = text_measure_utf8_styled(&text_system, {yi, bytes}, {&span, 1}, 400, 1'000'000, {utf16, 1024});
    check(m.ok() && m.line_count > 1 && elapsed_ms(start) < 250.0, "native_pathological_field_measures_in_bounded_time");
}

// Pathological markdown (unclosed brackets, emphasis runs, escapes) parses in
// linear time: an embed description is up to 4096 bytes.
void test_markdown_bounded(Check check) noexcept {
    static char source[4096];
    static char output[8192];
    static u32 styles[8192];
    static TextStyleSpan spans[512];
    static MessageMarkdownSemantic semantics[64];
    const char* units[]{"[", "*", "_", "\\", "~", "|", "`", "[a](", "**a", "> ", "<", "https://", "\xE2\x81\xA3"};
    bool bounded = true;
    for (const char* unit : units) {
        u32 length = 0;
        while (unit[length])
            ++length;
        u32 bytes = 0;
        while (bytes + length <= sizeof(source)) {
            for (u32 i = 0; i < length; ++i)
                source[bytes + i] = unit[i];
            bytes += length;
        }
        const u64 start = ticks();
        (void)message_markdown_parse({source, bytes}, Slice<char>{output, sizeof(output)},
                                     Slice<u32>{styles, 8192}, Slice<TextStyleSpan>{spans, 512}, {},
                                     Slice<MessageMarkdownSemantic>{semantics, 64});
        bounded = bounded && elapsed_ms(start) < 50.0;
    }
    check(bounded, "markdown_pathological_descriptions_parse_in_bounded_time");
}

// Converges one MESSAGE_CREATE through the text pass with the native engine
// and paints it; returns the row or null.
const MessageSceneRowWidget* converge_native(Utf8View payload, GatewayMessageEvent* message,
                                            MessagePresentationResult* painted,
                                            MessageMediaCommandStream* media) noexcept {
    auto& f = fixture;
    static u32 structurals[4096];
    static char packed_text[32768];
    static u64 ids[256];
    static GatewayMessageComponent components[8];
    static GatewayMessageEmbed embeds[10];
    static GatewayMessageEmbedField fields[8];
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {components, 8};
    scratch.embeds = {embeds, 10};
    scratch.embed_fields = {fields, 8};
    bool ok = gateway_decode_envelope(payload, {structurals, 4096}, 32, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {structurals, 4096}, 32, {packed_text, sizeof(packed_text)},
                                            {ids, 256}, &routed, &packed)
                  .ok() &&
              gateway_client_event_as_message(routed, packed, scratch, message);
    ok = ok && message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
         message_reconcile_create(&f.store, *message).ok() &&
         message_rich_store_initialize(&f.rich, {f.rich_record, 2}, {f.rich_order, 2},
                                       {f.rich_text, sizeof(f.rich_text)}) &&
         message_rich_store_apply(&f.rich, 1, f.record[0].revision, *message).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, 2}, {f.fenwick, 2}, 1, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(4000),
                                     false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, 2, 2, {f.widget, kWidgets}, {f.node, kWidgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 1024}, {f.semantics, 256}, {f.measurements, 4},
                                              {f.spoilers, 4}, {f.byte_styles, 32768}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 512}, measure, nullptr);
    ChatStateStore state{};
    NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, 1'791'600'000, 1};
    ok = ok && message_scene_update(&f.scene, {0, 0, 900, 4000}).ok() &&
         message_text_coordinator_converge(request).ok();
    const MessageSceneRowWidget* row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    paint_stream_reset(&f.paint, 900, 4000);
    if (!row || !message_media_command_stream_initialize(media, {f.media, 32}) ||
        !message_presentation_build(f.scene, kDefaultTheme, &f.paint, media, painted))
        row = nullptr;
    paint_stream_release(&f.paint);
    return row;
}

// fxtwitter/vxtwitter embeds as the server delivers them (synthetic copies of
// observed shapes): the decorated author name that blanked the list, a
// description with U+2063 line prefixes, U+200B/U+3164/U+2800/U+FE0F, escaped
// markdown, a quote and a linked statistics line, a footer icon and timestamp,
// and a video; then a four-photo post as four embeds sharing one URL.
void test_vxtwitter_embeds(Check check) noexcept {
    auto& f = fixture;
    const auto video = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://embed.example.com/i/status/1234567890123456789","timestamp":"2026-10-09T17:00:00Z","embeds":[{"type":"rich","url":"https://x.com/rowan_example/status/1234567890123456789","color":7193840,"description":"⁣Gardener tours the new seed library 🥺💔\n\n> \"Why does it smell like rain\\? It’s every spring\\.\\.\"\n\n⁣​ㅤ⠀ *not* a \\*joke\\* ❤️\n\n**[💬](https://x.com/intent/tweet?in_reply_to=1234567890123456789) 412 [🔁](https://x.com/intent/retweet?tweet_id=1234567890123456789) 377 [❤️](https://x.com/intent/like?tweet_id=1234567890123456789) 21.4K 👁️ 1.08M**","author":{"name":"rowan 𖧧 (@rowan_example)","url":"https://x.com/rowan_example","icon_url":"https://pbs.twimg.com/profile_images/1000000000000000001/avatar_200x200.jpg","proxy_icon_url":"https://images-ext-1.discordapp.net/external/a/https/pbs.twimg.com/profile_images/1000000000000000001/avatar_200x200.jpg"},"footer":{"text":"FxTwitter","icon_url":"https://assets.fxembed.com/logos/fxtwitter64.png","proxy_icon_url":"https://images-ext-1.discordapp.net/external/b/https/assets.fxembed.com/logos/fxtwitter64.png"},"timestamp":"2026-10-09T16:23:14+00:00","thumbnail":{"url":"https://pbs.twimg.com/amplify_video_thumb/1/img/a.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/c/https/pbs.twimg.com/amplify_video_thumb/1/img/a.jpg","width":1920,"height":1080,"flags":0},"video":{"url":"https://video.twimg.com/amplify_video/1/vid/avc1/1920x1080/a.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/d/https/video.twimg.com/amplify_video/1/vid/avc1/1920x1080/a.mp4","width":1920,"height":1080,"flags":0}}]}})json");
    GatewayMessageEvent message{};
    MessagePresentationResult painted{};
    MessageMediaCommandStream media{};
    const MessageSceneRowWidget* row = converge_native(video, &message, &painted, &media);
    check(row != nullptr, "vxtwitter_video_embed_converges_and_paints_natively");
    if (row) {
        const auto& p = f.presentation;
        const auto* author = command(p, 0x8002);
        const auto* description = command(p, 0x8004);
        const auto* footer = command(p, 0x8005);
        check(author && same(author->text, utf8_literal("rowan \xF0\x96\xA7\xA7 (@rowan_example)")) &&
                  has_style(author, utf8_literal("rowan"), text_style_bold) && author->measured_height > 0 &&
                  same(link_target(f, author), utf8_literal("https://x.com/rowan_example")),
              "vxtwitter_decorated_author_is_measured_and_linked");
        check(description && contains(description->text, utf8_literal("Gardener tours")) &&
                  description->measured_height > 0 && footer &&
                  starts_with(footer->text, utf8_literal("FxTwitter \xE2\x80\xA2 ")),
              "vxtwitter_description_and_footer_text_are_shown");
        bool drawn = true;
        for (u32 i = 0; i < p.command_count; ++i) {
            const auto& c = p.commands[i];
            if (c.source_id < 0x8000 || !c.text.bytes)
                continue;
            const Shaped s = shape_styled(c.text, c.styles, ui_rect_width(c.rectangle));
            // The Bamum Supplement scalar draws one missing glyph unless a system face covers it.
            const bool decorated = &c == author && !simple_script_covered[1];
            drawn = drawn && s.ok && s.notdef == (decorated ? 1u : 0u);
        }
        check(drawn, "vxtwitter_embed_text_draws_without_tofu_for_invisibles");
        u32 videos = 0;
        for (u32 i = 0; i < media.count; ++i)
            videos += media.storage[i].kind == MessageMediaCommandKind::embed &&
                              media.storage[i].embed_resource == static_cast<u8>(MediaEmbedResource::video)
                          ? 1u
                          : 0u;
        check(videos == 1, "vxtwitter_video_embed_paints_its_video");
    }
    message_scene_release(&f.scene);

    const auto photos = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"https://fixvx.com/a/status/2","timestamp":"2026-10-09T17:00:00Z","embeds":[{"type":"rich","url":"https://x.com/a/status/2","description":"four ⁣photos⁣","author":{"name":"ꕤ a ⵣ (@a)","url":"https://x.com/a"},"image":{"url":"https://pbs.twimg.com/media/1.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/1/https/pbs.twimg.com/media/1.jpg","width":1200,"height":900,"flags":0}},{"type":"rich","url":"https://x.com/a/status/2","image":{"url":"https://pbs.twimg.com/media/2.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/2/https/pbs.twimg.com/media/2.jpg","width":1200,"height":900,"flags":0}},{"type":"rich","url":"https://x.com/a/status/2","image":{"url":"https://pbs.twimg.com/media/3.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/3/https/pbs.twimg.com/media/3.jpg","width":900,"height":1200,"flags":0}},{"type":"rich","url":"https://x.com/a/status/2","image":{"url":"https://pbs.twimg.com/media/4.jpg","proxy_url":"https://images-ext-1.discordapp.net/external/4/https/pbs.twimg.com/media/4.jpg","width":1200,"height":1200,"flags":0}}]}})json");
    row = converge_native(photos, &message, &painted, &media);
    // One card: the merged embeds take no height and their images paint as
    // the four covering tiles of the card's 400x300 gallery.
    u32 images = 0;
    UiRect tiles[4]{};
    bool covered = true;
    for (u32 i = 0; row && i < media.count; ++i) {
        const auto& c = media.storage[i];
        if (c.kind != MessageMediaCommandKind::embed)
            continue;
        covered = covered && c.cover && c.embed_index < 4 &&
                  c.embed_resource == static_cast<u8>(MediaEmbedResource::image);
        if (c.embed_index < 4)
            tiles[c.embed_index] = c.rectangle;
        ++images;
    }
    const auto& items = row ? row->rich_item_layout : MessageRichItemLayout{};
    const float column = (400.0f - kMessageEmbedGalleryGap) * 0.5f;
    check(row && message.embeds.count == 4 && images == 4 && covered &&
              items.embed_media_widths[0] == 400 && items.embed_media_heights[0] == 300 &&
              items.heights[kMessageRichEmbedItemOffset + 1] == 0 &&
              items.heights[kMessageRichEmbedItemOffset + 3] == 0 &&
              close_to(ui_rect_width(tiles[0]), column) && close_to(ui_rect_height(tiles[0]), 148) &&
              close_to(tiles[1].left, tiles[0].right + 4) && close_to(tiles[2].top, tiles[0].bottom + 4) &&
              close_to(tiles[3].left, tiles[1].left) && close_to(tiles[3].top, tiles[2].top),
          "vxtwitter_four_photo_post_paints_one_gallery_card");
    if (row) {
        GatewayMessageEmbed members[4]{};
        for (u32 member = 0; member < 4; ++member) {
            members[member] = message.embeds[member];
            members[member].flags = member == 1 || member == 2 ? kGatewayMediaExplicitFlag : 0;
        }
        message.embeds = {members, 4};
        const auto publish_members = [&]() noexcept {
            const auto applied = message_rich_store_apply(&f.rich, 1, f.record[0].revision, message);
            return applied.ok() && !applied.storage_truncated;
        };
        ChatStateStore state{};
        NavigationView navigation{};
        const MessageTextCoordinatorRequest request{
            &f.scene,       &f.presentation, &f.cache, &state,        nullptr, &navigation,
            &kDefaultTheme, finish,          nullptr,  1'791'600'000, 1};
        const auto repaint_gallery = [&]() noexcept {
            message_scene_invalidate_geometry(&f.scene);
            return message_scene_update(&f.scene, {0, 0, 900, 4000}).ok() &&
                   message_text_coordinator_converge(request).ok() &&
                   paint_stream_reset(&f.paint, 900, 4000) &&
                   message_media_command_stream_initialize(&media, {f.media, 32}) &&
                   message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &painted);
        };
        const auto media_members = [&](u32 clear, u32 blurred) noexcept {
            u32 seen = 0;
            for (u32 index = 0; index < media.count; ++index) {
                const auto& image = media.storage[index];
                if (image.kind != MessageMediaCommandKind::embed)
                    continue;
                if (image.embed_index >= 4)
                    return false;
                const u32 bit = 1u << image.embed_index;
                const auto expected =
                    blurred & bit ? MessageMediaObscure::explicit_content : MessageMediaObscure::none;
                if ((seen & bit) || !((clear | blurred) & bit) ||
                    image.obscure != static_cast<u8>(expected) || !image.cover ||
                    !ui_rect_equal(image.rectangle, tiles[image.embed_index]))
                    return false;
                seen |= bit;
            }
            return seen == (clear | blurred);
        };
        const auto labels = [&](MessageMediaObscure obscure, u32 expected) noexcept {
            u32 seen = 0, count = 0, wanted = 0;
            for (u32 member = 0; member < 4; ++member)
                wanted += (expected >> member) & 1u;
            for (u32 index = 0; index < f.presentation.command_count; ++index) {
                const auto& label = f.presentation.commands[index];
                if (!same(label.text, message_media_obscure_label(obscure)))
                    continue;
                if (message_media_obscure_notice(obscure)
                        ? label.styles.count != 0
                        : label.styles.count != 1 || label.styles[0].flags != text_style_bold)
                    return false;
                ++count;
                for (u32 member = 0; member < 4; ++member) {
                    const auto cell = tiles[member];
                    const auto box = label.rectangle;
                    const bool fits = message_media_obscure_notice(obscure)
                                          ? box.left >= cell.left && box.right <= cell.right &&
                                                close_to(box.top, cell.top) &&
                                                close_to(box.bottom, cell.bottom) &&
                                                close_to(box.left + box.right, cell.left + cell.right)
                                          : ui_rect_equal(box, message_media_obscure_pill(cell, obscure));
                    if (fits)
                        seen |= 1u << member;
                }
            }
            return count == wanted && seen == expected;
        };
        const auto notices = [&]() noexcept {
            u32 seen = 0;
            for (u32 index = 0; index < f.paint.count; ++index) {
                const auto& notice = f.paint.quads[index];
                if (notice.radius != kDefaultTheme.scale.radius_sm || !notice.border_width)
                    continue;
                for (u32 member = 0; member < 4; ++member)
                    if (ui_rect_equal({notice.left, notice.top, notice.right, notice.bottom},
                                      tiles[member]))
                        seen |= 1u << member;
            }
            return seen;
        };
        message_scene_set_explicit_filter(&f.scene, MessageExplicitFilter::blur);
        check(publish_members() && repaint_gallery() && media_members(0b1001, 0b0110) &&
                  labels(MessageMediaObscure::explicit_content, 0b0110),
              "gallery_filters_blur_only_flagged_members_and_place_each_label_in_its_cell");
        check(message_scene_reveal_media(&f.scene,
                                         message_media_reveal_key(1, MessageMediaSource::embed, 1)) &&
                  repaint_gallery() && media_members(0b1011, 0b0100) &&
                  labels(MessageMediaObscure::explicit_content, 0b0100),
              "gallery_reveal_key_opens_one_member_and_keeps_flagged_sibling_obscured");
        message_scene_set_explicit_filter(&f.scene, MessageExplicitFilter::block);
        const bool block_ready = repaint_gallery();
        check(block_ready, "gallery_block_converges_and_paints");
        if (block_ready) {
            check(media_members(0b1001, 0), "gallery_block_omits_only_flagged_media");
            check(notices() == 0b0110, "gallery_block_paints_notice_in_each_flagged_cell");
            check(labels(MessageMediaObscure::blocked, 0b0110),
                  "gallery_block_places_label_in_each_flagged_cell");
        }
        members[0].flags = kGatewayMediaExplicitFlag;
        members[1].flags = members[2].flags = 0;
        const bool host_ready = publish_members() && repaint_gallery();
        check(host_ready, "gallery_flagged_host_converges_and_paints");
        if (host_ready) {
            check(media_members(0b1110, 0), "gallery_flagged_host_does_not_block_clear_members");
            check(notices() == 0b0001, "gallery_flagged_host_paints_notice_only_in_host_cell");
            check(labels(MessageMediaObscure::blocked, 0b0001),
                  "gallery_flagged_host_places_label_only_in_host_cell");
        }
        paint_stream_release(&f.paint);
    }
    message_scene_release(&f.scene);
}

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
    static u32 structurals[4096];
    static char packed_text[32768];
    static u64 ids[128];
    static GatewayMessageComponent components[8];
    static GatewayMessageEmbed embeds[4];
    static GatewayMessageEmbedField fields[4];
    static GatewayMessageAttachment attachments[4];
    static GatewayMessageSticker stickers[3];
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {components, 8};
    scratch.embeds = {embeds, 4};
    scratch.embed_fields = {fields, 4};
    scratch.attachments = {attachments, 4};
    scratch.stickers = {stickers, 3};
    GatewayMessageEvent message{};
    bool ok = gateway_decode_envelope(payload, {structurals, 4096}, 32, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {structurals, 4096}, 32, {packed_text, sizeof(packed_text)},
                                            {ids, 128}, &routed, &packed)
                  .ok() &&
              gateway_client_event_as_message(routed, packed, scratch, &message);
    if (decoded)
        *decoded = message;
    ok = ok && message_store_initialize(&f.store, {f.record, 2}, {f.order, 2}, {f.text, sizeof(f.text)}) &&
         message_reconcile_create(&f.store, message).ok() &&
         message_rich_store_initialize(&f.rich, {f.rich_record, 2}, {f.rich_order, 2},
                                       {f.rich_text, sizeof(f.rich_text)}) &&
         message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, 2}, {f.fenwick, 2}, 1, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000),
                                     false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, 2, 2, {f.widget, kWidgets}, {f.node, kWidgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 1024}, {f.semantics, 256}, {f.measurements, 4},
                                              {f.spoilers, 4}, {f.byte_styles, 32768}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 512}, measure, nullptr);
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
        bool ok = line && same(line->text, c_text(c.expected));
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
    check(line && same(line->text, utf8_literal("Original message was deleted")) && line->styles.count == 1 &&
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
    bool clean = line && starts_with(line->text, utf8_literal("Alice WWWW")) && !contains(line->text, utf8_literal("`")) &&
                 !contains(line->text, utf8_literal("\x1b")) && !contains(line->text, utf8_literal("[38;"));
    check(clean, "reply preview: a long truecolor ```ansi message shows its text without escapes or fences");
    if (row)
        message_scene_release(&f.scene);
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
    if (!command || !shaped_text_initialize(&output, {glyphs, 1024}, {runs, 256}, {clusters, 1024}) ||
        !shaped_text_bind_decorations(&output, {decorations, 64}))
        return result;
    const ConstSlice<TextStyleSpan> styles = command->styles;
    const Utf8View text = command->text;
    if (!text_shape_utf8_styled(&text_system, text, styles, maximum(1.0f, ui_rect_width(command->rectangle)), 4000,
                                {utf16, 1024}, &output)
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
        if (contains(p.commands[i].text, part))
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
        if (contains(c.text, utf8_literal("Example ANSI Colored Text Preview Tool")))
            title = &c;
        if (contains(c.text, utf8_literal("Small tool to make")))
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
              underline && block && !contains(code->text, utf8_literal("\x1b")) &&
              !contains(code->text, utf8_literal("```")) && height > 52.0f,
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
        if (contains(f.presentation.commands[i].text, utf8_literal("Card text")))
            body = &f.presentation.commands[i];
    check(card && same(text, utf8_literal("**Card** text\n\nsecond")) && card->thumbnail.url.bytes &&
              body && has_style(body, utf8_literal("Card"), text_style_bold),
          "component embed: sections, separators and thumbnail accessories map to the card's text and thumbnail");
    if (row)
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
    if (!text_system_initialize(&text_system, L"Segoe UI", 16, L"en-US").ok()) {
        check(false, "embed_parity_text_system");
        return;
    }
    // The client's policy (main.cpp); the bare shaping engine keeps DirectWrite parity.
    const bool previous = text_browser_invisibles();
    text_set_browser_invisibles(true);
    test_default_ignorables(check);
    test_markdown(check);
    test_link_hits(check);
    test_calendar(check);
    test_thumbnail_rules(check);
    test_embed_gallery(check);
    test_fxtwitter_embed(check);
    test_text_sizes(check);
    test_quote_blocks(check);
    test_markdown_bounded(check);
    probe_system_coverage();
    test_native_text_never_fails(check);
    test_vxtwitter_embeds(check);
    test_reply_previews(check);
    test_component_embed(check);
    test_author_header_isolation(check);
    text_set_browser_invisibles(previous);
    text_system_release(&text_system);
}
} // namespace maicord
