#pragma once

#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/layout/surface_geometry.hpp"
#include "maicord/media/core/asset_key.hpp"

namespace maicord {

// Room for the command card's "/command option: value ..." line.
inline constexpr u32 kUiTooltipMaximumTextBytes = 384;
inline constexpr u32 kUiTooltipMaximumBold = 3;
inline constexpr u32 kUiTooltipMaximumEmojiBytes = 48;
inline constexpr u32 kUiTooltipMaximumDetailBytes = 48;
// Chips layout: at most this many outlined chips, 28 DIPs tall with a 1 DIP outline and 8 DIPs of padding beside
// the label, 4 apart, wrapping like a row of tags.
inline constexpr u32 kUiTooltipMaximumChips = 8;
inline constexpr float kUiTooltipChipHeight = 28, kUiTooltipChipInset = 9, kUiTooltipChipGap = 4;
// Rows layout: at most this many rows of a 24 DIP picture and its line, 8 DIPs apart side to side and 4 between rows.
inline constexpr u32 kUiTooltipMaximumRows = 8;
inline constexpr float kUiTooltipRowPicture = 24, kUiTooltipRowGap = 4, kUiTooltipRowPictureGap = 8;
inline constexpr u64 kUiTooltipPointerDelayMs = 500;
inline constexpr u64 kUiTooltipKeyboardDelayMs = 350;

enum class UiTooltipTrigger : u8 {
    pointer,
    keyboard
};
enum class UiTooltipPlacement : u8 {
    above,
    below,
    left,
    right
};
// How the label lays out. label: text (with bold runs, an emoji, a detail line); chips: one outlined chip per line;
// card: the first line bold, the following lines in the muted colour below it, left-aligned (a badge's title and
// its subtitles); rows: one line per row beside the host's picture for it (row_media).
enum class UiTooltipLayout : u8 { label, chips, card, rows };
struct UiTooltipStyle {
    u32 pointer_delay_ms = static_cast<u32>(kUiTooltipPointerDelayMs);
    u32 keyboard_delay_ms = static_cast<u32>(kUiTooltipKeyboardDelayMs);
    float padding_x = kUiMenuPadding + 4, padding_y = kUiMenuPadding + 1;
    float margin = kUiFloatingSurfaceInset, gap = kUiFloatingSurfaceInset;
    float maximum_width = 320, maximum_height = 80;
    float radius = kUiControlCornerRadius, arrow_size = kUiControlCornerRadius;
    // A leading emoji's square and the space between it and the label.
    float emoji_size = 48, emoji_gap = 12;
};

struct UiTooltipKey {
    UiWidgetReference owner;
    u64 scope, identity, source_revision;
    u32 action;
};
bool ui_tooltip_key_equal(const UiTooltipKey&, const UiTooltipKey&) noexcept;

// A leading emoji (reaction and message emoji tooltips): a custom emoji or
// flag image the host draws from its media atlas when the key is valid, else
// a Unicode glyph drawn as jumbo text, else an icon (a sticker's) painted in
// the label colour. Its square is the style's emoji_size.
struct UiTooltipEmoji {
    MediaAssetKey media;
    Utf8View glyph;
    UiIcon icon = UiIcon::count;
};
// Descriptor labels are copied during offer. The source manager must remain
// alive until the host clears or replaces the offer. Hosts resolve current
// typed sources before offering; semantic/capability checks belong to them.
struct UiTooltipTarget {
    UiTooltipKey key;
    UiRect anchor;
    Utf8View label;
    UiTooltipPlacement placement;
    // Bold label ranges (offset and count; flags are ignored), ascending and
    // disjoint. With an emoji the label starts beside it instead of centred.
    ConstSlice<TextStyleSpan> bold;
    UiTooltipEmoji emoji;
    // A second line in the muted colour below the label ("Click to learn more").
    // With one, the label keeps to one line; an icon
    // then leads the label on its line and both lines centre.
    Utf8View detail;
    // Any layout but label lists its parts one per line ('\n' between); it takes no bold, emoji or detail (a card's
    // title is bold by itself).
    UiTooltipLayout layout;
    // Rows: each row's picture (an invalid key leaves its square empty).
    ConstSlice<MediaAssetKey> row_media;
};
struct UiTooltipScene {
    UiTooltipKey key;
    UiWidgetReference text_owner;
    UiTooltipStyle style;
    UiRect anchor, viewport, rectangle;
    UiSize text_size;
    // With a detail line: the label line's and the detail's measured sizes.
    UiSize label_size, detail_size;
    char text[kUiTooltipMaximumTextBytes];
    // The label's style runs: plain and bold, covering every byte.
    TextStyleSpan styles[kUiTooltipMaximumBold * 2 + 1];
    MediaAssetKey emoji_media;
    char emoji_glyph[kUiTooltipMaximumEmojiBytes];
    u64 show_at_ms;
    u32 revision;
    u16 text_bytes;
    u8 style_count, emoji_glyph_bytes;
    u8 emoji_icon; // UiIcon + 1; zero without one
    u8 detail_bytes;
    char detail[kUiTooltipMaximumDetailBytes];
    // Chips: their count and measured widths (DIPs, outline included). Card: label_size is the title's,
    // detail_size the subtitles'. Rows: their count, label_size the widest line, and the rows' pictures.
    u8 chip_count, row_count;
    UiTooltipLayout layout;
    u16 chip_widths[kUiTooltipMaximumChips];
    MediaAssetKey row_media[kUiTooltipMaximumRows];
    UiTooltipPlacement placement, preferred;
    UiTooltipTrigger trigger;
    bool pending, visible;
};
static_assert(sizeof(UiTooltipScene) <= 1024);

// Offers never capture input or allocate. Repeated identical offers retain
// their deadline; changing source or modality requires a fresh delay. New
// content for the visible source (a label that finished loading) replaces it
// in place at the next tick.
bool ui_tooltip_offer(UiTooltipScene*, const UiTooltipTarget&, UiRect viewport, UiTooltipTrigger,
                      u64 now_ms, const UiTooltipStyle& = UiTooltipStyle{}) noexcept;
// Text emits against a host-owned viewport root, independent from the source
// control's smaller clip. Clear before releasing either owner.
bool ui_tooltip_bind_text_owner(UiTooltipScene*, UiWidgetReference) noexcept;
bool ui_tooltip_clear(UiTooltipScene*) noexcept;
bool ui_tooltip_tick(UiTooltipScene*, u64 now_ms, UiTextMeasureCache*) noexcept;
u64 ui_tooltip_next_wake(const UiTooltipScene&) noexcept;
bool ui_tooltip_visible(const UiTooltipScene&) noexcept;
bool ui_tooltip_paint(const UiTooltipScene&, const Theme&, PaintStream*) noexcept;
bool ui_tooltip_text_output(const UiTooltipScene&, const Theme&, UiTextOutput*) noexcept;
// The leading emoji's square while visible; false without one.
bool ui_tooltip_emoji(const UiTooltipScene&, UiRect* square) noexcept;
// The index-th row's picture and its square while a rows tooltip is visible; false past the last row or for a row
// without a picture.
bool ui_tooltip_row_media(const UiTooltipScene&, u32 index, UiRect* square, MediaAssetKey* key) noexcept;
// A plain label's box for hosts that place, shape and draw the label
// themselves (the window caption): painted as above, without a text owner.
bool ui_tooltip_paint_box(PaintStream* paint, const Theme& theme, UiRect rectangle, UiRect anchor,
                          UiTooltipPlacement placement, const UiTooltipStyle& = UiTooltipStyle{}) noexcept;

} // namespace maicord
