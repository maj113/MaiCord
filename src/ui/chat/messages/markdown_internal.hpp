#pragma once

#include "maicord/ui/chat/messages/markdown.hpp"
#include "maicord/ui/text/code_highlight.hpp"
#include "maicord/core/memory.hpp"
#include <stddef.h>

namespace maicord::markdown_detail {

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

void source_byte(Parser* parser, u32 offset, char value, u32 flags) noexcept;
bool ascii_space(char value) noexcept;
bool unicode_space(u32 cp) noexcept;
bool starts_with(const Parser& parser, const char* value, u32 bytes) noexcept;
bool emit_byte(Parser* parser, char value, u32 added_style = 0, u32 source_offset = ~0u) noexcept;
bool emit_range(Parser* parser, u32 begin, u32 end, u32 added_style = 0) noexcept;
bool append_semantic(Parser* parser, MessageMarkdownSemanticKind kind, u64 target, u32 byte_offset,
                     u32 byte_count, u32 source_offset, u32 source_bytes, u8 reserved = 0) noexcept;
void close_delimiter(Parser* parser, const Delimiter& delimiter) noexcept;
bool valid_url(Utf8View input, u32 begin, u32 end) noexcept;
bool masked_link_closes(const Parser& parser) noexcept;
bool try_close_masked_link(Parser* parser) noexcept;
bool try_emit_autolink(Parser* parser, bool* matched) noexcept;
bool marker_occurs_later(const Parser& parser, u8 marker) noexcept;
bool try_emit_channel_link(Parser* parser, bool* matched) noexcept;
bool try_emit_attachment_link(Parser* parser, bool* matched) noexcept;
bool try_emit_entity_tag(Parser* parser, bool* matched) noexcept;
bool try_emit_named_mention(Parser* parser, bool* matched) noexcept;
bool starts_with_at(const Parser& parser, u32 at, const char* value, u32 bytes) noexcept;

} // namespace maicord::markdown_detail
