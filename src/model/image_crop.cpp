#include "maicord/model/image_crop.hpp"

namespace maicord {
namespace {
float clamp(float value, float low, float high) noexcept {
    return value < low ? low : value > high ? high : value;
}
bool sideways(u16 rotation) noexcept {
    return rotation % 180 != 0;
}
// The official client's layout at zoom 1: the image at the editor's height, a
// banner fitted to 17:6, and the mask the laid-out image covers.
void layout(ImageCropState* s) noexcept {
    const bool turned = sideways(s->rotation);
    const float vw = static_cast<float>(turned ? s->image_height : s->image_width);
    const float vh = static_cast<float>(turned ? s->image_width : s->image_height);
    const float h = s->editor_height > 1 ? s->editor_height : 1;
    const float w = vh > 0 ? h * vw / vh : h;
    const float widest = s->editor_width > 1 && s->editor_width < kImageCropMaximumMaskWidth
                             ? s->editor_width
                             : kImageCropMaximumMaskWidth;
    if (s->use == ImageCropUse::avatar) {
        s->base_width = w;
        s->base_height = h;
        s->mask_width = s->mask_height = w < h ? w : h;
        return;
    }
    const float crop_height = widest / kImageCropBannerAspect;
    float n = w, o = h;
    if (w > widest) {
        n = widest;
        o = widest / w * h;
    }
    if (w / h < kImageCropBannerAspect) {
        s->base_width = n;
        s->base_height = o;
    } else {
        s->base_width = crop_height / o * n;
        s->base_height = crop_height;
    }
    s->mask_width = s->base_width < widest ? s->base_width : widest;
    s->mask_height = s->mask_width / kImageCropBannerAspect;
}
} // namespace

ImageCropState image_crop_begin(ImageCropUse use, u32 image_width, u32 image_height, float editor_width,
                                float editor_height) noexcept {
    ImageCropState s{};
    s.image_width = image_width ? image_width : 1;
    s.image_height = image_height ? image_height : 1;
    s.editor_width = editor_width;
    s.editor_height = editor_height;
    s.zoom = kImageCropMinimumZoom;
    s.use = use;
    layout(&s);
    return s;
}

float image_crop_bound_x(const ImageCropState& s) noexcept {
    const float slack = (s.base_width * s.zoom - s.mask_width) * .5f;
    return slack > 0 ? slack : 0;
}
float image_crop_bound_y(const ImageCropState& s) noexcept {
    const float slack = (s.base_height * s.zoom - s.mask_height) * .5f;
    return slack > 0 ? slack : 0;
}

void image_crop_clamp(ImageCropState* s) noexcept {
    if (!s)
        return;
    s->zoom = clamp(s->zoom, kImageCropMinimumZoom, kImageCropMaximumZoom);
    const float bx = image_crop_bound_x(*s), by = image_crop_bound_y(*s);
    s->offset_x = clamp(s->offset_x, -bx, bx);
    s->offset_y = clamp(s->offset_y, -by, by);
}

void image_crop_resize(ImageCropState* s, float editor_width, float editor_height) noexcept {
    if (!s || (s->editor_width == editor_width && s->editor_height == editor_height))
        return;
    const float bx = image_crop_bound_x(*s), by = image_crop_bound_y(*s);
    const float rx = bx > 0 ? s->offset_x / bx : 0, ry = by > 0 ? s->offset_y / by : 0;
    s->editor_width = editor_width;
    s->editor_height = editor_height;
    layout(s);
    s->offset_x = rx * image_crop_bound_x(*s);
    s->offset_y = ry * image_crop_bound_y(*s);
    image_crop_clamp(s);
}

void image_crop_pan(ImageCropState* s, float dx, float dy) noexcept {
    if (!s)
        return;
    s->offset_x += dx;
    s->offset_y += dy;
    image_crop_clamp(s);
}

void image_crop_set_zoom(ImageCropState* s, float zoom) noexcept {
    if (!s)
        return;
    // The offset stays in screen units; zooming out clamps it.
    s->zoom = zoom;
    image_crop_clamp(s);
}

void image_crop_rotate(ImageCropState* s) noexcept {
    if (!s)
        return;
    s->rotation = static_cast<u16>((s->rotation + 90) % 360);
    const float x = s->offset_x;
    s->offset_x = -s->offset_y;
    s->offset_y = x;
    layout(s);
    image_crop_clamp(s);
}

void image_crop_reset(ImageCropState* s) noexcept {
    if (!s)
        return;
    s->zoom = kImageCropMinimumZoom;
    s->rotation = 0;
    s->offset_x = s->offset_y = 0;
    layout(s);
}

bool image_crop_edited(const ImageCropState& s) noexcept {
    return s.zoom != kImageCropMinimumZoom || s.rotation != 0 || s.offset_x != 0 || s.offset_y != 0;
}

ImageCropRect image_crop_image_rect(const ImageCropState& s) noexcept {
    const float w = s.base_width * s.zoom, h = s.base_height * s.zoom;
    const float cx = s.editor_width * .5f + s.offset_x, cy = s.editor_height * .5f + s.offset_y;
    return {cx - w * .5f, cy - h * .5f, cx + w * .5f, cy + h * .5f};
}

ImageCropRect image_crop_mask_rect(const ImageCropState& s) noexcept {
    const float cx = s.editor_width * .5f, cy = s.editor_height * .5f;
    return {cx - s.mask_width * .5f, cy - s.mask_height * .5f, cx + s.mask_width * .5f, cy + s.mask_height * .5f};
}

ImageCropOutput image_crop_output(const ImageCropState& s) noexcept {
    ImageCropOutput out{};
    out.rotation = s.rotation;
    const bool turned = sideways(s.rotation);
    // The laid-out element keeps the unrotated orientation (CSS size).
    const float css_w = (turned ? s.base_height : s.base_width) * s.zoom;
    const float css_h = (turned ? s.base_width : s.base_height) * s.zoom;
    if (!(css_w > 0) || !(css_h > 0))
        return out;
    const float c = static_cast<float>(s.image_width) / css_w;
    float dx = s.offset_x, dy = s.offset_y;
    switch (s.rotation) {
    case 90: dx = s.offset_y, dy = -s.offset_x; break;
    case 180: dx = -s.offset_x, dy = -s.offset_y; break;
    case 270: dx = -s.offset_y, dy = s.offset_x; break;
    default: break;
    }
    const float hw = (turned ? s.mask_height : s.mask_width) * .5f;
    const float hh = (turned ? s.mask_width : s.mask_height) * .5f;
    const float sx = (css_w * .5f - hw - dx) * c, sy = (css_h * .5f - hh - dy) * c;
    const float sw = hw * 2 * c, sh = hh * 2 * c;
    const float iw = static_cast<float>(s.image_width), ih = static_cast<float>(s.image_height);
    const float x = clamp(static_cast<float>(static_cast<i64>(sx + .5f)), 0, iw - 1);
    const float y = clamp(static_cast<float>(static_cast<i64>(sy + .5f)), 0, ih - 1);
    const float w = clamp(static_cast<float>(static_cast<i64>(sw + .5f)), 1, iw - x);
    const float h = clamp(static_cast<float>(static_cast<i64>(sh + .5f)), 1, ih - y);
    out.x = static_cast<u32>(x);
    out.y = static_cast<u32>(y);
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    const u32 visual_w = turned ? out.height : out.width, visual_h = turned ? out.width : out.height;
    const u32 max_w = s.use == ImageCropUse::avatar ? kImageCropAvatarMaximum : kImageCropBannerMaximumWidth;
    const u32 max_h = s.use == ImageCropUse::avatar ? kImageCropAvatarMaximum : kImageCropBannerMaximumHeight;
    out.output_width = visual_w < max_w ? visual_w : max_w;
    out.output_height = visual_h < max_h ? visual_h : max_h;
    return out;
}

void image_crop_unrotate(u16 rotation, float a, float b, float* p, float* q) noexcept {
    float u = a, v = b;
    switch (rotation) {
    case 90: u = b, v = 1 - a; break;
    case 180: u = 1 - a, v = 1 - b; break;
    case 270: u = 1 - b, v = a; break;
    default: break;
    }
    if (p)
        *p = u;
    if (q)
        *q = v;
}

} // namespace maicord
