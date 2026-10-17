#pragma once

#include "maicord/ui/icon.hpp"

namespace maicord {
struct UiResolvedThemeStyles;

enum class UiMenuIconPlacement : u8 {
    leading,
    trailing
};

// One optional icon and two placement bits. Zero preserves a plain row.
// Popup descriptors store this in two bytes of their existing padding.
struct UiMenuRowPresentation {
    u8 icon_plus_one; // zero: no icon; kUiMenuIconInvalid: rejected identity
    u8 secondary_line : 1;
    u8 trailing_icon : 1;
};
static_assert(sizeof(UiMenuRowPresentation) == 2 && alignof(UiMenuRowPresentation) == 1);
// Every manifest icon can lead a menu row; icon + 1 fills a whole byte.
inline constexpr u8 kUiMenuIconInvalid = 0xFF;
static_assert(static_cast<u32>(UiIcon::count) < kUiMenuIconInvalid);

constexpr UiMenuRowPresentation ui_menu_row_presentation(
    UiIcon icon, bool secondary_line = false,
    UiMenuIconPlacement placement = UiMenuIconPlacement::leading) noexcept {
    return {static_cast<u8>(icon < UiIcon::count && placement <= UiMenuIconPlacement::trailing
                                ? static_cast<u8>(icon) + 1
                                : kUiMenuIconInvalid),
            static_cast<u8>(secondary_line), static_cast<u8>(placement == UiMenuIconPlacement::trailing)};
}

constexpr bool ui_menu_row_presentation_valid(UiMenuRowPresentation presentation) noexcept {
    return presentation.icon_plus_one <= static_cast<u8>(UiIcon::count);
}

constexpr UiIcon ui_menu_row_icon(UiMenuRowPresentation presentation) noexcept {
    return presentation.icon_plus_one ? static_cast<UiIcon>(presentation.icon_plus_one - 1) : UiIcon::count;
}

struct UiMenuRowGeometry {
    UiRect icon;
    UiRect primary;
    UiRect detail;
    UiRect shortcut;
    UiRect clip;
};
static_assert(sizeof(UiMenuRowGeometry) == 80);

inline UiRect ui_menu_row_slot_clip(const UiMenuRowGeometry& geometry, UiRect slot) noexcept {
    return ui_rect_intersection(geometry.clip, slot);
}

// Borrowed, allocation-free rectangles shared by row paint and text output.
// Intrinsic label shaping, ownership and keyboard actions remain in the scene.
bool ui_menu_row_project(UiRect content, UiRect clip, UiMenuRowPresentation presentation, bool has_detail,
                         bool has_shortcut, UiMenuRowGeometry* output,
                         const UiResolvedThemeStyles* styles = nullptr) noexcept;

} // namespace maicord
