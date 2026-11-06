// The Direct3D 11 paged texture (src/gpu/d3d11/paged_texture.cpp) on the WARP device: one tiled RGBA8 texture whose
// 128-row tile bands map bands of a tile pool as pages are committed. Written rows read back, rows nothing backs read
// as zero, returning the last bands shrinks the pool and keeps the other page's pixels, and a band backed again
// starts transparent.
#include "maicord/gpu/api.hpp"
#include "d3d_harness.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

constexpr u32 kWidth = 256, kPageRows = 256, kPages = 2, kHeight = kPageRows * kPages;
// A band: 128 rows of two 128 x 128 tiles of 64 KiB.
constexpr u64 kBandBytes = 2u * 64u * 1024u;

u32 g_pixels[kWidth * kHeight];
u32 g_fill[kWidth * kPageRows];

// Rows [first, first + count) of the last readback all hold `value`.
bool rows_are(u32 first, u32 count, u32 value) noexcept {
    for (u32 index = first * kWidth; index < (first + count) * kWidth; ++index)
        if (g_pixels[index] != value)
            return false;
    return true;
}

void fill(ID3D11DeviceContext* context, GpuPagedTexture* paged, u32 first_row, u32 rows, u32 value) noexcept {
    for (u32 index = 0; index < kWidth * rows; ++index)
        g_fill[index] = value;
    const D3D11_BOX box{0, first_row, 0, kWidth, first_row + rows, 1};
    context->UpdateSubresource(paged->texture, 0, &box, g_fill, kWidth * 4, 0);
}

bool read(ID3D11Device* device, ID3D11DeviceContext* context, GpuPagedTexture* paged) noexcept {
    return d3d_harness::read_back(device, context, paged->texture, kWidth, kHeight, 4, reinterpret_cast<u8*>(g_pixels));
}

void run() noexcept {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const bool warp = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                                  D3D11_SDK_VERSION, &device, nullptr, &context));
    check(warp, "the WARP device");
    if (!warp)
        return;
    D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
    check(SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof(options))) &&
              options.TiledResourcesTier >= D3D11_TILED_RESOURCES_TIER_1,
          "WARP offers Tiled Resources Tier 1");

    GpuPagedTexture* paged = nullptr;
    const bool made = gpu_ok(gpu_paged_texture_create(
        device, {kWidth, kPageRows, kPages, GpuFormat::rgba8, gpu_bind_srv | gpu_bind_uav | gpu_bind_rtv}, &paged));
    D3D11_TEXTURE2D_DESC description{};
    if (made)
        paged->texture->GetDesc(&description);
    check(made && (description.MiscFlags & D3D11_RESOURCE_MISC_TILED) && description.Width == kWidth &&
              description.Height == kHeight && gpu_paged_texture_view(paged) && gpu_paged_texture_bytes(paged) == 0,
          "a tiled texture of both pages holds no memory before a commit");
    if (!made) {
        com_release(context);
        com_release(device);
        return;
    }
    const GpuPage second = gpu_paged_texture_page(paged, kPageRows);
    check(second.texture == paged->texture && second.uav == paged->uav && second.first_row == 0,
          "every page is the one texture, in virtual rows");

    // Page 0 backs one band, page 1 both of its bands.
    check(gpu_ok(gpu_paged_texture_commit(context, paged, 0, 128)) && gpu_paged_texture_bytes(paged) == kBandBytes &&
              gpu_ok(gpu_paged_texture_commit(context, paged, 1, kPageRows)) &&
              gpu_paged_texture_bytes(paged) == 3 * kBandBytes,
          "commits map tile bands from the pool");
    check(read(device, context, paged) && rows_are(0, kHeight, 0), "backed bands start transparent");
    fill(context, paged, 0, 128, 0xff0000ffu);
    fill(context, paged, 128, 128, 0xff00ff00u); // not backed: dropped
    fill(context, paged, kPageRows, kPageRows, 0xffff0000u);
    check(read(device, context, paged) && rows_are(0, 128, 0xff0000ffu) && rows_are(128, 128, 0) &&
              rows_are(kPageRows, kPageRows, 0xffff0000u),
          "backed rows keep what was written, the rest reads zero");

    // Returning page 1 (the pool's last bands) shrinks the pool; page 0 keeps its pixels.
    check(gpu_ok(gpu_paged_texture_commit(context, paged, 1, 0)) && gpu_paged_texture_bytes(paged) == kBandBytes &&
              read(device, context, paged) && rows_are(0, 128, 0xff0000ffu) && rows_are(kPageRows, kPageRows, 0),
          "a decommit returns its tiles and keeps the other page");
    check(gpu_ok(gpu_paged_texture_commit(context, paged, 0, 0)) && gpu_paged_texture_bytes(paged) == 0,
          "the pool goes with the last band");
    check(gpu_ok(gpu_paged_texture_commit(context, paged, 0, 128)) && gpu_paged_texture_bytes(paged) == kBandBytes &&
              read(device, context, paged) && rows_are(0, kHeight, 0),
          "a band backed again starts transparent");
    check(gpu_ok(gpu_paged_texture_commit(context, paged, 0, 128)) && gpu_paged_texture_bytes(paged) == kBandBytes,
          "committing what is backed changes nothing");
    check(gpu_paged_texture_commit(context, paged, 0, kPageRows + 128) != 0 &&
              gpu_paged_texture_commit(context, paged, kPages, 128) != 0,
          "rows past the page or a page past the texture are refused");

    gpu_paged_texture_release(&paged);
    check(!paged, "release clears the handle");
    com_release(context);
    com_release(device);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_paged_texture_entry) {
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_paged_texture");
}
