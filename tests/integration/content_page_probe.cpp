#include "probe_capture.hpp"
#include "content_page_probe.hpp"
#include "probe_media_upload.hpp"
#include "probe_present.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/core/log.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/store/store_scene.hpp"
#include "maicord/ui/theme/theme.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {
// The banner picture is white, the icon green: the scrim darkens the first and must leave the second alone.
constexpr MediaAssetKey kBanner = media_asset_guild_icon(9001, 1, false);
constexpr MediaAssetKey kIcon = media_asset_guild_icon(9002, 1, false);
constexpr UiRect kPanel{200, 40, 840, 560};
constexpr float kBannerHeight = 240;

bool failed(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "content_page_failure_stage", stage);
    return false;
}

bool upload(D3D11Renderer& r, MediaAssetKey key, u8 red, u8 green, u8 blue) noexcept {
    u8 pixels[16 * 16 * 4];
    for (u32 i = 0; i < 16 * 16; ++i) {
        pixels[i * 4] = red;
        pixels[i * 4 + 1] = green;
        pixels[i * 4 + 2] = blue;
        pixels[i * 4 + 3] = 255;
    }
    return renderer_upload_media_rgba8(&r, key, 16, 16, 16 * 4, {pixels, sizeof(pixels)}).ok();
}

bool media_key(const void*, u32 slot, const UiRect&, MediaAssetKey* key, u8* domain) noexcept {
    if (slot != 1 && slot != 2)
        return false;
    *key = slot == 1 ? kBanner : kIcon;
    *domain = 0;
    return true;
}

// The media batch of the queued quad that draws the resident `key`; ~0u for none.
u32 media_batch(const D3D11Renderer& r, MediaAssetKey key) noexcept {
    const MediaAtlasResult resident = media_atlas_peek(r.media_atlas, key);
    for (u32 i = 0; resident.ok() && i < r.media_quad_count; ++i) {
        const auto handle = r.media_draw.handle_data[i];
        if (r.media_draw.domain_data[i] || handle.slot != resident.handle.slot ||
            handle.generation != resident.handle.generation)
            continue;
        for (u32 b = 0; b < r.media_batch_count; ++b) {
            const auto& batch = r.media_draw.batch_data[b];
            if (i >= batch.first_quad && i < batch.first_quad + batch.quad_count)
                return b;
        }
    }
    return ~0u;
}

// The paint batch of the scrim's foot: the gradient that ends in the theme's banner foot colour; ~0u for none.
u32 scrim_batch(const D3D11Renderer& r, const Theme& theme) noexcept {
    const Color& foot = theme.tokens.overlay.banner_foot;
    const auto& paint = r.paint_stream;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        if (q.border_width != -2 || q.border.red != foot.red || q.border.green != foot.green ||
            q.border.blue != foot.blue || q.border.alpha != foot.alpha)
            continue;
        for (u32 b = 0; b < paint.batch_count; ++b)
            if (i >= paint.batches[b].first_quad && i < paint.batches[b].first_quad + paint.batches[b].quad_count)
                return b;
    }
    return ~0u;
}

struct Pixel {
    u8 red, green, blue;
};
// The drawn target's pixels at the given DIP points.
bool read_pixels(D3D11Renderer& r, const Theme& theme, const UiPoint* points, Pixel* out, u32 count) noexcept {
    if (!d3d11_renderer_draw_stream(&r, theme, true, true, nullptr) || !r.graphics.surface.target)
        return false;
    ProbeTargetReadback readback;
    D3D11_TEXTURE2D_DESC description{};
    if (readback.read_target(r, description) != 0)
        return false;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return false;
    const float scale = r.dpi / 96.f;
    for (u32 i = 0; i < count; ++i) {
        const u32 x = static_cast<u32>(points[i].x * scale), y = static_cast<u32>(points[i].y * scale);
        if (x >= description.Width || y >= description.Height)
            return false;
        const auto* pixel = static_cast<const u8*>(readback.mapped.pData) + static_cast<usize>(y) * readback.mapped.RowPitch +
                            x * 4;
        out[i] = {pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
        log_u64(LogLevel::info, "probe", "content_page_pixel", u64{out[i].red} << 16 | u64{out[i].green} << 8 | out[i].blue);
    }
    return true;
}
} // namespace

bool content_page_ui_probe(ContentPageProbeContext c) noexcept {
    auto& r = c.renderer;
    const Theme& theme = kDefaultTheme;
    static UiStoreScene scene;
    const u32 width = r.width, height = r.height;
    const float scale = r.dpi / 96.f;
    struct Restore {
        D3D11Renderer& r;
        u32 width, height;
        ~Restore() noexcept {
            renderer_bind_content_page(&r, nullptr, 0);
            ui_store_scene_release(&scene);
            probe_resize_surface(width, height);
        }
    } restore{r, width, height};
    if (!c.rebuild || !r.media_atlas || !ui_store_scene_open(&scene) ||
        !probe_resize_surface(static_cast<u32>(1024 * scale + .5f), static_cast<u32>(640 * scale + .5f)))
        return failed(1);
    if (!upload(r, kBanner, 255, 255, 255) || !upload(r, kIcon, 0, 255, 0))
        return failed(2);
    // The hub's header: a banner picture under the scrim, the server icon (foreground) at its foot.
    ui_store_scene_begin(&scene);
    UiStoreBuilder b(&scene);
    auto& banner = b.open(UiStoreKind::preview);
    banner.variant = static_cast<u8>(UiStoreAspect::fixed);
    banner.rh = kBannerHeight;
    banner.pad_x = banner.pad_y = 16;
    banner.fill = static_cast<u8>(UiStoreFill::scrim);
    banner.value = 1;
    b.open(UiStoreKind::row).anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
    auto& icon = b.open(UiStoreKind::preview);
    icon.variant = static_cast<u8>(UiStoreAspect::fixed);
    icon.flags = ui_store_foreground;
    icon.rw = icon.rh = 50;
    icon.aux = 8;
    icon.value = 2;
    b.close();
    b.close();
    b.close();
    b.close();
    scene.media_key = media_key;
    const UiRect column{kPanel.left + 16, kPanel.top + 16, kPanel.right - 16, kPanel.bottom};
    if (!b.ok() || !ui_store_scene_commit(&scene, kPanel, column, kPanel))
        return failed(3);
    scene.active = true;
    if (!renderer_bind_content_page(&r, &scene, 0) || !c.rebuild() || !probe_present(&r, theme) ||
        !probe_present(&r, theme))
        return failed(4);
    // Order: the banner picture, then the page's top paint layer with the scrim, then the icon picture.
    const u32 banner_media = media_batch(r, kBanner), icon_media = media_batch(r, kIcon);
    const u32 scrim = scrim_batch(r, theme);
    if (banner_media >= r.content_page_foreground_media_first_batch ||
        icon_media < r.content_page_foreground_media_first_batch ||
        icon_media >= r.content_page_foreground_media_end_batch || scrim == ~0u ||
        scrim < r.media_overlay_paint_first_batch)
        return failed(5);
    // Pixels: the banner's foot is near black over the white picture, its top stays light, the icon stays green.
    const float left = column.left, top = column.top, right = column.right, bottom = top + kBannerHeight;
    const UiPoint points[]{{(left + right) * .5f + 120, bottom - 4}, {(left + right) * .5f, top + 12},
                           {left + 16 + 25, bottom - 16 - 25}};
    Pixel pixels[3]{};
    if (!read_pixels(r, theme, points, pixels, 3))
        return failed(6);
    const bool foot = pixels[0].red < 48 && pixels[0].green < 48 && pixels[0].blue < 48;
    const bool head = pixels[1].red > 200 && pixels[1].green > 200 && pixels[1].blue > 200;
    const bool green = pixels[2].green > 200 && pixels[2].red < 64 && pixels[2].blue < 64;
    if (!foot || !head || !green)
        return failed(7);
    return true;
}
} // namespace maicord
