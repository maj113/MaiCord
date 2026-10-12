#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {

inline constexpr u32 kUiTooltipMaximumTextBytes = 192;
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
};

struct UiTooltipKey {
    UiWidgetReference owner;
    u64 scope, identity, source_revision;
    u32 action;
};
bool ui_tooltip_key_equal(const UiTooltipKey&, const UiTooltipKey&) noexcept;

// Descriptor labels are copied during offer. The source manager must remain
// alive until the host clears or replaces the offer. Hosts resolve current
// typed sources before offering; semantic/capability checks belong to them.
struct UiTooltipTarget {
    UiTooltipKey key;
    UiRect anchor;
    Utf8View label;
    UiTooltipPlacement placement = UiTooltipPlacement::above;
};
struct UiTooltipScene {
    UiTooltipKey key;
    UiWidgetReference text_owner;
    UiTooltipStyle style;
    UiRect anchor, viewport, rectangle;
    UiSize text_size;
    char text[kUiTooltipMaximumTextBytes];
    u64 show_at_ms;
    u32 revision;
    u16 text_bytes;
    UiTooltipPlacement placement, preferred;
    UiTooltipTrigger trigger;
    bool pending, visible;
};
static_assert(sizeof(UiTooltipScene) <= 512);

// Offers never capture input or allocate. Repeated identical offers retain
// their deadline; changing source, label or modality requires a fresh delay.
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

} // namespace maicord
