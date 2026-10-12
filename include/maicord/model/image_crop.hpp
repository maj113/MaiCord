#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

// The official client's "Edit Image" cropper for avatar and profile banner
// uploads. Geometry follows the web client: the image is laid out at the editor's
// height (banners then fitted to 17:6), the mask is the largest shape of the
// use that the laid-out image covers (avatars: its shorter side, banners: at
// most 432 DIP wide at 17:6), zoom runs 1-2 about the image's own centre and
// the image centre's offset from the mask centre is clamped so the mask is
// always covered. Rotation turns the image 90 degrees clockwise per step.
// All lengths are DIPs except the source rectangle (image pixels).
enum class ImageCropUse : u8 {
    avatar,
    banner,
};
inline constexpr float kImageCropMinimumZoom = 1.0f;
inline constexpr float kImageCropMaximumZoom = 2.0f;
// Widest mask (the official client's 432 px) and the profile banner aspect (17:6).
inline constexpr float kImageCropMaximumMaskWidth = 432.0f;
inline constexpr float kImageCropBannerAspect = 17.0f / 6.0f;
// Editor height: 350, or 200 when the window is 800 DIP tall or less.
inline constexpr float kImageCropEditorHeight = 350.0f;
inline constexpr float kImageCropShortEditorHeight = 200.0f;
inline constexpr float kImageCropShortViewport = 800.0f;
// Arrow keys pan 4 DIP, 40 with Shift.
inline constexpr float kImageCropKeyStep = 4.0f;
inline constexpr float kImageCropFastKeyStep = 40.0f;
// Still output caps (per axis, never upscaled): avatars 1024x1024, banners 2400x848.
inline constexpr u32 kImageCropAvatarMaximum = 1024;
inline constexpr u32 kImageCropBannerMaximumWidth = 2400;
inline constexpr u32 kImageCropBannerMaximumHeight = 848;

struct ImageCropState {
    // Source pixels in display orientation (EXIF applied), before rotation.
    u32 image_width, image_height;
    // The editor area; the mask is centred in it.
    float editor_width, editor_height;
    // Laid-out image at zoom 1 (rotated orientation) and the mask.
    float base_width, base_height, mask_width, mask_height;
    float zoom, offset_x, offset_y;
    u16 rotation;
    ImageCropUse use;
};
struct ImageCropRect {
    float left, top, right, bottom;
};
// Source rectangle in unrotated image pixels and the output size in the
// rotated (visual) orientation.
struct ImageCropOutput {
    u32 x, y, width, height;
    u32 output_width, output_height;
    u16 rotation;
};

inline float image_crop_editor_height(float viewport_height) noexcept {
    return viewport_height <= kImageCropShortViewport ? kImageCropShortEditorHeight : kImageCropEditorHeight;
}
// Lays out a newly picked image at zoom 1, centred.
ImageCropState image_crop_begin(ImageCropUse use, u32 image_width, u32 image_height, float editor_width,
                                float editor_height) noexcept;
// Re-lays out for a new editor size, keeping zoom, rotation and the offset
// ratio (the official client only refits at zoom 1; a ratio keeps the view stable).
void image_crop_resize(ImageCropState* state, float editor_width, float editor_height) noexcept;
// Largest offsets that keep the mask covered (>= 0 per axis).
float image_crop_bound_x(const ImageCropState& state) noexcept;
float image_crop_bound_y(const ImageCropState& state) noexcept;
void image_crop_clamp(ImageCropState* state) noexcept;
void image_crop_pan(ImageCropState* state, float dx, float dy) noexcept;
void image_crop_set_zoom(ImageCropState* state, float zoom) noexcept;
void image_crop_rotate(ImageCropState* state) noexcept;
void image_crop_reset(ImageCropState* state) noexcept;
// Zoom, rotation or offset differ from the initial layout (Reset enabled).
bool image_crop_edited(const ImageCropState& state) noexcept;
// Visual rectangles relative to the editor's top-left: the drawn image
// (rotated orientation) and the mask.
ImageCropRect image_crop_image_rect(const ImageCropState& state) noexcept;
ImageCropRect image_crop_mask_rect(const ImageCropState& state) noexcept;
// The official client's crop math (canvas crop of the laid-out image): the source
// rectangle under the mask, rounded and clamped inside the image, and the
// output size capped per axis for the use (never upscaled).
ImageCropOutput image_crop_output(const ImageCropState& state) noexcept;
// Maps a point in the output (visual) image, normalised to [0,1], to the
// unrotated crop rectangle, normalised to [0,1].
void image_crop_unrotate(u16 rotation, float a, float b, float* p, float* q) noexcept;

} // namespace maicord
