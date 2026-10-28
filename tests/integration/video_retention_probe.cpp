#include "probe_present.hpp"
#include "video_retention_probe.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/theme.hpp"
#include "nv12_test_picture.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
bool video_display_extent_probe(D3D11Renderer& r) noexcept {
    constexpr u32 width = 1920, height = 1080, bytes = width * height * 3 / 2;
    auto* pixels = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pixels) return false;
    memset(pixels, 128, bytes);
    Nv12TestPicture picture{};
    const bool created = picture.create(r.graphics.device, width, height, pixels);
    VirtualFree(pixels, 0, MEM_RELEASE);
    if (!created) return false;
    const auto key = media_asset_embed(0x987654, 1, 0, MediaEmbedResource::video,
                                       MediaAttachmentFormat::mp4, true, MediaResolutionTier::px512);
    auto* atlas = r.preview_media_atlas;
    const auto frame = picture.frame(width, height);
    auto result = renderer_present_video_frame(&r, key, frame);
    bool ok = atlas && result.ok() && result.allocation.width == 480 && result.allocation.height == 270;
    if (ok) {
        result = renderer_present_video_frame(&r, key, frame, 640);
        ok = result.ok() && result.allocation.width == 640 && result.allocation.height == 360;
    }
    if (ok) {
        // Expansion requests more pixels under the same playback identity.
        result = renderer_present_video_frame(&r, key, frame, 1920);
        ok = result.ok() && result.allocation.width == 1920 && result.allocation.height == 1080;
    }
    if (ok) {
        result = renderer_present_video_frame(&r, key, frame, 640);
        ok = result.ok() && result.allocation.width == 640 && result.allocation.height == 360;
    }
    if (atlas) media_atlas_forget(atlas, key);
    picture.release();
    log_u64(ok ? LogLevel::info : LogLevel::error, "probe", "native_video_display_extent", ok);
    return ok;
}
bool video_retention_probe(D3D11Renderer& r, MediaAssetKey key, u16 width, u16 height) noexcept {
    if (!width || !height || width > 256 || height > 256) return false;
    // Exercise the shipping NV12 -> atlas -> retained UI path, independently
    // of any decoder. Source dimensions and the visible widget stay fixed.
    const u32 storage_width = (width + 1u) & ~1u;
    const u32 storage_height = (height + 1u) & ~1u;
    u8 pixels[256 * 256 * 3 / 2]{};
    const u32 luma_bytes = storage_width * storage_height;
    for (u32 i = 0; i < luma_bytes; ++i) pixels[i] = 80;
    for (u32 i = luma_bytes; i < luma_bytes * 3u / 2u; ++i) pixels[i] = 128;
    Nv12TestPicture picture{};
    if (!picture.create(r.graphics.device, storage_width, storage_height, pixels)) return false;
    const GpuVideoFrame frame = picture.frame(width, height);
    const u32 text_builds = r.content_text_retention.builds;
    const u32 text_uploads = r.text_instance_uploads;
    const u32 layout_builds = r.paint_stream_builds;
    const u32 paint_uploads = r.paint_instance_uploads;
    const u32 media_uploads = r.media_instance_uploads;
    const u32 full_draws = r.full_present_draws;
    const u32 partial_draws = r.partial_present_draws;
    const u32 video_frames = r.media_gpu.video.frames;
    const u64 partial_pixels = r.partial_present_pixels;
    constexpr u32 count = 32;
    bool ok = true;
    for (u32 i = 0; ok && i < count; ++i) {
        ok = renderer_present_video_frame(&r, key, frame).ok() && probe_present(&r, kDefaultTheme);
    }
    picture.release();
    const u64 damage_pixels = r.partial_present_pixels - partial_pixels;
    ok = ok && r.content_text_retention.builds == text_builds &&
        r.text_instance_uploads == text_uploads && r.paint_stream_builds == layout_builds &&
        r.paint_instance_uploads == paint_uploads && r.media_instance_uploads == media_uploads &&
        r.full_present_draws == full_draws && r.partial_present_draws == partial_draws + count &&
        r.media_gpu.video.frames == video_frames + count && damage_pixels > 0 &&
        damage_pixels < static_cast<u64>(r.width) * r.height * count;
    log_u64(ok ? LogLevel::info : LogLevel::error, "probe", "video_retained_frames", r.media_gpu.video.frames - video_frames);
    log_u64(LogLevel::info, "probe", "video_retained_text_builds", r.content_text_retention.builds - text_builds);
    log_u64(LogLevel::info, "probe", "video_retained_full_draws", r.full_present_draws - full_draws);
    log_u64(LogLevel::info, "probe", "video_retained_partial_draws", r.partial_present_draws - partial_draws);
    log_u64(LogLevel::info, "probe", "video_retained_damage_pixels", damage_pixels);
    return ok;
}
}
