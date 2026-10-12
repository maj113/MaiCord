#include "maicord/ui/menu_row.hpp"

namespace maicord {
bool ui_menu_row_project(UiRect content, UiRect clip, UiMenuRowPresentation presentation, bool has_detail,
                         bool has_shortcut, UiMenuRowGeometry* output,
                         const UiResolvedThemeStyles* styles) noexcept {
    const UiThemeMetrics defaults{};
    const auto& metrics = styles && styles->ready ? styles->metrics : defaults;
    if (!output || !ui_rect_valid_or_empty(content) || !ui_rect_valid_or_empty(clip) ||
        !ui_menu_row_presentation_valid(presentation)) {
        return false;
    }
    UiMenuRowGeometry geometry{};
    geometry.clip = ui_rect_intersection(content, clip);
    if (!ui_rect_has_area(content)) {
        geometry.primary = content;
        *output = geometry;
        return true;
    }
    if (presentation.icon_plus_one) {
        const float extent =
            minimum(metrics.menu_icon, minimum(ui_rect_height(content), ui_rect_width(content) * .5f));
        const float top = (content.top + content.bottom - extent) * .5f;
        if (presentation.trailing_icon) {
            geometry.icon = {content.right - extent, top, content.right, top + extent};
            content.right = maximum(content.left, geometry.icon.left - metrics.menu_gap);
        } else {
            geometry.icon = {content.left, top, content.left + extent, top + extent};
            content.left = minimum(content.right, geometry.icon.right + metrics.menu_gap);
        }
    }
    if (has_shortcut) {
        const float width =
            minimum(kUiMenuShortcutMaximumWidth, ui_rect_width(content) * kUiMenuShortcutWidthFraction);
        geometry.shortcut = {content.right - width, content.top, content.right, content.bottom};
        content.right = maximum(content.left, geometry.shortcut.left - metrics.menu_gap);
    }
    geometry.primary = content;
    if (has_detail && presentation.secondary_line) {
        const float middle = (content.top + content.bottom) * .5f;
        geometry.primary.bottom = middle;
        geometry.detail = {content.left, middle, content.right, content.bottom};
    } else if (has_detail) {
        const float width =
            minimum(kUiMenuDetailMaximumWidth, ui_rect_width(content) * kUiMenuDetailWidthFraction);
        geometry.detail = {content.right - width, content.top, content.right, content.bottom};
        geometry.primary.right = maximum(content.left, geometry.detail.left - metrics.menu_gap);
    }
    *output = geometry;
    return true;
}
} // namespace maicord
