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
    // DIPs of visible window frame beside the client's sides beyond any at its
    // top (a restored Windows 11 window's 1px border): fills measure their
    // insets from the visible window edge.
    float frame;
    // The part whose tooltip shows below the bar, none while hidden.
    UiCaptionPart tooltip;
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
    // Hover and pressed fills: the same size and vertical inset, the close
    // fill as far from the window's top as from its visible right edge, with
    // the radius concentric with the window corner.
    UiRect fills[kUiCaptionButtonCount];
    UiRect update_fill;
    UiRect settings_fill;
    float fill_radius;
    // Span the title is centred in, between the icon and the trailing items.
    UiRect title;
};

// Height in DIPs from the theme (caption_height scale).
float ui_caption_bar_height(const Theme& theme) noexcept;
// `height` overrides the theme's (the band rounded to device pixels); fill
// insets snap to `pixel_scale` device pixels.
UiCaptionBarLayout ui_caption_bar_layout(const Theme& theme, float width, const UiCaptionBarState& state,
                                         float height = 0, float pixel_scale = 1) noexcept;
// caption for the drag band, none outside the bar.
UiCaptionPart ui_caption_bar_part(const UiCaptionBarLayout& layout, float x, float y) noexcept;
// Background, item and button fills, icons and glyphs. Glyph strokes snap to
// device pixels. The app icon itself is the renderer's. The window buttons
// come last, from `buttons_first`: a content scrim dims everything before them.
bool ui_caption_bar_paint(PaintStream* paint, const Theme& theme, const UiCaptionBarLayout& layout,
                          const UiCaptionBarState& state, float pixel_scale,
                          u32* buttons_first = nullptr) noexcept;
Color ui_caption_bar_title_color(const Theme& theme, const UiCaptionBarState& state) noexcept;
// What Windows hit-tests as its own: the drag band, the window menu and
// Maximize (snap layouts). Minimize, Close and the items are client controls,
// which keeps the system's caption-button tooltip away from them.
enum class UiCaptionNative : u8 {
    client,
    caption,
    system_menu,
    maximize,
};
UiCaptionNative ui_caption_bar_native(UiCaptionPart part) noexcept;
// Window buttons and trailing items show their name as a tooltip.
bool ui_caption_bar_part_tooltip(UiCaptionPart part) noexcept;
// Accessible name of an interactive part (empty for the drag band).
Utf8View ui_caption_bar_part_name(UiCaptionPart part, const UiCaptionBarState& state) noexcept;
// The part's rectangle in the layout (empty when hidden or not a target).
UiRect ui_caption_bar_part_rect(const UiCaptionBarLayout& layout, UiCaptionPart part) noexcept;

} // namespace maicord
