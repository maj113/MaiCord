#include "maicord/ui/text/ansi_text.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"
#include <stddef.h>
#include "markdown_internal.hpp"


namespace maicord {
using namespace markdown_detail;
namespace {
MessageMarkdownError walk(Parser* parser, u32 stop_depth = ~0u) noexcept;
MessageMarkdownResult parse_error(MessageMarkdownError error) noexcept {
    MessageMarkdownResult result{};
    result.error = error;
    return result;
}

bool emoji_base(u32 codepoint) noexcept {
    return (codepoint >= 0x1F000u && codepoint <= 0x1FAFFu) ||
           (codepoint >= 0x2600u && codepoint <= 0x27BFu) || codepoint == 0x00A9u || codepoint == 0x00AEu ||
           codepoint == 0x203Cu || codepoint == 0x2049u || codepoint == 0x2122u || codepoint == 0x2139u ||
           codepoint == 0x3030u || codepoint == 0x303Du || codepoint == 0x3297u || codepoint == 0x3299u;
}

bool emoji_modifier(u32 codepoint) noexcept {
    return codepoint == 0xFE0Fu || (codepoint >= 0x1F3FBu && codepoint <= 0x1F3FFu) ||
           (codepoint >= 0xE0020u && codepoint <= 0xE007Fu);
}

bool consume_codepoint(Utf8View source, u32* cursor, u32* codepoint) noexcept {
    if (*cursor >= source.bytes) {
        return false;
    }
    const Utf8DecodeResult decoded = utf8_decode_one(source.data + *cursor, source.bytes - *cursor);
    if (!decoded.valid || decoded.width == 0) {
        return false;
    }
    *cursor += decoded.width;
    *codepoint = decoded.codepoint;
    return true;
}

bool consume_keycap(Utf8View source, u32* cursor) noexcept {
    if (*cursor >= source.bytes) {
        return false;
    }
    const u32 begin = *cursor;
    const char value = source.data[(*cursor)++];
    if (!ascii_is_digit(value) && value != '#' && value != '*') {
        *cursor = begin;
        return false;
    }
    u32 codepoint = 0;
    u32 probe = *cursor;
    if (consume_codepoint(source, &probe, &codepoint) && codepoint == 0xFE0Fu) {
        *cursor = probe;
    }
    probe = *cursor;
    if (!consume_codepoint(source, &probe, &codepoint) || codepoint != 0x20E3u) {
        *cursor = begin;
        return false;
    }
    *cursor = probe;
    return true;
}

bool consume_custom_emoji(Utf8View source, u32* cursor) noexcept {
    if (*cursor >= source.bytes || source.data[*cursor] != '<') {
        return false;
    }
    const u32 begin = *cursor;
    u32 probe = begin + 1;
    if (probe < source.bytes && source.data[probe] == 'a') {
        ++probe;
    }
    if (probe >= source.bytes || source.data[probe++] != ':') {
        return false;
    }
    const u32 name = probe;
    while (probe < source.bytes && source.data[probe] != ':' && source.data[probe] != '>' &&
           !ascii_space(source.data[probe])) {
        ++probe;
    }
    if (probe == name || probe >= source.bytes || source.data[probe++] != ':') {
        return false;
    }
    const u32 digits = probe;
    while (probe < source.bytes && ascii_is_digit(source.data[probe])) {
        ++probe;
    }
    if (probe == digits || probe >= source.bytes || source.data[probe] != '>') {
        return false;
    }
    *cursor = probe + 1;
    return true;
}

bool consume_unicode_emoji(Utf8View source, u32* cursor) noexcept {
    const u32 begin = *cursor;
    if (consume_keycap(source, cursor)) {
        return true;
    }
    u32 codepoint = 0;
    if (!consume_codepoint(source, cursor, &codepoint) || !emoji_base(codepoint)) {
        *cursor = begin;
        return false;
    }
    for (;;) {
        const u32 before = *cursor;
        if (!consume_codepoint(source, cursor, &codepoint)) {
            *cursor = before;
            return true;
        }
        if (emoji_modifier(codepoint)) {
            continue;
        }
        if (codepoint != 0x200Du) {
            *cursor = before;
            return true;
        }
        if (!consume_codepoint(source, cursor, &codepoint) || !emoji_base(codepoint)) {
            *cursor = begin;
            return false;
        }
    }
}

Utf8View emoji_alias_at(Utf8View source, u32 begin, u32* end) noexcept {
    if (begin >= source.bytes || source.data[begin] != ':') return {};
    u32 cursor = begin + 1;
    while (cursor < source.bytes && cursor - begin <= 255) {
        const char c = source.data[cursor];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              ascii_is_digit(c) || c == '_' || c == '-' || c == '+')) break;
        ++cursor;
    }
    if (cursor == begin + 1 || cursor >= source.bytes || source.data[cursor] != ':') return {};
    // name::skin-tone-N selects the base emoji's Unicode skin-tone variant.
    u32 extended = cursor + 1;
    if (extended + 12 <= source.bytes && source.data[extended] == ':' &&
        emoji_catalog_name_starts_with({source.data + extended + 1, source.bytes - extended - 1},
                                      utf8_literal("skin-tone-"))) {
        extended += 11;
        if (extended + 1 < source.bytes && ascii_is_digit(source.data[extended]) && source.data[extended + 1] == ':') {
            const auto glyph = emoji_shortcode_glyph({source.data + begin + 1, extended - begin});
            if (glyph.bytes) { *end = extended + 2; return glyph; }
        }
    }
    const auto glyph = emoji_shortcode_glyph({source.data + begin + 1, cursor - begin - 1});
    if (glyph.bytes) *end = cursor + 1;
    return glyph;
}

bool emoji_only_source(Utf8View source) noexcept {
    u32 cursor = 0;
    u32 count = 0;
    while (cursor < source.bytes) {
        if (ascii_space(source.data[cursor])) {
            ++cursor;
            continue;
        }
        u32 alias_end = cursor;
        if (emoji_alias_at(source, cursor, &alias_end).bytes) cursor = alias_end;
        else if (!consume_custom_emoji(source, &cursor) && !consume_unicode_emoji(source, &cursor)) {
            return false;
        }
        if (++count > 30u) {
            return false;
        }
    }
    return count != 0;
}

// simple-markdown's escape rule /^\\([^0-9A-Za-z\s])/: a backslash escapes any
// one character that is not ASCII alphanumeric or whitespace ("\!", "\.").
// ASCII \s is \t \n \v \f \r and space; other controls and DEL escape.
bool markdown_escape_target(u32 cp) noexcept {
    if (cp < 0x80)
        return !(cp == ' ' || (cp >= '\t' && cp <= '\r')) &&
               !((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'));
    return !unicode_space(cp);
}

bool push_delimiter(Parser* parser, u32 style, u8 marker_bytes, DelimiterKind kind) noexcept {
    if (parser->depth >= kDelimiterCapacity) {
        return false;
    }
    parser->delimiters[parser->depth++] =
        Delimiter{parser->input_offset, parser->output_bytes, style, marker_bytes, kind, 0xFF};
    if (parser->replay_cache && parser->replay_cache->cache_count < kDelimiterCapacity) {
        const u32 index = parser->replay_cache->cache_count++;
        parser->replay_cache->cache[index] = {parser->input_offset, {}};
        parser->delimiters[parser->depth - 1].replay_cache_index = static_cast<u8>(index);
    }
    if (parser->output_mode == OutputMode::source) {
        SourceSink& sink = *parser->source_sink;
        SourceScope& scope = sink.scopes[parser->depth - 1];
        reset_value(&scope);
        scope.parent_style = sink.active_style;
        u32 cached = 0;
        while (cached < sink.cache_count && sink.cache[cached].input_offset != parser->input_offset) ++cached;
        if (cached == sink.cache_count) {
            // Keep the earliest upcoming openers, including all uninterrupted
            // active nesting. Replay fills exact pop outcomes; EOF leaves
            // explicit unmatched entries. Active scopes own copied outcomes,
            // so refreshing this cache cannot change an ancestor's decision.
            sink.cache_count = 1;
            sink.cache[0] = {parser->input_offset, {}};
            cached = 0;
            Parser replay;
            // Only active slots are read by the walker and every push initializes
            // its next slot first, so copy the scalar prefix and the active slots,
            // not all kDelimiterCapacity records.
            copy_bytes(&replay, parser, static_cast<u32>(offsetof(Parser, delimiters) +
                sizeof(Delimiter) * parser->depth));
            replay.source_sink = nullptr;
            replay.output_mode = OutputMode::recognition;
            replay.replay_cache = &sink;
            replay.delimiters[parser->depth - 1].replay_cache_index = 0;
            replay.input_offset += marker_bytes;
            replay.output_bytes += marker_bytes;
            if (walk(&replay, parser->depth - 1) != MessageMarkdownError::none) return false;
        }
        scope.closure = sink.cache[cached].closure;
        sink.active_style |= scope.closure.style;
    }
    return true;
}

bool block_prefix(Parser* parser) noexcept {
    if (!parser->line_start || parser->depth != 0) {
        return true;
    }
    const bool bio = parser->options && parser->options->profile_bio;
    const bool lists = !bio && !(parser->options && parser->options->no_lists);
    const bool headings = !bio && !(parser->options && parser->options->no_headings);
    // Reply previews drop block quotes; headings and lists render inline.
    const bool inline_reply = parser->options && parser->options->inline_reply;
    const auto leading_spaces = [&]() noexcept {
        u32 lead = 0;
        while (parser->input_offset + lead < parser->input.bytes &&
               parser->input.data[parser->input_offset + lead] == ' ' && lead < 64)
            ++lead;
        return lead;
    };
    // Block quotes: / *>>> +/ quotes the rest of the message and
    // / *>(?!>>) +/ one line; never inside another quote. One space after the
    // marker is stripped (/^ *> ?/); the quote content keeps block rules.
    if (!inline_reply && !(parser->persistent_style & text_style_quote)) {
        const u32 lead = leading_spaces();
        const u32 marker = parser->input_offset + lead;
        u32 prefix = 0;
        if (starts_with_at(*parser, marker, ">>> ", 4)) {
            prefix = lead + 4;
            parser->persistent_style |= text_style_quote;
        } else if (starts_with_at(*parser, marker, "> ", 2)) {
            prefix = lead + 2;
        }
        if (prefix != 0) {
            parser->input_offset += prefix;
            parser->line_style |= text_style_quote;
            ++parser->transformations;
        }
    }
    u32 indentation = 0;
    while (parser->input_offset + indentation < parser->input.bytes &&
           parser->input.data[parser->input_offset + indentation] == ' ' && indentation < 8) {
        ++indentation;
    }
    if (lists && (indentation & 1u) == 0 && indentation <= 6) {
        const u32 marker = parser->input_offset + indentation;
        bool unordered = false;
        u32 prefix_bytes = 0;
        if (marker + 1 < parser->input.bytes &&
            (parser->input.data[marker] == '-' || parser->input.data[marker] == '*') &&
            parser->input.data[marker + 1] == ' ') {
            unordered = true;
            prefix_bytes = indentation + 2;
        } else {
            u32 digits = marker;
            while (digits < parser->input.bytes && ascii_is_digit(parser->input.data[digits]) &&
                   digits - marker < 9) {
                ++digits;
            }
            if (digits > marker && digits + 1 < parser->input.bytes && parser->input.data[digits] == '.' &&
                parser->input.data[digits + 1] == ' ') {
                prefix_bytes = digits + 2 - parser->input_offset;
            }
        }
        if (prefix_bytes != 0) {
            const u32 depth = indentation / 2;
            const u32 style = text_style_list | (depth << kTextStyleListDepthShift) |
                              (unordered ? text_style_list_bullet : 0);
            const u32 begin = parser->input_offset;
            parser->input_offset += prefix_bytes;
            if (inline_reply) {
                // Inline list items: "• " before an unordered item, "N. " kept.
                if (unordered) {
                    static constexpr char bullet[] = "\xE2\x80\xA2 ";
                    for (u32 index = 0; index < sizeof(bullet) - 1; ++index)
                        if (!emit_byte(parser, bullet[index], 0, begin))
                            return false;
                } else if (!emit_range(parser, begin + indentation, parser->input_offset)) {
                    return false;
                }
                parser->line_start = false;
                ++parser->transformations;
                return true;
            }
            parser->line_style |= style;
            if (unordered) {
                for (u32 index = 0; index < prefix_bytes; ++index) {
                    if (!emit_byte(parser, ' ', 0, begin + index)) {
                        return false;
                    }
                }
            } else if (!emit_range(parser, begin, parser->input_offset)) {
                return false;
            }
            parser->line_start = false;
            if (parser->list_items != 0xFFFF) {
                ++parser->list_items;
            }
            ++parser->transformations;
            return true;
        }
    }
    if (headings) {
        const u32 lead = leading_spaces();
        const u32 marker = parser->input_offset + lead;
        u32 prefix = 0;
        u32 style = 0;
        if (starts_with_at(*parser, marker, "-# ", 3)) {
            prefix = 3;
            style = text_style_subtext;
        } else if (starts_with_at(*parser, marker, "### ", 4)) {
            prefix = 4;
            style = text_style_heading_3 | text_style_bold;
        } else if (starts_with_at(*parser, marker, "## ", 3)) {
            prefix = 3;
            style = text_style_heading_2 | text_style_bold;
        } else if (starts_with_at(*parser, marker, "# ", 2)) {
            prefix = 2;
            style = text_style_heading_1 | text_style_bold;
        }
        if (prefix != 0) {
            parser->input_offset += lead + prefix;
            // Inline headings keep the inherited size and are semibold; inline
            // subtext keeps the reply's size as well.
            parser->line_style |= inline_reply ? style & text_style_bold : style;
            ++parser->transformations;
        }
    }
    parser->line_start = false;
    return true;
}

u8 emphasis_marker(const Parser& parser, u32* style, DelimiterKind* kind) noexcept {
    *style = 0;
    *kind = DelimiterKind::emphasis;
    if (starts_with(parser, "```", 3)) {
        *style = text_style_monospace | text_style_code_block;
        *kind = DelimiterKind::code;
        return 3;
    }
    if (starts_with(parser, "***", 3)) {
        *style = text_style_bold | text_style_italic;
        return 3;
    }
    if (starts_with(parser, "**", 2)) {
        *style = text_style_bold;
        return 2;
    }
    if (starts_with(parser, "__", 2)) {
        *style = text_style_underline;
        return 2;
    }
    if (starts_with(parser, "~~", 2)) {
        *style = text_style_strikethrough;
        return 2;
    }
    if (starts_with(parser, "||", 2)) {
        *style = text_style_spoiler;
        *kind = DelimiterKind::spoiler;
        return 2;
    }
    if (starts_with(parser, "`", 1)) {
        *style = text_style_monospace;
        *kind = DelimiterKind::code;
        return 1;
    }
    if (starts_with(parser, "*", 1) || starts_with(parser, "_", 1)) {
        *style = text_style_italic;
        return 1;
    }
    return 0;
}

// Rules match: code, spoilers, ***, ** and __ may open before
// whitespace ("** a **" is bold, "` a `" code); a single * or _ and ~~ need
// text right after them.
bool marker_can_open(const Parser& parser, u8 bytes, DelimiterKind kind) noexcept {
    const u32 after = parser.input_offset + bytes;
    if (after >= parser.input.bytes)
        return false;
    const bool spaced = kind != DelimiterKind::emphasis ||
                        (bytes >= 2 && parser.input.data[parser.input_offset] != '~');
    return spaced || !ascii_space(parser.input.data[after]);
}

bool marker_can_close(const Parser& parser, const Delimiter& delimiter) noexcept {
    return parser.output_bytes > delimiter.output_offset + delimiter.marker_bytes;
}

MessageMarkdownResult finish(Parser* parser) noexcept {
    // Spoiler visibility is decided when its closing delimiter is processed.
    // Keep identity/source metadata, but never publish hidden flag artwork.
    for (u32 i = 0; i < parser->semantic_count; ++i) {
        auto& semantic = parser->semantics[i];
        if (semantic.kind == MessageMarkdownSemanticKind::regional_flag &&
            (parser->styles[semantic.byte_offset] & text_style_spoiler)) semantic.reserved |= 2u;
    }
    u32 span_count = 0;
    u32 offset = 0;
    const u32 base_style = parser->options ? parser->options->base_style : 0;
    const bool sized = parser->options && parser->options->text_sizes;
    const u8 base_size = sized && parser->options->base_size < text_size_count ? parser->options->base_size : u8{0};
    u32 run = 0;
    while (offset < parser->output_bytes) {
        const u32 style = parser->styles[offset] | base_style;
        while (run < parser->ansi_run_count && parser->ansi_runs[run].end <= offset)
            ++run;
        const bool colored = run < parser->ansi_run_count && parser->ansi_runs[run].begin <= offset;
        const u32 limit = colored                          ? parser->ansi_runs[run].end
                          : run < parser->ansi_run_count ? parser->ansi_runs[run].begin
                                                          : parser->output_bytes;
        u32 end = offset + 1;
        // Each bullet item is its own span: the renderer draws a bullet at the start of a span, so items sharing
        // flags must not merge across their line breaks.
        const bool bulleted = (style & text_style_list_bullet) != 0;
        while (end < limit && (parser->styles[end] | base_style) == style &&
               !(bulleted && parser->output[end - 1] == '\n')) {
            ++end;
        }
        if (style != 0 || base_size != 0) {
            if (span_count >= parser->spans.count) {
                return parse_error(MessageMarkdownError::span_capacity);
            }
            TextStyleSpan span{offset, end - offset, style,
                               sized ? message_markdown_text_size(style, base_size) : u8{0}};
            if (colored)
                ansi_span_set_colors(span, parser->ansi_runs[run].foreground, parser->ansi_runs[run].background);
            parser->spans[span_count++] = span;
        }
        offset = end;
    }
    MessageMarkdownResult result{};
    result.text = Utf8View{parser->output.data, parser->output_bytes};
    result.span_count = span_count;
    result.transformations = parser->transformations;
    result.syntax_tokens = parser->syntax_tokens;
    result.syntax_styled_bytes = parser->syntax_styled_bytes;
    result.resolved_entities = parser->resolved_entities;
    result.unresolved_entities = parser->unresolved_entities;
    result.timestamps = parser->timestamps;
    result.semantic_count = parser->semantic_count;
    result.next_timestamp_update_unix_seconds = parser->next_timestamp_update_unix_seconds;
    result.list_items = parser->list_items;
    result.code_blocks = parser->code_blocks;
    result.syntax_truncated = parser->syntax_truncated;
    result.semantics_truncated = parser->semantics_truncated;
    return result;
}

struct SourceCodeEmission {
    Parser* parser;
    u32 first;
    u32 cursor;
};

void emit_source_code_range(SourceCodeEmission* emission, u32 end, u32 style = 0) noexcept {
    Parser* parser = emission->parser;
    const u32 flags = parser->line_style | parser->persistent_style | style;
    while (emission->cursor < end) {
        const u32 offset = emission->first + emission->cursor++;
        source_byte(parser, offset, parser->input.data[offset], flags);
    }
}

bool emoji_alias_inside_url(Utf8View input, u32 begin, u32 end) noexcept {
    u32 first = begin;
    while (first && !ascii_space(input.data[first - 1]) && input.data[first - 1] != '<' &&
           input.data[first - 1] != '>' && input.data[first - 1] != '[' &&
           input.data[first - 1] != '(') --first;
    while (first < begin && (input.data[first] == '*' || input.data[first] == '_' ||
                            input.data[first] == '~' || input.data[first] == '|')) ++first;
    return first < begin && valid_url(input, first, end);
}

bool emit_closed_source_code(Parser* parser) noexcept {
    if (parser->output_mode != OutputMode::source || parser->depth == 0) return false;
    const Delimiter& delimiter = parser->delimiters[parser->depth - 1];
    const SourceScope& scope = parser->source_sink->scopes[parser->depth - 1];
    const SourceClosure& closure = scope.closure;
    if (delimiter.kind != DelimiterKind::code || !closure.closed) return false;
    if (!closure.removed_marker) {
        for (u32 i = 0; i < delimiter.marker_bytes; ++i)
            source_byte(parser, delimiter.input_offset + i, parser->input.data[delimiter.input_offset + i],
                parser->line_style | parser->persistent_style);
    }
    SourceCodeEmission emission{parser, closure.content_begin, 0};
    const u32 bytes = closure.close_offset - closure.content_begin;
    if (closure.language != CodeLanguage::none && closure.removed_marker) {
        const u32 remaining = parser->syntax_tokens < kCodeHighlightTokenMaximum
            ? kCodeHighlightTokenMaximum - parser->syntax_tokens : 0;
        const auto highlighted = code_highlight_ranges(
            {parser->input.data + closure.content_begin, bytes}, closure.language,
            +[](void* context, u32 begin, u32 end, u32 flags) noexcept {
                auto* emission = static_cast<SourceCodeEmission*>(context);
                emit_source_code_range(emission, begin);
                emit_source_code_range(emission, end, flags);
            }, &emission, remaining);
        parser->syntax_tokens += highlighted.token_count;
    }
    emit_source_code_range(&emission, bytes);
    parser->output_bytes += delimiter.marker_bytes + bytes;
    if (closure.removed_marker) parser->output_bytes -= delimiter.marker_bytes;
    // Raw code updates line_start but does not reset line_style at newlines.
    const u32 raw_begin = delimiter.input_offset + delimiter.marker_bytes;
    if (closure.close_offset > raw_begin)
        parser->line_start = parser->input.data[closure.close_offset - 1] == '\n';
    parser->input_offset = closure.close_offset + delimiter.marker_bytes;
    parser->source_sink->active_style = scope.parent_style;
    --parser->depth;
    return true;
}

constexpr u64 rule_bit(char value) noexcept {
    return u64{1} << (static_cast<u8>(value) & 63u);
}
// The one literal the emoticon rule keeps verbatim: the shrug face, whose "\_"
// is not an escape.
constexpr char kShrugFace[] = "\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF";
constexpr u32 kShrugFaceBytes = sizeof(kShrugFace) - 1;
// ASCII bytes an inline or line rule can start at: \n * : < @ [ \ ] _ ` | ~
// (h and s only when "http" or "steam" follows), and the 0xC2 0xAF that opens
// the shrug face. Anything else is emitted unchanged by walk().
constexpr u64 kRuleStartLow = rule_bit('\n') | rule_bit('*') | rule_bit(':') | rule_bit('<');
constexpr u64 kRuleStartHigh = rule_bit('@') | rule_bit('[') | rule_bit('\\') | rule_bit(']') | rule_bit('_') |
                               rule_bit('`') | rule_bit('|') | rule_bit('~');

inline bool plain_text_byte(Utf8View input, u32 at) noexcept {
    const u8 value = static_cast<u8>(input.data[at]);
    if (value >= 0x80u)
        return value != 0xC2u || at + 1 >= input.bytes || static_cast<u8>(input.data[at + 1]) != 0xAFu;
    if (((value < 64u ? kRuleStartLow : kRuleStartHigh) >> (value & 63u)) & 1u)
        return false;
    const u32 left = input.bytes - at;
    if (value == 'h')
        return !(left > 3 && bytes_equal(input.data + at + 1, "ttp", 3));
    if (value == 's')
        return !(left > 4 && bytes_equal(input.data + at + 1, "team", 4));
    return true;
}

MessageMarkdownError walk(Parser* state, u32 stop_depth) noexcept {
    Parser& parser = *state;
    const auto& options = *parser.options;
    const Utf8View input = parser.input;
    u32 flag_cursor = 0;
    RegionalFlagRange flag{};
    bool have_flag = parser.output_mode == OutputMode::transformed && regional_flag_next(input, &flag_cursor, &flag);

    while (parser.input_offset < parser.input.bytes) {
        if (parser.depth <= stop_depth && stop_depth != ~0u) return MessageMarkdownError::none;
        while (have_flag && flag.first_byte < parser.input_offset)
            have_flag = regional_flag_next(input, &flag_cursor, &flag);
        if (parser.depth != 0 && parser.delimiters[parser.depth - 1].kind == DelimiterKind::code) {
            const Delimiter& top = parser.delimiters[parser.depth - 1];
            u32 style = 0;
            DelimiterKind kind = DelimiterKind::emphasis;
            const u8 marker = emphasis_marker(parser, &style, &kind);
            if (kind == DelimiterKind::code && marker == top.marker_bytes &&
                marker_can_close(parser, top)) {
                const Delimiter closed = parser.delimiters[--parser.depth];
                close_delimiter(&parser, closed);
                parser.input_offset += marker;
                continue;
            }
            const char value = parser.input.data[parser.input_offset];
            if (!emit_byte(&parser, value)) {
                return MessageMarkdownError::output_too_small;
            }
            parser.line_start = value == '\n';
            ++parser.input_offset;
            continue;
        }

        // Mid-line plain text: no block rule applies, so a run of bytes no rule
        // starts at is copied with the line's style.
        if (parser.output_mode == OutputMode::transformed && (!parser.line_start || parser.depth != 0)) {
            const u32 begin = parser.input_offset;
            const u32 capacity = minimum(parser.output.count, parser.styles.count);
            const u32 room = parser.output_bytes < capacity ? capacity - parser.output_bytes : 0;
            u32 stop = have_flag ? flag.first_byte : input.bytes;
            stop = stop - begin > room ? begin + room : stop;
            const u32 base = parser.line_style | parser.persistent_style;
            u32 end = begin;
            for (u32 out = parser.output_bytes; end < stop && plain_text_byte(input, end); ++end, ++out) {
                const char byte = input.data[end];
                parser.output.data[out] = byte;
                parser.styles.data[out] =
                    base | (parser.emoji_only && !ascii_space(byte) ? text_style_jumbo_emoji : 0u);
            }
            if (end != begin) {
                parser.output_bytes += end - begin;
                parser.input_offset = end;
                parser.line_start = false;
                continue;
            }
        }

        if (!block_prefix(&parser)) {
            return MessageMarkdownError::output_too_small;
        }
        if (parser.input_offset >= parser.input.bytes) {
            break;
        }
        const char value = parser.input.data[parser.input_offset];
        if (value == '\n') {
            if (!emit_byte(&parser, value)) {
                return MessageMarkdownError::output_too_small;
            }
            ++parser.input_offset;
            parser.line_style = parser.persistent_style;
            parser.line_start = true;
            continue;
        }
        parser.line_start = false;
        if (!options.profile_bio && have_flag && flag.first_byte == parser.input_offset) {
            const u32 display_begin = parser.output_bytes;
            // Suppression is only admitted with its matching semantic slot.
            // The glyph bytes remain true Unicode and keep existing metrics.
            const bool admitted = parser.semantic_count < parser.semantics.count;
            if (!emit_range(&parser, flag.first_byte, flag.end_byte,
                            admitted ? text_style_custom_emoji : 0))
                return MessageMarkdownError::output_too_small;
            if (admitted && !append_semantic(&parser, MessageMarkdownSemanticKind::regional_flag,
                flag.sequence, display_begin, flag.end_byte - flag.first_byte,
                flag.first_byte, flag.end_byte - flag.first_byte))
                return MessageMarkdownError::output_too_small;
            parser.input_offset = flag.end_byte;
            have_flag = regional_flag_next(input, &flag_cursor, &flag);
            continue;
        }
        if (options.profile_bio && starts_with(parser, "```", 3)) {
            if (!emit_range(&parser, parser.input_offset, parser.input_offset + 3))
                return MessageMarkdownError::output_too_small;
            parser.input_offset += 3;
            continue;
        }

        // The emoticon rule: the shrug face is plain text, its "\_" no escape.
        if (static_cast<u8>(value) == 0xC2u && starts_with(parser, kShrugFace, kShrugFaceBytes)) {
            if (!emit_range(&parser, parser.input_offset, parser.input_offset + kShrugFaceBytes))
                return MessageMarkdownError::output_too_small;
            parser.input_offset += kShrugFaceBytes;
            continue;
        }
        if (value == '\\' && parser.input_offset + 1 < parser.input.bytes) {
            const Utf8DecodeResult escaped =
                utf8_decode_one(input.data + parser.input_offset + 1, input.bytes - parser.input_offset - 1);
            if (escaped.valid && escaped.width && markdown_escape_target(escaped.codepoint)) {
                ++parser.input_offset;
                if (!emit_range(&parser, parser.input_offset, parser.input_offset + escaped.width))
                    return MessageMarkdownError::output_too_small;
                parser.input_offset += escaped.width;
                ++parser.transformations;
                continue;
            }
        }
        if (try_close_masked_link(&parser)) {
            continue;
        }
        if (value == '[' && !options.no_links && masked_link_closes(parser)) {
            if (!push_delimiter(&parser, text_style_link, 1, DelimiterKind::masked_link) ||
                !emit_byte(&parser, value)) {
                return MessageMarkdownError::delimiter_capacity;
            }
            ++parser.input_offset;
            continue;
        }

        bool matched = false;
        if (value == 'h' &&
            (!try_emit_attachment_link(&parser, &matched) || (!matched && !try_emit_channel_link(&parser, &matched)))) {
            return MessageMarkdownError::output_too_small;
        }
        if (matched) {
            continue;
        }
        if (value == '<' || value == 'h' || value == 's') {
            if (!try_emit_autolink(&parser, &matched))
                return MessageMarkdownError::output_too_small;
            if (matched)
                continue;
        }

        if (value == ':' && parser.output_mode == OutputMode::transformed && !options.preserve_entities) {
            u32 end = parser.input_offset;
            const auto glyph = emoji_alias_at(input, parser.input_offset, &end);
            if (glyph.bytes && !emoji_alias_inside_url(input, parser.input_offset, end)) {
                const u32 begin = parser.input_offset;
                const u32 display_begin = parser.output_bytes;
                const u64 flag_sequence = regional_flag_sequence(glyph);
                const bool flag_admitted = !options.profile_bio && flag_sequence &&
                                          parser.semantic_count < parser.semantics.count;
                for (u32 byte = 0; byte < glyph.bytes; ++byte)
                    if (!emit_byte(&parser, glyph.data[byte], flag_admitted ? text_style_custom_emoji : 0, begin))
                        return MessageMarkdownError::output_too_small;
                if (flag_admitted && !append_semantic(&parser, MessageMarkdownSemanticKind::regional_flag,
                    flag_sequence, display_begin, glyph.bytes, begin, end - begin))
                    return MessageMarkdownError::output_too_small;
                parser.input_offset = end;
                ++parser.transformations;
                continue;
            }
        }
        if (!try_emit_entity_tag(&parser, &matched)) {
            return MessageMarkdownError::output_too_small;
        }
        if (matched) {
            continue;
        }
        if (!try_emit_named_mention(&parser, &matched)) {
            return MessageMarkdownError::output_too_small;
        }
        if (matched) {
            continue;
        }

        u32 style = 0;
        DelimiterKind kind = DelimiterKind::emphasis;
        const u8 marker = emphasis_marker(parser, &style, &kind);
        if (marker != 0) {
            if (parser.depth != 0) {
                const Delimiter& top = parser.delimiters[parser.depth - 1];
                if (top.kind == kind && top.marker_bytes == marker && top.style == style &&
                    marker_can_close(parser, top)) {
                    const Delimiter closed = parser.delimiters[--parser.depth];
                    close_delimiter(&parser, closed);
                    parser.input_offset += marker;
                    continue;
                }
            }
            if (marker_can_open(parser, marker, kind) && marker_occurs_later(parser, marker)) {
                if (!push_delimiter(&parser, style, marker, kind)) {
                    return MessageMarkdownError::delimiter_capacity;
                }
                if (emit_closed_source_code(&parser)) continue;
                for (u32 index = 0; index < marker; ++index) {
                    if (!emit_byte(&parser, parser.input.data[parser.input_offset + index], 0,
                                   parser.input_offset + index)) {
                        return MessageMarkdownError::output_too_small;
                    }
                }
                parser.input_offset += marker;
                continue;
            }
        }

        if (!emit_byte(&parser, value)) {
            return MessageMarkdownError::output_too_small;
        }
        ++parser.input_offset;
    }
    return MessageMarkdownError::none;
}

bool fenced(Utf8View input) noexcept {
    // Skipping three bytes after a non-backtick cannot step over a triple.
    for (u32 at = 2; at < input.bytes;) {
        if (input.data[at] != '`')
            at += 3;
        else if (input.data[at - 1] == '`' && input.data[at - 2] == '`')
            return true;
        else
            ++at;
    }
    return false;
}

MessageMarkdownResult parse_validated(Utf8View input, Slice<char> text_output, Slice<u32> byte_style_scratch,
                                      Slice<TextStyleSpan> span_output, const MessageMarkdownOptions& options,
                                      Slice<MessageMarkdownSemantic> semantic_output,
                                      Slice<AnsiColorRun> ansi_runs) noexcept {
    Parser parser{};
    parser.input = input;
    parser.output = text_output;
    parser.ansi_runs = ansi_runs;
    parser.styles = byte_style_scratch;
    parser.spans = span_output;
    parser.semantics = semantic_output;
    parser.options = &options;
    parser.line_start = true;
    parser.emoji_only = emoji_only_source(input);
    const auto error = walk(&parser);
    if (error != MessageMarkdownError::none) return parse_error(error);
    return finish(&parser);
}

__declspec(noinline) MessageMarkdownResult parse_fenced(Utf8View input, Slice<char> text_output,
                                                        Slice<u32> byte_style_scratch,
                                                        Slice<TextStyleSpan> span_output,
                                                        const MessageMarkdownOptions& options,
                                                        Slice<MessageMarkdownSemantic> semantic_output) noexcept {
    AnsiColorRun ansi_runs[kMessageMarkdownAnsiRunMaximum];
    return parse_validated(input, text_output, byte_style_scratch, span_output, options, semantic_output,
                           Slice<AnsiColorRun>{ansi_runs, kMessageMarkdownAnsiRunMaximum});
}

} // namespace

MessageMarkdownResult message_markdown_parse(Utf8View input, Slice<char> text_output,
                                             Slice<u32> byte_style_scratch,
                                             Slice<TextStyleSpan> span_output) noexcept {
    const MessageMarkdownOptions options{};
    return message_markdown_parse(input, text_output, byte_style_scratch, span_output, options,
                                  Slice<MessageMarkdownSemantic>{});
}

MessageMarkdownResult message_markdown_parse(Utf8View input, Slice<char> text_output,
                                             Slice<u32> byte_style_scratch,
                                             Slice<TextStyleSpan> span_output,
                                             const MessageMarkdownOptions& options,
                                             Slice<MessageMarkdownSemantic> semantic_output) noexcept {
    if ((!input.data && input.bytes != 0) || (!text_output.data && text_output.count != 0) ||
        (!byte_style_scratch.data && byte_style_scratch.count != 0) ||
        (!span_output.data && span_output.count != 0) ||
        (!semantic_output.data && semantic_output.count != 0) ||
        semantic_output.count > kMessageMarkdownSemanticMaximum) {
        return parse_error(MessageMarkdownError::invalid_input);
    }
    if (input.bytes > text_output.count || input.bytes > byte_style_scratch.count) {
        return parse_error(MessageMarkdownError::output_too_small);
    }
    if (!utf8_validate(input).valid) {
        return parse_error(MessageMarkdownError::invalid_utf8);
    }
    // Only a ``` fence can open a ```ansi block, the one user of the run buffer;
    // other messages parse without its 12 KiB frame.
    return fenced(input) ? parse_fenced(input, text_output, byte_style_scratch, span_output, options, semantic_output)
                         : parse_validated(input, text_output, byte_style_scratch, span_output, options,
                                           semantic_output, Slice<AnsiColorRun>{});
}

u32 message_markdown_source_styles(Utf8View input, Slice<TextStyleSpan> output) noexcept {
    if ((!input.data && input.bytes) || !output.data || !utf8_validate(input).valid) return 0;
    MessageMarkdownOptions options{};
    options.preserve_entities = true;
    options.spoiler_revealed = +[](void*, u32) noexcept { return true; };
    SourceSink sink{};
    sink.spans = output;
    Parser parser{};
    parser.input = input;
    parser.options = &options;
    parser.source_sink = &sink;
    parser.output_mode = OutputMode::source;
    parser.line_start = true;
    parser.emoji_only = emoji_only_source(input);
    if (walk(&parser) != MessageMarkdownError::none || sink.transformed_runs > output.count) return 0;
    return sink.span_count;
}

bool message_markdown_url_valid(Utf8View value) noexcept {
    return value.data && valid_url(value, 0, value.bytes);
}


} // namespace maicord
