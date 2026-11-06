// A colour bitmap glyph (CBDT) reaches the colour atlas: the generated fixture's tiles, built at two sizes, go
// through colr_paint_upload_pixels into an RGBA8 atlas, and a readback shows each tile at its allocation with the
// padding gutter transparent and nothing outside written. Linux runs it on whatever EGL finds.
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/render/effects/colr_paint.hpp"
#include "maicord/text/opentype/bitmap_color.hpp"
#include "../support/test_output.hpp"

namespace maicord {
#include "../generated/cbdt_fixture.inc"
namespace {

u32 g_failures = 0;
u32 g_checks = 0;

void check(bool condition, const char* label) noexcept {
    ++g_checks;
    test_check(&g_failures, condition, label);
}

constexpr u16 kAtlas = 64;
constexpr u16 kPadding = 2;
u32 g_atlas[kAtlas * kAtlas];

GlyphAtlasAllocation allocation(u16 x, u16 y, u32 width, u32 height) noexcept {
    GlyphAtlasAllocation a{};
    a.x = x;
    a.y = y;
    a.width = static_cast<u16>(width);
    a.height = static_cast<u16>(height);
    a.error = GlyphAtlasError::none;
    return a;
}

bool read_atlas(GpuDevice* device, GpuContext* context, GpuTexture* texture) noexcept {
    return gpu_ok(gpu_readback(device, context, texture, {0, 0, kAtlas, kAtlas}, g_atlas, kAtlas * 4u));
}

// The tile sits at `a`, the gutter around it is transparent, and every pixel outside both is still `outside`.
bool tile_in_atlas(const GlyphAtlasAllocation& a, const BitmapColorGlyph& tile, const bool* written, u32 outside) noexcept {
    for (u32 y = 0; y < kAtlas; ++y)
        for (u32 x = 0; x < kAtlas; ++x) {
            const bool in_tile = x >= a.x && x < a.x + a.width && y >= a.y && y < a.y + a.height;
            const bool in_gutter = x + kPadding >= a.x && x < a.x + a.width + kPadding && y + kPadding >= a.y &&
                                   y < a.y + a.height + kPadding;
            const u32 value = g_atlas[y * kAtlas + x];
            if (in_tile) {
                if (value != tile.pixels[(y - a.y) * tile.width + (x - a.x)])
                    return false;
            } else if (in_gutter) {
                if (value != 0)
                    return false;
            } else if (!written[y * kAtlas + x] && value != outside) {
                return false;
            }
        }
    return true;
}

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        check(false, "device creation");
        return;
    }
    test_write_text("device: ");
    test_write_line(gpu_device_description(device));

    SfntFace face{};
    CbdtFont font{};
    check(sfnt_open({cbdt_fixture, sizeof(cbdt_fixture)}, 0, &face) == SfntStatus::ok &&
              cbdt_open(face, &font) == SfntStatus::ok,
          "fixture opens");
    Arena arena{};
    check(arena_initialize(&arena, 16u * 1024u * 1024u, 0, 64 * 1024), "scratch arena");
    BitmapColorGlyph small{}, large{};
    check(bitmap_color_glyph_build(font, 1, 16.f, &arena, &small) == SfntStatus::ok && small.width == 8 &&
              small.height == 8,
          "8 by 8 tile of strike 16");
    check(bitmap_color_glyph_build(font, 1, 32.f, &arena, &large) == SfntStatus::ok && large.width == 16 &&
              large.height == 16,
          "16 by 16 tile of strike 32");

    ColrPaintGpu gpu{};
    static bool written[kAtlas * kAtlas];
    const GlyphAtlasAllocation first = allocation(4, 6, small.width, small.height);
    check(!colr_paint_upload_pixels(&gpu, context, first, kAtlas, kAtlas, 8, small.pixels, &arena),
          "an allocation inside the padding is refused");
    check(!colr_paint_upload_pixels(&gpu, context, allocation(60, 6, small.width, small.height), kAtlas, kAtlas,
                                    kPadding, small.pixels, &arena),
          "an allocation past the atlas is refused");
    check(colr_paint_upload_pixels(&gpu, context, first, kAtlas, kAtlas, kPadding, small.pixels, &arena),
          "the first tile uploads");
    check(gpu.surface && gpu.width == kAtlas && gpu.height == kAtlas, "the atlas surface is made at the atlas size");
    check(read_atlas(device, context, gpu_paged_texture_page(gpu.surface, 0).texture) &&
              tile_in_atlas(first, small, written, 0),
          "the first tile is in the atlas, its gutter clear, the rest untouched");
    for (u32 y = first.y - kPadding; y < first.y + first.height + kPadding; ++y)
        for (u32 x = first.x - kPadding; x < first.x + first.width + kPadding; ++x)
            written[y * kAtlas + x] = true;

    const GlyphAtlasAllocation second = allocation(20, 30, large.width, large.height);
    check(colr_paint_upload_pixels(&gpu, context, second, kAtlas, kAtlas, kPadding, large.pixels, &arena),
          "the second tile uploads into the existing texture");
    check(read_atlas(device, context, gpu_paged_texture_page(gpu.surface, 0).texture) &&
              tile_in_atlas(second, large, written, 0),
          "the second tile is in the atlas");
    // The first tile is intact.
    bool first_intact = true;
    for (u32 y = 0; y < first.height; ++y)
        for (u32 x = 0; x < first.width; ++x)
            first_intact = first_intact && g_atlas[(first.y + y) * kAtlas + first.x + x] == small.pixels[y * small.width + x];
    check(first_intact, "the first tile survives the second upload");

    check(!colr_paint_upload_pixels(&gpu, context, first, 128, 128, kPadding, small.pixels, &arena),
          "an atlas size other than the texture's is refused");

    colr_paint_gpu_release(&gpu);
    arena_release(&arena);
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_bitmap_glyph_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(maicord::g_failures, "maicord_gpu_bitmap_glyph");
}
