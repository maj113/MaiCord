#include "maicord/render/icon_atlas.hpp"

#include "maicord/ui/icon.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u16 kPadding = 1;

i32 floor_i32(float value) noexcept {
    const i32 truncated = static_cast<i32>(value);
    return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
}
i32 round_i32(float value) noexcept {
    return floor_i32(value + .5f);
}
float edge_error(float value) noexcept {
    const float distance = value - static_cast<float>(round_i32(value));
    return distance < 0 ? -distance : distance;
}

// Choose the translation (eighth-pixel steps within half a pixel) that puts
// the recorded stem edges closest to pixel boundaries; ties keep the origin.
float stem_offset(const IconStem* stems, u32 count, u8 axis, float units_to_pixels, float half_width) noexcept {
    float best = 0, best_cost = 3.4e38f;
    for (i32 step = 0; step <= 8; ++step) {
        const float offset = (step & 1 ? 1.f : -1.f) * static_cast<float>((step + 1) / 2) * .125f;
        float cost = 0;
        for (u32 i = 0; i < count; ++i) {
            if (stems[i].axis != axis)
                continue;
            const float center = stems[i].position * units_to_pixels + offset;
            cost += stems[i].length * (edge_error(center - half_width) + edge_error(center + half_width));
        }
        if (cost + 1e-4f < best_cost) {
            best_cost = cost;
            best = offset;
        }
    }
    return best;
}

u64 cell_key(u32 icon, bool solid, u16 size, float half_width) noexcept {
    return static_cast<u64>(icon & 1023) | (static_cast<u64>(solid) << 11) | (static_cast<u64>(size) << 12) |
           (static_cast<u64>(round_i32(half_width * 8) & 255) << 20) | (static_cast<u64>(icon & 255) << 28);
}

// Bounds and stroke determine the cache key. Optical offsets are needed only for a new raster.
bool place_bounds(const IconPack& pack, u32 index, UiRect box, float pixel_scale, float weight,
                  IconPlacement* placement) noexcept {
    const float extent = minimum(ui_rect_width(box), ui_rect_height(box));
    if (index >= pack.icon_count || !(pixel_scale > 0 && pixel_scale <= 8) || !(extent > 0))
        return false;
    const i32 size = round_i32(extent * pixel_scale);
    if (size < 4 || size > static_cast<i32>(kIconAtlasMaximumPixels))
        return false;
    const float center_x = (box.left + box.right) * .5f * pixel_scale;
    const float center_y = (box.top + box.bottom) * .5f * pixel_scale;
    const float half_width = ui_icon_stroke_pixels(extent, pixel_scale, weight) * .5f;
    *placement = {round_i32(center_x - size * .5f), round_i32(center_y - size * .5f), static_cast<u16>(size),
                  half_width, 0, 0};
    return true;
}

void align_stems(const IconPack& pack, u32 index, IconPlacement* placement) noexcept {
    IconStem stems[kIconMaximumStems]{};
    const u32 stem_count = icon_pack_stems(pack, index, stems, kIconMaximumStems);
    const float units_to_pixels = static_cast<float>(placement->size) / kIconPackGrid;
    placement->offset_x = stem_offset(stems, stem_count, 0, units_to_pixels, placement->stroke_half_width);
    placement->offset_y = stem_offset(stems, stem_count, 1, units_to_pixels, placement->stroke_half_width);
}
} // namespace

bool icon_atlas_initialize(IconAtlas* atlas, ID3D11Device* device) noexcept {
    if (!atlas || !device || atlas->ready)
        return false;
    if (!glyph_atlas_initialize(&atlas->allocator, kIconAtlasExtent, kIconAtlasExtent, kPadding,
                                {atlas->entries, kIconAtlasEntries}, {atlas->rows, kIconAtlasRows}) ||
        !glyph_atlas_gpu_initialize(&atlas->gpu, device, kIconAtlasExtent, kIconAtlasExtent,
                                    kIconMaximumOutlineCommands)
             .ok())
        return false;
    atlas->resolved_dpi = 0;
    atlas->ready = true;
    return true;
}

void icon_atlas_release(IconAtlas* atlas) noexcept {
    if (!atlas)
        return;
    glyph_atlas_gpu_release(&atlas->gpu);
    atlas->ready = false;
    atlas->resolved_dpi = 0;
}

bool icon_atlas_stale(const IconAtlas& atlas, u32 dpi, float weight) noexcept {
    return atlas.ready && (atlas.resolved_dpi != dpi || atlas.resolved_weight != weight);
}

bool icon_atlas_resolve(IconAtlas* atlas, ID3D11DeviceContext* context, const PaintQuad* source,
                        PaintQuad* destination, u32 count, u32 dpi, float weight) noexcept {
    if (!source || !destination)
        return false;
    if (!atlas || !context || dpi == 0) {
        memmove(destination, source, static_cast<usize>(count) * sizeof(PaintQuad));
        return false;
    }
    const float scale = static_cast<float>(dpi) / 96.f;
    atlas->resolved_dpi = dpi;
    atlas->resolved_weight = weight;
    glyph_atlas_clear_pins(&atlas->allocator);
    GlyphRasterBatch batch(&atlas->gpu, context);
    for (u32 i = 0; i < count; ++i) {
        if (!ui_icon_quad(source[i])) {
            destination[i] = source[i];
            continue;
        }
        PaintQuad quad = source[i];
        quad.reserved_x = quad.reserved_y = -1;
        const UiIconQuadData data = ui_icon_quad_data(quad);
        const IconPack& pack = icon_pack_builtin();
        IconPlacement place{};
        const float quad_weight = data.weight > 0 ? data.weight : weight;
        if (!atlas->ready || data.icon >= static_cast<u32>(UiIcon::count) ||
            !place_bounds(pack, data.icon, {quad.left, quad.top, quad.right, quad.bottom}, scale,
                          quad_weight, &place)) {
            ++atlas->unresolved;
            destination[i] = quad;
            continue;
        }
        const u64 key = cell_key(data.icon, data.solid, place.size, place.stroke_half_width);
        GlyphAtlasAllocation cell = glyph_atlas_find(&atlas->allocator, key);
        if (!cell.ok()) {
            align_stems(pack, data.icon, &place);
            cell = glyph_atlas_allocate_unique(&atlas->allocator, key, place.size, place.size);
            GlyphOutline outline{atlas->commands, kIconMaximumOutlineCommands, 0, 0, 0, 0, 0, 0, {}, false};
            const float units_to_pixels = static_cast<float>(place.size) / kIconPackGrid;
            const GlyphRasterBounds bounds{0, 0, place.size, place.size, place.size, place.size};
            if (cell.ok() &&
                (!icon_pack_decode(pack, data.icon, {units_to_pixels, place.offset_x, place.offset_y, data.solid},
                                   &outline) ||
                 !batch.add(outline, bounds, cell, 1.f, kPadding, place.stroke_half_width).ok())) {
                // The cell holds no coverage; drop every cell rather than track a hole.
                batch.cancel();
                glyph_atlas_reset(&atlas->allocator);
                memmove(destination + i, source + i, static_cast<usize>(count - i) * sizeof(PaintQuad));
                return false;
            }
            if (cell.ok())
                ++atlas->rasterized;
        }
        if (!cell.ok() || !glyph_atlas_pin(&atlas->allocator, cell.handle)) {
            ++atlas->unresolved;
            destination[i] = quad;
            continue;
        }
        quad.left = static_cast<float>(place.left) / scale;
        quad.top = static_cast<float>(place.top) / scale;
        quad.right = static_cast<float>(place.left + place.size) / scale;
        quad.bottom = static_cast<float>(place.top + place.size) / scale;
        quad.radius = scale;
        quad.reserved_x = cell.x;
        quad.reserved_y = cell.y;
        destination[i] = quad;
    }
    return batch.finish();
}

} // namespace maicord
