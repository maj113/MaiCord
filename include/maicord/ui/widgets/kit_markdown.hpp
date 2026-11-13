#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/widgets/kit.hpp"

// Markdown block: the chat's parser (message_markdown_parse) into display text and style spans the text layer
// shapes. Retained per source and width: a block is parsed once per source hash and its height measured once
// per width bucket. The storage is the caller's (a UiMarkdownBank, or any arrays).
// The block is cut into parts: a run of plain paragraphs, or one list item. An item is its own wrapped text
// command inset by its level, with its marker (a bullet, a dash from the second level, or the number) drawn
// beside it, so wrapped lines hang under the item's text. This is the kit's own list style: chat keeps the
// parser's bullet decoration.
namespace maicord {

inline constexpr u32 kUiMarkdownSourceMaximum = 8 * 1024, kUiMarkdownPartMaximum = 64;
// Span storage holds the parse output in its upper half and the per-part spans in its lower half.
inline constexpr u32 kUiMarkdownSpanStorage = 2 * kTextStyleSpanMaximum;
enum class UiMarkdownMarker : u8 { none, bullet, dash, number };
struct UiMarkdownPart {
    // The text of the part (an item's text starts after its marker) and its spans, relative to it.
    u32 offset, bytes;
    u16 span_first, span_count;
    // A number marker's text ("1.") inside the block text.
    u32 marker_offset;
    u8 marker_bytes, level;
    UiMarkdownMarker marker;
    // Placement at the block's measured width.
    float top, height;
};
// A block's own storage: the transformed text (never longer than its source), the span storage
// (kUiMarkdownSpanStorage entries) and the parts (kUiMarkdownPartMaximum entries).
struct UiMarkdown {
    Slice<char> text;
    Slice<TextStyleSpan> spans;
    Slice<UiMarkdownPart> parts;
    u32 text_bytes, span_count, part_count;
    u64 source_signature;
    // The width bucket the parts were placed at (DIPs, whole), and the height.
    float measured_width, height;
    // Size of the body text in TextSize codes (text_size_sm: 14/20, text_size_md: 16/22).
    u8 body_size;
    bool parsed;
};
// Parses `source` (at most kUiMarkdownSourceMaximum bytes) into the block's storage; `body_size` is the text size
// code of the body. `###` headings become a small bold uppercase eyebrow instead of an inline heading. `scratch`
// holds one u32 per source byte for the parser's work and may be reused by the next call. An unchanged source
// (same signature and body size) keeps the previous parse. False on a parse error: the block is left unparsed.
bool ui_markdown_parse(UiMarkdown*, const Utf8View& source, u8 body_size, Slice<u32> scratch) noexcept;
// Wrapped height at `width`, cached with the parts' placement for the bucket (width rounded to whole DIPs).
float ui_markdown_height(UiMarkdown*, float width, UiTextMeasureCache*) noexcept;
u32 ui_markdown_part_count(const UiMarkdown&) noexcept;
// The text of part `index` (`marker` false) or its list marker (true) inside `rectangle`, whose width is the one the
// block is measured at (the block is measured here when it is not); empty when the part has none or lies outside
// `clip`. Nested items take `muted`. A `fade` above zero fades the parts that end within that many DIPs above the
// rectangle's bottom edge (a block shown clipped), line by part. The command borrows the block's storage.
UiTextCommand ui_markdown_command(UiMarkdown*, u32 index, bool marker, UiTextMeasureCache*, UiWidgetReference owner,
                                  const UiRect& rectangle, const UiRect& clip, const Color& color, const Color& muted,
                                  float fade = 0) noexcept;

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
