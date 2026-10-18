#include "probe_present.hpp"
#include "animation_preview_probe.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/theme.hpp"

#include <Windows.h>
#include <d3d11_1.h>

namespace maicord {
bool png_preview_probe(D3D11Renderer& r) noexcept;
namespace {
bool atlas_pressure_pixels(D3D11Renderer& r) noexcept {
    MediaAtlas atlas{};
    MediaAtlasGpu gpu{};
    MediaAtlasEntry entries[16]{};
    u32 buckets[32]{};
    GlyphAtlasEntry allocations[16]{};
    GlyphAtlasRow rows[8]{};
    static u32 pixels[128 * 64];
    static u8 indices[128 * 160];
    const auto key = [](u64 id) noexcept { return media_asset_attachment(id, MediaAttachmentFormat::gif); };
    const u16 heights[] = {32, 32, 64, 32, 64, 32};
    bool ok = media_atlas_initialize(&atlas, 128, 256, 0, {entries, 16}, {buckets, 32}, {allocations, 16}, {rows, 8}) &&
        media_atlas_gpu_initialize(&gpu, r.graphics.device, r.graphics.context1, 128, 256).ok();
    for (u32 i = 0; ok && i < 6; ++i) {
        const u32 color = i == 0 ? 0xff0000ff : i == 5 ? 0xff00ff00 : 0xffffffff;
        for (auto& pixel : pixels) pixel = color;
        ok = media_atlas_gpu_upload_rgba8(&gpu, &atlas, r.graphics.context1, key(i + 1), 128, heights[i],
            128 * 4, {reinterpret_cast<const u8*>(pixels), static_cast<u32>(128 * heights[i] * 4)}).ok();
    }
    media_atlas_begin_visibility(&atlas);
    const auto top = media_atlas_find(&atlas, key(1));
    const auto bottom = media_atlas_find(&atlas, key(6));
    GifFrame frame{};
    frame.width = 128;
    frame.height = 160;
    GifPalette palette{};
    palette.count = 2;
    palette.rgba[0] = 0xffff0000;
    const auto gif = ok ? media_atlas_gpu_begin_gif(&gpu, &atlas, r.graphics.device, r.graphics.context1,
        key(7), 128, 160, frame, palette, {indices, sizeof(indices)}) : MediaAtlasGpuResult{};
    ok = ok && gif.ok() && top.ok() && bottom.ok() && gif.allocation.evicted_entries == 4 &&
         !gif.allocation.atlas_reset && media_atlas_validate(atlas, top.handle) && media_atlas_validate(atlas, bottom.handle);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 128;
    description.Height = 256;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* readback = nullptr;
    ok = ok && SUCCEEDED(r.graphics.device->CreateTexture2D(&description, nullptr, &readback));
    if (ok) {
        r.graphics.context1->CopyResource(readback, gpu.texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(r.graphics.context1->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            for (u32 y = 0; y < 256; ++y) {
                if (y >= 192 && y < 224) continue;
                const u32 expected = y < 32 ? 0xff0000ff : y >= 224 ? 0xff00ff00 : 0xffff0000;
                const auto* row = reinterpret_cast<const u32*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
                for (u32 x = 0; x < 128; ++x) ok = ok && row[x] == expected;
            }
            r.graphics.context1->Unmap(readback, 0);
        }
    }
    if (readback) readback->Release();
    media_atlas_gpu_release(&gpu);
    log_write(ok ? LogLevel::info : LogLevel::error, "probe", ok ? "atlas_pressure_pixels_pass" : "atlas_pressure_pixels_failed");
    return ok;
}
}

bool animation_preview_probe(D3D11Renderer& r) noexcept {
    if (!png_preview_probe(r)) return false;
    if (!r.preview_media_atlas) return false;
    auto* identity_texture = r.media_gpu.texture;
    auto* identity_backup = r.media_gpu.apng_backup_texture;
    const u32 identity_entries = r.media_atlas->entry_count;
    const u32 layouts = r.paint_stream_builds;
    const u32 text_builds = r.content_text_retention.builds;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 4;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* readback = nullptr;
    if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &readback))) return false;
    const auto pixels_equal = [&](MediaAssetKey key, u32 rgba) noexcept {
        const auto item = media_atlas_find(r.preview_media_atlas, key);
        if (!item.ok() || media_atlas_find(r.media_atlas, key).ok()) return false;
        const D3D11_BOX box{item.x, item.y, 0, static_cast<UINT>(item.x + 4), static_cast<UINT>(item.y + 4), 1};
        r.graphics.context1->CopySubresourceRegion(readback, 0, 0, 0, 0, r.media_gpu.preview_texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(r.graphics.context1->Map(readback, 0, D3D11_MAP_READ, 0, &mapped))) return false;
        bool equal = true;
        for (u32 y = 0; y < 4; ++y) {
            const auto* row = reinterpret_cast<const u32*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
            for (u32 x = 0; x < 4; ++x) equal = equal && row[x] == rgba;
        }
        r.graphics.context1->Unmap(readback, 0);
        return equal;
    };
    const auto gif = media_asset_attachment(0x47494600, MediaAttachmentFormat::gif, MediaResolutionTier::px64);
    GifFrame previous{}, frame{};
    frame.width = frame.height = 4;
    frame.dispose = GifDispose::previous;
    GifPalette palette{};
    palette.count = 2;
    palette.rgba[0] = 0xff0000ff;
    palette.rgba[1] = 0xff00ff00;
    u8 indices[16]{};
    bool ok = renderer_begin_gif(&r, gif, 4, 4, frame, palette, {indices, 16}).ok() &&
              pixels_equal(gif, palette.rgba[0]);
    previous = frame;
    for (auto& index : indices) index = 1;
    frame.dispose = GifDispose::none;
    ok = ok && renderer_advance_gif(&r, gif, previous, frame, palette, {indices, 16}).ok() &&
         pixels_equal(gif, palette.rgba[1]);
    const auto webp = media_asset_embed(0x57454250, 2, 0, MediaEmbedResource::image,
                                       MediaAttachmentFormat::webp, true, MediaResolutionTier::px64);
    AnimatedWebpFrame webp_frame{};
    webp_frame.width = webp_frame.height = 4;
    webp_frame.blend = AnimatedWebpBlend::source;
    webp_frame.codec = AnimatedWebpCodec::vp8l_lossless;
    u32 rgba[16];
    for (auto& pixel : rgba) pixel = 0xffff0000;
    ok = ok && renderer_begin_animated_webp(&r, webp, 4, 4, 0, webp_frame,
        {reinterpret_cast<const u8*>(rgba), sizeof(rgba)}).ok() && pixels_equal(webp, rgba[0]);
    for (auto& pixel : rgba) pixel = 0xffffffff;
    ok = ok && renderer_advance_animated_webp(&r, webp, 0, webp_frame, webp_frame,
        {reinterpret_cast<const u8*>(rgba), sizeof(rgba)}).ok() && pixels_equal(webp, rgba[0]);
    readback->Release();
    ok = ok && r.media_gpu.texture == identity_texture && r.media_gpu.apng_backup_texture == identity_backup &&
         r.media_atlas->entry_count == identity_entries && r.paint_stream_builds == layouts &&
         r.content_text_retention.builds == text_builds;
    // These test images are resident but are not in the scene. A full layout
    // rebuild (scroll/channel switch) must release their visibility pins.
    const auto offscreen = media_atlas_find(r.preview_media_atlas, gif);
    const auto allocation = r.preview_media_atlas->entries[offscreen.handle.slot].allocation;
    ok = ok && offscreen.ok() && glyph_atlas_is_pinned(r.preview_media_atlas->allocations, allocation);
    r.retained_presentation.valid = false;
    ok = ok && probe_present(&r, kDefaultTheme) &&
         !glyph_atlas_is_pinned(r.preview_media_atlas->allocations, allocation);
    ok = ok && atlas_pressure_pixels(r);
    log_write(ok ? LogLevel::info : LogLevel::error, "probe", ok ? "animation_preview_pixels_pass" : "animation_preview_pixels_failed");
    return ok;
}
} // namespace maicord
