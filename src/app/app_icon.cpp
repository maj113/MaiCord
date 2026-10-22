#include "app_icon.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/media/png.hpp"

namespace maicord {

bool app_icon_decode(HMODULE module, u32 extent, u32* pixels) noexcept {
    constexpr u32 kLargest = kAppIconLargestExtent;
    const HRSRC group = module ? FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14)) : nullptr;
    const HGLOBAL group_data = group ? LoadResource(module, group) : nullptr;
    const auto* directory = group_data ? static_cast<const u8*>(LockResource(group_data)) : nullptr;
    const u32 directory_bytes = group ? SizeofResource(module, group) : 0;
    if (!directory || directory_bytes < 6 || !pixels || !extent || extent > kLargest)
        return false;
    // GRPICONDIR: three WORDs, then 14-byte entries ending in the RT_ICON id.
    const u32 count = minimum<u32>(directory[4] | directory[5] << 8, (directory_bytes - 6) / 14);
    u32 best = 0, best_id = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* entry = directory + 6 + i * 14;
        const u32 size = entry[0] ? entry[0] : 256;
        if (size >= extent && size <= kLargest && (!best || size < best))
            best = size, best_id = static_cast<u32>(entry[12] | entry[13] << 8);
    }
    const HRSRC image =
        best ? FindResourceW(module, MAKEINTRESOURCEW(best_id), MAKEINTRESOURCEW(3)) : nullptr;
    const HGLOBAL image_data = image ? LoadResource(module, image) : nullptr;
    const auto* encoded = image_data ? static_cast<const u8*>(LockResource(image_data)) : nullptr;
    if (!encoded)
        return false;
    struct Scratch {
        PngImage png;
        DeflateWorkspace workspace;
        u8 scanlines[(kLargest + 1) * kLargest];
    };
    ScratchPages<Scratch> scratch;
    if (!scratch.data)
        return false;
    PngImage& png = scratch.data->png;
    const u8* scanlines = scratch.data->scanlines;
    const auto decoded = png_decode_scanlines({encoded, SizeofResource(module, image)}, &png,
                                              &scratch.data->workspace,
                                              {scratch.data->scanlines, static_cast<u32>(sizeof(Scratch::scanlines))});
    if (!decoded.ok() || png.width != best || png.height != best || png.color_type != 3 ||
        png.bit_depth != 8)
        return false;
    if (extent == best) {
        // Reuse the decoded palette: exact-size icons need no averaging.
        for (u32 i = 0; i < png.palette_count; ++i) {
            const auto* rgba = reinterpret_cast<const u8*>(png.palette_rgba + i);
            const u32 alpha = rgba[3];
            png.palette_rgba[i] = (rgba[2] * alpha + 127) / 255 |
                                 ((rgba[1] * alpha + 127) / 255) << 8 |
                                 ((rgba[0] * alpha + 127) / 255) << 16 | alpha << 24;
        }
        for (u32 y = 0; y < extent; ++y)
            for (u32 x = 0; x < extent; ++x)
                pixels[y * extent + x] = png.palette_rgba[scanlines[y * png.row_stride + 1 + x]];
        return true;
    }
    const u32 step = best;
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            // Source rows and columns covering this pixel.
            const u32 x0 = x * step / extent, x1 = maximum(x0 + 1, (x + 1) * step / extent);
            const u32 y0 = y * step / extent, y1 = maximum(y0 + 1, (y + 1) * step / extent);
            u32 sum[4]{};
            for (u32 sy = y0; sy < y1; ++sy)
                for (u32 sx = x0; sx < x1; ++sx) {
                    const auto* rgba = reinterpret_cast<const u8*>(
                        png.palette_rgba + scanlines[sy * png.row_stride + 1 + sx]);
                    // Premultiply before averaging.
                    sum[0] += rgba[2] * rgba[3];
                    sum[1] += rgba[1] * rgba[3];
                    sum[2] += rgba[0] * rgba[3];
                    sum[3] += rgba[3] * 255u;
                }
            const u32 samples = (x1 - x0) * (y1 - y0) * 255u;
            const auto channel = [&](u32 value) noexcept { return (value + samples / 2) / samples; };
            pixels[y * extent + x] =
                channel(sum[0]) | channel(sum[1]) << 8 | channel(sum[2]) << 16 | channel(sum[3]) << 24;
        }
    return true;
}

} // namespace maicord
