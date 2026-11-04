// A colour bitmap glyph at a size: the strike's PNG decoded by the in-house decoder and scaled to the pixel size the
// glyph is drawn at, premultiplied, with its placement. Box averaging when shrinking (a 109 px strike drawn at 20 px
// averages about 5 by 5 source pixels per target pixel), bilinear when enlarging.
#include "maicord/core/memory.hpp"
#include "maicord/media/image/png.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"

namespace maicord {
namespace {

float floor_float(float value) noexcept {
    const i32 truncated = static_cast<i32>(value);
    return static_cast<float>(value < static_cast<float>(truncated) ? truncated - 1 : truncated);
}

float ceil_float(float value) noexcept {
    return -floor_float(-value);
}

float minimum(float a, float b) noexcept {
    return a < b ? a : b;
}

float maximum(float a, float b) noexcept {
    return a > b ? a : b;
}

// One sample of a packed or byte-aligned single-channel row at pixel `x`: 1, 2, 4, 8 or 16 bits.
u32 single_sample(const u8* row, u32 x, u32 depth) noexcept {
    if (depth == 16)
        return static_cast<u32>(row[x * 2]) << 8 | row[x * 2 + 1];
    if (depth == 8)
        return row[x];
    const u32 bit = x * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
}

// The straight-alpha RGBA pixels of a decoded PNG (red in the low byte), for every colour type and bit depth the
// format allows, with tRNS palette alphas and colour keys. Sixteen-bit samples keep their high byte.
bool expand(const PngImage& png, const u8* scanlines, u32* rgba) noexcept {
    const u32 depth = png.bit_depth;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
        return false;
    const u32 step = depth == 16 ? 2 : 1;
    for (u32 y = 0; y < png.height; ++y) {
        const u8* row = scanlines + static_cast<u64>(y) * png.row_stride + 1;
        for (u32 x = 0; x < png.width; ++x) {
            u32 r = 0, g = 0, b = 0, a = 255;
            switch (png.color_type) {
            case 0: {
                const u32 sample = single_sample(row, x, depth);
                const u32 maximum_sample = (1u << depth) - 1;
                r = g = b = depth == 16 ? sample >> 8 : sample * 255 / maximum_sample;
                if (png.has_transparency && sample == png.transparent_gray)
                    a = 0;
                break;
            }
            case 2: {
                const u8* p = row + x * png.bytes_per_pixel;
                r = p[0];
                g = p[step];
                b = p[2 * step];
                if (png.has_transparency) {
                    const u32 raw_r = depth == 16 ? static_cast<u32>(p[0]) << 8 | p[1] : p[0];
                    const u32 raw_g = depth == 16 ? static_cast<u32>(p[2]) << 8 | p[3] : p[1];
                    const u32 raw_b = depth == 16 ? static_cast<u32>(p[4]) << 8 | p[5] : p[2];
                    if (raw_r == png.transparent_red && raw_g == png.transparent_green && raw_b == png.transparent_blue)
                        a = 0;
                }
                break;
            }
            case 3: {
                const u32 index = single_sample(row, x, depth);
                if (depth == 16 || index >= png.palette_count)
                    return false;
                const u32 color = png.palette_rgba[index];
                r = color & 0xFF;
                g = (color >> 8) & 0xFF;
                b = (color >> 16) & 0xFF;
                a = color >> 24;
                break;
            }
            case 4: {
                if (depth < 8)
                    return false;
                const u8* p = row + x * png.bytes_per_pixel;
                r = g = b = p[0];
                a = p[step];
                break;
            }
            case 6: {
                if (depth < 8)
                    return false;
                const u8* p = row + x * png.bytes_per_pixel;
                r = p[0];
                g = p[step];
                b = p[2 * step];
                a = p[3 * step];
                break;
            }
            default:
                return false;
            }
            rgba[static_cast<u64>(y) * png.width + x] = r | g << 8 | b << 16 | a << 24;
        }
    }
    return true;
}

// The source samples that make target index `index` of one axis, as (first, count) and a weight per sample. `origin`
// is where the bitmap's edge sits in target pixels from the tile's edge, `scale` target pixels per source pixel and
// `extent` the source size. Weights are fractions of the target pixel; their sum is below one at the bitmap's edge.
struct Taps {
    u32 first;
    u32 count;
};

Taps taps(u32 index, float origin, float scale, u32 extent, float* weights, u32 capacity) noexcept {
    Taps result{0, 0};
    if (scale <= 1.0f) {
        const float low = maximum(0.0f, (static_cast<float>(index) - origin) / scale);
        const float high = minimum(static_cast<float>(extent), (static_cast<float>(index) + 1.0f - origin) / scale);
        if (!(high > low))
            return result;
        const u32 first = static_cast<u32>(low);
        u32 last = static_cast<u32>(ceil_float(high));
        if (last > extent)
            last = extent;
        if (last - first > capacity)
            last = first + capacity;
        for (u32 k = first; k < last; ++k) {
            const float overlap = minimum(static_cast<float>(k) + 1.0f, high) - maximum(static_cast<float>(k), low);
            weights[k - first] = overlap > 0.0f ? overlap * scale : 0.0f;
        }
        result = {first, last - first};
        return result;
    }
    const float centre = (static_cast<float>(index) + 0.5f - origin) / scale;
    if (centre < 0.0f || centre > static_cast<float>(extent))
        return result;
    const float position = centre - 0.5f;
    const float base = floor_float(position);
    const float t = position - base;
    i32 k0 = static_cast<i32>(base);
    i32 k1 = k0 + 1;
    float w0 = 1.0f - t;
    float w1 = t;
    if (k0 < 0) {
        k0 = 0;
        w0 = 1.0f;
        w1 = 0.0f;
    }
    if (k1 >= static_cast<i32>(extent)) {
        k1 = static_cast<i32>(extent) - 1;
        if (k0 > k1)
            k0 = k1;
    }
    if (k0 == k1) {
        weights[0] = 1.0f;
        result = {static_cast<u32>(k0), 1};
        return result;
    }
    weights[0] = w0;
    weights[1] = w1;
    result = {static_cast<u32>(k0), 2};
    return result;
}

u32 round_channel(float value, u32 limit) noexcept {
    const i32 rounded = static_cast<i32>(value + 0.5f);
    const u32 clamped = rounded < 0 ? 0u : static_cast<u32>(rounded);
    return clamped > limit ? limit : clamped;
}

} // namespace

SfntStatus bitmap_color_glyph_build(const CbdtFont& font, u16 glyph, float pixels_per_em, Arena* arena,
                                    BitmapColorGlyph* output) noexcept {
    if (!output)
        return SfntStatus::invalid_input;
    reset_value(output);
    if (!arena || !arena->base || !(pixels_per_em > 0.0f) || pixels_per_em > 4096.0f)
        return SfntStatus::invalid_input;
    u32 strike_index = 0;
    SfntStatus status = cbdt_select_strike(font, pixels_per_em, glyph, &strike_index);
    if (status != SfntStatus::ok)
        return status;
    CbdtStrike strike{};
    CbdtGlyph image{};
    status = cbdt_strike(font, strike_index, &strike);
    if (status != SfntStatus::ok)
        return status;
    status = cbdt_glyph(font, strike_index, glyph, &image);
    if (status != SfntStatus::ok)
        return status;
    if (image.width == 0 || image.height == 0)
        return SfntStatus::malformed;

    const float scale = pixels_per_em / static_cast<float>(strike.ppem_y);
    const float x0 = static_cast<float>(image.bearing_x) * scale;
    const float y0 = -static_cast<float>(image.bearing_y) * scale;
    const float x1 = x0 + static_cast<float>(image.width) * scale;
    const float y1 = y0 + static_cast<float>(image.height) * scale;
    const float left = floor_float(x0);
    const float top = floor_float(y0);
    const float width_value = ceil_float(x1) - left;
    const float height_value = ceil_float(y1) - top;
    if (!(width_value >= 1.0f) || !(height_value >= 1.0f) || width_value > kBitmapColorGlyphExtentMaximum ||
        height_value > kBitmapColorGlyphExtentMaximum)
        return SfntStatus::unsupported;
    const u32 width = static_cast<u32>(width_value);
    const u32 height = static_cast<u32>(height_value);
    const float origin_x = x0 - left;
    const float origin_y = y0 - top;

    // The tile is allocated first and stays; the decode and the scaling run in scratch above it that the call
    // releases. A failure releases the tile too.
    const ArenaMark start{arena->offset};
    auto* pixels = arena_allocate<u32>(arena, width * height);
    if (!pixels)
        return SfntStatus::output_too_small;
    struct Rewind {
        Arena* arena;
        ArenaMark kept;
        ArenaMark failed;
        bool succeeded;
        ~Rewind() noexcept { arena_rewind(arena, succeeded ? kept : failed); }
    } rewind{arena, ArenaMark{arena->offset}, start, false};

    auto* png = arena_allocate<PngImage>(arena);
    auto* workspace = static_cast<DeflateWorkspace*>(arena_allocate(arena, sizeof(DeflateWorkspace), 64));
    if (!png || !workspace)
        return SfntStatus::output_too_small;
    if (!png_parse(image.png, png).ok())
        return SfntStatus::malformed;
    if (png->width != image.width || png->height != image.height || png->interlace > 1)
        return SfntStatus::malformed;
    const u32 buffer_bytes = png_decode_buffer_bytes(*png);
    if (buffer_bytes == 0)
        return SfntStatus::unsupported;
    auto* scanlines = static_cast<u8*>(arena_allocate(arena, buffer_bytes, 16));
    auto* rgba = arena_allocate<u32>(arena, png->width * png->height);
    if (!scanlines || !rgba)
        return SfntStatus::output_too_small;
    if (!png_decode_parsed_scanlines(png, workspace, {scanlines, buffer_bytes}).ok())
        return SfntStatus::malformed;
    if (!expand(*png, scanlines, rgba))
        return SfntStatus::unsupported;

    // Horizontal pass: every source row to `width` premultiplied columns of four floats.
    auto* rows = arena_allocate<float>(arena, png->height * width * 4);

    if (!rows)
        return SfntStatus::output_too_small;
    // A box tap covers 1 / scale source pixels (at most 4096 / 1 for the sizes accepted); bilinear needs two.
    constexpr u32 kTapCapacity = 4400;
    auto* weights = arena_allocate<float>(arena, kTapCapacity);
    if (!weights)
        return SfntStatus::output_too_small;
    for (u32 column = 0; column < width; ++column) {
        const Taps span = taps(column, origin_x, scale, png->width, weights, kTapCapacity);
        for (u32 y = 0; y < png->height; ++y) {
            float sum[4] = {0, 0, 0, 0};
            for (u32 k = 0; k < span.count; ++k) {
                const u32 pixel = rgba[y * png->width + span.first + k];
                const float a = static_cast<float>(pixel >> 24);
                const float w = weights[k];
                sum[0] += static_cast<float>(pixel & 0xFF) * a * w;
                sum[1] += static_cast<float>((pixel >> 8) & 0xFF) * a * w;
                sum[2] += static_cast<float>((pixel >> 16) & 0xFF) * a * w;
                sum[3] += a * 255.0f * w;
            }
            float* cell = rows + (static_cast<u64>(y) * width + column) * 4;
            cell[0] = sum[0];
            cell[1] = sum[1];
            cell[2] = sum[2];
            cell[3] = sum[3];
        }
    }

    for (u32 row = 0; row < height; ++row) {
        const Taps span = taps(row, origin_y, scale, png->height, weights, kTapCapacity);
        for (u32 column = 0; column < width; ++column) {
            float sum[4] = {0, 0, 0, 0};
            for (u32 k = 0; k < span.count; ++k) {
                const float* cell = rows + (static_cast<u64>(span.first + k) * width + column) * 4;
                const float w = weights[k];
                sum[0] += cell[0] * w;
                sum[1] += cell[1] * w;
                sum[2] += cell[2] * w;
                sum[3] += cell[3] * w;
            }
            // The sums are premultiplied by a * 255 (alpha in 0..255 times colour in 0..255): back to 0..255.
            const u32 alpha = round_channel(sum[3] / 255.0f, 255);
            const u32 red = round_channel(sum[0] / 255.0f, alpha);
            const u32 green = round_channel(sum[1] / 255.0f, alpha);
            const u32 blue = round_channel(sum[2] / 255.0f, alpha);
            pixels[row * width + column] = red | green << 8 | blue << 16 | alpha << 24;
        }
    }
    output->pixels = pixels;
    output->width = width;
    output->height = height;
    output->left = static_cast<i32>(left);
    output->top = static_cast<i32>(top);
    output->advance = static_cast<float>(image.advance) * scale;
    rewind.succeeded = true;
    return SfntStatus::ok;
}

} // namespace maicord
