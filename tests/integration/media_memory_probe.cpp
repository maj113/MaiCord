// GPU memory of the media atlases, their disposal backups, the animation
// frame caches and one live H.264 or H.265 stream, measured with DXGI video-memory
// queries around synthetic workloads on the probe renderer. Every scenario
// starts from released media resources and the client's paged atlases; pixels
// written before an idle trim are read back afterwards at the same place.
// MAICORD_MEDIA_MEMORY_REPORT=<path> appends a plain-text report (no captures).
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/media/image/png.hpp"
#include "maicord/core/log.hpp"
#include "video_retention_probe.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "../support/hevc_stream_fixture.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/gpu/api.hpp"

namespace maicord {
namespace {

// The client's atlas records (src/app/account/account_ui_runtime.hpp); the
// geometry is MediaAtlasSurface's.
constexpr u32 kPrimaryEntries = 4'096;
constexpr u32 kPrimaryRows = 128;
constexpr u32 kPreviewEntries = 512;
constexpr u32 kPreviewRows = 128;

struct MediaMemoryFixture {
    MediaAtlas primary, preview;
    MediaAtlasEntry primary_entries[kPrimaryEntries];
    u32 primary_buckets[kPrimaryEntries * 2];
    GlyphAtlasEntry primary_allocations[kPrimaryEntries];
    GlyphAtlasRow primary_rows[kPrimaryRows];
    MediaAtlasEntry preview_entries[kPreviewEntries];
    u32 preview_buckets[kPreviewEntries * 2];
    GlyphAtlasEntry preview_allocations[kPreviewEntries];
    GlyphAtlasRow preview_rows[kPreviewRows];
    u8 pixels[2'048 * 1'152 * 4];
    u8 scanlines[(2'048 * 4 + 1) * 1'152];
    PngImage png;
    u8 indices[512 * 288];
    u8 stream[1u << 20];
    u8 bits[1u << 20];
};
MediaMemoryFixture g_memory;

struct Report {
    HANDLE file = INVALID_HANDLE_VALUE;
    void line(const char* name, u64 value) noexcept {
        log_u64(LogLevel::info, "probe", name, value);
        if (file == INVALID_HANDLE_VALUE)
            return;
        char text[160];
        u32 n = 0;
        for (; name[n] && n < 120; ++n)
            text[n] = name[n];
        text[n++] = '=';
        char digits[24];
        u32 d = 0;
        do {
            digits[d++] = static_cast<char>('0' + value % 10u);
            value /= 10u;
        } while (value && d < 20);
        while (d)
            text[n++] = digits[--d];
        text[n++] = '\r';
        text[n++] = '\n';
        DWORD written = 0;
        WriteFile(file, text, n, &written, nullptr);
    }
};

struct Usage {
    u64 local, nonlocal;
};

// Released D3D11 objects are destroyed only after the GPU passes their last
// use; wait for an event query (bounded) so freed memory leaves CurrentUsage.
void settle(ID3D11DeviceContext* context) noexcept {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    ID3D11Query* query = nullptr;
    const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
    if (device && SUCCEEDED(device->CreateQuery(&description, &query))) {
        context->End(query);
        context->Flush();
        BOOL done = FALSE;
        for (u32 spin = 0; spin < 500 && context->GetData(query, &done, sizeof(done), 0) != S_OK; ++spin)
            Sleep(1);
        query->Release();
    }
    com_release(device);
    context->Flush();
    Sleep(2);
}

Usage usage(IDXGIAdapter3* adapter, ID3D11DeviceContext* context) noexcept {
    settle(context);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, remote{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &remote)))
        return {};
    return {local.CurrentUsage, remote.CurrentUsage};
}

// Bytes of memory backing the renderer's media surfaces (atlases, backups, caches).
u64 media_bytes(const D3D11Renderer& r) noexcept {
    return gpu_paged_texture_bytes(r.media_gpu.surface) + gpu_paged_texture_bytes(r.media_gpu.preview_surface) +
           gpu_paged_texture_bytes(r.media_gpu.backup) + gpu_paged_texture_bytes(r.media_gpu.preview_backup) +
           gpu_paged_texture_bytes(r.animation_gpu.surface) +
           gpu_paged_texture_bytes(r.preview_animation_gpu.surface);
}

constexpr u8 pattern(u32 seed, u32 x, u32 y, u32 channel) noexcept {
    return static_cast<u8>(channel == 3 ? 255u
                                        : (x * (7u + channel) + y * (3u + channel * 2u) + seed * 31u) & 255u);
}

void fill(u32 seed, u16 width, u16 height) noexcept {
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            for (u32 c = 0; c < 4; ++c)
                g_memory.pixels[(y * width + x) * 4u + c] = pattern(seed, x, y, c);
}

bool upload(D3D11Renderer& r, MediaAssetKey key, u32 seed, u16 width, u16 height) noexcept {
    fill(seed, width, height);
    return renderer_upload_media_straight_rgba8(&r, key, width, height, width * 4u,
                                                {g_memory.pixels, u32{width} * height * 4u})
        .ok();
}

// Viewer-sized pictures use the PNG path (the straight-RGBA path is capped at
// 512 px): unfiltered RGBA8 scanlines with the same pattern.
bool upload_png(D3D11Renderer& r, MediaAssetKey key, u32 seed, u16 width, u16 height) noexcept {
    PngImage& image = g_memory.png;
    image = PngImage{};
    image.width = width;
    image.height = height;
    image.bit_depth = 8;
    image.color_type = 6;
    image.channels = 4;
    image.bytes_per_pixel = 4;
    image.row_bytes = u32{width} * 4u;
    image.row_stride = image.row_bytes + 1u;
    image.inflated_bytes = image.row_stride * height;
    for (u32 y = 0; y < height; ++y) {
        u8* row = g_memory.scanlines + y * image.row_stride;
        row[0] = 0;
        for (u32 x = 0; x < width; ++x)
            for (u32 c = 0; c < 4; ++c)
                row[1 + x * 4u + c] = pattern(seed, x, y, c);
    }
    return renderer_upload_png_scanlines(&r, key, image, {g_memory.scanlines, image.inflated_bytes}).ok();
}

// Samples a sparse grid of one resident allocation through a staging copy.
bool resident_matches(D3D11Renderer& r, MediaAtlas* atlas, const GpuPagedTexture* surface, MediaAssetKey key,
                      u32 seed) noexcept {
    const MediaAtlasResult a = media_atlas_peek(atlas, key);
    ID3D11Texture2D* texture = surface ? surface->texture : nullptr;
    if (!a.ok() || !texture)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Width = a.width;
    description.Height = a.height;
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    const D3D11_BOX box{a.x, a.y, 0, static_cast<UINT>(a.x + a.width), static_cast<UINT>(a.y + a.height), 1};
    r.graphics.context1->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = SUCCEEDED(r.graphics.context1->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    for (u32 y = 0; ok && y < a.height; y += 7)
        for (u32 x = 0; ok && x < a.width; x += 5) {
            const u8* p = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4u;
            for (u32 c = 0; ok && c < 4; ++c)
                ok = p[c] == pattern(seed, x, y, c);
        }
    if (mapped.pData)
        r.graphics.context1->Unmap(staging, 0);
    staging->Release();
    return ok;
}

// The client's surfaces, empty: both atlases and both animation frame caches.
bool bind_production_atlases(D3D11Renderer& r) noexcept {
    renderer_unbind_media_atlas(&r);
    auto& f = g_memory;
    animation_frame_cache_gpu_release(&r.animation_gpu);
    animation_frame_cache_gpu_release(&r.preview_animation_gpu);
    return animation_frame_cache_gpu_initialize(&r.animation_gpu) &&
           animation_frame_cache_gpu_initialize(&r.preview_animation_gpu, true) &&
           media_atlas_initialize(&f.primary, kMediaAtlasExtent, kMediaAtlasPageRows, 1,
                                  {f.primary_entries, kPrimaryEntries}, {f.primary_buckets, kPrimaryEntries * 2},
                                  {f.primary_allocations, kPrimaryEntries}, {f.primary_rows, kPrimaryRows}) &&
           media_atlas_configure_surface(&f.primary, MediaAtlasSurface::identity) &&
           media_atlas_initialize(&f.preview, kPreviewMediaAtlasExtent, kPreviewMediaAtlasPageRows, 1,
                                  {f.preview_entries, kPreviewEntries}, {f.preview_buckets, kPreviewEntries * 2},
                                  {f.preview_allocations, kPreviewEntries}, {f.preview_rows, kPreviewRows}) &&
           media_atlas_configure_surface(&f.preview, MediaAtlasSurface::preview) &&
           renderer_bind_media_atlas(&r, &f.primary) && renderer_bind_preview_media_atlas(&r, &f.preview);
}

GifPalette gray_palette(u32 shift) noexcept {
    GifPalette palette{};
    palette.count = 256;
    for (u32 i = 0; i < 256; ++i)
        palette.rgba[i] = 0xff000000u | (((i + shift) & 255u) * 0x010101u);
    return palette;
}

GifFrame gif_frame(u16 width, u16 height, u16 x, u16 y, GifDispose dispose) noexcept {
    GifFrame frame{};
    frame.width = width;
    frame.height = height;
    frame.x_offset = x;
    frame.y_offset = y;
    frame.dispose = dispose;
    return frame;
}

// Begins a GIF, advances `frames` pictures through the renderer and captures
// each one into the animation frame cache, as the media pump does.
bool play_gif(D3D11Renderer& r, MediaAssetKey key, u16 width, u16 height, GifDispose dispose, u32 frames) noexcept {
    for (u32 i = 0; i < u32{width} * height && i < sizeof(g_memory.indices); ++i)
        g_memory.indices[i] = static_cast<u8>(i * 13u);
    GifFrame previous = gif_frame(width, height, 0, 0, dispose);
    if (!renderer_begin_gif(&r, key, width, height, previous, gray_palette(0), {g_memory.indices, u32{width} * height})
             .ok())
        return false;
    for (u32 index = 0; index < frames; ++index) {
        GifFrame frame = gif_frame(static_cast<u16>(width / 2), static_cast<u16>(height / 2),
                                   static_cast<u16>((index * 7u) % (width / 2)),
                                   static_cast<u16>((index * 5u) % (height / 2)), dispose);
        if (index && !renderer_advance_gif(&r, key, previous, frame, gray_palette(index),
                                           {g_memory.indices, u32{frame.width} * frame.height})
                          .ok())
            return false;
        AnimationFrameCacheDescriptor descriptor{};
        descriptor.key.asset = key;
        descriptor.key.frame_index = index;
        descriptor.key.source_generation = 1;
        descriptor.control.gif = frame;
        descriptor.duration_us = 40'000;
        descriptor.width = width;
        descriptor.height = height;
        descriptor.kind = AnimationFrameCacheKind::gif;
        if (!renderer_capture_animation_frame(&r, descriptor))
            return false;
        if (index)
            previous = frame;
    }
    return true;
}

struct StreamCapture {
    D3D11Renderer* renderer;
    u32 frames, failures;
};

void on_stream_frame(void* context, u32 ssrc, const GpuStreamFrame& frame) noexcept {
    auto* capture = static_cast<StreamCapture*>(context);
    ++capture->frames;
    // Call video is reduced to its tile in the preview atlas.
    if (!renderer_present_video_frame(capture->renderer, media_asset_call_video(7, ssrc), *frame.gpu, 640).ok())
        ++capture->failures;
}

void on_stream_keyframe(void*, u32) noexcept {}

bool push_stream(GpuStreamDecoder* decoder, h264_fixture::Output* out, u32 timestamp) noexcept {
    const bool ok = out->ok && gpu_stream_decoder_push(decoder, {out->data, out->count}, timestamp);
    out->count = 0;
    out->ok = true;
    return ok;
}

// One 1080p camera-like stream: IDR plus P pictures, presented into the
// preview atlas. Returns the decoder (still alive) for measurement.
GpuStreamDecoder* start_stream(D3D11Renderer& r, StreamCapture* capture, bool restricted) noexcept {
    using namespace h264_fixture;
    capture->renderer = &r;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create(
        {r.graphics.device, 0x5150, {capture, on_stream_frame, on_stream_keyframe}, GpuStreamCodec::h264, true});
    if (!decoder)
        return nullptr;
    Output out{g_memory.stream, sizeof(g_memory.stream), 0, true};
    Bits bits{g_memory.bits, sizeof(g_memory.bits), 0, true};
    // WebRTC senders signal max_num_reorder_frames=0 and one reference; the
    // unrestricted variant exercises the adaptive-reorder surface budget.
    const Sps sps{1, 100, 120, 68, 2, 1, restricted, 0, 8, 40};
    const Pps pps{1, 1};
    write_idr(&out, &bits, sps, pps, 3, 0, 68, true, true);
    bool ok = push_stream(decoder, &out, 1'000);
    for (u16 i = 1; ok && i <= 5; ++i) {
        write_p(&out, &bits, sps, pps, i, static_cast<u16>(i * 2u), true, 4);
        ok = push_stream(decoder, &out, 1'000u + i * 3'000u);
    }
    if (!ok || capture->frames == 0 || capture->failures) {
        gpu_stream_decoder_destroy(decoder);
        return nullptr;
    }
    return decoder;
}

// The same 1080p stream shape in H.265 (CTB 32, one slice per CTU row).
GpuStreamDecoder* start_hevc_stream(D3D11Renderer& r, StreamCapture* capture) noexcept {
    using namespace hevc_fixture;
    capture->renderer = &r;
    if (!(gpu_stream_decoder_codecs(r.graphics.device) & gpu_stream_codec_bit(GpuStreamCodec::h265)))
        return nullptr;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create(
        {r.graphics.device, 0x5150, {capture, on_stream_frame, on_stream_keyframe}, GpuStreamCodec::h265, true});
    if (!decoder)
        return nullptr;
    Output out{g_memory.stream, sizeof(g_memory.stream), 0, true};
    Bits bits{g_memory.bits, sizeof(g_memory.bits), 0, true};
    const Sps sps{1, 1920, 1088, 8, 5, 1, 2, 8, 123};
    const Pps pps{1, 1};
    write_idr(&out, &bits, sps, pps, 3, ctb_columns(sps));
    bool ok = push_stream(decoder, &out, 1'000);
    for (u16 i = 1; ok && i <= 5; ++i) {
        write_picture(&out, &bits, sps, pps, kTrailR, i, 0);
        ok = push_stream(decoder, &out, 1'000u + i * 3'000u);
    }
    if (!ok || capture->frames == 0 || capture->failures) {
        gpu_stream_decoder_destroy(decoder);
        return nullptr;
    }
    return decoder;
}

} // namespace

bool media_memory_probe(D3D11Renderer& r) noexcept {
    Report report{};
    wchar_t path[1024]{};
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_MEDIA_MEMORY_REPORT", path, 1024);
    if (length && length < 1024)
        report.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* base = nullptr;
    IDXGIAdapter3* adapter = nullptr;
    bool ok = SUCCEEDED(r.graphics.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
              SUCCEEDED(dxgi->GetAdapter(&base)) &&
              SUCCEEDED(base->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter)));
    com_release(base);
    com_release(dxgi);
    auto* old_primary = r.media_atlas;
    auto* old_preview = r.preview_media_atlas;
    char name[96];
    const auto label = [&name](const char* metric) noexcept -> const char* {
        u32 n = 0;
        const char* const parts[] = {"media_memory_paged_", metric};
        for (const char* part : parts)
            for (u32 i = 0; part[i] && n < 95; ++i)
                name[n++] = part[i];
        name[n] = 0;
        return name;
    };
    const auto measure = [&](const char* scenario, Usage start) noexcept {
        const Usage now = usage(adapter, r.graphics.context1);
        char metric[64];
        u32 n = 0;
        const char* const delta_parts[] = {scenario, "_local_delta"};
        for (const char* part : delta_parts)
            for (u32 i = 0; part[i] && n < 63; ++i)
                metric[n++] = part[i];
        metric[n] = 0;
        report.line(label(metric), now.local > start.local ? now.local - start.local : 0);
        n = 0;
        const char* const owned_parts[] = {scenario, "_owned"};
        for (const char* part : owned_parts)
            for (u32 i = 0; part[i] && n < 63; ++i)
                metric[n++] = part[i];
        metric[n] = 0;
        report.line(label(metric), media_bytes(r));
    };
    // Per surface: the identity and preview atlases, their backups, the two frame caches.
    const auto surfaces = [&](const char* scenario) noexcept {
        const GpuPagedTexture* const owned[] = {r.media_gpu.surface,   r.media_gpu.preview_surface,
                                                r.media_gpu.backup,    r.media_gpu.preview_backup,
                                                r.animation_gpu.surface, r.preview_animation_gpu.surface};
        const char* const names[] = {"_identity", "_preview", "_identity_backup", "_preview_backup",
                                     "_identity_frames", "_preview_frames"};
        for (u32 index = 0; index < 6; ++index) {
            char metric[64];
            u32 n = 0;
            const char* const parts[] = {scenario, names[index]};
            for (const char* part : parts)
                for (u32 i = 0; part[i] && n < 63; ++i)
                    metric[n++] = part[i];
            metric[n] = 0;
            report.line(label(metric), gpu_paged_texture_bytes(owned[index]));
        }
    };

    u64 clock = 10'000'000; // Synthetic monotonic milliseconds for recency and idle trims.
    // (a) Bound but unused atlases: the login screen and an empty account.
    ok = ok && bind_production_atlases(r);
    renderer_media_maintain(&r, clock += 1'000);
    Usage start = ok ? usage(adapter, r.graphics.context1) : Usage{};
    if (ok)
        measure("idle", start);

    // (b) A timeline: avatars, emoji, still previews and GIFs (one with
    // previous-disposal) plus their animation frame captures.
    for (u32 i = 0; ok && i < 48; ++i)
        ok = upload(r, media_asset_user_avatar(5'000 + i, i + 1, false), i, 80, 80);
    for (u32 i = 0; ok && i < 24; ++i)
        ok = upload(r, media_asset_custom_emoji(6'000 + i, false), 100 + i, 48, 48);
    for (u32 i = 0; ok && i < 10; ++i)
        ok = upload(r, media_asset_attachment(7'000 + i, MediaAttachmentFormat::png, MediaResolutionTier::px512),
                    200 + i, 512, 288);
    for (u32 i = 0; ok && i < 3; ++i)
        ok = play_gif(r, media_asset_attachment(8'000 + i, MediaAttachmentFormat::gif, MediaResolutionTier::px512),
                      498, 280, i == 0 ? GifDispose::previous : GifDispose::background, 8);
    for (u32 i = 0; ok && i < 4; ++i)
        ok = play_gif(r, media_asset_custom_emoji(9'000 + i, true), 48, 48,
                      i == 0 ? GifDispose::previous : GifDispose::none, 8);
    if (ok) {
        measure("timeline", start);
        surfaces("timeline");
    }
    bool pixels = ok;
    for (u32 i = 0; pixels && i < 48; i += 7)
        pixels = resident_matches(r, &g_memory.primary, r.media_gpu.surface,
                                  media_asset_user_avatar(5'000 + i, i + 1, false), i);
    for (u32 i = 0; pixels && i < 10; ++i)
        pixels = resident_matches(r, &g_memory.preview, r.media_gpu.preview_surface,
                                  media_asset_attachment(7'000 + i, MediaAttachmentFormat::png, MediaResolutionTier::px512),
                                  200 + i);
    report.line(label("timeline_pixels"), pixels ? 1 : 0);
    ok = ok && pixels;

    if (ok) {
        // Idle trim with the last three previews and eight avatars still on screen: everything else goes,
        // what stays keeps its place and the rows that empty return their memory.
        renderer_media_maintain(&r, clock += 1'000);
        media_atlas_begin_visibility(&g_memory.primary);
        media_atlas_begin_visibility(&g_memory.preview);
        // No animation frame is on screen any more.
        animation_frame_cache_gpu_begin_visibility(&r.animation_gpu);
        animation_frame_cache_gpu_begin_visibility(&r.preview_animation_gpu);
        for (u32 i = 7; i < 10; ++i)
            media_atlas_find(&g_memory.preview, media_asset_attachment(7'000 + i, MediaAttachmentFormat::png,
                                                                       MediaResolutionTier::px512));
        for (u32 i = 40; i < 48; ++i)
            media_atlas_find(&g_memory.primary, media_asset_user_avatar(5'000 + i, i + 1, false));
        const u64 before = media_bytes(r);
        const u64 due = renderer_media_next_trim_ms(r);
        clock += 30'000;
        const bool trimmed = due && due <= clock && renderer_media_maintain(&r, clock);
        const u64 frames =
            gpu_paged_texture_bytes(r.animation_gpu.surface) + gpu_paged_texture_bytes(r.preview_animation_gpu.surface);
        const bool evicted = !media_atlas_peek(&g_memory.preview, media_asset_attachment(7'000, MediaAttachmentFormat::png,
                                                                                          MediaResolutionTier::px512))
                                  .ok();
        report.line(label("trim_ran"), trimmed);
        report.line(label("trim_frame_bytes"), frames);
        report.line(label("trim_backups"), (r.media_gpu.backup ? 1u : 0u) + (r.media_gpu.preview_backup ? 2u : 0u));
        pixels = trimmed && media_bytes(r) < before && !frames && !r.media_gpu.backup && !r.media_gpu.preview_backup &&
                 evicted;
        for (u32 i = 7; pixels && i < 10; ++i)
            pixels = resident_matches(
                r, &g_memory.preview, r.media_gpu.preview_surface,
                media_asset_attachment(7'000 + i, MediaAttachmentFormat::png, MediaResolutionTier::px512), 200 + i);
        for (u32 i = 40; pixels && i < 48; ++i)
            pixels = resident_matches(r, &g_memory.primary, r.media_gpu.surface,
                                      media_asset_user_avatar(5'000 + i, i + 1, false), i);
        report.line(label("trim_pixels"), pixels ? 1 : 0);
        report.line(label("trim_preview_rows"), glyph_atlas_committed_rows(g_memory.preview.allocations));
        report.line(label("trim_primary_rows"), glyph_atlas_committed_rows(g_memory.primary.allocations));
        if (pixels) {
            measure("trimmed", start);
            surfaces("trimmed");
            // The driver keeps freed heaps for reuse; IDXGIDevice3::Trim
            // returns them (diagnostic only, the client does not call it).
            IDXGIDevice3* trimmable = nullptr;
            if (SUCCEEDED(r.graphics.device->QueryInterface(__uuidof(IDXGIDevice3),
                                                            reinterpret_cast<void**>(&trimmable)))) {
                trimmable->Trim();
                trimmable->Release();
                measure("trimmed_dxgi_trim", start);
            }
        }
        ok = pixels && renderer_media_next_trim_ms(r) == 0;
    }

    // (c) Media viewer: one 4K image decoded at the 2048 px viewer tier.
    ok = ok && bind_production_atlases(r);
    start = ok ? usage(adapter, r.graphics.context1) : Usage{};
    const MediaAssetKey viewer =
        media_asset_attachment(10'000, MediaAttachmentFormat::png, MediaResolutionTier::px2048);
    ok = ok && upload_png(r, viewer, 300, 2'048, 1'152);
    if (ok) {
        measure("viewer", start);
        surfaces("viewer");
    }
    pixels = ok && resident_matches(r, &g_memory.preview, r.media_gpu.preview_surface, viewer, 300);
    report.line(label("viewer_pixels"), pixels ? 1 : 0);
    ok = ok && pixels;

    // (d) One synthetic 1080p camera stream presented as a call tile
    // (WebRTC-style restricted SPS, then the adaptive-reorder variant).
    // (e) The same stream in H.265 when the GPU decodes it.
    constexpr const char* kStreams[] = {"stream", "stream_unrestricted", "stream_hevc"};
    constexpr const char* kSurfaces[] = {"stream_surfaces", "stream_unrestricted_surfaces", "stream_hevc_surfaces"};
    constexpr const char* kDecoderBytes[] = {"stream_decoder_bytes", "stream_unrestricted_decoder_bytes",
                                             "stream_hevc_decoder_bytes"};
    for (u32 variant = 0; ok && variant < 3; ++variant) {
        ok = bind_production_atlases(r);
        start = ok ? usage(adapter, r.graphics.context1) : Usage{};
        StreamCapture capture{};
        GpuStreamDecoder* decoder = !ok           ? nullptr
                                    : variant < 2 ? start_stream(r, &capture, variant == 0)
                                                  : start_hevc_stream(r, &capture);
        if (!decoder) {
            log_write(LogLevel::warning, "probe", "media_memory_stream_skipped");
            break;
        }
        const auto stats = gpu_stream_decoder_stats(decoder);
        measure(kStreams[variant], start);
        const Usage active = usage(adapter, r.graphics.context1);
        report.line(label(kSurfaces[variant]), stats.surfaces);
        if (variant == 0) {
            // Hidden call view: the decoder releases its DXVA objects.
            const bool released = gpu_stream_decoder_release_idle(decoder);
            const Usage idle = usage(adapter, r.graphics.context1);
            report.line(label("stream_hidden_freed"), active.local > idle.local ? active.local - idle.local : 0);
            ok = released && gpu_stream_decoder_stats(decoder).surfaces == 0;
        }
        gpu_stream_decoder_destroy(decoder);
        const Usage gone = usage(adapter, r.graphics.context1);
        report.line(label(kDecoderBytes[variant]), active.local > gone.local ? active.local - gone.local : 0);
    }

    ok = ok && bind_production_atlases(r) && video_display_extent_probe(r);
    // Leave the renderer in its shipping configuration for later probes.
    bind_production_atlases(r);
    renderer_unbind_media_atlas(&r);
    if (old_primary)
        renderer_bind_media_atlas(&r, old_primary);
    if (old_preview)
        renderer_bind_preview_media_atlas(&r, old_preview);
    com_release(adapter);
    if (report.file != INVALID_HANDLE_VALUE)
        CloseHandle(report.file);
    log_write(LogLevel::info, "probe", ok ? "media_memory_probe_passed" : "media_memory_probe_failed");
    return ok;
}

} // namespace maicord
