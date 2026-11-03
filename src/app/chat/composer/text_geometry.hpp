#pragma once
#include "../../shell/content_point.hpp"
#include "maicord/render/text/edit_layout.hpp"
#include "maicord/ui/text/secure_text_store.hpp"

namespace maicord::app_detail {
// The native text store's rectangles for a text area given in content DIPs.
__declspec(noinline) inline bool text_store_screen_geometry(const UiRect& bounds,
                                                            SecureTextStoreGeometry* output) noexcept {
    POINT origin{};
    if (!application_content_to_screen(&origin) ||
        !GetWindowRect(g_app.window, output->window_screen.native()))
        return false;
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96.0f;
    output->text_screen = {origin.x + static_cast<LONG>(bounds.left * scale),
                           origin.y + static_cast<LONG>(bounds.top * scale),
                           origin.x + static_cast<LONG>(bounds.right * scale),
                           origin.y + static_cast<LONG>(bounds.bottom * scale)};
    return true;
}
// The union of one range's per-line selection metrics (count is at least one).
__declspec(noinline) inline UiRect text_metrics_bounds(const TextEditMetric* metrics, u32 count) noexcept {
    UiRect bounds{metrics[0].left, metrics[0].top, metrics[0].left + metrics[0].width,
                  metrics[0].top + metrics[0].height};
    for (u32 i = 1; i < count; ++i) {
        bounds.left = minimum(bounds.left, metrics[i].left);
        bounds.top = minimum(bounds.top, metrics[i].top);
        bounds.right = maximum(bounds.right, metrics[i].left + metrics[i].width);
        bounds.bottom = maximum(bounds.bottom, metrics[i].top + metrics[i].height);
    }
    return bounds;
}
} // namespace maicord::app_detail
