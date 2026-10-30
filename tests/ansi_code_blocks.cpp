#include "maicord/core/memory.hpp"
#include "maicord/ui/ansi_text.hpp"
#include "maicord/ui/code_highlight.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/text.hpp"


namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

struct Piece {
    u32 begin, end;
    AnsiTextStyle style;
};
struct Scan {
    Utf8View input;
    char text[512];
    u32 bytes;
    Piece pieces[64];
    u32 count;
};

bool collect(void* context, u32 begin, u32 end, AnsiTextStyle style) noexcept {
    auto& scan = *static_cast<Scan*>(context);
    if (scan.count == 64 || end - begin > sizeof(scan.text) - scan.bytes)
        return false;
    scan.pieces[scan.count++] = {scan.bytes, scan.bytes + (end - begin), style};
    copy_bytes(scan.text + scan.bytes, scan.input.data + begin, end - begin);
    scan.bytes += end - begin;
    return true;
}

Scan scan(Utf8View input) noexcept {
    Scan result{};
    result.input = input;
    ansi_text_scan(input, collect, &result);
    return result;
}

bool text_is(const Scan& scan, Utf8View expected) noexcept {
    return scan.bytes == expected.bytes && bytes_equal(scan.text, expected.data, expected.bytes);
}

// The style of visible byte `at`.
AnsiTextStyle style_at(const Scan& scan, u32 at) noexcept {
    for (u32 i = 0; i < scan.count; ++i)
        if (scan.pieces[i].begin <= at && at < scan.pieces[i].end)
            return scan.pieces[i].style;
    return AnsiTextStyle{~0u, 0xFFFF, 0xFFFF};
}

bool styled(const Scan& scan, u32 at, u32 flags, u16 foreground, u16 background) noexcept {
    return style_at(scan, at) == AnsiTextStyle{flags, foreground, background};
}

constexpr u16 xterm(u32 index) noexcept {
    return static_cast<u16>(index + 1u);
}

void test_sgr(Check check) noexcept {
    constexpr u32 bold = text_style_bold, italic = text_style_italic, underline = text_style_underline,
                  strike = text_style_strikethrough;
    auto s = scan(utf8_literal("\x1b[31mred\x1b[0m plain"));
    check(text_is(s, utf8_literal("red plain")) && styled(s, 0, 0, xterm(1), 0) && styled(s, 3, 0, 0, 0) &&
              s.count == 2,
          "ansi: SGR 31 colours until 0 resets, escapes hidden");
    s = scan(utf8_literal("\x1b[1;31;40mX\x1b[mY\x1b[4;9;3mZ"));
    check(styled(s, 0, bold, xterm(1), xterm(0)) && styled(s, 1, 0, 0, 0) &&
              styled(s, 2, underline | strike | italic, 0, 0),
          "ansi: combined codes apply in order; an empty SGR resets");
    s = scan(utf8_literal("\x1b[91;101mA\x1b[97;107mB\x1b[30;47mC\x1b[37;40mD"));
    check(styled(s, 0, 0, xterm(9), xterm(9)) && styled(s, 1, 0, xterm(15), xterm(15)) &&
              styled(s, 2, 0, xterm(0), xterm(7)) && styled(s, 3, 0, xterm(7), xterm(0)),
          "ansi: 30-37/40-47 use palette 0-7, 90-97/100-107 the bright 8-15");
    s = scan(utf8_literal("\x1b[2;7;21;25;38;48;50;54;108;999999999999mX"));
    check(text_is(s, utf8_literal("X")) && styled(s, 0, 0, 0, 0), "ansi: unsupported codes are ignored");
    s = scan(utf8_literal("\x1b[1;3;4;9mA\x1b[22mB"));
    check(styled(s, 0, bold | italic | underline | strike, 0, 0) && styled(s, 1, underline | strike, 0, 0),
          "ansi: 22 clears bold and italic but cannot remove an ancestor's decorations");
    s = scan(utf8_literal("\x1b[4mA\x1b[24mB\x1b[4;31mC\x1b[24mD\x1b[3mE\x1b[23mF"));
    check(styled(s, 0, underline, 0, 0) && styled(s, 1, 0, 0, 0) && styled(s, 2, underline, xterm(1), 0) &&
              styled(s, 3, underline, xterm(1), 0) && styled(s, 4, underline | italic, xterm(1), 0) &&
              styled(s, 5, underline, xterm(1), 0),
          "ansi: 23/24 close italic/underline only when that tag is innermost");
    s = scan(utf8_literal("\x1b[3;3;3mA\x1b[23;23mB\x1b[23mC"));
    check(styled(s, 0, italic, 0, 0) && styled(s, 1, italic, 0, 0) && styled(s, 2, 0, 0, 0),
          "ansi: repeated tags close one at a time");
    s = scan(utf8_literal("\x1b[41mA\x1b[49mB\x1b[31mC\x1b[39mD\x1b[0mE"));
    check(styled(s, 0, 0, 0, xterm(1)) && styled(s, 1, 0, 0, xterm(1)) && styled(s, 2, 0, xterm(1), xterm(1)) &&
              styled(s, 3, 0, 0, xterm(1)) && styled(s, 4, 0, 0, 0),
          "ansi: 49 is transparent over an outer background; 39 restores the default colour");
    s = scan(utf8_literal("a\x1b[8mhidden\nline\x1b[0mb\x1b[5;6;53mc"));
    check(text_is(s, utf8_literal("abc")) && styled(s, 2, 0, 0, 0), "ansi: 8 hides text; blink/overline draw nothing");
    // Lines carry no colour of their own: they follow each run's foreground,
    // so a colour opened inside <u>/<strike> recolours the line under it too.
    s = scan(utf8_literal("\x1b[31;4mA\x1b[32mB\x1b[24mC\x1b[0;4;31mD\x1b[0;34;9mE\x1b[39mF"));
    check(styled(s, 0, underline, xterm(1), 0) && styled(s, 1, underline, xterm(2), 0) &&
              styled(s, 2, underline, xterm(2), 0) && styled(s, 3, underline, xterm(1), 0) &&
              styled(s, 4, strike, xterm(4), 0) && styled(s, 5, strike, 0, 0) && s.count == 6,
          "ansi: underline and line-through persist across colour changes inside them");
    s = scan(utf8_literal("\x1b[31;4m\x1b[32;4mG\x1b[24mH\x1b[24mI"));
    check(styled(s, 0, underline, xterm(2), 0) && styled(s, 1, underline, xterm(2), 0) &&
              styled(s, 2, underline, xterm(2), 0),
          "ansi: 24 closes only an innermost underline");
}

void test_extended_and_malformed(Check check) noexcept {
    auto s = scan(utf8_literal("\x1b[38;5;196mA\x1b[48;5;3mB\x1b[38;5;007mC\x1b[38;5;256mD\x1b[48;5;0255mE"));
    check(text_is(s, utf8_literal("ABCDE")) && styled(s, 0, 0, xterm(196), 0) &&
              styled(s, 1, 0, xterm(196), xterm(3)) && styled(s, 2, 0, xterm(196), xterm(3)) &&
              styled(s, 4, 0, xterm(196), xterm(3)),
          "ansi: xterm 38;5/48;5 take canonical 0-255 only");
    s = scan(utf8_literal("\x1b[38;2;255;0;0mA\x1b[48;2;256;0;0mB\x1b[1;38;2;0;0;255mC"));
    // Inside a longer SGR, 38;2;0;0;255 is just codes: 38 and 2 do nothing, 0 resets.
    check(styled(s, 0, 0, xterm(196), 0) && styled(s, 1, 0, xterm(196), xterm(16)) && styled(s, 2, 0, 0, 0),
          "ansi: truecolor maps to the nearest fixed colour (channels mod 256) only as its own sequence");
    // Colored-text generator output: truecolor runs, reverse
    // video (ignored, as the official client does) and a truecolor background.
    s = scan(utf8_literal("\x1b[38;2;255;0;0mH \x1b[38;2;255;255;0mi \x1b[0m to \x1b[7;31mSample \x1b[0m's "
                          "\x1b[48;2;88;101;242m \x1b[38;2;255;255;255mPalette \x1b[0mText Previewer!"));
    check(text_is(s, utf8_literal("H i  to Sample 's  Palette Text Previewer!")) && styled(s, 0, 0, xterm(196), 0) &&
              styled(s, 2, 0, xterm(226), 0) && styled(s, 5, 0, 0, 0) && styled(s, 8, 0, xterm(1), 0) &&
              styled(s, 15, 0, 0, 0) && styled(s, 18, 0, 0, xterm(63)) && styled(s, 19, 0, xterm(231), xterm(63)) &&
              styled(s, 27, 0, 0, 0),
          "ansi: colored-text generator output renders with nearest-256 truecolor and hidden escapes");
    s = scan(utf8_literal("\x1b[;1mA\x1b[31;;32mB\x1b[1234mC\x1b" "X\x1b[31"));
    check(text_is(s, utf8_literal("mA;32mBCX")) && styled(s, 0, 0, 0, 0) && styled(s, 8, 0, 0, 0),
          "ansi: malformed SGR falls to the catch-all, truncated escapes vanish");
    s = scan(utf8_literal("\x1b[2KA\x1b[KB\x1b[1JC\x1b[12;34fD\x1b[(BE\x1b[1234;1fF"));
    check(text_is(s, utf8_literal("ABCDE4;1fF")), "ansi: erase, cursor and charset sequences are removed");
    s = scan(utf8_literal("a\x08\x08" "b\r\nc\rd"));
    check(text_is(s, utf8_literal("ab\r\nc\rd")), "ansi: backspaces vanish, CR and LF stay text");
    s = scan(utf8_literal("\x1b[31mone\ntwo\x1b[3\n1mX"));
    check(text_is(s, utf8_literal("one\ntwo\n1mX")) && s.count == 2 && styled(s, 4, 0, xterm(1), 0) &&
              styled(s, 8, 0, xterm(1), 0),
          "ansi: styles continue across lines; a sequence split by a newline is not SGR");
    check(scan(utf8_literal("\x1b")).bytes == 0 && scan(utf8_literal("\x1b[")).bytes == 0 &&
              scan(utf8_literal("")).count == 0,
          "ansi: empty and lone-escape inputs produce no text");

    // Long runs of styled units, then an unterminated parameter list.
    static char long_input[3 * 1024];
    u32 bytes = 0;
    const char unit[] = "\x1b[31mx\x1b[1;4;41my\x1b[0m";
    while (bytes + sizeof(unit) - 1 <= 1024) {
        copy_bytes(long_input + bytes, unit, sizeof(unit) - 1);
        bytes += sizeof(unit) - 1;
    }
    long_input[bytes++] = '\x1b';
    long_input[bytes++] = '[';
    while (bytes + 2 <= sizeof(long_input)) {
        long_input[bytes++] = '1';
        long_input[bytes++] = ';';
    }
    struct Counter {
        u32 pieces, visible;
    } counter{};
    const bool scanned = ansi_text_scan(
        {long_input, bytes},
        +[](void* context, u32 begin, u32 end, AnsiTextStyle) noexcept {
            auto& c = *static_cast<Counter*>(context);
            ++c.pieces;
            c.visible += end - begin;
            return true;
        },
        &counter);
    // Two coloured letters per unit, then the unterminated tail: the
    // catch-all eats "ESC[1;1" and the rest is one plain piece.
    const u32 units = 1024u / (sizeof(unit) - 1u);
    const u32 tail = bytes - units * (sizeof(unit) - 1u) - 5u;
    check(scanned && counter.pieces == 2 * units + 1 && counter.visible == 2 * units + tail,
          "ansi: long escape runs and an unterminated parameter tail scan completely");
}

struct Parsed {
    char text[1024];
    u32 styles[1024];
    TextStyleSpan spans[64];
    MessageMarkdownResult result;
};

Parsed& parse(Utf8View input, bool editor = false, bool reply = false) noexcept {
    static Parsed parsed;
    parsed = Parsed{};
    MessageMarkdownOptions options{};
    options.preserve_entities = editor;
    options.inline_reply = reply;
    parsed.result = message_markdown_parse(input, {parsed.text, 1024}, {parsed.styles, 1024}, {parsed.spans, 64},
                                           options, {});
    return parsed;
}

const TextStyleSpan* span_at(const Parsed& parsed, u32 at) noexcept {
    for (u32 i = 0; i < parsed.result.span_count; ++i)
        if (parsed.spans[i].byte_offset <= at && at < parsed.spans[i].byte_offset + parsed.spans[i].byte_count)
            return parsed.spans + i;
    return nullptr;
}

u32 find(const Parsed& parsed, const char* needle) noexcept {
    const u32 bytes = c_string_length(needle);
    for (u32 i = 0; i + bytes <= parsed.result.text.bytes; ++i)
        if (bytes_equal(parsed.text + i, needle, bytes))
            return i;
    return ~0u;
}

bool contains_escape(const Parsed& parsed) noexcept {
    for (u32 i = 0; i < parsed.result.text.bytes; ++i)
        if (parsed.text[i] == '\x1b')
            return true;
    return false;
}

void test_markdown(Check check) noexcept {
    check(code_language_from_tag(utf8_literal("ansi")) == CodeLanguage::ansi &&
              code_language_from_tag(utf8_literal("ANSI")) == CodeLanguage::ansi,
          "ansi: the code block tag is case-insensitive");
    const auto source = utf8_literal("```ansi\n\x1b[1;31mred\x1b[0m \x1b[44mblue\x1b[0m\n```");
    auto& p = parse(source);
    const u32 red = find(p, "red"), blue = find(p, "blue");
    const TextStyleSpan* red_span = red == ~0u ? nullptr : span_at(p, red);
    const TextStyleSpan* blue_span = blue == ~0u ? nullptr : span_at(p, blue);
    const TextStyleSpan* gap = red == ~0u ? nullptr : span_at(p, red + 3);
    check(p.result.ok() && !contains_escape(p) && red_span && blue_span && gap &&
              red_span->byte_offset == red && red_span->byte_count == 3 &&
              (red_span->flags & (text_style_bold | text_style_code_block | text_style_monospace)) ==
                  (text_style_bold | text_style_code_block | text_style_monospace) &&
              ansi_span_foreground(*red_span) == xterm(1) && ansi_span_background(*red_span) == 0 &&
              ansi_span_foreground(*blue_span) == 0 && ansi_span_background(*blue_span) == xterm(4) &&
              blue_span->byte_count == 4 && !(blue_span->flags & text_style_bold) &&
              ansi_span_foreground(*gap) == 0 && ansi_span_background(*gap) == 0 &&
              (gap->flags & text_style_code_block),
          "ansi: a ```ansi block yields styled, coloured spans without escapes");

    auto& spoiler = parse(utf8_literal("x ||```ansi\n\x1b[31mA\x1b[0mB```||"));
    const u32 a = find(spoiler, "AB");
    check(spoiler.result.ok() && a != ~0u && span_at(spoiler, a) && span_at(spoiler, a)->byte_count == 1 &&
              ansi_span_foreground(*span_at(spoiler, a)) == xterm(1) && span_at(spoiler, a + 1) &&
              ansi_span_foreground(*span_at(spoiler, a + 1)) == 0,
          "ansi: colour runs follow later delimiter removals");

    auto& inline_code = parse(utf8_literal("`\x1b[31mx` and ```py\n\x1b[31my```"));
    check(inline_code.result.ok() && find(inline_code, "\x1b[31mx") != ~0u && find(inline_code, "\x1b[31my") != ~0u,
          "ansi: inline code and other languages keep the raw sequences");
    auto& editor = parse(source, true);
    check(editor.result.ok() && find(editor, "\x1b[1;31mred") != ~0u,
          "ansi: editors keep the raw sequences like the official client's composer");
    auto& plain = parse(utf8_literal("\x1b[31mnot code"));
    check(plain.result.ok() && find(plain, "\x1b[31m") != ~0u, "ansi: text outside code blocks is untouched");

    // Underline and line-through opened before the colour still mark the
    // coloured run's spans, which carry the foreground the lines draw in.
    auto& lines = parse(utf8_literal("__u__ ```ansi\n\x1b[2;4m\x1b[9m\x1b[32mTESTINGTESTING\x1b[0m plain\n```"));
    const u32 testing = find(lines, "TESTINGTESTING");
    const TextStyleSpan* green = testing == ~0u ? nullptr : span_at(lines, testing);
    const TextStyleSpan* outside = span_at(lines, find(lines, "u"));
    const TextStyleSpan* plain_tail = span_at(lines, find(lines, "plain"));
    constexpr u32 both = text_style_underline | text_style_strikethrough;
    check(lines.result.ok() && green && green->byte_offset == testing && green->byte_count == 14 &&
              (green->flags & both) == both && ansi_span_foreground(*green) == xterm(2) && outside &&
              (outside->flags & text_style_underline) && ansi_span_foreground(*outside) == 0 && plain_tail &&
              !(plain_tail->flags & both) && ansi_span_foreground(*plain_tail) == 0,
          "ansi: lines opened before a colour mark the coloured run, which keeps its foreground");
}

// The reply preview (inline reply rules): a fence becomes inline code
// without its language line or edge newlines; ```ansi drops only its SGR
// escapes and keeps no colour; block quotes stay literal.
void test_inline_reply(Check check) noexcept {
    auto& ansi = parse(utf8_literal("```ansi\n\n\x1b[1;31mred\x1b[0m \x1b[38;2;1;2;3mblue\x1b[m\n\n```"), false, true);
    const u32 red = find(ansi, "red");
    bool colourless = true, block = false;
    for (u32 i = 0; i < ansi.result.span_count; ++i) {
        colourless &= ansi_span_foreground(ansi.spans[i]) == 0 && ansi_span_background(ansi.spans[i]) == 0;
        block |= (ansi.spans[i].flags & text_style_code_block) != 0;
    }
    check(ansi.result.ok() && red == 0 && find(ansi, "red blue\x1b[m") == 0 && ansi.result.text.bytes == 11 &&
              span_at(ansi, red) && span_at(ansi, red)->flags == text_style_monospace && colourless && !block,
          "reply: ```ansi becomes one uncoloured inline code run without SGR escapes (ESC[m does not match "
          "/\\x1B\\[\\d+(?:[:;]\\d+)*m/)");
    auto& code = parse(utf8_literal("say ```py\nprint(1)\n``` > not a quote"), false, true);
    check(code.result.ok() && find(code, "say print(1) > not a quote") == 0 && span_at(code, 4) &&
              span_at(code, 4)->flags == text_style_monospace && !span_at(code, 0),
          "reply: other languages lose their tag and highlighting; inline text keeps literal markers");
    auto& blocks = parse(utf8_literal("> quote\n# Head\n- item\n1. first"), false, true);
    const u32 head = find(blocks, "Head");
    check(blocks.result.ok() && find(blocks, "> quote\nHead\n\xE2\x80\xA2 item\n1. first") == 0 && head != ~0u &&
              span_at(blocks, head) && span_at(blocks, head)->flags == text_style_bold && !span_at(blocks, 0),
          "reply: block quotes stay literal, headings are inline bold, list markers inline text");
    auto& message = parse(utf8_literal("```ansi\n\x1b[31mred\x1b[0m\n```"));
    check(message.result.ok() && span_at(message, 0) && (span_at(message, 0)->flags & text_style_code_block) &&
              ansi_span_foreground(*span_at(message, 0)) == xterm(1),
          "reply: the inline rules leave ordinary message ```ansi blocks coloured");
}

bool rgb_is(Color a, u32 rgb) noexcept {
    const float r = static_cast<float>((rgb >> 16) & 255) / 255.f, g = static_cast<float>((rgb >> 8) & 255) / 255.f,
                b = static_cast<float>(rgb & 255) / 255.f;
    const auto close = [](float x, float y) noexcept { return x - y < .002f && y - x < .002f; };
    return close(a.red, r) && close(a.green, g) && close(a.blue, b) && a.alpha == 1.f;
}

void test_palette(Check check) noexcept {
    const auto& dark = kDefaultTheme.tokens.ansi;
    const auto& light = kLightTheme.tokens.ansi;
    check(rgb_is(dark.black, 0x000000) && rgb_is(dark.red, 0xec6361) && rgb_is(dark.green, 0x45a366) &&
              rgb_is(dark.yellow, 0xce8100) && rgb_is(dark.blue, 0x4591ec) && rgb_is(dark.magenta, 0xf549c9) &&
              rgb_is(dark.cyan, 0x049faa) && rgb_is(dark.white, 0xb6b7bc) && rgb_is(dark.bright_black, 0x70717a) &&
              rgb_is(dark.bright_red, 0xff938e) && rgb_is(dark.bright_white, 0xffffff) &&
              rgb_is(light.red, 0xe75858) && rgb_is(light.bright_blue, 0x5865f2) && rgb_is(light.cyan, 0x0098a3) &&
              rgb_is(ui_theme_amoled().tokens.ansi.red, 0xec6361),
          "ansi: theme tokens default to the official dark and light palettes");
    Theme custom = kDefaultTheme;
    custom.tokens.ansi.bright_cyan = Color{.1f, .2f, .3f, 1};
    check(rgb_is(ansi_palette_color(kDefaultTheme, 1), 0xec6361) && rgb_is(ansi_palette_color(kLightTheme, 4), 0x3789ea) &&
              ansi_palette_color(custom, 14).blue == .3f && rgb_is(ansi_palette_color(kDefaultTheme, 16), 0x000000) &&
              rgb_is(ansi_palette_color(kDefaultTheme, 196), 0xff0000) &&
              rgb_is(ansi_palette_color(kDefaultTheme, 110), 0x87afd7) &&
              rgb_is(ansi_palette_color(kDefaultTheme, 232), 0x080808) &&
              rgb_is(ansi_palette_color(kDefaultTheme, 255), 0xeeeeee) && ansi_nearest_fixed_color(250, 3, 2) == 196 &&
              ansi_nearest_fixed_color(128, 128, 128) == 244,
          "ansi: palette indices resolve through tokens (0-15) and the xterm table (16-255)");
    TextStyleSpan span{0, 1, text_style_code_block};
    ansi_span_set_colors(span, xterm(255), xterm(0));
    const TextDecoration decoration = ansi_background_decoration(1, 2, 3, 4, text_style_code_block, xterm(200));
    check(ansi_span_foreground(span) == xterm(255) && ansi_span_background(span) == xterm(0) &&
              ansi_decoration_background(decoration) == xterm(200) &&
              ansi_decoration_background(TextDecoration{0, 0, 1, 1, text_style_code_block,
                                                        TextDecorationKind::background, 0, 0, 0}) == 0,
          "ansi: span and decoration encodings round-trip");
    TextDecoration line{0, 0, 1, 1, text_style_code_block, TextDecorationKind::strikethrough, 0, 0, 0};
    ansi_decoration_set_line_color(line, xterm(9));
    TextDecoration none{0, 0, 1, 1, text_style_code_block, TextDecorationKind::underline, 0, 0, 0};
    ansi_decoration_set_line_color(none, kAnsiColorNone);
    TextDecoration background = line;
    background.kind = TextDecorationKind::background;
    check(ansi_decoration_line_color(line) == xterm(9) && ansi_decoration_line_color(none) == 0 &&
              ansi_decoration_line_color(background) == 0 && ansi_decoration_background(line) == 0,
          "ansi: line colours round-trip on underline/line-through quads only");
}

struct Output {
    TextGlyph glyphs[256];
    TextRun runs[64];
    u16 clusters[256];
    TextDecoration decorations[64];
    ShapedText text;
};

u32 ansi_rectangles(const ShapedText& text, TextDecoration* first, TextDecoration* last) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < text.decoration_count; ++i) {
        if (!ansi_decoration_background(text.decorations[i]))
            continue;
        if (!count)
            *first = text.decorations[i];
        *last = text.decorations[i];
        ++count;
    }
    return count;
}

void layout_case(Check check, TextSystem* system, const char* name) noexcept {
    static Output output;
    output = Output{};
    static wchar_t utf16[256];
    if (!shaped_text_initialize(&output.text, {output.glyphs, 256}, {output.runs, 64}, {output.clusters, 256}, {output.decorations, 64})) {
        check(false, name);
        return;
    }
    // "plain " then a red-on-blue stretch long enough to wrap in 110 DIPs.
    const auto text = utf8_literal("plain colour colour colour");
    TextStyleSpan styles[2]{{0, 6, text_style_monospace | text_style_code_block},
                            {6, 20, text_style_monospace | text_style_code_block}};
    ansi_span_set_colors(styles[1], xterm(1), xterm(4));
    const auto shaped = text_shape_utf8(system, text, {styles, 2}, 110, 1000, {utf16, 256}, &output.text);
    TextDecoration first{}, last{};
    const u32 count = shaped.ok() ? ansi_rectangles(output.text, &first, &last) : 0;
    bool glyphs = shaped.ok() && ansi_text_tag_glyphs(output.text, text, {styles, 2});
    u32 red = 0;
    for (u32 i = 0; glyphs && i < output.text.glyph_count; ++i)
        red += output.text.glyphs[i].reserved == xterm(1);
    glyphs = glyphs && output.text.glyph_count >= 26 && red >= 18 && output.text.glyphs[0].reserved == 0;
    check(shaped.ok() && output.text.line_count >= 2 && count >= 2 && first.top < last.top &&
              first.left > 0 && first.right > first.left && last.left < first.left && last.bottom > last.top &&
              glyphs,
          name);
    shaped_text_reset(&output.text);
}

// Underline and line-through quads take the foreground of the run they sit
// under, also where both lines share a run; a markdown underline outside a
// block and lines under an uncoloured block run keep the text colour.
void line_case(Check check, TextSystem* system) noexcept {
    static Output output;
    output = Output{};
    static wchar_t utf16[256];
    if (!shaped_text_initialize(&output.text, {output.glyphs, 256}, {output.runs, 64}, {output.clusters, 256}, {output.decorations, 64})) {
        check(false, "ansi: line layout storage");
        return;
    }
    const auto text = utf8_literal("plain red green both struck gray");
    constexpr u32 block = text_style_monospace | text_style_code_block;
    constexpr u32 under = text_style_underline, struck = text_style_strikethrough;
    // Bold alternates so each span shapes as its own run.
    TextStyleSpan styles[6]{{0, 6, under},
                            {6, 4, block | under},
                            {10, 6, block | text_style_bold | under},
                            {16, 5, block | under | struck},
                            {21, 7, block | text_style_bold | struck},
                            {28, 4, block | struck}};
    ansi_span_set_colors(styles[1], xterm(1), 0);
    ansi_span_set_colors(styles[2], xterm(2), 0);
    ansi_span_set_colors(styles[3], xterm(196), 0);
    ansi_span_set_colors(styles[4], xterm(4), xterm(3));
    const auto shaped = text_shape_utf8(system, text, {styles, 6}, 1000, 1000, {utf16, 256}, &output.text);
    u32 plain = 0, red = 0, green = 0, both_under = 0, both_struck = 0, blue = 0, gray = 0, other = 0;
    for (u32 i = 0; shaped.ok() && i < output.text.decoration_count; ++i) {
        const TextDecoration& d = output.text.decorations[i];
        if (d.kind != TextDecorationKind::underline && d.kind != TextDecorationKind::strikethrough)
            continue;
        const u16 color = ansi_decoration_line_color(d);
        const bool line = d.kind == TextDecorationKind::underline;
        const bool coded = (d.style_flags & text_style_code_block) != 0;
        if (line && !color && !coded)
            ++plain;
        else if (line && color == xterm(1))
            ++red;
        else if (line && color == xterm(2))
            ++green;
        else if (line && color == xterm(196))
            ++both_under;
        else if (!line && color == xterm(196))
            ++both_struck;
        else if (!line && color == xterm(4))
            ++blue;
        else if (!line && !color && coded)
            ++gray;
        else
            ++other;
    }
    check(shaped.ok() && plain == 1 && red == 1 && green == 1 && both_under == 1 && both_struck >= 1 && blue >= 1 &&
              gray >= 1 && other == 0,
          "ansi: native layout draws each underline and line-through in its run's foreground, both lines included");
    shaped_text_reset(&output.text);
}

// A colored-text generator's component-embed text as such a page serves it: a
// heading link, then a block where every letter has its own colour and every
// letter of "like that" opens another underline. Each glyph paints the
// foreground of the byte its cluster starts at and each underline segment
// the foreground of the run above it: orange under the first "l" too, as
// the official client draws it.
constexpr Utf8View kColoredTextEmbed = utf8_literal(
    "### [Example ANSI Colored Text Preview Tool](https://colors.example.com/ansi-text-preview/)\n```an"
    "si\nSmall tool to make \x1b[48;2;59;61;78m\x1b[2;3;2m\x1b[38;2;255;0;0mr\x1b[2;2m\x1b[3;2m"
    "\x1b[48;2;59;61;78m\x1b[38;2;255;255;0ma\x1b[2;2m\x1b[3;2m\x1b[48;2;59;61;78m\x1b[38;2;0;255;0mi\x1b[2;2m"
    "\x1b[3;2m\x1b[48;2;59;61;78m\x1b[38;2;0;255;255mn\x1b[2;2m\x1b[3;2m\x1b[48;2;59;61;78m\x1b[38;2;0;0;255m"
    "\x1b[7;34mb\x1b[0m\x1b[2;2m\x1b[3;2m\x1b[38;2;0;0;255m\x1b[48;2;59;61;78m\x1b[0m\x1b[2;2m\x1b[3;2m"
    "\x1b[48;2;59;61;78m\x1b[38;2;255;0;255mo\x1b[2;2m\x1b[3;2m\x1b[48;2;59;61;78m\x1b[38;2;255;0;0mw\x1b[0m"
    "\x1b[2;2m\x1b[3;2m\x1b[48;2;59;61;78m\x1b[0m\x1b[48;2;59;61;78m\x1b[0m \x1b[48;2;88;101;242m"
    "\x1b[38;2;255;255;255m\x1b[2;1;2mglowing\x1b[0m\x1b[38;2;255;255;255m\x1b[48;2;88;101;242m\x1b[0m"
    "\x1b[48;2;88;101;242m\x1b[0m text \x1b[2;4;2m\x1b[38;2;244;141;100ml\x1b[2;2m\x1b[4;2m\x1b[38;2;239;135;108m"
    "i\x1b[2;2m\x1b[4;2m\x1b[38;2;235;130;115mk\x1b[2;2m\x1b[4;2m\x1b[38;2;231;124;123me\x1b[2;2m\x1b[4;2m"
    "\x1b[38;2;226;119;131m \x1b[2;2m\x1b[4;2m\x1b[38;2;222;113;138mt\x1b[2;2m\x1b[4;2m\x1b[38;2;217;108;146mh"
    "\x1b[2;2m\x1b[4;2m\x1b[38;2;213;102;154ma\x1b[2;2m\x1b[4;2m\x1b[38;2;209;97;162mt\x1b[0m\x1b[2;2m\x1b[4;2m"
    "\x1b[0m!\n```");

// The owner's sample: underline and line-through opened before the colour,
// both drawn green under the green run; then lines over a recoloured run.
constexpr Utf8View kBothLines = utf8_literal(
    "```ansi\nplain \x1b[2;4m\x1b[9m\x1b[32mTESTINGTESTING\x1b[0m tail \x1b[4;9;31mRED\x1b[34mBLUE\x1b[0m end\n```");

struct PaintCounts {
    u32 checked, underlined, struck, wrong;
};

// Shapes parsed markdown like the renderer and checks every glyph's colour
// tag and the underline/line-through segment under each glyph against the
// foreground of its span; glyphs without a line have none under them.
bool paint_case(TextSystem* system, Utf8View input, float width, bool interactive, bool measured,
                PaintCounts* counts) noexcept {
    struct Storage {
        Output output;
        TextHitRegion hits[64];
        struct Pen {
            float left, right, baseline;
        } pens[256];
    };
    static Storage storage;
    storage = Storage{};
    *counts = PaintCounts{};
    Output& output = storage.output;
    static wchar_t utf16[256];
    auto& p = parse(input);
    const ConstSlice<TextStyleSpan> spans{p.spans, p.result.span_count};
    const Utf8View text = p.result.text;
    // Interactive shapes take one hit span: the heading, else the first byte.
    const u32 heading = find(p, "\n");
    if (!p.result.ok() || !text.bytes ||
        !shaped_text_initialize(&output.text, {output.glyphs, 256}, {output.runs, 64}, {output.clusters, 256}, {output.decorations, 64}, {storage.hits, 64}))
        return false;
    // A drawn message was measured first: the shaper then lays out the
    // measured lines (the multi-line draw path) at the measured height.
    float height = 1000;
    if (measured) {
        const auto measure = text_measure_utf8(system, text, spans, width, 1000, {utf16, 256});
        if (!measure.ok())
            return false;
        height = measure.height;
    }
    const TextHitSpan link{0, heading == ~0u ? 1u : heading, 0, 0};
    const auto shaped = interactive ? text_shape_utf8(system, text, spans, width, height,
                                                      {utf16, 256}, &output.text, TextWrapping::wrap, TextOverflow::clip, {&link, 1})
                                    : text_shape_utf8(system, text, spans, width, height, {utf16, 256},
                                                      &output.text);
    if (!shaped.ok() || !ansi_text_tag_glyphs(output.text, text, spans))
        return false;
    // Each glyph's pen position and baseline (ASCII: one byte, one cluster).
    auto& pens = storage.pens;
    for (u32 run = 0; run < output.text.run_count; ++run) {
        const TextRun& r = output.text.runs[run];
        float x = r.baseline_x;
        for (u32 g = r.first_glyph; g < r.first_glyph + r.glyph_count && g < output.text.glyph_count && g < 256;
             ++g) {
            pens[g] = {x, x + output.text.glyphs[g].advance, r.baseline_y};
            x += output.text.glyphs[g].advance;
        }
    }
    for (u32 at = 0; at < text.bytes && at < output.text.cluster_count; ++at) {
        const u32 glyph = output.text.clusters[at];
        if (text.data[at] == ' ' || text.data[at] == '\n' || glyph >= output.text.glyph_count || glyph >= 256)
            continue; // blank, or hidden by layout
        const TextStyleSpan* span = span_at(p, at);
        const u16 foreground = span ? ansi_span_foreground(*span) : u16{kAnsiColorNone};
        ++counts->checked;
        counts->wrong += output.text.glyphs[glyph].reserved != foreground;
        const auto pen = pens[glyph];
        const float centre = (pen.left + pen.right) * 0.5f;
        // The line of this kind under the glyph's centre on its own line:
        // an underline just below the baseline, a line-through above it.
        const auto line_under = [&](TextDecorationKind kind) noexcept -> const TextDecoration* {
            const bool under = kind == TextDecorationKind::underline;
            for (u32 i = 0; i < output.text.decoration_count; ++i) {
                const TextDecoration& d = output.text.decorations[i];
                if (d.kind == kind && d.left <= centre && centre < d.right &&
                    (under ? d.top >= pen.baseline - 1.0f && d.top <= pen.baseline + 8.0f
                           : d.top >= pen.baseline - 12.0f && d.top < pen.baseline))
                    return &d;
            }
            return nullptr;
        };
        const TextDecoration* under = line_under(TextDecorationKind::underline);
        const TextDecoration* through = line_under(TextDecorationKind::strikethrough);
        const bool underlined = span && (span->flags & text_style_underline);
        const bool struck = span && (span->flags & text_style_strikethrough);
        counts->underlined += underlined;
        counts->struck += struck;
        counts->wrong += underlined ? !under || ansi_decoration_line_color(*under) != foreground : under != nullptr;
        counts->wrong += struck ? !through || ansi_decoration_line_color(*through) != foreground : through != nullptr;
        // A run's first underline segment starts at its first glyph.
        const TextStyleSpan* before = at ? span_at(p, at - 1) : nullptr;
        if (underlined && (!before || !(before->flags & text_style_underline)))
            counts->wrong += !under || under->left < pen.left - 0.5f || under->left > pen.left + 0.5f;
    }
    shaped_text_reset(&output.text);
    return true;
}

void test_layout(Check check) noexcept {
    TextSystem system{};
    if (!text_system_initialize(&system, L"Segoe UI", 16).ok()) {
        check(false, "ansi: text system");
        return;
    }
    layout_case(check, &system,
                "ansi: native layout draws one background per wrapped line and tags red glyphs");
    line_case(check, &system);
    // The colored-text embed's "like that": eight underlined letters, the first
    // ("l", orange) underlined in orange.
    auto& colored = parse(kColoredTextEmbed);
    const u32 like = find(colored, "like");
    const TextStyleSpan* l = like == ~0u ? nullptr : span_at(colored, like);
    const TextStyleSpan* i = like == ~0u ? nullptr : span_at(colored, like + 1);
    check(l && i && ansi_span_foreground(*l) != kAnsiColorNone && ansi_span_foreground(*i) != kAnsiColorNone &&
              (l->flags & text_style_underline) && (i->flags & text_style_underline),
          "ansi: the colored-text embed parses each underlined letter of \"like that\" with its own colour");
    bool shaped = true, edges = true, lines = true;
    constexpr float widths[]{1000.0f, 400.0f, 300.0f, 240.0f, 150.0f};
    for (const float width : widths)
        for (u32 variant = 0; variant < 4; ++variant) {
            PaintCounts counts{};
            const bool painted =
                paint_case(&system, kColoredTextEmbed, width, (variant & 1) != 0, (variant & 2) != 0, &counts);
            shaped = shaped && painted && counts.checked >= 70 && counts.underlined == 8 && counts.struck == 0;
            edges = edges && painted && counts.wrong == 0;
            lines = paint_case(&system, kBothLines, width, (variant & 1) != 0, (variant & 2) != 0, &counts) &&
                    counts.underlined == 21 && counts.struck == 21 && counts.wrong == 0 && lines;
        }
    check(shaped, "ansi: the colored-text embed shapes its eight underlined letters, wrapped, measured and with hits");
    check(edges, "ansi: colored-text embed glyphs and underline segments take each run's foreground (the first "
                 "underlined letter's segment orange like its glyph), wrapped, measured and with hits");
    check(lines, "ansi: underline and line-through opened before a colour both draw in each run's foreground");
    text_system_release(&system);
}

struct PlainParse {
    MessageMarkdownResult result;
    char text[256];
    u32 styles[256];
    TextStyleSpan spans[32];
};

PlainParse parse_plain(Utf8View input, u32 text_capacity = 256) noexcept {
    PlainParse parsed{};
    parsed.result = message_markdown_parse(input, {parsed.text, text_capacity}, {parsed.styles, 256},
                                           {parsed.spans, 32});
    return parsed;
}

bool text_equals(const PlainParse& parsed, Utf8View expected) noexcept {
    return parsed.result.ok() && parsed.result.text.bytes == expected.bytes &&
           bytes_equal(parsed.result.text.data, expected.data, expected.bytes);
}

// Text runs that no rule starts at are copied in one go; every rule start
// inside them must still be seen, including near h/s link prefixes.
void test_plain_runs(Check check) noexcept {
    const auto bold = parse_plain(utf8_literal("plain **bold** tail"));
    check(text_equals(bold, utf8_literal("plain bold tail")) && bold.result.span_count == 1 &&
              bold.spans[0].byte_offset == 6 && bold.spans[0].byte_count == 4 &&
              bold.spans[0].flags == text_style_bold,
          "markdown runs: emphasis inside plain text");
    const auto words = parse_plain(utf8_literal("history shows steam hot http"));
    check(text_equals(words, utf8_literal("history shows steam hot http")) && words.result.span_count == 0,
          "markdown runs: words starting with h or s stay plain");
    const auto link = parse_plain(utf8_literal("see http://a.b/c ok"));
    check(text_equals(link, utf8_literal("see http://a.b/c ok")) && link.result.span_count == 1 &&
              link.spans[0].byte_offset == 4 && link.spans[0].byte_count == 12 &&
              (link.spans[0].flags & text_style_link) != 0,
          "markdown runs: a bare URL in plain text links");
    const auto steam = parse_plain(utf8_literal("run steam://rungameid/1 now"));
    check(steam.result.span_count == 1 && steam.spans[0].byte_offset == 4 &&
              (steam.spans[0].flags & text_style_link) != 0,
          "markdown runs: a steam URL in plain text links");
    const auto jumbo = parse_plain(utf8_literal("\xF0\x9F\x98\x80 \xF0\x9F\x98\x80"));
    check(jumbo.result.ok() && jumbo.result.span_count == 2 && jumbo.spans[0].byte_count == 4 &&
              jumbo.spans[1].byte_offset == 5 && (jumbo.spans[0].flags & text_style_jumbo_emoji) != 0,
          "markdown runs: emoji-only text keeps its spaces unstyled");
    const auto quote = parse_plain(utf8_literal("> quoted line\nplain after"));
    check(text_equals(quote, utf8_literal("quoted line\nplain after")) && quote.result.span_count == 1 &&
              quote.spans[0].byte_count == 12 && (quote.spans[0].flags & text_style_quote) != 0,
          "markdown runs: a quote styles only its own line");
    const auto small = parse_plain(utf8_literal("abcdefgh"), 4);
    check(small.result.error == MessageMarkdownError::output_too_small,
          "markdown runs: output smaller than the input is rejected");
    const auto fence = parse_plain(utf8_literal("some words here ```\ncode\n```"));
    check(fence.result.ok() && fence.result.code_blocks == 1 &&
              text_equals(fence, utf8_literal("some words here code\n")),
          "markdown runs: a fence after plain text is found");
    const char* const escapes[] = {"\x1B[1;31m", "\x1B[31:1m", "\x1B[m", "\x1B[1;m", "\x1B[3", "x"};
    const u32 widths[] = {7, 7, 0, 0, 0, 0};
    bool sgr = true;
    for (u32 i = 0; i < 6; ++i) {
        u32 bytes = 0;
        while (escapes[i][bytes])
            ++bytes;
        sgr = sgr && ansi_text_sgr_bytes(escapes[i], bytes) == widths[i];
    }
    check(sgr, "ansi: the reply preview SGR matcher accepts digits joined by ; or :");
}
} // namespace

void test_ansi_code_blocks(Check check) noexcept {
    test_sgr(check);
    test_extended_and_malformed(check);
    test_markdown(check);
    test_inline_reply(check);
    test_palette(check);
    test_layout(check);
    test_plain_runs(check);
}
} // namespace maicord
