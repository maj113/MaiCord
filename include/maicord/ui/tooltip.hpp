#pragma once

#include "maicord/ui/icon.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {

inline constexpr u32 kUiTooltipMaximumTextBytes = 192;
inline constexpr u32 kUiTooltipMaximumBold = 3;
inline constexpr u32 kUiTooltipMaximumEmojiBytes = 48;
inline constexpr u32 kUiTooltipMaximumDetailBytes = 48;
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
    UiTooltipPlacement placement = UiTooltipPlacement::above;
    // Bold label ranges (offset and count; flags are ignored), ascending and
    // disjoint. With an emoji the label starts beside it instead of centred.
    ConstSlice<TextStyleSpan> bold{};
    UiTooltipEmoji emoji{};
    // A second line in the muted colour below the label (the official
    // "Click to learn more"). With one, the label keeps to one line; an icon
    // then leads the label on its line and both lines centre.
    Utf8View detail{};
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
    UiTooltipPlacement placement, preferred;
    UiTooltipTrigger trigger;
    bool pending, visible;
};
static_assert(sizeof(UiTooltipScene) <= 640);

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
// A plain label's box for hosts that place, shape and draw the label
// themselves (the window caption): painted as above, without a text owner.
bool ui_tooltip_paint_box(PaintStream* paint, const Theme& theme, UiRect rectangle, UiRect anchor,
                          UiTooltipPlacement placement, const UiTooltipStyle& = UiTooltipStyle{}) noexcept;

} // namespace maicord
