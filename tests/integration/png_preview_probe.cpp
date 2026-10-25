#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/log.hpp"
#include "nv12_test_picture.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/media/asset.hpp"
namespace maicord {
namespace {
bool wide_video_preview_probe(D3D11Renderer& r, MediaAtlas& atlas, MediaAtlasGpu& gpu) noexcept {
    constexpr u32 width = 2560, height = 16, luma_bytes = width * height;
    auto* pixels = static_cast<u8*>(
        VirtualAlloc(nullptr, luma_bytes * 3u / 2u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Nv12TestPicture picture{};
    ID3D11Texture2D* readback = nullptr;
    ID3D11Query* ready = nullptr;
    bool ok = pixels != nullptr;
    if (ok) {
        for (u32 i = 0; i < luma_bytes; ++i)
            pixels[i] = 80;
        for (u32 i = luma_bytes; i < luma_bytes * 3u / 2u; ++i)
            pixels[i] = 128;
        ok = picture.create(r.graphics.device, width, height, pixels);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = description.Height = 1;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (ok)
            ok = SUCCEEDED(r.graphics.device->CreateTexture2D(&description, nullptr, &readback));
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        if (ok)
            ok = SUCCEEDED(r.graphics.device->CreateQuery(&query, &ready));
    }
    const auto key = media_asset_attachment(987655, MediaAttachmentFormat::mp4);
    GpuVideoFrame frame = picture.frame(width, height);
    frame.bt709 = frame.full_range = true;
    const u32 conversions = gpu.video.frames, cleared = gpu.cleared_regions;
    const u32 entries = atlas.entry_count;
    MediaAtlasResult first{};
    for (u32 pass = 0; ok && pass < 2; ++pass) {
        const u8 gray = pass ? 144 : 80;
        if (pass) {
            for (u32 i = 0; i < luma_bytes; ++i)
                pixels[i] = gray;
            r.graphics.context1->UpdateSubresource(picture.texture, 0, nullptr, pixels, width, 0);
        }
        const auto uploaded = media_atlas_gpu_present_video(&gpu, &atlas, r.graphics.device,
                                                            r.graphics.context1, key, frame, true);
        ok = uploaded.ok() && uploaded.allocation.width == width && uploaded.allocation.height == height;
        if (!ok)
            break;
        if (!pass)
            first = uploaded.allocation;
        ok = uploaded.allocation.cached == (pass != 0) && uploaded.allocation.x == first.x &&
             uploaded.allocation.y == first.y && uploaded.allocation.handle.slot == first.handle.slot &&
             uploaded.allocation.handle.generation == first.handle.generation;
        if (!ok)
            break;
        const u32 x = first.x + width / 2, y = first.y + height / 2;
        const D3D11_BOX pixel{x, y, 0, x + 1, y + 1, 1};
        r.graphics.context1->CopySubresourceRegion(readback, 0, 0, 0, 0, gpu.preview_texture, 0, &pixel);
        r.graphics.context1->End(ready);
        r.graphics.context1->Flush();
        const u64 deadline = GetTickCount64() + 1000;
        BOOL complete = FALSE;
        do {
            const HRESULT result = r.graphics.context1->GetData(ready, &complete, sizeof(complete),
                                                                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(result)) {
                ok = false;
                break;
            }
            if (result == S_OK && complete)
                break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        ok = ok && complete;
        if (ok) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT mapped_status;
            do {
                mapped_status=r.graphics.context1->Map(
                    readback,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                if(mapped_status!=DXGI_ERROR_WAS_STILL_DRAWING)break;
                Sleep(1);
            } while(GetTickCount64()<deadline);
            ok=SUCCEEDED(mapped_status);
            if(!ok)log_u64(LogLevel::error,"probe","video_readback_map_error",static_cast<u32>(mapped_status));
            if (ok) {
                // Full-range neutral chroma produces the exact source gray.
                const u32 expected = 0xff000000u | u32{gray} * 0x010101u;
                ok = *static_cast<const u32*>(mapped.pData) == expected;
                r.graphics.context1->Unmap(readback, 0);
            }
        }
    }
    ok = ok && gpu.video.frames == conversions + 2 && gpu.cleared_regions == cleared + 1 &&
         atlas.entry_count == entries + 1 && media_atlas_validate(atlas, first.handle);
    if (ok) {
        // A visible shape above the codec budget is refused before allocation.
        auto invalid = frame;
        invalid.width = 4097;
        const auto rejected = media_atlas_gpu_present_video(&gpu, &atlas, r.graphics.device,
                                                            r.graphics.context1, key, invalid, true);
        // A4096-wide visible shape cannot fit the existing one-pixel padding.
        invalid.width = 4096;
        const auto padded = media_atlas_gpu_present_video(
            &gpu, &atlas, r.graphics.device, r.graphics.context1,
            media_asset_attachment(987656, MediaAttachmentFormat::mp4), invalid, true);
        ok = rejected.error == MediaAtlasGpuError::invalid_input &&
             padded.error == MediaAtlasGpuError::atlas_failure &&
             padded.allocation.error == MediaAtlasError::too_large && gpu.video.frames == conversions + 2 &&
             atlas.entry_count == entries + 1 && media_atlas_validate(atlas, first.handle);
    }
    if (ready)
        ready->Release();
    if (readback)
        readback->Release();
    picture.release();
    if (pixels)
        VirtualFree(pixels, 0, MEM_RELEASE);
    log_write(ok ? LogLevel::info : LogLevel::error, "probe",
              ok ? "video_2560_preview_cached_pixels_exact" : "video_2560_preview_failed");
    return ok;
}
} // namespace
bool png_preview_probe(D3D11Renderer& r) noexcept {
    MediaAtlas atlas{};
    MediaAtlasGpu gpu{};
    MediaAtlasEntry entries[4]{};
    u32 buckets[8]{};
    GlyphAtlasEntry allocations[4]{};
    GlyphAtlasRow rows[4]{};
    PngImage image{};
    bool ok =
        png_image_configure(&image, 2048, 16, 8, 6) &&
        media_atlas_initialize(&atlas, 4096, 64, 1, {entries, 4}, {buckets, 8}, {allocations, 4},
                               {rows, 4}) &&
        media_atlas_gpu_initialize(&gpu, r.graphics.device, r.graphics.context1, 16, 16).ok() &&
        media_atlas_gpu_initialize_preview(&gpu, r.graphics.device, r.graphics.context1, 4096, 64).ok();
    auto* scanlines = static_cast<u8*>(
        VirtualAlloc(nullptr, image.inflated_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    ok = ok && scanlines;
    if (ok)
        for (u32 y = 0; y < image.height; ++y) {
            auto* row = scanlines + y * image.row_stride + 1;
            for (u32 x = 0; x < image.width; ++x) {
                row[x * 4] = x & 1 ? 255 : 0;
                row[x * 4 + 1] = y & 1 ? 255 : 0;
                row[x * 4 + 2] = x & 1 ? 0 : 255;
                row[x * 4 + 3] = 255;
            }
        }
    const auto key =
        media_asset_attachment(987654, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
    if (ok)
        ok = media_atlas_gpu_upload_preview_png(&gpu, &atlas, r.graphics.device, r.graphics.context1, key,
                                                image, {scanlines, image.inflated_bytes})
                 .ok();
    const auto item = media_atlas_find(&atlas, key);
    ID3D11Texture2D* readback = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok = ok && item.ok() && SUCCEEDED(r.graphics.device->CreateTexture2D(&description, nullptr, &readback));
    if (ok) {
        const D3D11_BOX box{item.x, item.y, 0, item.x + image.width, item.y + image.height, 1};
        r.graphics.context1->CopySubresourceRegion(readback, 0, 0, 0, 0, gpu.preview_texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(r.graphics.context1->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            for (u32 y = 0; y < image.height; ++y) {
                const auto* row = reinterpret_cast<const u32*>(static_cast<const u8*>(mapped.pData) +
                                                               y * mapped.RowPitch);
                for (u32 x = 0; x < image.width; ++x)
                    ok = ok &&
                         row[x] == (0xff000000u | (y & 1 ? 0xff00u : 0u) | (x & 1 ? 0xffu : 0xff0000u));
            }
            r.graphics.context1->Unmap(readback, 0);
        }
    }
    if (readback)
        readback->Release();
    if (scanlines)
        VirtualFree(scanlines, 0, MEM_RELEASE);
    log_write(ok ? LogLevel::info : LogLevel::error, "probe",
              ok ? "png_2048_preview_pixels_exact" : "png_2048_preview_failed");
    if (ok)
        ok = wide_video_preview_probe(r, atlas, gpu);
    media_atlas_gpu_release(&gpu);
    return ok;
}
} // namespace maicord
