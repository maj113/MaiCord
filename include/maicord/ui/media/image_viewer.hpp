#pragma once
#include "maicord/ui/widgets/button_keyboard.hpp"
#include "maicord/ui/widgets/icon.hpp"
namespace maicord {
struct PaintStream;
struct Theme;

struct UiTextOutput;
// Toolbar/gallery button indices.
enum UiImageViewerButton : u32 {
    ui_image_viewer_zoom_out,
    ui_image_viewer_zoom_in,
    ui_image_viewer_fit,
    ui_image_viewer_close,
    ui_image_viewer_save,
    ui_image_viewer_open_original,
    ui_image_viewer_copy,
    ui_image_viewer_fullscreen,
    ui_image_viewer_previous,
    ui_image_viewer_next,
    // Page mode only: the header's Jump to message and the unavailable state's Retry.
    ui_image_viewer_jump,
    ui_image_viewer_retry,
    ui_image_viewer_button_count
};
// Optional actions; each hidden action is unfocusable and paints nothing.
enum UiImageViewerAction : u8 {
    ui_image_viewer_action_open = 1u << 1,
    ui_image_viewer_action_copy = 1u << 2,
};
enum class UiImageViewerMedia : u8 { ready, loading, failed };
enum class UiImageViewerPageKind : u8 { image, video, audio };
// A content-area page (media tab): the strings are borrowed from the page and outlive the viewer's use of them.
struct UiImageViewerPage {
    Utf8View title, source;
    UiIcon glyph;
    UiImageViewerPageKind kind;
    // Jump to message shows disabled ("Message unavailable") without it; a playlist reserves the queue column.
    bool can_jump, queue;
};
// Page regions: the header band; the player (the picture, or the stage the audio card centres in); the transport
// band under it (video and audio); the queue column beside them (a playlist, when the page is wide enough).
struct UiImageViewerPageLayout {
    UiRect header, player, transport, queue;
};
// Zoom bounds relative to fit, and one wheel notch (120 units).
inline constexpr float kUiImageViewerMaximumZoom = 16, kUiImageViewerWheelStep = 1.25f;
// Reusable media surface: a modal viewer, or a page (ui_image_viewer_set_page). Image ownership and decoding
// remain external.
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
    // zoom is relative to fit; fit is DIPs per source pixel when fitted.
    float zoom, reveal, fit, dpi_scale;
    // Opacity of the toolbar, chevrons and labels; idle video playback hides them.
    float chrome;
    u32 revision;
    u8 focus, item, item_count, actions, spinner_phase;
    UiImageViewerMedia media;
    bool active, dragging, keyboard_focus, can_save, video, fullscreen;
    // Page mode: not modal, theme colours, a header band in place of the scrim's toolbar row.
    bool page;
    UiImageViewerPage page_info;
    bool copied; // Copy image confirmation: the action shows a check briefly.
    char counter[16], caption[160];
    u32 counter_bytes, caption_bytes;
};
bool ui_image_viewer_open(UiImageViewer* viewer, const UiRect& viewport, UiSize pixels) noexcept;
bool ui_image_viewer_layout(UiImageViewer* viewer, const UiRect& viewport) noexcept;
// Device pixels per DIP; actual size and 1:1 pixel snapping follow it.
bool ui_image_viewer_set_dpi(UiImageViewer* viewer, u32 dpi) noexcept;
bool ui_image_viewer_set_save_available(UiImageViewer* viewer, bool available) noexcept;
// Optional Open original / Copy image actions (Save keeps its own setter).
bool ui_image_viewer_set_actions(UiImageViewer* viewer, u8 actions) noexcept;
// Gallery position: resets zoom/pan for the new item; videos have no zoom.
bool ui_image_viewer_set_item(UiImageViewer* viewer, UiSize pixels, u32 item, u32 count, bool video) noexcept;
// Footer caption (alt text or file name); truncated, never owned by the caller.
bool ui_image_viewer_set_caption(UiImageViewer* viewer, Utf8View caption) noexcept;
bool ui_image_viewer_set_media(UiImageViewer* viewer, UiImageViewerMedia media) noexcept;
bool ui_image_viewer_set_chrome(UiImageViewer* viewer, float opacity) noexcept;
bool ui_image_viewer_set_fullscreen(UiImageViewer* viewer, bool fullscreen) noexcept;
// Open/close progress in [0, 1]; scales the scrim and interpolates `display`.
void ui_image_viewer_set_reveal(UiImageViewer* viewer, float reveal, const UiRect& origin) noexcept;
// Turns an open viewer into a page (single item, no gallery, no full screen); its header shows `page`.
bool ui_image_viewer_set_page(UiImageViewer* viewer, const UiImageViewerPage& page) noexcept;
UiImageViewerPageLayout ui_image_viewer_page_layout(const UiImageViewer& viewer) noexcept;
// Re-applies a zoom (relative to fit) and pan kept from an earlier visit; both are clamped as the viewer's own.
void ui_image_viewer_set_view(UiImageViewer* viewer, float zoom, UiPoint pan) noexcept;
void ui_image_viewer_zoom(UiImageViewer* viewer, float factor, UiPoint anchor) noexcept;
// Wheel delta in WHEEL_DELTA units: one notch zooms by kUiImageViewerWheelStep,
// smaller (precision touchpad) deltas by the matching fraction of it.
void ui_image_viewer_wheel(UiImageViewer* viewer, i32 delta, UiPoint anchor) noexcept;
// Zoom at which one source pixel covers one device pixel.
float ui_image_viewer_actual_zoom(const UiImageViewer& viewer) noexcept;
// Double-click: fit toggles to actual size when that is larger (else twice
// fit), anchored at the pointer; any other zoom returns to fit.
void ui_image_viewer_toggle_zoom(UiImageViewer* viewer, UiPoint anchor) noexcept;
void ui_image_viewer_pan(UiImageViewer* viewer, UiPoint delta) noexcept;
UiRect ui_image_viewer_button(const UiImageViewer& viewer, u32 index) noexcept;
// Tooltip text: the action and its shortcut.
Utf8View ui_image_viewer_tooltip(const UiImageViewer& viewer, u32 index) noexcept;
bool ui_image_viewer_spinner_visible(const UiImageViewer& viewer) noexcept;
// Media fades with the reveal only when there is no inline preview to travel to.
float ui_image_viewer_media_opacity(const UiImageViewer& viewer) noexcept;
// Gallery neighbour of item in direction, wrapping at both ends.
constexpr u32 ui_image_viewer_step(u32 item, u32 count, i32 direction) noexcept {
    return count ? static_cast<u32>((static_cast<i64>(item % count) + count + direction % static_cast<i32>(count)) %
                                    count)
                 : 0;
}

// Keyboard commands; the host applies them and owns platform side effects.
enum class UiImageViewerCommand : u8 {
    none,
    dismiss,
    exit_fullscreen,
    toggle_fullscreen,
    previous,
    next,
    zoom_in,
    zoom_out,
    fit,
    actual_size,
    pan_left,
    pan_right,
    pan_up,
    pan_down,
    save,
    copy,
    focus_next,
    focus_previous,
    activate,
    video, // the hosted player handles the key (Space, M, digits, arrows)
};
enum class UiImageViewerKey : u8 {
    escape,
    tab,
    enter,
    space,
    left,
    right,
    up,
    down,
    home,
    plus,
    minus,
    zero,
    one,
    f,
    s,
    c,
    other,
};
struct UiImageViewerKeyModifiers {
    bool control, shift, alt;
};
UiImageViewerCommand ui_image_viewer_key_command(const UiImageViewer& viewer, UiImageViewerKey key,
                                                 UiImageViewerKeyModifiers modifiers) noexcept;
// Applies a geometry command (zoom, fit, actual size, pan); others return false.
bool ui_image_viewer_apply(UiImageViewer* viewer, UiImageViewerCommand command) noexcept;

// Scrim and toolbar, painted below the media quad.
bool ui_image_viewer_paint(const UiImageViewer& viewer, const Theme& theme, PaintStream* paint) noexcept;
// Gallery chevrons and the loading/error state, painted above the media quad.
bool ui_image_viewer_paint_overlay(const UiImageViewer& viewer, const Theme& theme, PaintStream* paint) noexcept;
bool ui_image_viewer_text_output(const UiImageViewer& v, const Theme& theme, UiTextOutput* text) noexcept;
} // namespace maicord
