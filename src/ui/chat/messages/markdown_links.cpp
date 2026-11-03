#include "markdown_internal.hpp"
#include "maicord/ui/text/ansi_text.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
using namespace markdown_detail;

namespace markdown_detail {
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

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// JavaScript \s beyond ASCII (U+0085 is not part of it).
bool unicode_space(u32 cp) noexcept {
    return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
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

bool emit_byte(Parser* parser, char value, u32 added_style, u32 source_offset) noexcept {
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
    const u32 jumbo = parser->emoji_only && !ascii_space(value) ? text_style_jumbo_emoji : 0u;
    parser->styles[parser->output_bytes] =
        parser->line_style | parser->persistent_style | added_style | jumbo;
    ++parser->output_bytes;
    return true;
}

bool emit_range(Parser* parser, u32 begin, u32 end, u32 added_style) noexcept {
    if (begin > end || end > parser->input.bytes) {
        return false;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        if (!emit_byte(parser, parser->input.data[offset], added_style, offset)) {
            return false;
        }
    }
    return true;
}

bool append_semantic(Parser* parser, MessageMarkdownSemanticKind kind, u64 target, u32 byte_offset,
                     u32 byte_count, u32 source_offset, u32 source_bytes, u8 reserved) noexcept {
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

static bool semantic_hovered(const Parser& parser, MessageMarkdownSemanticKind kind, u32 source_offset) noexcept {
    return parser.options && parser.options->has_hover && parser.options->hovered_kind == kind &&
           parser.options->hovered_source_offset == source_offset;
}

static void apply_style(Parser* parser, u32 begin, u32 end, u32 style) noexcept {
    if (begin > end || end > parser->output_bytes) {
        return;
    }
    for (u32 offset = begin; offset < end; ++offset) {
        parser->styles[offset] |= style;
    }
}

namespace {
bool ascii_identifier(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || ascii_is_digit(value) ||
           value == '_' || value == '-' || value == '+' || value == '.' || value == '#';
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
void strip_ansi_sgr(Parser* parser, u32 begin) noexcept {
    u32 write = begin;
    for (u32 read = begin; read < parser->output_bytes;) {
        const u32 escape = ansi_text_sgr_bytes(parser->output.data + read, parser->output_bytes - read);
        if (escape) {
            read += escape;
            continue;
        }
        parser->output[write] = parser->output[read];
        parser->styles[write] = parser->styles[read];
        ++write;
        ++read;
    }
    parser->output_bytes = write;
}
} // namespace
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

namespace {
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

} // namespace
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

namespace {
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
#pragma clang loop vectorize(disable) interleave(disable)
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

} // namespace
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

namespace {
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

bool ascii_letter(char value) noexcept {
    return static_cast<u8>((static_cast<u8>(value) | 0x20) - 'a') < 26;
}

bool take_literal(Utf8View input, u32* cursor, const char* literal, u32 bytes) noexcept {
    if (bytes > input.bytes - *cursor || !bytes_equal(input.data + *cursor, literal, bytes))
        return false;
    *cursor += bytes;
    return true;
}

// One /channels/ path segment: digits or letters and '-' ((\d+|[a-zA-Z-]+));
// value stays zero for a non-numeric segment.
bool channel_segment(Utf8View input, u32* cursor, u64* value, bool* numeric) noexcept {
    const u32 begin = *cursor;
    u32 end = begin;
    while (end < input.bytes && ascii_is_digit(input.data[end]))
        ++end;
    *numeric = end != begin;
    if (!*numeric)
        while (end < input.bytes && (ascii_letter(input.data[end]) || input.data[end] == '-'))
            ++end;
    u64 parsed = 0;
    if (end == begin || (*numeric && (!decimal_u64(input, begin, end, &parsed) || parsed == 0)))
        return false;
    *value = parsed;
    *cursor = end;
    return true;
}

// The channel and attachment URL rules and the invite scan only run at a
// candidate URL (or once per message): compiled for size. Mention, timestamp
// and chip emission stay compiled for speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// The channel/message URL rule: https:// discord.com or
// discordapp.com (optionally canary./ptb.), /channels/<guild|@me>/<channel>,
// an optional /<message> and ?query. A prefix match: text after the last
// segment stays text. A channel URL has message_id zero; a guild-only URL and
// one with a non-numeric channel or message segment are ordinary links.
bool channel_link_at(Utf8View input, u32 begin, MessageLink* link) noexcept {
    if (begin > input.bytes)
        return false;
    u32 cursor = begin;
    if (!take_literal(input, &cursor, "https://", 8))
        return false;
    if (!take_literal(input, &cursor, "canary.", 7))
        take_literal(input, &cursor, "ptb.", 4);
    if (!take_literal(input, &cursor, "discord", 7))
        return false;
    take_literal(input, &cursor, "app", 3);
    if (!take_literal(input, &cursor, ".com/channels/", 14))
        return false;
    u64 guild_id = 0, channel_id = 0, message_id = 0;
    bool numeric = false;
    if (!take_literal(input, &cursor, "@me", 3)) {
        const u32 guild_begin = cursor;
        while (cursor < input.bytes && ascii_is_digit(input.data[cursor]))
            ++cursor;
        if (!decimal_u64(input, guild_begin, cursor, &guild_id) || guild_id == 0)
            return false;
    }
    if (!take_literal(input, &cursor, "/", 1) || !channel_segment(input, &cursor, &channel_id, &numeric) ||
        !numeric)
        return false;
    // A thread message:
    // /channels/<guild>/<parent>/threads/<thread>/<message>, no query.
    u32 end = cursor;
    if (take_literal(input, &end, "/threads/", 9)) {
        u64 thread_id = 0;
        if (!guild_id || !channel_segment(input, &end, &thread_id, &numeric) || !numeric ||
            !take_literal(input, &end, "/", 1) || !channel_segment(input, &end, &message_id, &numeric) || !numeric)
            return false;
        *link = MessageLink{guild_id, thread_id, message_id, end - begin, channel_id};
        return true;
    }
    if (take_literal(input, &end, "/", 1)) {
        if (channel_segment(input, &end, &message_id, &numeric) && !numeric)
            return false;
        if (message_id)
            cursor = end;
    }
    // ?query: [\w%=&+-]+
    if (cursor + 1 < input.bytes && input.data[cursor] == '?') {
        end = cursor + 1;
        while (end < input.bytes && (ascii_is_alnum(input.data[end]) || input.data[end] == '_' ||
                                     input.data[end] == '%' || input.data[end] == '=' || input.data[end] == '&' ||
                                     input.data[end] == '+' || input.data[end] == '-'))
            ++end;
        if (end > cursor + 1)
            cursor = end;
    }
    *link = MessageLink{guild_id, channel_id, message_id, cursor - begin};
    return true;
}
#pragma clang attribute pop

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

// Chip icon cells are measured spaces the renderer draws the icon over: an em
// (U+2003) for channel, server, message and file icons, half an em (U+2002)
// for the chevron, then the trailing margins (U+2005 4 px,
// U+2006 3 px, U+200A 2 px at 16 px). These spaces never break before
// themselves (UAX #14 BA); a closing U+200D allows no break after it either,
// so an icon never wraps away from what follows it. (U+2060, the nowrap
// nowrap glue, is a default-ignorable the chat hides as U+200C.)
constexpr Utf8View kChipCell = utf8_literal("\xE2\x80\x83");
constexpr Utf8View kChipHalfCell = utf8_literal("\xE2\x80\x82");
constexpr Utf8View kChipGap4 = utf8_literal("\xE2\x80\x85");
constexpr Utf8View kChipGap3 = utf8_literal("\xE2\x80\x86");
constexpr Utf8View kChipGap2 = utf8_literal("\xE2\x80\x8A");
constexpr Utf8View kChipGlue = utf8_literal("\xE2\x80\x8D");

struct Chip {
    Parser* parser;
    u32 style;
    u32 source_begin;
    bool hovered;
};

bool emit_chip_cell(const Chip& chip, MessageMarkdownSemanticKind kind, u64 target, Utf8View cell,
                    Utf8View gap, u8 reserved = 0) noexcept {
    Parser* parser = chip.parser;
    const u32 at = parser->output_bytes;
    return emit_view(parser, cell, chip.style) &&
           append_semantic(parser, kind, target, at, cell.bytes, chip.source_begin, 0,
                           static_cast<u8>(reserved | (chip.hovered ? kMessageMarkdownChipHovered : 0))) &&
           emit_view(parser, gap, chip.style) && emit_view(parser, kChipGlue, chip.style);
}

bool emit_chip_icon(const Chip& chip, MessageMarkdownChipIcon icon, Utf8View gap = kChipGap4) noexcept {
    return emit_chip_cell(chip, MessageMarkdownSemanticKind::chip_icon, static_cast<u64>(icon), kChipCell, gap);
}

// The chevron between a chip's container and its target: 4 px, a
// half-em chevron, 2 px.
bool emit_chip_separator(const Chip& chip) noexcept {
    return emit_view(chip.parser, kChipGap4, chip.style) &&
           emit_chip_cell(chip, MessageMarkdownSemanticKind::chip_icon,
                          static_cast<u64>(MessageMarkdownChipIcon::chevron), kChipHalfCell, kChipGap2);
}

// Server and channel names are cut to 32 UTF-16 units: 31 code points with
// trailing spaces and dots removed, then an ellipsis.
bool emit_chip_name(const Chip& chip, Utf8View name) noexcept {
    u32 units = 0, kept = 0, points = 0;
    for (u32 at = 0; at < name.bytes;) {
        const Utf8DecodeResult decoded = utf8_decode_one(name.data + at, name.bytes - at);
        if (!decoded.valid || !decoded.width)
            return false;
        units += decoded.codepoint > 0xFFFFu ? 2u : 1u;
        if (++points <= 31)
            kept = at + decoded.width;
        at += decoded.width;
    }
    if (units <= 32)
        return emit_view(chip.parser, name, chip.style);
    while (kept && (ascii_space(name.data[kept - 1]) || name.data[kept - 1] == '.'))
        --kept;
    return emit_view(chip.parser, {name.data, kept}, chip.style) &&
           emit_view(chip.parser, utf8_literal("\xE2\x80\xA6"), chip.style);
}

bool emit_chip_channel(const Chip& chip, MessageMarkdownChipIcon icon, Utf8View name) noexcept {
    return emit_chip_icon(chip, icon) && emit_chip_name(chip, name);
}

// The server's icon, or for a server without one its initials, then its name and the chevron.
bool emit_chip_guild(const Chip& chip, const MessageMarkdownChannelTarget& target) noexcept {
    return emit_chip_cell(chip, MessageMarkdownSemanticKind::guild_icon, target.guild_id, kChipCell, kChipGap3,
                          target.guild_icon ? 0 : kMessageMarkdownChipInitials) &&
           emit_chip_name(chip, target.guild_name) && emit_chip_separator(chip);
}

bool chip_view_valid(Utf8View value) noexcept {
    return (value.data || !value.bytes) && (!value.bytes || utf8_validate(value).valid);
}

enum class ChipResult : u8 { chip, text, none };

// The chip for a channel reference: <#id> and
// /channels/ URLs. kind/target name the chip's semantic. A URL to a channel of
// an unknown non-private server stays an ordinary link (ChipResult::none).
bool emit_channel_chip(Parser* parser, const MessageLink& link, bool url, u32 source_begin,
                       ChipResult* result) noexcept {
    const MessageMarkdownOptions& options = *parser->options;
    MessageMarkdownChannelTarget target{};
    const auto resolve = [&](u64 channel_id) noexcept {
        reset_value(&target);
        if (!options.resolve_channel(options.context, link.guild_id, channel_id, &target) ||
            !chip_view_valid(target.name) || !chip_view_valid(target.guild_name) ||
            !chip_view_valid(target.parent_name) ||
            static_cast<u8>(target.icon) >= static_cast<u8>(MessageMarkdownChipIcon::count) ||
            static_cast<u8>(target.parent_icon) >= static_cast<u8>(MessageMarkdownChipIcon::count))
            reset_value(&target);
    };
    resolve(link.channel_id);
    // A thread that is not loaded shows its parent channel.
    if (!(target.flags & message_markdown_channel_known) && link.parent_channel_id)
        resolve(link.parent_channel_id);
    const u8 flags = target.flags;
    const bool message = link.message_id != 0;
    const auto kind = message ? MessageMarkdownSemanticKind::message_link : MessageMarkdownSemanticKind::channel_mention;
    const bool hovered = semantic_hovered(*parser, kind, source_begin);
    const Chip chip{parser, text_style_mention | (hovered ? u32{text_style_interactive_hover} : 0u), source_begin,
                    hovered};
    const u32 display_begin = parser->output_bytes;
    *result = ChipResult::chip;
    bool ok = true;
    if (!(flags & message_markdown_channel_known)) {
        ok = ((flags & message_markdown_channel_guild_known) && !(flags & message_markdown_channel_same_guild)
                  ? emit_chip_guild(chip, target)
                  : true) &&
             emit_chip_icon(chip, MessageMarkdownChipIcon::text_channel) &&
             emit_view(parser, utf8_literal("unknown"), chip.style | text_style_italic);
    } else if (!(flags & message_markdown_channel_viewable)) {
        ok = emit_chip_channel(chip, MessageMarkdownChipIcon::locked, utf8_literal("No Access"));
    } else if (!(flags & message_markdown_channel_mentionable)) {
        *result = ChipResult::text;
        ok = emit_byte(parser, '#') && emit_view(parser, target.name, 0);
    } else if (!(flags & message_markdown_channel_guild_known)) {
        if (flags & message_markdown_channel_private) {
            ok = emit_chip_channel(chip, target.icon, target.name) && emit_chip_separator(chip) &&
                 emit_chip_icon(chip, MessageMarkdownChipIcon::message);
        } else if (url) {
            *result = ChipResult::none;
            return true;
        } else {
            *result = ChipResult::text;
            ok = emit_view(parser, utf8_literal("#unknown-channel"), 0);
        }
    } else if (flags & message_markdown_channel_same_guild) {
        if (!message)
            ok = emit_chip_channel(chip, target.icon, target.name);
        else if ((flags & message_markdown_channel_forum_post) && (flags & message_markdown_channel_parent_known))
            ok = emit_chip_channel(chip, target.parent_icon, target.parent_name) && emit_chip_separator(chip) &&
                 emit_chip_channel(chip, target.icon, target.name);
        else
            ok = emit_chip_channel(chip, target.icon, target.name) && emit_chip_separator(chip) &&
                 emit_chip_icon(chip, MessageMarkdownChipIcon::message);
    } else {
        ok = emit_chip_guild(chip, target) &&
             (message && !(flags & message_markdown_channel_forum_post)
                  ? emit_chip_icon(chip, MessageMarkdownChipIcon::message)
                  : emit_chip_channel(chip, target.icon, target.name));
    }
    if (!ok)
        return false;
    if (flags & message_markdown_channel_known)
        saturating_increment(parser->resolved_entities);
    else
        saturating_increment(parser->unresolved_entities);
    return *result != ChipResult::chip ||
           append_semantic(parser, kind, message ? link.message_id : link.channel_id, display_begin,
                           parser->output_bytes - display_begin, source_begin, link.source_bytes);
}

bool inside_masked_link(const Parser& parser) noexcept {
    return parser.depth != 0 && parser.delimiters[parser.depth - 1].kind == DelimiterKind::masked_link;
}

} // namespace
bool try_emit_channel_link(Parser* parser, bool* matched) noexcept {
    *matched = false;
    MessageLink link{};
    const u32 source_begin = parser->input_offset;
    const MessageMarkdownOptions* options = parser->options;
    // Embed titles keep this rule (only the attachment-link rule is omitted there);
    // masked link text does not.
    if (inside_masked_link(*parser) || !channel_link_at(parser->input, source_begin, &link)) {
        return true;
    }
    if (options && options->preserve_entities) {
        *matched = true;
        parser->input_offset += link.source_bytes;
        return emit_range(parser, source_begin, parser->input_offset, text_style_link);
    }
    if (options && options->resolve_channel) {
        ChipResult result{};
        if (!emit_channel_chip(parser, link, true, source_begin, &result))
            return false;
        if (result == ChipResult::none)
            return true;
    } else {
        // Plain-text surfaces: a message URL reads "#channel › 💬"; a channel
        // URL stays an ordinary link.
        if (!link.message_id)
            return true;
        const u32 display_begin = parser->output_bytes;
        u32 channel_style = text_style_mention | text_style_link;
        u32 suffix_style = text_style_link;
        if (semantic_hovered(*parser, MessageMarkdownSemanticKind::message_link, source_begin)) {
            channel_style |= text_style_interactive_hover;
            suffix_style |= text_style_interactive_hover | text_style_underline;
        }
        Utf8View channel_name{};
        if (options && options->resolve_entity) {
            channel_name = options->resolve_entity(options->context, MessageMarkdownEntityKind::channel,
                                                   link.channel_id);
            if (!chip_view_valid(channel_name))
                reset_value<Utf8View>(&channel_name);
        }
        if (!emit_byte(parser, '#', channel_style) ||
            !(channel_name.bytes != 0 ? emit_view(parser, channel_name, channel_style)
                                      : emit_decimal(parser, link.channel_id, channel_style)) ||
            !emit_view(parser, utf8_literal(" \xE2\x80\xBA \xF0\x9F\x92\xAC"), suffix_style) ||
            !append_semantic(parser, MessageMarkdownSemanticKind::message_link, link.message_id, display_begin,
                             parser->output_bytes - display_begin, source_begin, link.source_bytes)) {
            return false;
        }
    }
    parser->input_offset += link.source_bytes;
    ++parser->transformations;
    *matched = true;
    return true;
}

namespace {
bool ascii_host_label(char value) noexcept {
    return ascii_is_alnum(value) || value == '-';
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// The attachment-link rule (prefix match):
// ^https://(?:[A-Za-z0-9-]+\.)*(?:(?:media|images)(?:-[A-Za-z0-9]+)?\.discordapp\.net|
//   cdn(?:-[A-Za-z0-9]+)?\.discordapp\.com)/(?:attachments|ephemeral-attachments)/\d+/\d+/
//   ([A-Za-z0-9._-]*[A-Za-z0-9_-])(?:[?][a-zA-Z0-9?&=_-]*)?
// Returns the matched bytes (zero: no match) and the file name range.
u32 attachment_link_at(Utf8View input, u32 begin, u32* name_begin, u32* name_end) noexcept {
    u32 cursor = begin;
    if (begin > input.bytes || !take_literal(input, &cursor, "https://", 8))
        return 0;
    const u32 host = cursor;
    while (cursor < input.bytes && (ascii_host_label(input.data[cursor]) || input.data[cursor] == '.'))
        ++cursor;
    const u32 host_end = cursor;
    if (!take_literal(input, &cursor, "/", 1))
        return 0;
    // The last three labels: <service>.discordapp.<net|com>.
    constexpr u32 suffix = 15; // ".discordapp.net"
    if (host_end - host <= suffix)
        return 0;
    const char* tail = input.data + host_end - suffix;
    const bool net = bytes_equal(tail, ".discordapp.net", suffix);
    if (!net && !bytes_equal(tail, ".discordapp.com", suffix))
        return 0;
    u32 service = host_end - suffix;
    while (service > host && input.data[service - 1] != '.')
        --service;
    // Leading labels: each non-empty, then a dot.
    for (u32 at = host; at < service; ++at)
        if (input.data[at] == '.' && (at == host || input.data[at - 1] == '.'))
            return 0;
    const Utf8View label{input.data + service, host_end - suffix - service};
    u32 name = 0;
    if (net && label.bytes >= 5 && bytes_equal(label.data, "media", 5))
        name = 5;
    else if (net && label.bytes >= 6 && bytes_equal(label.data, "images", 6))
        name = 6;
    else if (!net && label.bytes >= 3 && bytes_equal(label.data, "cdn", 3))
        name = 3;
    if (!name)
        return 0;
    if (name != label.bytes) {
        if (label.data[name] != '-' || name + 1 == label.bytes)
            return 0;
        for (u32 at = name + 1; at < label.bytes; ++at)
            if (!ascii_is_alnum(label.data[at]))
                return 0;
    }
    if (!take_literal(input, &cursor, "attachments/", 12) &&
        !take_literal(input, &cursor, "ephemeral-attachments/", 22))
        return 0;
    for (u32 id = 0; id < 2; ++id) {
        const u32 digits = cursor;
        while (cursor < input.bytes && ascii_is_digit(input.data[cursor]))
            ++cursor;
        if (cursor == digits || !take_literal(input, &cursor, "/", 1))
            return 0;
    }
    const u32 file = cursor;
    while (cursor < input.bytes &&
           (ascii_is_alnum(input.data[cursor]) || input.data[cursor] == '.' || input.data[cursor] == '_' ||
            input.data[cursor] == '-'))
        ++cursor;
    while (cursor > file && input.data[cursor - 1] == '.')
        --cursor;
    if (cursor == file)
        return 0;
    *name_begin = file;
    *name_end = cursor;
    if (cursor < input.bytes && input.data[cursor] == '?') {
        ++cursor;
        while (cursor < input.bytes &&
               (ascii_is_alnum(input.data[cursor]) || input.data[cursor] == '?' || input.data[cursor] == '&' ||
                input.data[cursor] == '=' || input.data[cursor] == '_' || input.data[cursor] == '-'))
            ++cursor;
    }
    return cursor - begin;
}
#pragma clang attribute pop

} // namespace
// A CDN attachment URL becomes a mention-style chip: a paperclip and the file
// name. The link semantic keeps the whole URL as its source.
bool try_emit_attachment_link(Parser* parser, bool* matched) noexcept {
    *matched = false;
    const MessageMarkdownOptions* options = parser->options;
    if ((options && (options->no_links || options->preserve_entities)) || inside_masked_link(*parser))
        return true;
    const u32 begin = parser->input_offset;
    u32 name_begin = 0, name_end = 0;
    const u32 bytes = attachment_link_at(parser->input, begin, &name_begin, &name_end);
    if (!bytes)
        return true;
    const bool hovered = semantic_hovered(*parser, MessageMarkdownSemanticKind::link, begin);
    const Chip chip{parser, text_style_mention | (hovered ? u32{text_style_interactive_hover} : 0u), begin, hovered};
    const u32 display_begin = parser->output_bytes;
    if (!emit_chip_icon(chip, MessageMarkdownChipIcon::attachment, kChipGap2) ||
        !emit_range(parser, name_begin, name_end, chip.style) ||
        !append_semantic(parser, MessageMarkdownSemanticKind::link, 0, display_begin,
                         parser->output_bytes - display_begin, begin, bytes, kMessageMarkdownAttachmentChip))
        return false;
    parser->input_offset = begin + bytes;
    ++parser->transformations;
    *matched = true;
    return true;
}

namespace {
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

} // namespace
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
        u64 channel_id = 0;
        if (parser->options && parser->options->resolve_channel && !parser->options->preserve_entities &&
            decimal_u64(parser->input, begin + 2, end, &channel_id) && channel_id) {
            ChipResult result{};
            if (!emit_channel_chip(parser, MessageLink{0, channel_id, 0, end + 1 - begin}, false, begin, &result))
                return false;
            parser->input_offset = end + 1;
            ++parser->transformations;
            return true;
        }
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

namespace {
bool word_boundary_after(Utf8View input, u32 offset) noexcept {
    if (offset >= input.bytes) {
        return true;
    }
    const char value = input.data[offset];
    return !((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || ascii_is_digit(value) ||
             value == '_');
}

} // namespace
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
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
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
} // namespace markdown_detail

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
    InviteLink link;
    for (u32 at = 0; at < content.bytes && count < limit;) {
        if ((content.data[at] != 'h' && content.data[at] != 'H') ||
            !(!at || !invite_url_byte(content.data[at - 1]) || content.data[at - 1] == '(') ||
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
    if (!channel_link_at(value, 0, &parsed) || !parsed.message_id || parsed.source_bytes != value.bytes) {
        return false;
    }
    *link = parsed;
    return true;
}

bool message_markdown_attachment_link(Utf8View value, Utf8View* name) noexcept {
    u32 name_begin = 0, name_end = 0;
    if (!value.data || !name || attachment_link_at(value, 0, &name_begin, &name_end) != value.bytes)
        return false;
    *name = {value.data + name_begin, name_end - name_begin};
    return true;
}
} // namespace maicord
#pragma clang attribute pop
