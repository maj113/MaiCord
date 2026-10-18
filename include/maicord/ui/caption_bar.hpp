#pragma once

#include "maicord/ui/paint.hpp"

namespace maicord {

// The window caption drawn in place of the system title bar: the app icon
// (window menu), a drag band with a centred title, optional trailing items
// (update badge, Settings) and the minimize, maximize/restore and close
// buttons. Geometry is in caption DIPs with the origin at the client's
// top-left corner.
enum class UiCaptionPart : u8 {
    none,
    caption,
    icon,
    update,
    settings,
    minimize,
    maximize,
    close,
};

// The update badge: hidden unless an update is on its way or ready.
enum class UiCaptionUpdate : u8 {
    hidden,
    downloading,
    installing,
    ready,
};

inline constexpr u32 kUiCaptionButtonCount = 3;

struct UiCaptionBarState {
    UiCaptionPart hovered;
    UiCaptionPart pressed;
    bool active;
    bool maximized;
    UiCaptionUpdate update;
    // The Settings entry, offered while no account is signed in.
    bool settings;
    // Download progress, 0..1.
    float progress;
};

struct UiCaptionBarLayout {
    float width;
    float height;
    // The app icon's box; the renderer draws the exe's icon there.
    UiRect icon;
    // The window menu target around the icon.
    UiRect icon_target;
    // Trailing items, empty while hidden.
    UiRect update;
    UiRect settings;
    // minimize, maximize, close: full-height hit targets.
    UiRect buttons[kUiCaptionButtonCount];
    // Span the title is centred in, between the icon and the trailing items.
    UiRect title;
};

// Height in DIPs from the theme (caption_height scale).
float ui_caption_bar_height(const Theme& theme) noexcept;
// `height` overrides the theme's (the band rounded to device pixels).
UiCaptionBarLayout ui_caption_bar_layout(const Theme& theme, float width, const UiCaptionBarState& state,
                                         float height = 0) noexcept;
// caption for the drag band, none outside the bar.
UiCaptionPart ui_caption_bar_part(const UiCaptionBarLayout& layout, float x, float y) noexcept;
// Background, item and button fills, icons and glyphs. Glyph strokes snap to
// device pixels. The app icon itself is the renderer's.
bool ui_caption_bar_paint(PaintStream* paint, const Theme& theme, const UiCaptionBarLayout& layout,
                          const UiCaptionBarState& state, float pixel_scale) noexcept;
Color ui_caption_bar_title_color(const Theme& theme, const UiCaptionBarState& state) noexcept;
// Accessible name of an interactive part (empty for the drag band).
Utf8View ui_caption_bar_part_name(UiCaptionPart part, const UiCaptionBarState& state) noexcept;
// The part's rectangle in the layout (empty when hidden or not a target).
UiRect ui_caption_bar_part_rect(const UiCaptionBarLayout& layout, UiCaptionPart part) noexcept;

} // namespace maicord
