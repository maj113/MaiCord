#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/widgets/kit.hpp"

// Markdown block: the chat's parser (message_markdown_parse) into display text and style spans the text layer
// shapes as one styled command. Retained per source and width: a block is parsed once per source hash and its
// height measured once per width bucket. The storage is the caller's (a UiMarkdownBank, or any arrays).
namespace maicord {

inline constexpr u32 kUiMarkdownSourceMaximum = 8 * 1024, kUiMarkdownSpanMaximum = kTextStyleSpanMaximum;
// A block's own storage: the transformed text (never longer than its source) and the span list.
struct UiMarkdown {
    Slice<char> text;
    Slice<TextStyleSpan> spans;
    u32 text_bytes, span_count;
    u64 source_signature;
    // The width bucket the cached height was measured at (DIPs, whole), and the height.
    float measured_width, height;
    // Size of the body text in TextSize codes (text_size_sm: 14/20, text_size_md: 16/22).
    u8 body_size;
    bool parsed;
};
struct UiMarkdownOptions {
    // `###` headings become a small bold uppercase eyebrow instead of an inline heading.
    bool eyebrow_h3;
    u8 body_size;
};
// Parses `source` (at most kUiMarkdownSourceMaximum bytes) into the block's storage. `scratch` holds one u32
// per source byte for the parser's work and may be reused by the next call. An unchanged source (same
// signature and options) keeps the previous parse. False on a parse error: the block is left unparsed.
bool ui_markdown_parse(UiMarkdown*, const Utf8View& source, const UiMarkdownOptions&, Slice<u32> scratch) noexcept;
// Wrapped height at `width`, cached for the bucket (width rounded to whole DIPs).
float ui_markdown_height(UiMarkdown*, float width, const UiKitMeasure*) noexcept;
// The block as one text command; the command borrows the block's storage.
UiTextCommand ui_markdown_command(const UiMarkdown&, UiWidgetReference owner, const UiRect& rectangle, const UiRect& clip,
                                  const Color& color) noexcept;
// Body line height of a size code.
float ui_markdown_line_height(u8 body_size) noexcept;

// Retained blocks for a scene: the bank allocates itself (one arena, committed as blocks fill) on first use and is
// released when the page hides. A scene keeps a `UiMarkdownBank*` that starts null.
inline constexpr u32 kUiMarkdownBankBlocks = 6;
struct UiMarkdownBank {
    Arena arena;
    UiMarkdown blocks[kUiMarkdownBankBlocks];
    u64 owners[kUiMarkdownBankBlocks];
    u32 stamps[kUiMarkdownBankBlocks];
    u32 clock;
    Slice<u32> scratch;
};
// The block of `owner` (a control id), claiming the least recently used slot; null when memory is unavailable.
UiMarkdown* ui_markdown_bank_block(UiMarkdownBank** slot, u64 owner) noexcept;
Slice<u32> ui_markdown_bank_scratch(UiMarkdownBank*) noexcept;
void ui_markdown_bank_release(UiMarkdownBank** slot) noexcept;

} // namespace maicord
