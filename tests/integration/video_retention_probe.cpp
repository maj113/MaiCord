#include "probe_present.hpp"
#include "video_retention_probe.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/theme.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
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
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = storage_width; desc.Height = storage_height;
    desc.Format = DXGI_FORMAT_NV12;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    const D3D11_SUBRESOURCE_DATA data{pixels, storage_width, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(r.graphics.device->CreateTexture2D(&desc, &data, &texture))) return false;
    GpuVideoFrame frame{};
    frame.texture = texture; frame.width = width; frame.height = height;
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
    texture->Release();
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
