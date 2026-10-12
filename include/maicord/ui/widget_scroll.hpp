#pragma once

#include "maicord/ui/widget_manager.hpp"

namespace maicord {

// Shared nearest-edge policy for retained rectangles and virtual model rows.
bool ui_scroll_reveal_offset(float first, float last, float viewport_first, float viewport_last,
    float offset, float maximum_offset, float* result) noexcept;

struct UiWidgetScrollGeometry {
    UiRect viewport;
    UiPoint offset;
    UiPoint maximum;
};

// Retained content only. Virtualized owners supply their full range through
// their model binding; resident rows alone cannot describe that range.
bool ui_widget_scroll_geometry(const UiWidgetManager& manager, UiWidgetHandle owner,
                               UiWidgetScrollGeometry* geometry) noexcept;

struct UiWidgetRevealResult {
    UiLayoutError error;
    u32 scrolled;
    bool fully_visible;
    constexpr bool ok() const noexcept { return error == UiLayoutError::none; }
};

// Reveal an eligible retained widget through all scrollable ancestors. Plans
// nearest-edge movement on both axes and arranges once only if offsets change.
// Does not change focus. Callers synchronize their scene's cached offsets.
UiWidgetRevealResult ui_widget_reveal(UiWidgetManager* manager, UiWidgetHandle target) noexcept;

} // namespace maicord
