#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/code_highlight.hpp"
#include "maicord/ui/ansi_text.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include <stddef.h>


namespace maicord {
namespace {

inline constexpr u32 kDelimiterCapacity = 32;

enum class DelimiterKind : u8 {
    emphasis,
    code,
    masked_link,
    spoiler,
};

enum class OutputMode : u8 { transformed, source, recognition };

struct Delimiter {
    u32 input_offset;
    u32 output_offset;
    u32 style;
    u8 marker_bytes;
    DelimiterKind kind;
    u8 replay_cache_index;
};

// A coloured stretch of a ```ansi block in output coordinates; finish() cuts
// spans at run edges and stores the colours in TextStyleSpan::reserved.
struct AnsiColorRun {
    u32 begin;
    u32 end;
    u16 foreground;
    u16 background;
};

struct SourceClosure {
    u32 close_offset;
    u32 content_begin;
    u32 style;
    CodeLanguage language;
    bool removed_marker;
    bool closed;
};

struct SourceScope {
    SourceClosure closure;
    u32 parent_style;
};

struct SourceCacheEntry {
    u32 input_offset;
    SourceClosure closure;
};

struct SourceSink {
    Slice<TextStyleSpan> spans;
    SourceScope scopes[kDelimiterCapacity];
    SourceCacheEntry cache[kDelimiterCapacity];
    u32 cache_count;
    u32 active_style;
    u32 span_count;
    u32 transformed_runs;
    u32 previous_flags;
    bool have_previous;
    bool saturated;
};

struct Parser {
    Utf8View input;
    Slice<char> output;
    Slice<u32> styles;
    Slice<u32> source_offsets;
    Slice<TextStyleSpan> spans;
    Slice<MessageMarkdownSemantic> semantics;
    Slice<AnsiColorRun> ansi_runs;
    const MessageMarkdownOptions* options;
    u32 ansi_run_count;
    u32 input_offset;
    u32 output_bytes;
    u32 depth;
    u32 transformations;
    u32 syntax_tokens;
    u32 syntax_styled_bytes;
    u32 resolved_entities;
    u32 unresolved_entities;
    u32 timestamps;
    u32 semantic_count;
    u64 next_timestamp_update_unix_seconds;
    u16 list_items;
    u8 code_blocks;
    u32 line_style;
    u32 persistent_style;
    bool line_start;
    bool emoji_only;
    bool syntax_truncated;
    bool semantics_truncated;
    SourceSink* source_sink;
    SourceSink* replay_cache;
    OutputMode output_mode;
    Delimiter delimiters[kDelimiterCapacity];
};

static_assert(is_standard_layout_v<Parser> && is_trivially_copyable_v<Parser>);
static_assert(sizeof(Delimiter) == 16);

MessageMarkdownError walk(Parser* parser, u32 stop_depth = ~0u) noexcept;
bool ascii_space(char value) noexcept;

void source_byte(Parser* parser, u32 offset, char value, u32 flags) noexcept {
    SourceSink& sink = *parser->source_sink;
    flags |= sink.active_style;
    if (parser->emoji_only && !ascii_space(value)) flags |= text_style_jumbo_emoji;
    if (!sink.have_previous || flags != sink.previous_flags) {
        if (flags) ++sink.transformed_runs;
        sink.previous_flags = flags;
        sink.have_previous = true;
    }
    flags &= ~(text_style_jumbo_emoji | text_style_custom_emoji |
               text_style_spoiler | text_style_list_bullet);
    if (!flags || sink.saturated) return;
    if (sink.span_count) {
        auto& last = sink.spans[sink.span_count - 1];
        if (last.flags == flags && last.byte_offset + last.byte_count == offset) {
            ++last.byte_count;
            return;
        }
    }
    if (sink.span_count == sink.spans.count) {
        sink.saturated = true;
        return;
    }
    sink.spans[sink.span_count++] = {offset, 1, flags};
}

MessageMarkdownResult parse_error(MessageMarkdownError error) noexcept {
    MessageMarkdownResult result{};
    result.error = error;
    return result;
}

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
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

bool ascii_identifier(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || ascii_is_digit(value) ||
           value == '_' || value == '-' || value == '+' || value == '.' || value == '#';
}

// JavaScript \s (the escape rule's exclusion), beyond ASCII.
bool unicode_space(u32 cp) noexcept {
    return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

// simple-markdown's escape rule /^\\([^0-9A-Za-z\s])/: a backslash escapes any
// one character that is not ASCII alphanumeric or whitespace ("\!", "\.").
bool markdown_escape_target(u32 cp) noexcept {
    if (cp < 0x80)
        return cp > ' ' && cp != 0x7F && !((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
                                           (cp >= 'A' && cp <= 'Z'));
    return !unicode_space(cp) && cp != 0x85;
}

bool starts_with(const Parser& parser, const char* value, u32 bytes) noexcept {
    if (bytes > parser.input.bytes - parser.input_offset) {
        return false;
    }
    for (u32 index = 0; index < bytes; ++index) {
        if (parser.input.data[parser.input_offset + index] != value[index]) {
            return false;
        }
    }
    return true;
}

bool emit_byte(Parser* parser, char value, u32 added_style = 0, u32 source_offset = ~0u) noexcept {
    if (source_offset == ~0u) source_offset = parser->input_offset;
    if (parser->output_mode != OutputMode::transformed) {
        if (parser->output_mode == OutputMode::source) {
            const auto& top = parser->source_sink->scopes[parser->depth ? parser->depth - 1 : 0].closure;
            const bool opening_marker = parser->depth && top.removed_marker &&
                source_offset >= parser->delimiters[parser->depth - 1].input_offset &&
                source_offset < parser->delimiters[parser->depth - 1].input_offset +
                                    parser->delimiters[parser->depth - 1].marker_bytes;
            if (!opening_marker) source_byte(parser, source_offset, value,
                parser->line_style | parser->persistent_style | added_style);
        }
        ++parser->output_bytes;
        return true;
    }
    if (parser->output_bytes >= parser->output.count || parser->output_bytes >= parser->styles.count) {
        return false;
    }
    parser->output[parser->output_bytes] = value;
    if (parser->source_offsets.data)
        parser->source_offsets[parser->output_bytes] = source_offset;
    const u32 jumbo = parser->emoji_only && !ascii_space(value) ? text_style_jumbo_emoji : 0u;
    parser->styles[parser->output_bytes] =
        parser->line_style | parser->persistent_style | added_style | jumbo;
    ++parser->output_bytes;
    return true;
}

bool emit_range(Parser* parser, u32 begin, u32 end, u32 added_style = 0) noexcept {
    if (begin > end || end > parser->input.bytes) {
        return false;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        if (!emit_byte(parser, parser->input.data[offset], added_style, offset)) {
            return false;
        }
        if (parser->source_offsets.data)
            parser->source_offsets[parser->output_bytes - 1] = offset;
    }
    return true;
}

void remove_output(Parser* parser, u32 offset, u32 bytes) noexcept {
    if (bytes == 0 || offset > parser->output_bytes || bytes > parser->output_bytes - offset) {
        return;
    }
    const u32 remaining = parser->output_bytes - offset - bytes;
    if (parser->output_mode != OutputMode::transformed) {
        parser->output_bytes -= bytes;
        return;
    }
    for (u32 index = 0; index < remaining; ++index) {
        parser->output[offset + index] = parser->output[offset + bytes + index];
        parser->styles[offset + index] = parser->styles[offset + bytes + index];
        if (parser->source_offsets.data)
            parser->source_offsets[offset + index] = parser->source_offsets[offset + bytes + index];
    }
    parser->output_bytes -= bytes;
    for (u32 index = 0; index < parser->ansi_run_count; ++index) {
        AnsiColorRun& run = parser->ansi_runs[index];
        const auto shifted = [&](u32 position) noexcept {
            return position < offset ? position : position < offset + bytes ? offset : position - bytes;
        };
        run.begin = shifted(run.begin);
        run.end = shifted(run.end);
    }
    for (u32 index = 0; index < parser->semantic_count; ++index) {
        MessageMarkdownSemantic& semantic = parser->semantics[index];
        if (semantic.byte_offset >= offset + bytes) {
            semantic.byte_offset -= bytes;
        }
    }
}

bool append_semantic(Parser* parser, MessageMarkdownSemanticKind kind, u64 target, u32 byte_offset,
                     u32 byte_count, u32 source_offset, u32 source_bytes, u8 reserved = 0) noexcept {
    if (byte_count == 0 || byte_offset > parser->output_bytes ||
        byte_count > parser->output_bytes - byte_offset || source_offset > parser->input.bytes ||
        source_bytes > parser->input.bytes - source_offset || source_bytes > 0xFFFFu) {
        return false;
    }
    if (parser->semantic_count >= parser->semantics.count) {
        parser->semantics_truncated = true;
        return true;
    }
    parser->semantics[parser->semantic_count++] = MessageMarkdownSemantic{
        target, byte_offset, byte_count, source_offset, static_cast<u16>(source_bytes), kind, reserved};
    return true;
}

bool semantic_hovered(const Parser& parser, MessageMarkdownSemanticKind kind, u32 source_offset) noexcept {
    return parser.options && parser.options->has_hover && parser.options->hovered_kind == kind &&
           parser.options->hovered_source_offset == source_offset;
}

void apply_style(Parser* parser, u32 begin, u32 end, u32 style) noexcept {
    if (begin > end || end > parser->output_bytes) {
        return;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        parser->styles[offset] |= style;
    }
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

bool valid_code_language(const Parser& parser, u32 begin, u32 end) noexcept {
    if (begin >= end || end - begin > 32) {
        return false;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        const char value = parser.output[offset];
        if (value == '\r' && offset + 1 == end) {
            continue;
        }
        if (!ascii_identifier(value)) {
            return false;
        }
    }
    return true;
}

struct AnsiCompaction {
    Parser* parser;
    u32 base;
    u32 write;
};

bool compact_ansi_piece(void* context, u32 begin, u32 end, AnsiTextStyle style) noexcept {
    auto& compaction = *static_cast<AnsiCompaction*>(context);
    Parser* parser = compaction.parser;
    const u32 from = compaction.base + begin, to = compaction.write, bytes = end - begin;
    for (u32 i = 0; i < bytes; ++i) {
        parser->output[to + i] = parser->output[from + i];
        parser->styles[to + i] = parser->styles[from + i] | style.flags;
        if (parser->source_offsets.data)
            parser->source_offsets[to + i] = parser->source_offsets[from + i];
    }
    compaction.write += bytes;
    if (style.foreground == kAnsiColorNone && style.background == kAnsiColorNone)
        return true;
    if (parser->ansi_run_count) {
        AnsiColorRun& last = parser->ansi_runs[parser->ansi_run_count - 1];
        if (last.end == to && last.foreground == style.foreground && last.background == style.background) {
            last.end = to + bytes;
            return true;
        }
    }
    if (parser->ansi_run_count == parser->ansi_runs.count) {
        parser->syntax_truncated = true;
        return true;
    }
    parser->ansi_runs[parser->ansi_run_count++] =
        AnsiColorRun{to, to + bytes, style.foreground, style.background};
    return true;
}

// ANSI-to-HTML pass over a closed ```ansi block: escapes and hidden text
// leave the display text, SGR styles join the byte styles and colours become
// runs. The block is the output tail, so nothing follows it.
void transform_ansi_block(Parser* parser, u32 begin) noexcept {
    const u32 end = parser->output_bytes;
    AnsiCompaction compaction{parser, begin, begin};
    ansi_text_scan(Utf8View{parser->output.data + begin, end - begin}, compact_ansi_piece, &compaction);
    if (compaction.write != end) {
        parser->output_bytes = compaction.write;
        ++parser->transformations;
    }
}

// A reply preview removes /\x1B\[(\d+(?:[:;]\d+)*)m/g from a ```ansi block
// (the tail of the output) and shows the remaining text uncoloured.
u32 ansi_sgr_bytes(const char* text, u32 bytes) noexcept {
    if (bytes < 4 || text[0] != '\x1B' || text[1] != '[' || !ascii_is_digit(text[2]))
        return 0;
    u32 at = 2;
    while (true) {
        while (at < bytes && ascii_is_digit(text[at]))
            ++at;
        if (at < bytes && text[at] == 'm')
            return at + 1;
        if (at + 1 < bytes && (text[at] == ';' || text[at] == ':') && ascii_is_digit(text[at + 1])) {
            ++at;
            continue;
        }
        return 0;
    }
}
void strip_ansi_sgr(Parser* parser, u32 begin) noexcept {
    u32 write = begin;
    for (u32 read = begin; read < parser->output_bytes;) {
        const u32 escape = ansi_sgr_bytes(parser->output.data + read, parser->output_bytes - read);
        if (escape) {
            read += escape;
            continue;
        }
        parser->output[write] = parser->output[read];
        parser->styles[write] = parser->styles[read];
        if (parser->source_offsets.data)
            parser->source_offsets[write] = parser->source_offsets[read];
        ++write;
        ++read;
    }
    parser->output_bytes = write;
}

void close_delimiter(Parser* parser, const Delimiter& delimiter) noexcept {
    if (parser->output_mode != OutputMode::transformed) {
        SourceClosure closure{};
        closure.closed = true;
        closure.close_offset = parser->input_offset;
        closure.content_begin = delimiter.input_offset + delimiter.marker_bytes;
        if (delimiter.kind == DelimiterKind::code && delimiter.marker_bytes == 3) {
            u32 newline = closure.content_begin;
            while (newline < parser->input_offset && parser->input.data[newline] != '\n') ++newline;
            bool valid = newline > closure.content_begin && newline - closure.content_begin <= 32;
            for (u32 offset = closure.content_begin; valid && offset < newline; ++offset) {
                const char value = parser->input.data[offset];
                valid = ascii_identifier(value) || (value == '\r' && offset + 1 == newline);
            }
            if (newline < parser->input_offset && valid) {
                closure.language = code_language_from_tag({parser->input.data + closure.content_begin,
                    newline - closure.content_begin});
                parser->output_bytes -= newline + 1 - closure.content_begin;
                closure.content_begin = newline + 1;
            }
            u32 code = closure.content_begin;
            while (code < parser->input_offset && parser->input.data[code] == '\n') ++code;
            if (code < parser->input_offset) {
                parser->output_bytes -= code - closure.content_begin;
                closure.content_begin = code;
            }
        }
        if (parser->output_bytes > delimiter.output_offset + delimiter.marker_bytes) {
            parser->output_bytes -= delimiter.marker_bytes;
            closure.removed_marker = true;
            closure.style = delimiter.style & ~text_style_spoiler;
        }
        if (parser->replay_cache && delimiter.replay_cache_index != 0xFF)
            parser->replay_cache->cache[delimiter.replay_cache_index].closure = closure;
        if (parser->output_mode == OutputMode::source)
            parser->source_sink->active_style = parser->source_sink->scopes[parser->depth].parent_style;
        return;
    }
    u32 content_begin = delimiter.output_offset + delimiter.marker_bytes;
    CodeLanguage language = CodeLanguage::none;
    if (delimiter.kind == DelimiterKind::code && delimiter.marker_bytes == 3) {
        u32 newline = content_begin;
        while (newline < parser->output_bytes && parser->output[newline] != '\n') {
            ++newline;
        }
        if (newline < parser->output_bytes && valid_code_language(*parser, content_begin, newline)) {
            language = code_language_from_tag(
                Utf8View{parser->output.data + content_begin, newline - content_begin});
            remove_output(parser, content_begin, newline - content_begin + 1);
            ++parser->transformations;
        }
        // Blank lines after the fence go when code follows them.
        u32 lead = 0;
        while (content_begin + lead < parser->output_bytes && parser->output[content_begin + lead] == '\n')
            ++lead;
        if (lead != 0 && content_begin + lead < parser->output_bytes) {
            remove_output(parser, content_begin, lead);
            ++parser->transformations;
        }
    }
    content_begin = delimiter.output_offset + delimiter.marker_bytes;
    const u32 content_bytes = parser->output_bytes - content_begin;
    if (content_bytes == 0) {
        return;
    }
    remove_output(parser, delimiter.output_offset, delimiter.marker_bytes);
    u32 applied_style = delimiter.style;
    const u32 source_bytes = parser->input_offset + delimiter.marker_bytes - delimiter.input_offset;
    const bool reply_fence = delimiter.kind == DelimiterKind::code && delimiter.marker_bytes == 3 &&
                             parser->options && parser->options->inline_reply;
    if (reply_fence) {
        // Reply previews match /^```(?:lang\n)?\n*([^\n][^]*?)\n*```/ and render
        // group 2 as inline code; ```ansi first drops /\x1B\[\d+([:;]\d+)*m/.
        u32 lead = 0;
        while (delimiter.output_offset + lead < parser->output_bytes &&
               parser->output[delimiter.output_offset + lead] == '\n')
            ++lead;
        remove_output(parser, delimiter.output_offset, lead);
        while (parser->output_bytes > delimiter.output_offset && parser->output[parser->output_bytes - 1] == '\n')
            --parser->output_bytes;
        if (language == CodeLanguage::ansi)
            strip_ansi_sgr(parser, delimiter.output_offset);
        apply_style(parser, delimiter.output_offset, parser->output_bytes, text_style_monospace);
        ++parser->transformations;
        return;
    }
    if (delimiter.kind == DelimiterKind::spoiler) {
        const bool revealed =
            parser->options && parser->options->spoiler_revealed &&
            parser->options->spoiler_revealed(parser->options->context, delimiter.input_offset);
        if (revealed) {
            applied_style &= ~text_style_spoiler;
        }
        if (semantic_hovered(*parser, MessageMarkdownSemanticKind::spoiler, delimiter.input_offset)) {
            applied_style |= text_style_interactive_hover;
        }
    }
    apply_style(parser, delimiter.output_offset, parser->output_bytes, applied_style);
    if (delimiter.kind == DelimiterKind::spoiler) {
        append_semantic(parser, MessageMarkdownSemanticKind::spoiler, 0, delimiter.output_offset,
                        parser->output_bytes - delimiter.output_offset, delimiter.input_offset,
                        source_bytes);
    }
    if (delimiter.kind == DelimiterKind::code && delimiter.marker_bytes == 3) {
        if (parser->code_blocks != 0xFF) {
            ++parser->code_blocks;
        }
        // Editors (preserve_entities) keep the raw escapes.
        if (language == CodeLanguage::ansi && !(parser->options && parser->options->preserve_entities)) {
            transform_ansi_block(parser, delimiter.output_offset);
        } else if (language != CodeLanguage::none) {
            const u32 remaining = parser->syntax_tokens < kCodeHighlightTokenMaximum
                                      ? kCodeHighlightTokenMaximum - parser->syntax_tokens
                                      : 0;
            if (remaining == 0) {
                parser->syntax_truncated = true;
            } else {
                const u32 code_bytes = parser->output_bytes - delimiter.output_offset;
                const CodeHighlightResult highlighted = code_highlight(
                    Utf8View{parser->output.data + delimiter.output_offset, code_bytes}, language,
                    Slice<u32>{parser->styles.data + delimiter.output_offset, code_bytes}, remaining);
                parser->syntax_tokens += highlighted.token_count;
                parser->syntax_styled_bytes += highlighted.styled_bytes;
                parser->syntax_truncated = parser->syntax_truncated || highlighted.truncated;
            }
        }
    }
    ++parser->transformations;
}

bool validate_utf8(Utf8View input) noexcept {
    u32 offset = 0;
    while (offset < input.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(input.data + offset, input.bytes - offset);
        if (!decoded.valid || decoded.width == 0) {
            return false;
        }
        offset += decoded.width;
    }
    return true;
}

bool valid_url(Utf8View input, u32 begin, u32 end) noexcept {
    if (begin >= end || end - begin > 2'048) {
        return false;
    }
    constexpr const char* schemes[] = {"https://", "http://", "discord://"};
    static constexpr u32 lengths[] = {8, 7, 10};
    bool scheme = false;
    for (u32 item = 0; item < 3; ++item) {
        if (lengths[item] <= end - begin) {
            bool equal = true;
            for (u32 index = 0; index < lengths[item]; ++index) {
                equal = equal && input.data[begin + index] == schemes[item][index];
            }
            scheme = scheme || equal;
        }
    }
    if (!scheme) {
        return false;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        if (ascii_space(input.data[offset]) || input.data[offset] == '<' || input.data[offset] == '>') {
            return false;
        }
    }
    return true;
}

// simple-markdown's masked-link target: "(" optional spaces, an optional "<",
// a URL without whitespace in which "\x" escapes and one level of balanced
// parentheses are allowed, an optional ">", an optional quoted title, spaces
// and ")". `open` indexes the "(".
bool masked_link_target(Utf8View input, u32 open, u32* url_begin, u32* url_end, u32* close) noexcept {
    const u32 limit = input.bytes - open > 4'096 ? open + 4'096 : input.bytes;
    const auto blank = [&](u32 at) noexcept { return input.data[at] == ' ' || input.data[at] == '\t'; };
    u32 at = open + 1;
    while (at < limit && blank(at))
        ++at;
    const bool angle = at < limit && input.data[at] == '<';
    at += angle ? 1u : 0u;
    const u32 begin = at;
    while (at < limit) {
        const char value = input.data[at];
        if (value == '\\' && at + 1 < limit && !ascii_space(input.data[at + 1])) {
            at += 2;
            continue;
        }
        if (value == '(') {
            u32 inner = at + 1;
            while (inner < limit && input.data[inner] != ')' && input.data[inner] != '(' &&
                   !ascii_space(input.data[inner]))
                ++inner;
            if (inner >= limit || input.data[inner] != ')')
                return false;
            at = inner + 1;
            continue;
        }
        if (value == ')' || ascii_space(value) || (angle && value == '>'))
            break;
        ++at;
    }
    const u32 end = at;
    if (angle && at < limit && input.data[at] == '>')
        ++at;
    u32 cursor = at;
    while (cursor < limit && blank(cursor))
        ++cursor;
    if (cursor > at && cursor < limit && (input.data[cursor] == '"' || input.data[cursor] == '\'')) {
        const char quote = input.data[cursor];
        u32 title = cursor + 1;
        while (title < limit && input.data[title] != quote)
            ++title;
        if (title >= limit)
            return false;
        cursor = title + 1;
        while (cursor < limit && blank(cursor))
            ++cursor;
    }
    if (cursor >= limit || input.data[cursor] != ')' || !valid_url(input, begin, end))
        return false;
    *url_begin = begin;
    *url_end = end;
    *close = cursor;
    return true;
}

// A "[" opens a masked link only when a "](target)" completes it; otherwise
// it is literal text and must not hold the delimiter stack (which would also
// suppress every later block rule and closing marker).
bool masked_link_closes(const Parser& parser) noexcept {
    const Utf8View input = parser.input;
    u32 nesting = 0;
    for (u32 at = parser.input_offset + 1; at < input.bytes && at - parser.input_offset < 4'096; ++at) {
        const char value = input.data[at];
        if (value == '\\') {
            ++at;
            continue;
        }
        if (value == '[') {
            if (++nesting > 10)
                return false;
            continue;
        }
        if (value != ']')
            continue;
        if (nesting) {
            --nesting;
            continue;
        }
        u32 begin = 0, end = 0, close = 0;
        return at > parser.input_offset + 1 && at + 1 < input.bytes && input.data[at + 1] == '(' &&
               masked_link_target(input, at + 1, &begin, &end, &close);
    }
    return false;
}

bool try_close_masked_link(Parser* parser) noexcept {
    if (parser->depth == 0 || parser->input_offset + 2 > parser->input.bytes ||
        parser->input.data[parser->input_offset] != ']' ||
        parser->input.data[parser->input_offset + 1] != '(') {
        return false;
    }
    u32 link = parser->depth;
    while (link != 0 && parser->delimiters[link - 1].kind == DelimiterKind::emphasis)
        --link;
    if (link == 0 || parser->delimiters[link - 1].kind != DelimiterKind::masked_link)
        return false;
    u32 url_begin = 0, url_end = 0, close = 0;
    if (!masked_link_target(parser->input, parser->input_offset + 1, &url_begin, &url_end, &close))
        return false;
    const Delimiter delimiter = parser->delimiters[link - 1];
    if (parser->output_bytes <= delimiter.output_offset + delimiter.marker_bytes) {
        return false;
    }
    // Emphasis opened inside the link text and never closed stays literal.
    parser->depth = link - 1;
    close_delimiter(parser, delimiter);
    const u32 display_bytes = parser->output_bytes - delimiter.output_offset;
    if (semantic_hovered(*parser, MessageMarkdownSemanticKind::link, url_begin)) {
        apply_style(parser, delimiter.output_offset, parser->output_bytes,
                    text_style_interactive_hover | text_style_underline);
    }
    append_semantic(parser, MessageMarkdownSemanticKind::link, 0, delimiter.output_offset, display_bytes,
                    url_begin, url_end - url_begin);
    parser->input_offset = close + 1;
    return true;
}

// The markdown url rule /^((?:https?|steam):\/\/[^\s<]+[^<.,:;"'\]\s])/ with its
// trailing-")" balancing. Returns the URL's byte length, or 0.
u32 autolink_bytes(Utf8View input, u32 begin) noexcept {
    constexpr const char* schemes[] = {"https://", "http://", "steam://"};
    static constexpr u32 lengths[] = {8, 7, 8};
    u32 scheme = 0;
    for (u32 item = 0; item < 3 && !scheme; ++item)
        if (lengths[item] <= input.bytes - begin && bytes_equal(input.data + begin, schemes[item], lengths[item]))
            scheme = lengths[item];
    if (!scheme)
        return 0;
    u32 end = begin + scheme;
    while (end < input.bytes && end - begin < 4'096) {
        const char value = input.data[end];
        if (ascii_space(value) || value == '<')
            break;
        if (static_cast<u8>(value) >= 0x80) {
            const Utf8DecodeResult decoded = utf8_decode_one(input.data + end, input.bytes - end);
            if (!decoded.valid || !decoded.width || unicode_space(decoded.codepoint))
                break;
            end += decoded.width;
            continue;
        }
        ++end;
    }
    const auto trailing = [](char value) noexcept {
        return value == '.' || value == ',' || value == ':' || value == ';' || value == '"' || value == '\'' ||
               value == ']';
    };
    while (end > begin + scheme && trailing(input.data[end - 1]))
        --end;
    if (end < begin + scheme + 2)
        return 0;
    if (input.data[end - 1] == ')') {
        u32 opens = 0, closes = 0;
        for (u32 at = begin; at < end; ++at) {
            opens += input.data[at] == '(' ? 1u : 0u;
            closes += input.data[at] == ')' ? 1u : 0u;
        }
        if (closes > opens)
            --end;
    }
    u32 host = begin + scheme;
    while (host < end && input.data[host] != '/' && input.data[host] != '?' && input.data[host] != '#')
        ++host;
    return host > begin + scheme ? end - begin : 0;
}

// Bare URLs and "<url>" (the autolink rule /^<([^: >]+:\/[^ >]+)>/) become
// link semantics whose source is the URL itself; the URL bytes are emitted
// verbatim so "_", "*" or ":" inside them never start formatting or emoji.
bool try_emit_autolink(Parser* parser, bool* matched) noexcept {
    *matched = false;
    if (parser->options && parser->options->no_links)
        return true;
    if (parser->depth != 0 && parser->delimiters[parser->depth - 1].kind == DelimiterKind::masked_link)
        return true;
    const Utf8View input = parser->input;
    const u32 at = parser->input_offset;
    u32 begin = at, bytes = 0, consumed = 0;
    if (input.data[at] == '<') {
        u32 end = at + 1;
        while (end < input.bytes && end - at < 4'096 && input.data[end] != ' ' && input.data[end] != '>' &&
               input.data[end] != '\n')
            ++end;
        if (end >= input.bytes || input.data[end] != '>')
            return true;
        u32 colon = at + 1;
        while (colon < end && input.data[colon] != ':')
            ++colon;
        if (colon == at + 1 || colon + 1 >= end || input.data[colon + 1] != '/' ||
            !valid_url(input, at + 1, end))
            return true;
        begin = at + 1;
        bytes = end - begin;
        consumed = end + 1 - at;
    } else {
        bytes = autolink_bytes(input, at);
        // Inside open emphasis the closing marker ends the URL ("**url**").
        if (bytes && parser->depth != 0) {
            const Delimiter& top = parser->delimiters[parser->depth - 1];
            for (u32 offset = at + 1; offset + top.marker_bytes <= at + bytes; ++offset) {
                if (bytes_equal(input.data + offset, input.data + top.input_offset, top.marker_bytes)) {
                    bytes = offset - at;
                    break;
                }
            }
            if (bytes < 10)
                bytes = 0;
        }
        consumed = bytes;
    }
    if (!bytes)
        return true;
    const u32 display_begin = parser->output_bytes;
    u32 style = text_style_link;
    if (semantic_hovered(*parser, MessageMarkdownSemanticKind::link, begin))
        style |= text_style_interactive_hover | text_style_underline;
    if (!emit_range(parser, begin, begin + bytes, style) ||
        !append_semantic(parser, MessageMarkdownSemanticKind::link, 0, display_begin, bytes, begin, bytes))
        return false;
    parser->input_offset = at + consumed;
    if (consumed != bytes)
        ++parser->transformations;
    *matched = true;
    return true;
}

// An opener without any later marker that could close it never closes (a
// single "*" and "_" both close italics).
bool marker_occurs_later(const Parser& parser, u8 marker) noexcept {
    const Utf8View input = parser.input;
    const char first = input.data[parser.input_offset];
    const bool italic = marker == 1 && (first == '*' || first == '_');
    for (u32 at = parser.input_offset + marker; at + marker <= input.bytes; ++at)
        if (italic ? input.data[at] == '*' || input.data[at] == '_'
                   : bytes_equal(input.data + at, input.data + parser.input_offset, marker))
            return true;
    return false;
}

bool decimal_range(Utf8View input, u32 begin, u32 end) noexcept {
    if (begin >= end || end - begin > 20) {
        return false;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        if (!ascii_is_digit(input.data[offset])) {
            return false;
        }
    }
    return true;
}

bool emit_view(Parser* parser, Utf8View value, u32 style) noexcept;

bool decimal_u64(Utf8View input, u32 begin, u32 end, u64* value) noexcept {
    return begin < end && decimal_parse({input.data + begin, end - begin}, value);
}

bool message_link_at(Utf8View input, u32 begin, MessageLink* link) noexcept {
    // Each of these hosts routes /channels/<guild|@me>/<channel>/<message>.
    constexpr const char* prefixes[] = {"https://discord.com/channels/", "https://ptb.discord.com/channels/",
                                        "https://canary.discord.com/channels/",
                                        "https://discordapp.com/channels/"};
    if (begin > input.bytes) {
        return false;
    }
    u32 prefix_bytes = 0;
    for (const char* prefix : prefixes) {
        u32 bytes = 0;
        while (prefix[bytes])
            ++bytes;
        if (bytes <= input.bytes - begin && bytes_equal(input.data + begin, prefix, bytes)) {
            prefix_bytes = bytes;
            break;
        }
    }
    if (!prefix_bytes) {
        return false;
    }
    u32 cursor = begin + prefix_bytes;
    const u32 guild_begin = cursor;
    u64 guild_id = 0;
    if (input.bytes - cursor >= 3 && bytes_equal(input.data + cursor, "@me", 3)) {
        cursor += 3;
    } else {
        while (cursor < input.bytes && ascii_is_digit(input.data[cursor])) {
            ++cursor;
        }
        if (!decimal_u64(input, guild_begin, cursor, &guild_id))
            return false;
    }
    if (cursor >= input.bytes || input.data[cursor++] != '/') {
        return false;
    }
    const u32 channel_begin = cursor;
    while (cursor < input.bytes && ascii_is_digit(input.data[cursor])) {
        ++cursor;
    }
    u64 channel_id = 0;
    if (!decimal_u64(input, channel_begin, cursor, &channel_id) || cursor >= input.bytes ||
        input.data[cursor++] != '/') {
        return false;
    }
    const u32 message_begin = cursor;
    while (cursor < input.bytes && ascii_is_digit(input.data[cursor])) {
        ++cursor;
    }
    u64 message_id = 0;
    if (!decimal_u64(input, message_begin, cursor, &message_id)) {
        return false;
    }
    if (cursor < input.bytes) {
        const char next = input.data[cursor];
        // Sentence punctuation terminates a permalink. Only characters that
        // could extend a path/identifier make this candidate invalid.
        if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || ascii_is_digit(next) ||
            next == '_' || next == '-' || next == '/') {
            return false;
        }
    }
    *link = MessageLink{guild_id, channel_id, message_id, cursor - begin};
    return true;
}

bool emit_decimal(Parser* parser, u64 value, u32 style) noexcept {
    char digits[20];
    const u32 count = decimal_write(digits, value);
    for (u32 index = 0; index < count; ++index) {
        if (!emit_byte(parser, digits[index], style)) {
            return false;
        }
    }
    return true;
}

bool try_emit_message_link(Parser* parser, bool* matched) noexcept {
    *matched = false;
    MessageLink link{};
    const u32 source_begin = parser->input_offset;
    if (!message_link_at(parser->input, source_begin, &link)) {
        return true;
    }
    if (parser->options && parser->options->preserve_entities) {
        *matched = true;
        parser->input_offset += link.source_bytes;
        return emit_range(parser, source_begin, parser->input_offset, text_style_link);
    }
    const u32 display_begin = parser->output_bytes;
    u32 channel_style = text_style_mention | text_style_link;
    u32 suffix_style = text_style_link;
    if (semantic_hovered(*parser, MessageMarkdownSemanticKind::message_link, source_begin)) {
        channel_style |= text_style_interactive_hover;
        suffix_style |= text_style_interactive_hover | text_style_underline;
    }
    Utf8View channel_name{};
    if (parser->options && parser->options->resolve_entity) {
        channel_name = parser->options->resolve_entity(parser->options->context,
                                                       MessageMarkdownEntityKind::channel, link.channel_id);
        if ((!channel_name.data && channel_name.bytes != 0) ||
            (channel_name.bytes != 0 && !utf8_validate(channel_name).valid)) {
            reset_value<Utf8View>(&channel_name);
        }
    }
    if (!emit_byte(parser, '#', channel_style) ||
        !(channel_name.bytes != 0 ? emit_view(parser, channel_name, channel_style)
                                  : emit_decimal(parser, link.channel_id, channel_style)) ||
        !emit_view(parser, utf8_literal(" \xE2\x80\xBA \xF0\x9F\x92\xAC"), suffix_style)) {
        return false;
    }
    if (!append_semantic(parser, MessageMarkdownSemanticKind::message_link, link.message_id, display_begin,
                         parser->output_bytes - display_begin, source_begin, link.source_bytes)) {
        return false;
    }
    parser->input_offset += link.source_bytes;
    ++parser->transformations;
    *matched = true;
    return true;
}

bool output_available(const Parser& parser, u32 bytes) noexcept {
    return parser.output_bytes <= parser.output.count && parser.output_bytes <= parser.styles.count &&
           bytes <= parser.output.count - parser.output_bytes &&
           bytes <= parser.styles.count - parser.output_bytes;
}

bool emit_view(Parser* parser, Utf8View value, u32 style) noexcept {
    if ((!value.data && value.bytes != 0) || !output_available(*parser, value.bytes)) {
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        if (!emit_byte(parser, value.data[index], style)) {
            return false;
        }
    }
    return true;
}

bool emit_entity_tag(Parser* parser, MessageMarkdownEntityKind entity_kind,
                     MessageMarkdownSemanticKind semantic_kind, u32 tag_begin, u32 digits_begin,
                     u32 digits_end, char sigil, u32 tag_end) noexcept {
    u64 id = 0;
    if (!decimal_u64(parser->input, digits_begin, digits_end, &id) || id == 0) {
        return false;
    }
    if (parser->options && parser->options->preserve_entities) {
        parser->input_offset = tag_end + 1;
        return emit_range(parser, tag_begin, tag_end + 1, text_style_mention);
    }
    const u32 display_begin = parser->output_bytes;
    u32 style = text_style_mention;
    if (semantic_hovered(*parser, semantic_kind, tag_begin)) {
        style |= text_style_interactive_hover;
    }
    Utf8View resolved{};
    if (parser->options && parser->options->resolve_entity) {
        resolved = parser->options->resolve_entity(parser->options->context, entity_kind, id);
        if ((!resolved.data && resolved.bytes != 0) ||
            (resolved.bytes != 0 && !utf8_validate(resolved).valid)) {
            reset_value<Utf8View>(&resolved);
        }
    }
    const bool use_resolved = resolved.bytes != 0 && output_available(*parser, resolved.bytes + 1);
    if (!emit_byte(parser, sigil, style) ||
        !(use_resolved ? emit_view(parser, resolved, style)
                       : emit_range(parser, digits_begin, digits_end, style))) {
        return false;
    }
    if (use_resolved) {
        saturating_increment(parser->resolved_entities);
    } else {
        saturating_increment(parser->unresolved_entities);
    }
    append_semantic(parser, semantic_kind, id, display_begin, parser->output_bytes - display_begin,
                    tag_begin, tag_end + 1 - tag_begin);
    parser->input_offset = tag_end + 1;
    ++parser->transformations;
    return true;
}

bool try_emit_timestamp(Parser* parser, u32 begin, u32 end, bool* matched) noexcept {
    *matched = false;
    if (begin + 3 >= end || parser->input.data[begin + 1] != 't' || parser->input.data[begin + 2] != ':' ||
        !parser->options || !parser->options->format_timestamp) {
        return true;
    }
    u32 digits_end = begin + 3;
    while (digits_end < end && ascii_is_digit(parser->input.data[digits_end])) {
        ++digits_end;
    }
    char style_character = 'f';
    if (digits_end < end) {
        if (digits_end + 2 != end || parser->input.data[digits_end] != ':') {
            return true;
        }
        style_character = parser->input.data[digits_end + 1];
        const bool valid_style = style_character == 't' || style_character == 'T' ||
                                 style_character == 'd' || style_character == 'D' ||
                                 style_character == 'f' || style_character == 'F' ||
                                 style_character == 's' || style_character == 'S' || style_character == 'R';
        if (!valid_style) {
            return true;
        }
    }
    u64 seconds = 0;
    if (!decimal_u64(parser->input, begin + 3, digits_end, &seconds)) {
        return true;
    }
    char formatted[kMessageMarkdownTimestampMaximumBytes]{};
    const TimestampFormatResult result =
        parser->options->format_timestamp(parser->options->context, seconds, style_character,
                                          Slice<char>{formatted, kMessageMarkdownTimestampMaximumBytes});
    if (!result.ok || result.bytes == 0 || result.bytes > sizeof(formatted) ||
        !utf8_validate(Utf8View{formatted, result.bytes}).valid) {
        return true;
    }
    u32 style = text_style_timestamp | text_style_link;
    if (semantic_hovered(*parser, MessageMarkdownSemanticKind::timestamp, begin)) {
        style |= text_style_interactive_hover | text_style_underline;
    }
    const u32 display_begin = parser->output_bytes;
    if (!emit_view(parser, Utf8View{formatted, result.bytes}, style)) {
        return false;
    }
    append_semantic(parser, MessageMarkdownSemanticKind::timestamp, seconds, display_begin, result.bytes,
                    begin, end + 1 - begin);
    saturating_increment(parser->timestamps);
    if (result.next_update_unix_seconds != 0 &&
        (parser->next_timestamp_update_unix_seconds == 0 ||
         result.next_update_unix_seconds < parser->next_timestamp_update_unix_seconds)) {
        parser->next_timestamp_update_unix_seconds = result.next_update_unix_seconds;
    }
    parser->input_offset = end + 1;
    ++parser->transformations;
    *matched = true;
    return true;
}

// A command mention name: one to three space-separated words of
// 1-32 letters, numbers, '-' or '_'. Non-ASCII code points stand in for the
// Unicode letter/number classes.
bool command_mention_name(Utf8View input, u32 begin, u32 end) noexcept {
    u32 words = 0, length = 0;
    for (u32 index = begin; index < end; ++index) {
        const u8 value = static_cast<u8>(input.data[index]);
        if (value == ' ') {
            if (length == 0 || ++words == 3)
                return false;
            length = 0;
            continue;
        }
        const bool word = ascii_is_digit(static_cast<char>(value)) || (value >= 'a' && value <= 'z') ||
                          (value >= 'A' && value <= 'Z') || value == '-' || value == '_' || value >= 0x80u;
        if (!word)
            return false;
        // Continuation bytes do not start a new code point.
        if ((value & 0xC0u) != 0x80u && ++length > 32)
            return false;
    }
    return length != 0;
}

bool try_emit_entity_tag(Parser* parser, bool* matched) noexcept {
    *matched = false;
    if (parser->options && parser->options->profile_bio && starts_with(*parser, "<@", 2))
        return true;
    if (parser->input.data[parser->input_offset] != '<') {
        return true;
    }
    u32 end = parser->input_offset + 1;
    while (end < parser->input.bytes && end - parser->input_offset <= 128 &&
           parser->input.data[end] != '>' && parser->input.data[end] != '\n' &&
           parser->input.data[end] != '\r') {
        ++end;
    }
    if (end >= parser->input.bytes || parser->input.data[end] != '>') {
        return true;
    }
    const u32 begin = parser->input_offset;
    if (!try_emit_timestamp(parser, begin, end, matched)) {
        return false;
    }
    if (*matched) {
        return true;
    }
    if (begin + 2 < end && parser->input.data[begin + 1] == '@') {
        u32 digits = begin + 2;
        MessageMarkdownEntityKind entity_kind = MessageMarkdownEntityKind::user;
        MessageMarkdownSemanticKind semantic_kind = MessageMarkdownSemanticKind::user_mention;
        if (parser->input.data[digits] == '!') {
            ++digits;
        } else if (parser->input.data[digits] == '&') {
            ++digits;
            entity_kind = MessageMarkdownEntityKind::role;
            semantic_kind = MessageMarkdownSemanticKind::role_mention;
        }
        if (decimal_range(parser->input, digits, end)) {
            *matched = true;
            return emit_entity_tag(parser, entity_kind, semantic_kind, begin, digits, end, '@', end);
        }
    }
    if (begin + 2 < end && parser->input.data[begin + 1] == '#' &&
        decimal_range(parser->input, begin + 2, end)) {
        *matched = true;
        return emit_entity_tag(parser, MessageMarkdownEntityKind::channel,
                               MessageMarkdownSemanticKind::channel_mention, begin, begin + 2, end, '#',
                               end);
    }
    u32 name_begin = 0;
    if (begin + 4 < end && parser->input.data[begin + 1] == ':') {
        name_begin = begin + 2;
    } else if (begin + 5 < end && parser->input.data[begin + 1] == 'a' &&
               parser->input.data[begin + 2] == ':') {
        name_begin = begin + 3;
    }
    if (name_begin != 0) {
        u32 separator = name_begin;
        while (separator < end && parser->input.data[separator] != ':') {
            ++separator;
        }
        if (separator > name_begin && separator < end && decimal_range(parser->input, separator + 1, end)) {
            u64 emoji_id = 0;
            if (!decimal_u64(parser->input, separator + 1, end, &emoji_id) || emoji_id == 0) {
                return true;
            }
            *matched = true;
            const u32 display_begin = parser->output_bytes;
            if (parser->options && parser->options->profile_bio) {
                if (!emit_byte(parser, ':') || !emit_range(parser, name_begin, separator) ||
                    !emit_byte(parser, ':'))
                    return false;
                parser->input_offset = end + 1;
                return true;
            }
            if (parser->options && parser->options->preserve_entities) {
                parser->input_offset = end + 1;
                return emit_range(parser, begin, end + 1);
            }
            // U+FFFC reserves one shaped inline-object cell.  The renderer
            // suppresses its glyph and places the atlas emoji in this semantic
            // cell, so the raw <:name:id> token is never exposed visually.
            static constexpr char replacement[] = {'\xEF', '\xBF', '\xBC'};
            for (u32 byte = 0; byte < 3; ++byte) {
                if (!emit_byte(parser, replacement[byte], text_style_custom_emoji)) {
                    return false;
                }
            }
            const bool animated = parser->input.data[begin + 1] == 'a';
            if (!append_semantic(parser, MessageMarkdownSemanticKind::custom_emoji, emoji_id, display_begin,
                                 3, begin, end + 1 - begin, animated ? 1u : 0u)) {
                return false;
            }
            parser->input_offset = end + 1;
            ++parser->transformations;
            return true;
        }
    }
    if (begin + 4 < end && parser->input.data[begin + 1] == '/') {
        u32 separator = end;
        while (separator > begin + 2 && parser->input.data[separator - 1] != ':') {
            --separator;
        }
        u64 command_id = 0;
        if (separator > begin + 2 && separator < end && decimal_range(parser->input, separator, end) &&
            command_mention_name(parser->input, begin + 2, separator - 1) &&
            decimal_u64(parser->input, separator, end, &command_id) && command_id != 0) {
            *matched = true;
            if (parser->options && parser->options->preserve_entities) {
                parser->input_offset = end + 1;
                return emit_range(parser, begin, end + 1, text_style_mention);
            }
            const bool bio = parser->options && parser->options->profile_bio;
            u32 style = bio ? 0u : text_style_mention;
            if (!bio && semantic_hovered(*parser, MessageMarkdownSemanticKind::command_mention, begin))
                style |= text_style_interactive_hover;
            const u32 display_begin = parser->output_bytes;
            if (!emit_byte(parser, '/', style) || !emit_range(parser, begin + 2, separator - 1, style)) {
                return false;
            }
            if (!bio && !append_semantic(parser, MessageMarkdownSemanticKind::command_mention, command_id,
                                         display_begin, parser->output_bytes - display_begin, begin,
                                         end + 1 - begin)) {
                return false;
            }
            parser->input_offset = end + 1;
            ++parser->transformations;
            return true;
        }
    }
    return true;
}

bool word_boundary_after(Utf8View input, u32 offset) noexcept {
    if (offset >= input.bytes) {
        return true;
    }
    const char value = input.data[offset];
    return !((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || ascii_is_digit(value) ||
             value == '_');
}

bool try_emit_named_mention(Parser* parser, bool* matched) noexcept {
    *matched = false;
    constexpr const char* names[] = {"@everyone", "@here"};
    static constexpr u32 lengths[] = {9, 5};
    for (u32 item = 0; item < 2; ++item) {
        if (starts_with(*parser, names[item], lengths[item]) &&
            word_boundary_after(parser->input, parser->input_offset + lengths[item])) {
            *matched = true;
            const u32 begin = parser->input_offset;
            parser->input_offset += lengths[item];
            ++parser->transformations;
            return emit_range(parser, begin, parser->input_offset, text_style_mention);
        }
    }
    return true;
}

bool starts_with_at(const Parser& parser, u32 at, const char* value, u32 bytes) noexcept {
    if (at > parser.input.bytes || bytes > parser.input.bytes - at)
        return false;
    for (u32 index = 0; index < bytes; ++index)
        if (parser.input.data[at + index] != value[index])
            return false;
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
                    if (parser->source_offsets.data)
                        parser->source_offsets[parser->output_bytes - 1] = begin + index;
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

// A fence may open before a space or newline ("```\ncode\n```"); other
// markers need text right after them.
bool marker_can_open(const Parser& parser, u8 bytes, DelimiterKind kind) noexcept {
    const u32 after = parser.input_offset + bytes;
    return after < parser.input.bytes &&
           ((kind == DelimiterKind::code && bytes == 3) || !ascii_space(parser.input.data[after]));
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
        while (end < limit && (parser->styles[end] | base_style) == style) {
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
        if (value == 'h' && !try_emit_message_link(&parser, &matched)) {
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
                    if (parser.source_offsets.data)
                        parser.source_offsets[parser.output_bytes - 1] = parser.input_offset + index;
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
                                             Slice<MessageMarkdownSemantic> semantic_output,
                                             Slice<u32> source_offsets) noexcept {
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
    if (source_offsets.data && source_offsets.count < text_output.count)
        return parse_error(MessageMarkdownError::output_too_small);
    if (!validate_utf8(input)) {
        return parse_error(MessageMarkdownError::invalid_utf8);
    }
    AnsiColorRun ansi_runs[kMessageMarkdownAnsiRunMaximum];
    Parser parser{};
    parser.input = input;
    parser.output = text_output;
    parser.ansi_runs = Slice<AnsiColorRun>{ansi_runs, kMessageMarkdownAnsiRunMaximum};
    parser.styles = byte_style_scratch;
    parser.source_offsets = source_offsets;
    parser.spans = span_output;
    parser.semantics = semantic_output;
    parser.options = &options;
    parser.line_start = true;
    parser.emoji_only = emoji_only_source(input);
    const auto error = walk(&parser);
    if (error != MessageMarkdownError::none) return parse_error(error);
    return finish(&parser);
}

u32 message_markdown_source_styles(Utf8View input, Slice<char> text, Slice<u32> styles, Slice<u32> map,
                                   Slice<TextStyleSpan> output) noexcept {
    if (input.bytes > text.count || input.bytes > styles.count || input.bytes > map.count || !output.data)
        return 0;
    MessageMarkdownOptions options{};
    options.preserve_entities = true;
    options.spoiler_revealed = +[](void*, u32) noexcept { return true; };
    const auto parsed = message_markdown_parse(input, text, styles, output, options, {}, map);
    if (!parsed.ok())
        return 0;
    // The base transform only removes bytes. Walk backwards to expand styles
    // into source coordinates in the same scratch, leaving delimiters plain.
    u32 end = input.bytes;
    for (u32 i = parsed.text.bytes; i != 0;) {
        --i;
        const u32 destination = map[i];
        if (destination >= input.bytes || destination < i || destination > end)
            return 0;
        const u32 flags = styles[i] & ~(text_style_jumbo_emoji | text_style_custom_emoji |
                                        text_style_spoiler | text_style_list_bullet);
        while (end > destination + 1)
            styles[--end] = 0;
        styles[destination] = flags;
        end = destination;
    }
    while (end)
        styles[--end] = 0;
    u32 count = 0;
    for (u32 first = 0; first < input.bytes;) {
        u32 last = first + 1;
        while (last < input.bytes && styles[last] == styles[first])
            ++last;
        if (styles[first]) {
            if (count == output.count)
                break;
            output[count++] = {first, last - first, styles[first]};
        }
        first = last;
    }
    return count;
}

u32 message_markdown_source_styles(Utf8View input, Slice<TextStyleSpan> output) noexcept {
    if ((!input.data && input.bytes) || !output.data || !validate_utf8(input)) return 0;
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

namespace {
bool invite_starts(Utf8View input, u32 at, const char* text) noexcept {
    for (u32 i = 0; text[i]; ++i)
        if (at + i >= input.bytes || ascii_lower(input.data[at + i]) != text[i])
            return false;
    return true;
}
u32 invite_literal_bytes(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    return bytes;
}
bool invite_code_byte(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || ascii_is_digit(c) || c == '-';
}
bool invite_url_byte(char c) noexcept {
    return static_cast<u8>(c) > 0x20 && c != '<' && c != '>' && c != '"' && c != '`';
}
// Parses one invite URL at input[begin]; source_bytes covers the whole URL.
bool invite_link_at(Utf8View input, u32 begin, InviteLink* link) noexcept {
    u32 at = begin;
    if (invite_starts(input, at, "https://"))
        at += 8;
    else if (invite_starts(input, at, "http://"))
        at += 7;
    else
        return false;
    if (invite_starts(input, at, "www."))
        at += 4;
    bool short_host = false;
    static constexpr const char* kHosts[] = {"discord.gg", "canary.discord.com", "ptb.discord.com", "discord.com",
                                             "discordapp.com"};
    bool host = false;
    for (u32 h = 0; h < 5 && !host; ++h) {
        const u32 bytes = invite_literal_bytes(kHosts[h]);
        if (invite_starts(input, at, kHosts[h]) && at + bytes < input.bytes && input.data[at + bytes] == '/') {
            short_host = h == 0;
            at += bytes;
            host = true;
        }
    }
    if (!host)
        return false;
    u64 event_guild = 0, event_id = 0;
    u32 code_begin = at, code_bytes = 0;
    if (!short_host && invite_starts(input, at, "/events/")) {
        // /events/<guild>/<event>[/<recurrence>]
        at += 8;
        const auto number = [&](u64* value) noexcept {
            const u32 begin = at;
            u64 parsed = 0;
            while (at < input.bytes && ascii_is_digit(input.data[at]) && at - begin < 20) {
                parsed = parsed * 10 + static_cast<u64>(input.data[at] - '0');
                ++at;
            }
            *value = parsed;
            return at > begin && parsed != 0;
        };
        if (!number(&event_guild) || at >= input.bytes || input.data[at] != '/')
            return false;
        ++at;
        code_begin = at;
        if (!number(&event_id))
            return false;
        code_bytes = at - code_begin;
        if (at + 1 < input.bytes && input.data[at] == '/' && ascii_is_digit(input.data[at + 1])) {
            u64 recurrence = 0;
            ++at;
            number(&recurrence);
        }
    } else {
        if (!short_host) {
            if (!invite_starts(input, at, "/invite/"))
                return false;
            at += 8;
        } else {
            ++at;
        }
        code_begin = at;
        while (at < input.bytes && invite_code_byte(input.data[at]))
            ++at;
        code_bytes = at - code_begin;
    }
    if (!code_bytes || code_bytes > kInviteCodeMaximumBytes)
        return false;
    if (at < input.bytes && input.data[at] == '/')
        ++at;
    if (at < input.bytes && input.data[at] == '.' && (at + 1 == input.bytes || !invite_url_byte(input.data[at + 1])))
        ++at;
    else if (at + 1 < input.bytes && input.data[at] == '.' && invite_url_byte(input.data[at + 1]))
        return false;
    // Only a query or fragment may follow the path.
    if (at < input.bytes && (input.data[at] == '?' || input.data[at] == '#'))
        while (at < input.bytes && invite_url_byte(input.data[at]))
            ++at;
    // Sentence punctuation ends the link; anything else extends an invalid path.
    if (at < input.bytes && invite_url_byte(input.data[at])) {
        const char c = input.data[at];
        if (c != ')' && c != ']' && c != ',' && c != ';' && c != ':' && c != '!' && c != '\'' && c != '*')
            return false;
    }
    InviteLink parsed{};
    parsed.source_offset = begin;
    parsed.source_bytes = at - begin;
    parsed.code_bytes = static_cast<u8>(code_bytes);
    copy_bytes(parsed.code, input.data + code_begin, code_bytes);
    parsed.event_guild_id = event_guild;
    parsed.event_id = event_id;
    *link = parsed;
    return true;
}
} // namespace

bool message_markdown_invite_link(Utf8View value, InviteLink* link) noexcept {
    if ((!value.data && value.bytes) || !link)
        return false;
    InviteLink parsed{};
    if (!invite_link_at(value, 0, &parsed) || parsed.source_bytes != value.bytes)
        return false;
    *link = parsed;
    return true;
}

u32 message_markdown_find_invites(Utf8View content, Slice<InviteLink> output) noexcept {
    if ((!content.data && content.bytes) || (!output.data && output.count))
        return 0;
    const u32 limit = minimum<u32>(output.count, kInviteLinksPerMessage);
    u32 count = 0;
    for (u32 at = 0; at < content.bytes && count < limit;) {
        const bool boundary = !at || !invite_url_byte(content.data[at - 1]) || content.data[at - 1] == '(';
        InviteLink link{};
        if (!boundary || (content.data[at] != 'h' && content.data[at] != 'H') ||
            !invite_link_at(content, at, &link)) {
            ++at;
            continue;
        }
        at += link.source_bytes;
        bool duplicate = false;
        for (u32 i = 0; i < count && !duplicate; ++i)
            duplicate = output[i].code_bytes == link.code_bytes && output[i].event_id == link.event_id &&
                        output[i].event_guild_id == link.event_guild_id &&
                        bytes_equal(output[i].code, link.code, link.code_bytes);
        if (!duplicate)
            output[count++] = link;
    }
    return count;
}

bool message_markdown_message_link(Utf8View value, MessageLink* link) noexcept {
    if ((!value.data && value.bytes != 0) || !link) {
        return false;
    }
    MessageLink parsed{};
    if (!message_link_at(value, 0, &parsed) || parsed.source_bytes != value.bytes) {
        return false;
    }
    *link = parsed;
    return true;
}

} // namespace maicord
