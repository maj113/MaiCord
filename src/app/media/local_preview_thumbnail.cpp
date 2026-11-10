#include "maicord/app/media/local_preview_thumbnail.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
void emit_row(LocalThumbnailSink* sink) noexcept {
    const u32 out_y = (sink->row - 1) / sink->scale;
    u8* target = sink->output + static_cast<usize>(out_y) * sink->width * 4u;
    for (u32 x = 0; x < sink->width; ++x) {
        u32* sum = sink->sums + x * 5u;
        const u32 samples = sum[4];
        const u32 alpha = sum[3];
        target[x * 4u + 3] = static_cast<u8>((alpha + samples / 2) / samples);
        for (u32 c = 0; c < 3; ++c)
            target[x * 4u + c] = alpha ? static_cast<u8>((sum[c] + alpha / 2) / alpha) : 0;
        for (u32 c = 0; c < 5; ++c)
            sum[c] = 0;
    }
}
u8 sample_bits(const u8* row, u32 index, u32 depth) noexcept {
    const u32 bit = index * depth;
    const u32 shift = 8u - depth - (bit & 7u);
    return static_cast<u8>(row[bit >> 3] >> shift & ((1u << depth) - 1u));
}
} // namespace

bool local_thumbnail_begin(LocalThumbnailSink* sink, u32 source_width, u32 source_height, u32 extent,
                           Slice<u8> output, Slice<u32> sums) noexcept {
    if (!sink || !source_width || !source_height || !extent)
        return false;
    const u32 scale = local_thumbnail_scale(source_width, source_height, extent);
    const u32 width = local_thumbnail_extent(source_width, scale);
    const u32 height = local_thumbnail_extent(source_height, scale);
    if (!output.data || !sums.data || static_cast<u64>(width) * height * 4u > output.count ||
        static_cast<u64>(width) * 5u > sums.count || scale > 200)
        return false;
    *sink = {output.data, sums.data, source_width, source_height, scale, width, height, 0};
    clear_bytes(sums.data, width * 5u * sizeof(u32));
    return true;
}

bool local_thumbnail_push_row(LocalThumbnailSink* sink, const u8* rgba) noexcept {
    if (!sink || !sink->output || !rgba || sink->row >= sink->source_height)
        return false;
    if (sink->scale == 1) {
        u8* target = sink->output + static_cast<usize>(sink->row) * sink->width * 4u;
        for (u32 x = 0; x < sink->width; ++x) {
            const u8* pixel = rgba + x * 4u;
            const u8 alpha = pixel[3];
            target[x * 4u] = alpha ? pixel[0] : 0;
            target[x * 4u + 1] = alpha ? pixel[1] : 0;
            target[x * 4u + 2] = alpha ? pixel[2] : 0;
            target[x * 4u + 3] = alpha;
        }
        // begin() and each complete row leave the sums clear at scale 1.
        ++sink->row;
        return true;
    }
    // Each horizontal group uses one destination, including the final partial group.
    u32 column = 0;
    for (u32 x = 0; x < sink->source_width; ++column) {
        const u32 remaining = sink->source_width - x;
        const u32 count = remaining < sink->scale ? remaining : sink->scale;
        u32* sum = sink->sums + column * 5u;
        for (u32 i = 0; i < count; ++i, ++x) {
            const u8* pixel = rgba + x * 4u;
            const u32 alpha = pixel[3];
            sum[0] += pixel[0] * alpha;
            sum[1] += pixel[1] * alpha;
            sum[2] += pixel[2] * alpha;
            sum[3] += alpha;
            ++sum[4];
        }
    }
    ++sink->row;
    if (sink->row % sink->scale == 0 || sink->row == sink->source_height)
        emit_row(sink);
    return true;
}

bool local_thumbnail_png_row(const PngImage& image, const u8* row, u8* rgba) noexcept {
    if (!row || !rgba || !image.width)
        return false;
    const u32 depth = image.bit_depth;
    const bool wide = depth == 16;
    for (u32 x = 0; x < image.width; ++x) {
        u8* out = rgba + x * 4u;
        switch (image.color_type) {
        case 0: { // Grayscale; tRNS compares the raw sample.
            u32 raw = 0;
            u8 value = 0;
            if (wide) {
                raw = static_cast<u32>(row[x * 2]) << 8 | row[x * 2 + 1];
                value = row[x * 2];
            } else if (depth == 8) {
                raw = value = row[x];
            } else {
                raw = sample_bits(row, x, depth);
                value = static_cast<u8>(raw * 255u / ((1u << depth) - 1u));
            }
            out[0] = out[1] = out[2] = value;
            out[3] = raw == image.transparent_gray ? 0 : 255;
            break;
        }
        case 2: {
            const u32 stride = wide ? 6u : 3u;
            const u8* p = row + x * stride;
            u32 r = p[0], g = wide ? p[2] : p[1], b = wide ? p[4] : p[2];
            out[0] = static_cast<u8>(r);
            out[1] = static_cast<u8>(g);
            out[2] = static_cast<u8>(b);
            if (wide) {
                r = static_cast<u32>(p[0]) << 8 | p[1];
                g = static_cast<u32>(p[2]) << 8 | p[3];
                b = static_cast<u32>(p[4]) << 8 | p[5];
            }
            out[3] = r == image.transparent_red && g == image.transparent_green && b == image.transparent_blue ? 0 : 255;
            break;
        }
        case 3: {
            const u32 index = depth == 8 ? row[x] : sample_bits(row, x, depth);
            if (index >= image.palette_count)
                return false;
            const u32 color = image.palette_rgba[index];
            out[0] = static_cast<u8>(color);
            out[1] = static_cast<u8>(color >> 8);
            out[2] = static_cast<u8>(color >> 16);
            out[3] = static_cast<u8>(color >> 24);
            break;
        }
        case 4: {
            const u8* p = row + x * (wide ? 4u : 2u);
            out[0] = out[1] = out[2] = p[0];
            out[3] = wide ? p[2] : p[1];
            break;
        }
        case 6: {
            const u8* p = row + x * (wide ? 8u : 4u);
            for (u32 c = 0; c < 4; ++c)
                out[c] = wide ? p[c * 2] : p[c];
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

bool local_thumbnail_gif_row(const GifFrame& frame, const GifPalette& palette, ConstSlice<u8> indices, u32 canvas_width,
                             u32 y, u8* rgba) noexcept {
    if (!rgba || !canvas_width || (!indices.data && indices.count))
        return false;
    clear_bytes(rgba, canvas_width * 4u);
    if (y < frame.y_offset || y - frame.y_offset >= frame.height)
        return true;
    const u32 frame_y = y - frame.y_offset;
    for (u32 x = 0; x < frame.width && frame.x_offset + x < canvas_width; ++x) {
        const u64 at = static_cast<u64>(frame_y) * frame.width + x;
        if (at >= indices.count)
            return false;
        const u32 index = indices[static_cast<u32>(at)];
        if (index >= 256)
            return false;
        const u32 color = palette.rgba[index];
        u8* out = rgba + (frame.x_offset + x) * 4u;
        out[0] = static_cast<u8>(color);
        out[1] = static_cast<u8>(color >> 8);
        out[2] = static_cast<u8>(color >> 16);
        out[3] = static_cast<u8>(color >> 24);
    }
    return true;
}

bool local_thumbnail_orient(Slice<u8> pixels, u32* width, u32* height, u8 orientation, Slice<u8> scratch) noexcept {
    if (orientation < 1 || orientation > 8)
        return false;
    const u32 w = *width, h = *height;
    const u64 bytes = static_cast<u64>(w) * h * 4u;
    if (orientation == 1)
        return bytes <= pixels.count;
    if (!pixels.data || !scratch.data || bytes > pixels.count || bytes > scratch.count)
        return false;
    copy_bytes(scratch.data, pixels.data, static_cast<u32>(bytes));
    const bool swap = orientation >= 5;
    const u32 out_w = swap ? h : w, out_h = swap ? w : h;
    for (u32 y = 0; y < out_h; ++y)
        for (u32 x = 0; x < out_w; ++x) {
            u32 sx = x, sy = y;
            switch (orientation) {
            case 2: sx = w - 1 - x; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            case 8: sx = w - 1 - y; sy = x; break;
            default: break;
            }
            copy_bytes(pixels.data + (static_cast<usize>(y) * out_w + x) * 4u,
                       scratch.data + (static_cast<usize>(sy) * w + sx) * 4u, 4);
        }
    *width = out_w;
    *height = out_h;
    return true;
}

namespace {
// Where fitted cell `cell` of `cells` starts over `source` texels, in 1/256 texels.
u64 fit_edge(u32 cell, u32 source, u32 cells) noexcept {
    return u64{cell} * source * 256u / cells;
}
// The fitted cell the source texel at `position` starts in (by the rounded edges above).
u32 fit_cell(u32 position, u32 source, u32 cells) noexcept {
    const u32 cell = static_cast<u32>(u64{position} * cells / source);
    return fit_edge(cell + 1, source, cells) <= u64{position} * 256u ? cell + 1 : cell;
}
// Writes the fitted row sums[0] holds and moves the next one up.
void fit_emit(MediaFitSink* s) noexcept {
    const u32 r = s->cell_row, cells = s->region.width;
    const u64 rows = fit_edge(r + 1, s->fit.source_height, s->fit.height) - fit_edge(r, s->fit.source_height, s->fit.height);
    u8* target = s->output + static_cast<usize>(r - s->region.y) * s->stride + s->lead;
    for (u32 i = 0; i < cells; ++i) {
        const u32 c = s->region.x + i;
        const u64 area = (fit_edge(c + 1, s->fit.source_width, s->fit.width) - fit_edge(c, s->fit.source_width, s->fit.width)) * rows;
        const u64* sum = s->sums + i * 4u;
        const u64 alpha = sum[0];
        u8* out = target + i * 4u;
        // Uncovered area (outside a frame's region) counts as clear.
        const u64 coverage = (alpha + area / 2u) / area;
        out[3] = static_cast<u8>(coverage < 255u ? coverage : 255u);
        for (u32 k = 0; k < 3; ++k)
            out[k] = alpha ? static_cast<u8>((sum[k + 1] + alpha / 2u) / alpha) : 0;
    }
    copy_bytes(s->sums, s->sums + cells * 4u, cells * 4u * sizeof(u64));
    clear_bytes(s->sums + cells * 4u, cells * 4u * sizeof(u64));
    ++s->cell_row;
}
void fit_add(u64* sum, const u8* pixel, u64 weight) noexcept {
    const u64 weighted = pixel[3] * weight;
    sum[0] += weighted;
    sum[1] += weighted * pixel[0];
    sum[2] += weighted * pixel[1];
    sum[3] += weighted * pixel[2];
}
} // namespace

bool media_fit_plan(u32 source_width, u32 source_height, u32 box_width, u32 box_height, MediaFit* fit) noexcept {
    if (!fit || !source_width || !source_height || !box_width || !box_height ||
        (source_width <= box_width && source_height <= box_height))
        return false;
    u32 width = box_width, height = box_height;
    if (u64{source_width} * box_height >= u64{source_height} * box_width)
        height = static_cast<u32>((u64{source_height} * box_width + source_width / 2u) / source_width);
    else
        width = static_cast<u32>((u64{source_width} * box_height + source_height / 2u) / source_height);
    *fit = {source_width, source_height, width ? width : 1u, height ? height : 1u};
    return true;
}

MediaFitRegion media_fit_region(const MediaFit& fit, u32 x, u32 y, u32 width, u32 height) noexcept {
    const u32 left = fit_cell(x, fit.source_width, fit.width);
    const u32 top = fit_cell(y, fit.source_height, fit.height);
    const u32 right = static_cast<u32>((u64{x + width} * fit.width + fit.source_width - 1u) / fit.source_width);
    const u32 bottom = static_cast<u32>((u64{y + height} * fit.height + fit.source_height - 1u) / fit.source_height);
    return {left, top, right - left, bottom - top};
}

bool media_fit_begin(MediaFitSink* sink, const MediaFit& fit, MediaFitRegion source, u8* output, u32 stride, u32 lead,
                     Slice<u8> scratch) noexcept {
    if (!output || !fit.width || !fit.height || fit.width > fit.source_width ||
        fit.height > fit.source_height || !source.width || !source.height || source.x >= fit.source_width ||
        source.width > fit.source_width - source.x || source.y >= fit.source_height ||
        source.height > fit.source_height - source.y)
        return false;
    const MediaFitRegion region = media_fit_region(fit, source.x, source.y, source.width, source.height);
    if (u64{lead} + u64{region.width} * 4u > stride || !scratch.data ||
        (reinterpret_cast<usize>(scratch.data) & 7u) != 0 ||
        media_fit_scratch_bytes(source.width, region.width) > scratch.count)
        return false;
    auto* sums = reinterpret_cast<u64*>(scratch.data);
    auto* columns = reinterpret_cast<u32*>(sums + region.width * 8u);
    clear_bytes(sums, region.width * 8u * sizeof(u64));
    for (u32 x = 0; x < source.width; ++x) {
        const u32 column = source.x + x;
        const u32 cell = fit_cell(column, fit.source_width, fit.width);
        const u64 end = fit_edge(cell + 1, fit.source_width, fit.width), start = u64{column} * 256u;
        columns[x] = (cell - region.x) << 9 | (end >= start + 256u ? 256u : static_cast<u32>(end - start));
    }
    *sink = {fit, source, region, output, stride, lead, sums, columns, 0, region.y};
    return true;
}

bool media_fit_push_row(MediaFitSink* s, const u8* rgba) noexcept {
    if (!s->output || !rgba || s->row >= s->source.height)
        return false;
    const u32 y = s->source.y + s->row;
    if (fit_cell(y, s->fit.source_height, s->fit.height) != s->cell_row)
        return false;
    const u64 top = u64{y} * 256u, end = fit_edge(s->cell_row + 1, s->fit.source_height, s->fit.height);
    // A source row splits between at most two fitted rows, a texel between at most two cells.
    const u32 upper = end >= top + 256u ? 256u : static_cast<u32>(end - top), lower = 256u - upper;
    u64* next = s->sums + s->region.width * 4u;
    for (u32 x = 0; x < s->source.width; ++x) {
        const u8* pixel = rgba + x * 4u;
        if (!pixel[3])
            continue;
        const u32 entry = s->columns[x], cell = entry >> 9, left = entry & 511u, right = 256u - left;
        fit_add(s->sums + cell * 4u, pixel, u64{left} * upper);
        if (right)
            fit_add(s->sums + (cell + 1u) * 4u, pixel, u64{right} * upper);
        if (lower) {
            fit_add(next + cell * 4u, pixel, u64{left} * lower);
            if (right)
                fit_add(next + (cell + 1u) * 4u, pixel, u64{right} * lower);
        }
    }
    ++s->row;
    if (end <= top + 256u)
        fit_emit(s);
    if (s->row == s->source.height)
        while (s->cell_row < s->region.y + s->region.height)
            fit_emit(s);
    return true;
}

} // namespace maicord
