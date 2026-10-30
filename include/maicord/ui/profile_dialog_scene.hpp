#pragma once
#include "maicord/model/image_crop.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/button_keyboard.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct Theme;

// Modal dialogs of the profile editor, drawn above its settings page:
// the "Edit Image" cropper for avatar and banner uploads, and the
// avatar decoration / profile effect pickers. The host owns all values and
// requests; the scene is a retained projection with hit testing.
enum class UiProfileDialogKind : u8 {
    none,
    crop,
    decoration,
    effect,
};
enum class UiProfileDialogAction : u8 {
    none,
    close,
    cancel,
    apply,
    rotate,
    reset,
    // The zoom slider track or thumb (drag through ui_profile_dialog_zoom_at).
    zoom,
    // The crop viewport (drag pans the image).
    pan,
    // A picker tile; the part's source is the item index.
    item,
};

// One picker tile: zero sku is "None". The thumbnail comes from the CDN.
struct UiProfileDialogItem {
    u64 sku;
    Utf8View label;
    ProfileString<384> url;
    MediaAssetKey key;
    bool animated, owned;
};
inline constexpr u32 kUiProfileDialogItemCapacity = 96;

struct UiProfileDialogContent {
    UiProfileDialogKind kind;
    Utf8View title, apply_label;
    // Crop: the picked image in the preview atlas and the crop state. The
    // scene lays out the viewport and writes it back through
    // ui_profile_dialog_crop_viewport; the host keeps zoom and offset.
    MediaAssetKey image;
    ImageCropState crop;
    bool image_ready;
    // Pickers: tiles and the selected one; the preview shows the avatar
    // (and its decoration) or the effect over a mini profile card.
    ConstSlice<UiProfileDialogItem> items;
    u32 selected;
    // Pickers: the avatar the decoration previews on (url + key, circular)
    // and the label under the large preview.
    ProfileMedia avatar;
    Utf8View preview_label;
    Utf8View status;
    bool busy, failed, can_apply, can_reset;
};

struct UiProfileDialogPart {
    UiWidgetHandle widget;
    Utf8View text;
    UiProfileDialogAction action;
    u16 source;
    u32 style;
    u8 role;
};
// The most parts any dialog lays out: a picker's frame, grid, section header,
// at most 7 rows of 3 visible tiles, previews, status and buttons come to
// about 35.
inline constexpr u32 kUiProfileDialogPartCapacity = 64;

struct UiProfileDialogScene {
    UiWidgetManager widgets;
    UiWidget widget_storage[kUiProfileDialogPartCapacity + 4];
    UiLayoutNode node_storage[kUiProfileDialogPartCapacity + 4];
    UiProfileDialogPart parts[kUiProfileDialogPartCapacity];
    u32 part_count;
    UiProfileDialogContent content;
    UiTextMeasureCache* measure;
    UiButtonKeyboard keyboard;
    UiRect viewport, rectangle, crop_viewport, slider_track, grid_viewport;
    float grid_scroll, grid_height;
    u32 revision;
    bool active, focus_visible, failed; // failed: a part could not be added during the last update
};

// The crop editor area for a viewport: the host lays its ImageCropState out
// in this size (image_crop_begin / image_crop_resize) before updating.
UiRect ui_profile_dialog_crop_editor(UiRect viewport) noexcept;
// Opens (or refreshes) the dialog for `content`; strings and items are
// borrowed until the next update.
bool ui_profile_dialog_update(UiProfileDialogScene* scene, const UiProfileDialogContent& content,
                              UiRect viewport) noexcept;
void ui_profile_dialog_close(UiProfileDialogScene* scene) noexcept;
UiWidgetHandle ui_profile_dialog_hit_test(const UiProfileDialogScene& scene, float x, float y) noexcept;
UiProfileDialogAction ui_profile_dialog_action(const UiProfileDialogScene& scene, UiWidgetHandle widget,
                                               u32* source = nullptr) noexcept;
// Zoom slider value in [0, 1] at a pointer x.
float ui_profile_dialog_zoom_at(const UiProfileDialogScene& scene, float x) noexcept;
bool ui_profile_dialog_scroll(UiProfileDialogScene* scene, float delta) noexcept;
// Media in draw order: index runs from zero until false.
bool ui_profile_dialog_media(const UiProfileDialogScene& scene, u32 index, ProfileMedia* output) noexcept;
bool ui_profile_dialog_paint(const UiProfileDialogScene& scene, const Theme& theme, PaintStream* paint) noexcept;
// Over the media: crop mask edge, slider and selection rings.
bool ui_profile_dialog_foreground(const UiProfileDialogScene& scene, const Theme& theme,
                                  PaintStream* paint) noexcept;
bool ui_profile_dialog_text_output(const UiProfileDialogScene& scene, const Theme& theme,
                                   UiTextOutput* output) noexcept;

} // namespace maicord
