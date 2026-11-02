#include "../../../src/app/store/store_preview.hpp"
#include "../../support/media_fixture.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/core/asset.hpp"

// The store's picture pipeline: a tile's pixel width, the atlas identity of a picture at a size, and the
// reduction of a catalog PNG to that size (an integer box factor that never drops below the tile).
namespace maicord {
namespace store_preview_test {
using namespace app_detail;

u32 failures;
u8 g_encoded[512 * 1024];
u8 g_zlib[512 * 1024];

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

void test_pixels_and_key() noexcept {
    check(store_preview_pixels(253, 120) == 320 && store_preview_pixels(253, 96) == 256 && store_preview_pixels(64, 96) == 64,
          "store preview: a tile's width in pixels rounds up to 32");
    check(store_preview_pixels(0, 96) == 0 && store_preview_pixels(-4, 96) == 0 && store_preview_pixels(5000, 96) == 1920,
          "store preview: an empty tile needs nothing and a huge one is capped");
    const u8 a[32]{1, 2, 3, 4, 5, 6, 7, 8}, b[32]{9, 2, 3, 4, 5, 6, 7, 8}, zero[32]{};
    const MediaAssetKey still = store_preview_key(a, 320, false);
    check(media_asset_key_valid(still) && still.kind == MediaAssetKind::local_draft_preview && media_asset_large_preview(still),
          "store preview: the identity is a valid large preview in the preview atlas");
    check(!media_asset_key_equal(still, store_preview_key(a, 352, false)) && !media_asset_key_equal(still, store_preview_key(a, 320, true)) &&
              !media_asset_key_equal(still, store_preview_key(b, 320, false)) && media_asset_key_equal(still, store_preview_key(a, 320, false)),
          "store preview: size, loop and digest each make another identity");
    check(media_asset_key_valid(store_preview_key(zero, 64, false)), "store preview: even an all-zero digest gets a valid identity");
}

void test_decode() noexcept {
    PngFixtureResult fixture{};
    check(build_rgba8_png_fixture(128, 32, {g_encoded, sizeof(g_encoded)}, {g_zlib, sizeof(g_zlib)}, &fixture, png_fixture_rgba),
          "store preview: the PNG fixture builds");
    StorePreviewImage image{};
    check(store_preview_decode({g_encoded, fixture.encoded_bytes}, 32, &image) && image.width == 32 && image.height == 8 && image.pixels,
          "store preview: a 4x wider source is reduced by four");
    u32 colored = 0;
    for (u32 i = 0; image.pixels && i < image.width * image.height * 4; ++i)
        colored += image.pixels[i] != 0;
    check(colored != 0, "store preview: the reduced picture carries the source's colour");
    store_preview_free(&image);
    check(!image.pixels && store_preview_decode({g_encoded, fixture.encoded_bytes}, 50, &image) && image.width == 64 && image.height == 16,
          "store preview: the reduction stops at the last factor that keeps the tile covered");
    store_preview_free(&image);
    check(store_preview_decode({g_encoded, fixture.encoded_bytes}, 320, &image) && image.width == 128 && image.height == 32,
          "store preview: a tile wider than the picture keeps it whole");
    store_preview_free(&image);
    u8 damaged[64];
    copy_bytes(damaged, g_encoded, sizeof(damaged));
    damaged[1] ^= 0x55;
    check(!store_preview_decode({damaged, sizeof(damaged)}, 64, &image) && !image.pixels && !store_preview_decode({g_encoded, 0}, 64, &image) &&
              !store_preview_decode({g_encoded, fixture.encoded_bytes}, 0, &image),
          "store preview: damaged, empty and zero-width requests fail without a picture");
}

} // namespace store_preview_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_store_preview_entry() noexcept {
    using namespace maicord::store_preview_test;
    test_pixels_and_key();
    test_decode();
    constexpr char pass[] = "PASS store preview\n";
    constexpr char fail[] = "FAIL store preview\n";
    maicord::test_write_text(failures ? fail : pass);
    ExitProcess(failures ? 1 : 0);
}
