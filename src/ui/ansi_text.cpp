#include "maicord/ui/ansi_text.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/text.hpp"

namespace maicord {
namespace {

struct AnsiState {
    u16 foreground;
    u16 background;
    bool bold;
    bool italic;
    bool underline;
    bool strikethrough;
    bool hidden;
};

constexpr bool same_state(const AnsiState& a, const AnsiState& b) noexcept {
    return a.foreground == b.foreground && a.background == b.background && a.bold == b.bold &&
           a.italic == b.italic && a.underline == b.underline && a.strikethrough == b.strikethrough &&
           a.hidden == b.hidden;
}

// Only <i> and <u> can be closed individually (23/24 close the innermost tag
// when it matches); every other tag ends only at a reset.
enum class AnsiTag : u8 { other, italic, underline };

struct AnsiEntry {
    AnsiState state; // The effective style inside this tag.
    u32 count;       // Identical consecutive tags share one entry.
    AnsiTag tag;
};

// Emulates the ansi-to-html tag stack, which grows with every code. Equal
// consecutive tags share one entry and the innermost 64 distinct entries are
// kept; a close can only remove an innermost <i>/<u>, so the oldest entries
// matter only after 64 distinct tags were closed back to back.
inline constexpr u32 kAnsiStackCapacity = 64;

struct AnsiStack {
    AnsiEntry entries[kAnsiStackCapacity];
    AnsiState current;
    AnsiState floor; // State inside the newest entry that no longer fits.
    u32 bottom;
    u32 size;
    bool truncated;

    void push(AnsiTag tag, const AnsiState& next) noexcept {
        if (size) {
            AnsiEntry& top = entries[(bottom + size - 1u) % kAnsiStackCapacity];
            if (top.tag == tag && same_state(top.state, next) && top.count != ~0u) {
                ++top.count;
                current = next;
                return;
            }
        }
        if (size == kAnsiStackCapacity) {
            floor = entries[bottom].state;
            bottom = (bottom + 1u) % kAnsiStackCapacity;
            --size;
            truncated = true;
        }
        entries[(bottom + size++) % kAnsiStackCapacity] = AnsiEntry{next, 1, tag};
        current = next;
    }
    void close(AnsiTag tag) noexcept {
        if (!size)
            return;
        AnsiEntry& top = entries[(bottom + size - 1u) % kAnsiStackCapacity];
        if (top.tag != tag)
            return;
        if (--top.count)
            return;
        --size;
        current = size ? entries[(bottom + size - 1u) % kAnsiStackCapacity].state
                       : (truncated ? floor : AnsiState{});
    }
    void reset() noexcept {
        size = 0;
        truncated = false;
        reset_value<AnsiState>(&current);
    }
};

constexpr bool digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

// parseInt of a decimal piece; every value above 107 is equally unknown.
u32 code_value(const char* data, u32 begin, u32 end) noexcept {
    u32 value = 0;
    for (u32 i = begin; i < end; ++i) {
        value = value * 10u + static_cast<u32>(data[i] - '0');
        if (value > 100000u)
            value = 100000u;
    }
    return value;
}

void apply_code(AnsiStack& stack, u32 code) noexcept {
    AnsiState next = stack.current;
    switch (code) {
    case 0:
        stack.reset();
        return;
    case 1:
        next.bold = true;
        stack.push(AnsiTag::other, next);
        return;
    case 3:
        next.italic = true;
        stack.push(AnsiTag::italic, next);
        return;
    case 4:
        next.underline = true;
        stack.push(AnsiTag::underline, next);
        return;
    case 8:
        next.hidden = true;
        stack.push(AnsiTag::other, next);
        return;
    case 9:
        next.strikethrough = true;
        stack.push(AnsiTag::other, next);
        return;
    case 22:
        // font-weight:normal;font-style:normal. text-decoration:none on a
        // descendant cannot remove an ancestor's underline or line-through.
        next.bold = false;
        next.italic = false;
        stack.push(AnsiTag::other, next);
        return;
    case 23:
        stack.close(AnsiTag::italic);
        return;
    case 24:
        stack.close(AnsiTag::underline);
        return;
    case 39:
        next.foreground = kAnsiColorNone;
        stack.push(AnsiTag::other, next);
        return;
    case 49:
    case 53:
        // 49 lets an enclosing background show; overline (53) is not drawn.
        stack.push(AnsiTag::other, next);
        return;
    default:
        break;
    }
    if (code == 5 || code == 6) {
        stack.push(AnsiTag::other, next); // <blink>: no visible style.
    } else if (code >= 30 && code <= 37) {
        next.foreground = static_cast<u16>(1u + code - 30u);
        stack.push(AnsiTag::other, next);
    } else if (code >= 40 && code <= 47) {
        next.background = static_cast<u16>(1u + code - 40u);
        stack.push(AnsiTag::other, next);
    } else if (code >= 90 && code <= 97) {
        next.foreground = static_cast<u16>(9u + code - 90u);
        stack.push(AnsiTag::other, next);
    } else if (code >= 100 && code <= 107) {
        next.background = static_cast<u16>(9u + code - 100u);
        stack.push(AnsiTag::other, next);
    }
}

// Applies a colour span ("color:X"/"background-color:X"); an undefined colour
// still opens a span (it can hide an innermost <i>/<u> from 23/24).
void push_color(AnsiStack& stack, bool background, u16 color) noexcept {
    AnsiState next = stack.current;
    if (color != kAnsiColorNone)
        (background ? next.background : next.foreground) = color;
    stack.push(AnsiTag::other, next);
}

u32 digit_run(Utf8View input, u32 at) noexcept {
    u32 end = at;
    while (end < input.bytes && digit(input.data[end]))
        ++end;
    return end;
}

bool literal(Utf8View input, u32 at, const char* text, u32 bytes) noexcept {
    if (bytes > input.bytes - at)
        return false;
    for (u32 i = 0; i < bytes; ++i)
        if (input.data[at + i] != text[i])
            return false;
    return true;
}

// ("0" + Number(digits).toString(16)).substr(-2) as a channel byte; false when
// Number() overflows to Infinity (the colour string is then invalid CSS).
bool channel_byte(const char* data, u32 begin, u32 end, u32* value) noexcept {
    while (begin < end && data[begin] == '0')
        ++begin;
    const u32 digits = end - begin;
    if (digits <= 19) {
        u64 number = 0;
        for (u32 i = begin; i < end; ++i)
            number = number * 10u + static_cast<u64>(data[i] - '0');
        if (digits > 15) // Above 2^53 Number() rounds to the nearest double.
            number = static_cast<u64>(static_cast<double>(number));
        *value = static_cast<u32>(number & 255u);
        return true;
    }
    // Doubles of 1e19 or more (above 2^63) are multiples of 2^11: low byte 0.
    *value = 0;
    if (digits < 309)
        return true;
    if (digits > 309)
        return false;
    static constexpr char maximum[] = "17976931348623158";
    for (u32 i = 0; i < sizeof(maximum) - 1u; ++i) {
        if (data[begin + i] != maximum[i])
            return data[begin + i] < maximum[i];
    }
    return true;
}

struct PendingText {
    AnsiTextSink sink;
    void* context;
    u32 begin;
    u32 end;
    AnsiTextStyle style;
    bool stopped;

    void flush() noexcept {
        if (begin != end && !stopped && !sink(context, begin, end, style))
            stopped = true;
        begin = end;
    }
    void text(u32 first, u32 last, const AnsiState& state) noexcept {
        if (state.hidden) {
            flush();
            begin = end = last;
            return;
        }
        const AnsiTextStyle next{(state.bold ? u32{text_style_bold} : 0u) |
                                     (state.italic ? u32{text_style_italic} : 0u) |
                                     (state.underline ? u32{text_style_underline} : 0u) |
                                     (state.strikethrough ? u32{text_style_strikethrough} : 0u),
                                 state.foreground, state.background};
        if (begin != end && (end != first || !(next == style)))
            flush();
        if (begin == end) {
            begin = first;
            style = next;
        }
        end = last;
    }
    void gap(u32 at) noexcept {
        flush();
        begin = end = at;
    }
};

// One escape at input[at] (ESC). Returns the first byte after the removed
// sequence, applying any SGR/colour effect. Patterns are tried in the order
// ansi-to-html tries them.
u32 consume_escape(Utf8View input, u32 at, AnsiStack& stack) noexcept {
    const char* s = input.data;
    const u32 n = input.bytes;
    if (at + 1u < n && s[at + 1u] == '[') {
        const u32 j = at + 2u;
        // ^\x1b\[[012]?K
        if (j < n && s[j] == 'K')
            return j + 1u;
        if (j + 1u < n && s[j] >= '0' && s[j] <= '2' && s[j + 1u] == 'K')
            return j + 2u;
        // ^\x1b\[\(B
        if (literal(input, j, "(B", 2))
            return j + 2u;
        // ^\x1b\[[34]8;2;\d+;\d+;\d+m
        if (j < n && (s[j] == '3' || s[j] == '4') && literal(input, j + 1u, "8;2;", 4)) {
            u32 cursor = j + 5u, bounds[6]{};
            bool matched = true;
            for (u32 channel = 0; channel < 3 && matched; ++channel) {
                const u32 end = digit_run(input, cursor);
                const char terminator = channel == 2 ? 'm' : ';';
                matched = end > cursor && end < n && s[end] == terminator;
                bounds[channel * 2u] = cursor;
                bounds[channel * 2u + 1u] = end;
                cursor = end + 1u;
            }
            if (matched) {
                u32 rgb[3]{};
                bool valid = true;
                for (u32 channel = 0; channel < 3; ++channel)
                    valid = channel_byte(s, bounds[channel * 2u], bounds[channel * 2u + 1u], rgb + channel) &&
                            valid;
                push_color(stack, s[j] == '4',
                           valid ? static_cast<u16>(1u + ansi_nearest_fixed_color(rgb[0], rgb[1], rgb[2]))
                                 : kAnsiColorNone);
                return cursor;
            }
        }
        // ^\x1b\[38;5;(\d+)m and ^\x1b\[48;5;(\d+)m: colors[digits], so only
        // canonical decimal 0-255 names a colour.
        if (j < n && (s[j] == '3' || s[j] == '4') && literal(input, j + 1u, "8;5;", 4)) {
            const u32 first = j + 5u, end = digit_run(input, first);
            if (end > first && end < n && s[end] == 'm') {
                const u32 digits = end - first;
                const u32 value = digits <= 3 ? code_value(s, first, end) : 256u;
                const bool canonical = digits == 1 || s[first] != '0';
                push_color(stack, s[j] == '4',
                           canonical && value <= 255u ? static_cast<u16>(value + 1u) : kAnsiColorNone);
                return end + 1u;
            }
        }
        // ^\x1b\[((?:\d{1,3};?)+|)m: digits and single semicolons, starting
        // with a digit, or nothing.
        u32 end = j;
        bool doubled = false;
        while (end < n && (digit(s[end]) || s[end] == ';')) {
            doubled = doubled || (s[end] == ';' && end > j && s[end - 1u] == ';');
            ++end;
        }
        if (end < n && s[end] == 'm' && (end == j || (digit(s[j]) && !doubled))) {
            if (end == j) {
                apply_code(stack, 0);
            } else {
                for (u32 piece = j; piece <= end;) {
                    u32 stop = piece;
                    while (stop < end && s[stop] != ';')
                        ++stop;
                    if (stop > piece) // parseInt('') is NaN: no effect.
                        apply_code(stack, code_value(s, piece, stop));
                    piece = stop + 1u;
                }
            }
            return end + 1u;
        }
        // ^\x1b\[\d?J
        if (j < n && s[j] == 'J')
            return j + 1u;
        if (j + 1u < n && digit(s[j]) && s[j + 1u] == 'J')
            return j + 2u;
        // ^\x1b\[\d{0,3};\d{0,3}f
        const u32 first_end = digit_run(input, j);
        if (first_end - j <= 3 && first_end < n && s[first_end] == ';') {
            const u32 second_end = digit_run(input, first_end + 1u);
            if (second_end - first_end - 1u <= 3 && second_end < n && s[second_end] == 'f')
                return second_end + 1u;
        }
    }
    // ^\x1b\[?[\d;]{0,3}
    u32 cursor = at + 1u;
    if (cursor < n && s[cursor] == '[')
        ++cursor;
    for (u32 i = 0; i < 3 && cursor < n && (digit(s[cursor]) || s[cursor] == ';'); ++i)
        ++cursor;
    return cursor;
}

u32 fixed_level(u32 step) noexcept {
    return step ? step * 40u + 55u : 0u;
}

void fixed_rgb(u8 index, u32* red, u32* green, u32* blue) noexcept {
    if (index >= 232) {
        *red = *green = *blue = 8u + 10u * (index - 232u);
        return;
    }
    const u32 cube = index - 16u;
    *red = fixed_level(cube / 36u);
    *green = fixed_level((cube / 6u) % 6u);
    *blue = fixed_level(cube % 6u);
}

} // namespace

bool ansi_text_scan(Utf8View input, AnsiTextSink sink, void* context) noexcept {
    if ((!input.data && input.bytes) || !sink)
        return false;
    AnsiStack stack{};
    PendingText pending{sink, context, 0, 0, {}, false};
    for (u32 at = 0; at < input.bytes && !pending.stopped;) {
        const char value = input.data[at];
        if (value == '\x1b') {
            const u32 next = consume_escape(input, at, stack);
            pending.gap(next);
            at = next;
        } else if (value == '\x08') {
            u32 next = at + 1u;
            while (next < input.bytes && input.data[next] == '\x08')
                ++next;
            pending.gap(next);
            at = next;
        } else {
            u32 next = at + 1u;
            while (next < input.bytes && input.data[next] != '\x1b' && input.data[next] != '\x08')
                ++next;
            pending.text(at, next, stack.current);
            at = next;
        }
    }
    pending.flush();
    return !pending.stopped;
}

bool ansi_text_tag_glyphs(const ShapedText& text, Utf8View source, ConstSlice<TextStyleSpan> styles) noexcept {
    bool colored = false;
    for (u32 index = 0; index < styles.count && !colored; ++index)
        colored = ansi_span_foreground(styles[index]) != kAnsiColorNone;
    if (!colored || !text.clusters || !text.cluster_count || !text.glyphs || (!source.data && source.bytes))
        return false;
    constexpr u16 untagged = 0xFFFFu;
    for (u32 glyph = 0; glyph < text.glyph_count; ++glyph)
        text.glyphs[glyph].reserved = untagged;
    u32 style = 0, unit = 0;
    for (u32 byte = 0; byte < source.bytes && unit < text.cluster_count;) {
        const Utf8DecodeResult decoded = utf8_decode_one(source.data + byte, source.bytes - byte);
        const u32 width = decoded.valid && decoded.width ? decoded.width : 1u;
        while (style < styles.count && styles[style].byte_offset + styles[style].byte_count <= byte)
            ++style;
        const u16 code = style < styles.count && styles[style].byte_offset <= byte ? ansi_span_foreground(styles[style])
                                                                                  : kAnsiColorNone;
        const u32 units = decoded.valid && decoded.codepoint > 0xFFFFu ? 2u : 1u;
        for (u32 k = 0; k < units && unit < text.cluster_count; ++k, ++unit) {
            const u32 glyph = text.clusters[unit];
            if (glyph < text.glyph_count && text.glyphs[glyph].reserved == untagged)
                text.glyphs[glyph].reserved = code;
        }
        byte += width;
    }
    for (u32 run = 0; run < text.run_count; ++run) {
        const TextRun& shaped = text.runs[run];
        u16 previous = kAnsiColorNone;
        for (u32 glyph = shaped.first_glyph; glyph < shaped.first_glyph + shaped.glyph_count && glyph < text.glyph_count;
             ++glyph) {
            if (text.glyphs[glyph].reserved == untagged)
                text.glyphs[glyph].reserved = previous;
            previous = text.glyphs[glyph].reserved;
        }
    }
    for (u32 glyph = 0; glyph < text.glyph_count; ++glyph) // Glyphs outside every run.
        if (text.glyphs[glyph].reserved == untagged)
            text.glyphs[glyph].reserved = kAnsiColorNone;
    return true;
}

u8 ansi_nearest_fixed_color(u32 red, u32 green, u32 blue) noexcept {
    u32 best = 16, best_distance = ~0u;
    for (u32 index = 16; index < 256; ++index) {
        u32 r = 0, g = 0, b = 0;
        fixed_rgb(static_cast<u8>(index), &r, &g, &b);
        const i32 dr = static_cast<i32>(r) - static_cast<i32>(red & 255u);
        const i32 dg = static_cast<i32>(g) - static_cast<i32>(green & 255u);
        const i32 db = static_cast<i32>(b) - static_cast<i32>(blue & 255u);
        const u32 distance = static_cast<u32>(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    return static_cast<u8>(best);
}

Color ansi_palette_color(const Theme& theme, u8 index) noexcept {
    using Member = Color UiTheme_ansi_tokens::*;
    static constexpr Member palette[16]{
        &UiTheme_ansi_tokens::black,          &UiTheme_ansi_tokens::red,
        &UiTheme_ansi_tokens::green,          &UiTheme_ansi_tokens::yellow,
        &UiTheme_ansi_tokens::blue,           &UiTheme_ansi_tokens::magenta,
        &UiTheme_ansi_tokens::cyan,           &UiTheme_ansi_tokens::white,
        &UiTheme_ansi_tokens::bright_black,   &UiTheme_ansi_tokens::bright_red,
        &UiTheme_ansi_tokens::bright_green,   &UiTheme_ansi_tokens::bright_yellow,
        &UiTheme_ansi_tokens::bright_blue,    &UiTheme_ansi_tokens::bright_magenta,
        &UiTheme_ansi_tokens::bright_cyan,    &UiTheme_ansi_tokens::bright_white,
    };
    if (index < 16)
        return theme.tokens.ansi.*palette[index];
    u32 red = 0, green = 0, blue = 0;
    fixed_rgb(index, &red, &green, &blue);
    return color_from_rgb24(red << 16 | green << 8 | blue);
}

} // namespace maicord
