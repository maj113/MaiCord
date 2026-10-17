#pragma once
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/button_keyboard.hpp"
namespace maicord {
// Toolbar/gallery button indices.
enum UiImageViewerButton : u32 {
    ui_image_viewer_zoom_out,
    ui_image_viewer_zoom_in,
    ui_image_viewer_fit,
    ui_image_viewer_close,
    ui_image_viewer_save,
    ui_image_viewer_open_original,
    ui_image_viewer_copy,
    ui_image_viewer_previous,
    ui_image_viewer_next,
    ui_image_viewer_button_count
};
// Optional actions; each hidden action is unfocusable and paints nothing.
enum UiImageViewerAction : u8 {
    ui_image_viewer_action_save = 1u << 0,
    ui_image_viewer_action_open = 1u << 1,
    ui_image_viewer_action_copy = 1u << 2,
};
inline constexpr float kUiImageViewerScrimOpacity = .92f;
// Reusable modal media surface. Image ownership and decoding remain external.
struct UiImageViewer {
    UiWidgetManager widgets;
    UiWidget widget_storage[ui_image_viewer_button_count + 1];
    UiLayoutNode layout_storage[ui_image_viewer_button_count + 1];
    UiWidgetHandle buttons[ui_image_viewer_button_count];
    UiButtonKeyboard keyboard;
    UiWidgetReference owner;
    // `image` is the settled layout; `display` follows the open/close motion
    // from `origin` (the inline preview) and is what media/video paint into.
    UiRect viewport, body, image, display, origin;
    UiSize pixels;
    UiPoint pan, pointer;
    float zoom, reveal;
    u32 revision;
    u8 focus, item, item_count, actions;
    bool active, dragging, keyboard_focus, can_save, video;
    bool copied; // Copy image confirmation: the action shows a check briefly.
    char footer[128];
    u32 footer_bytes;
};
bool ui_image_viewer_open(UiImageViewer* viewer, UiRect viewport, UiSize pixels) noexcept;
bool ui_image_viewer_layout(UiImageViewer* viewer, UiRect viewport) noexcept;
bool ui_image_viewer_set_save_available(UiImageViewer* viewer, bool available) noexcept;
// Optional Open original / Copy image actions (Save keeps its own setter).
bool ui_image_viewer_set_actions(UiImageViewer* viewer, u8 actions) noexcept;
// Gallery position: resets zoom/pan for the new item; videos have no zoom.
bool ui_image_viewer_set_item(UiImageViewer* viewer, UiSize pixels, u32 item, u32 count, bool video) noexcept;
// Open/close progress in [0, 1]; scales the scrim and interpolates `display`.
void ui_image_viewer_set_reveal(UiImageViewer* viewer, float reveal, UiRect origin) noexcept;
void ui_image_viewer_zoom(UiImageViewer* viewer, float factor, UiPoint anchor) noexcept;
void ui_image_viewer_pan(UiImageViewer* viewer, UiPoint delta) noexcept;
UiRect ui_image_viewer_button(const UiImageViewer& viewer, u32 index) noexcept;
// Scrim and toolbar, painted below the media quad.
bool ui_image_viewer_paint(const UiImageViewer& viewer, const Theme& theme, PaintStream* paint) noexcept;
// Gallery chevrons, painted above the media quad.
bool ui_image_viewer_paint_overlay(const UiImageViewer& viewer, const Theme& theme, PaintStream* paint) noexcept;
bool ui_image_viewer_text_output(const UiImageViewer& v, const Theme& theme, UiTextOutput* text) noexcept;
} // namespace maicord
