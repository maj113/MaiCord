#pragma once
#include "../../shell/content_point.hpp"
#include "maicord/render/text/edit_layout.hpp"
#include "maicord/ui/text/secure_text_store.hpp"

namespace maicord::app_detail {
// The native text store's rectangles for a text area given in content DIPs: client pixels, the window being the
// content area below the caption.
__declspec(noinline) inline bool text_store_geometry(const UiRect& bounds, SecureTextStoreGeometry* output) noexcept {
    if (!g_app.window)
        return false;
    const WindowMetrics client = window_metrics(g_app.window);
    const i32 caption = static_cast<i32>(g_app.renderer.caption_height);
    output->window = {0, caption, static_cast<i32>(client.width_px), static_cast<i32>(client.height_px)};
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96.0f;
    output->text = {static_cast<i32>(bounds.left * scale), caption + static_cast<i32>(bounds.top * scale),
                    static_cast<i32>(bounds.right * scale), caption + static_cast<i32>(bounds.bottom * scale)};
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
// The UTF-16 position under a native text store's client point in `layout` (text_edit_layout_point_geometry).
// Defined with the text form.
bool text_store_point_geometry(const TextEditLayoutOwner& layout, SecureTextPoint client, bool round, u32* position,
                               bool* inside) noexcept;
// The editing keys of a SecureEdit shown in `layout`: Ctrl+Z/Y/A/C/X/V, Backspace, Delete, the arrows, Home and End.
// True when *result is to be applied; false for any other key and for one with nothing to do. `history` is false
// while a text service composes or the field is secret, `clipboard` false for a secret field. A paste takes at most
// 8000 bytes, the edit's capacity if less. Defined with the text form.
bool secure_edit_command_key(SecureEdit* edit, const TextEditLayoutOwner& layout, u32 code, bool control, bool shift,
                             bool history, bool clipboard, SecureEditResult* result) noexcept;
// A pointer press (or drag, with `extend`) at the content-DIP point: false when it hits no text.
bool secure_edit_pointer_select(SecureEdit* edit, const TextEditLayoutOwner& layout, float x, float y, bool extend,
                                SecureEditResult* result) noexcept;
} // namespace maicord::app_detail
