#pragma once

#include "probe_present.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/render/raster_clip.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace surface_motion_probe_detail {

inline u64 bytes_hash(u64 hash, const void* data, usize bytes) noexcept {
    const auto* input = static_cast<const u8*>(data);
    for (usize i = 0; i < bytes; ++i)
        hash = (hash ^ input[i]) * 1099511628211ull;
    return hash;
}

struct RetainedSnapshot {
    u64 geometry;
    u32 builds, paint_uploads, media_uploads, text_uploads, text_rebuilds, glyph_builds;
    u32 source_refreshes, preview_uploads;
};

inline RetainedSnapshot snapshot(const D3D11Renderer& r) noexcept {
    u64 h = 1469598103934665603ull;
    h = bytes_hash(h, r.paint_stream.quads, r.paint_stream.count * sizeof(PaintQuad));
    h = bytes_hash(h, r.paint_stream.batches, r.paint_stream.batch_count * sizeof(PaintBatch));
    h = bytes_hash(h, r.paint_stream.clips, r.paint_stream.clip_count * sizeof(PaintClip));
    h = bytes_hash(h, r.media_draw.quad_data, r.media_quad_count * sizeof(MediaQuad));
    h = bytes_hash(h, r.media_draw.batch_data, r.media_batch_count * sizeof(MediaDrawBatch));
    h = bytes_hash(h, r.media_draw.handle_data, r.media_quad_count * sizeof(MediaAtlasHandle));
    h = bytes_hash(h, r.media_draw.domain_data, r.media_quad_count);
    const u32 markers[]{r.media_picker_paint_first_batch,
                        r.media_picker_paint_end_batch,
                        r.media_picker_foreground_first_batch,
                        r.media_picker_text_first_batch,
                        r.media_picker_text_end_batch,
                        r.media_picker_media_first_batch,
                        r.media_picker_media_end_batch,
                        r.popup_paint_first_batch,
                        r.popup_paint_end_batch,
                        r.popup_text_first_batch,
                        r.popup_text_end_batch,
                        r.popup_scene_media_first_batch,
                        r.popup_scene_media_end_batch,
                        r.settings_choice_paint_first_batch,
                        r.settings_choice_paint_end_batch,
                        r.settings_choice_text_first_batch,
                        r.settings_choice_text_end_batch,
                        r.settings_choice_media_first_batch,
                        r.settings_choice_media_end_batch};
    h = bytes_hash(h, markers, sizeof(markers));
    if (r.content_text) {
        h = bytes_hash(h, r.content_text->quads, r.content_text->quad_count * sizeof(GlyphQuad));
        h = bytes_hash(h, r.content_text->batches, r.content_text->batch_count * sizeof(GlyphDrawBatch));
    }
    return {h,
            r.paint_stream_builds,
            r.paint_instance_uploads,
            r.media_instance_uploads,
            r.text_instance_uploads,
            r.content_text ? r.content_text->rebuilds : 0,
            r.glyph_text.cache_builds,
            r.media_source_refreshes,
            r.preview_media_uploads};
}

inline bool same(const RetainedSnapshot& a, const RetainedSnapshot& b) noexcept {
    return a.geometry == b.geometry && a.builds == b.builds && a.paint_uploads == b.paint_uploads &&
           a.media_uploads == b.media_uploads && a.text_uploads == b.text_uploads &&
           a.text_rebuilds == b.text_rebuilds && a.glyph_builds == b.glyph_builds &&
           a.preview_uploads == b.preview_uploads;
}

// One bounded staging texture and four bounded CPU images for this offline
// measurement only. Reading/capturing never calls a draw or uploads instances.
struct Readback {
    ID3D11Texture2D* staging{};
    u8* images{};
    u32 width{}, height{}, bytes{};
    ~Readback() noexcept {
        if (staging)
            staging->Release();
        if (images)
            VirtualFree(images, 0, MEM_RELEASE);
    }
    bool initialize(D3D11Renderer& r) noexcept {
        if (!r.width || !r.height || static_cast<u64>(r.width) * r.height > 2'097'152)
            return false;
        width = r.width;
        height = r.height;
        bytes = width * height * 4;
        images = static_cast<u8*>(
            VirtualAlloc(nullptr, static_cast<usize>(bytes) * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ID3D11Resource* resource = nullptr;
        r.graphics.target->GetResource(&resource);
        ID3D11Texture2D* source = nullptr;
        const HRESULT hr =
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
        resource->Release();
        if (FAILED(hr) || !images) {
            if (source)
                source->Release();
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        source->Release();
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(r.graphics.device->CreateTexture2D(&desc, nullptr, &staging));
    }
    bool read(D3D11Renderer& r, u32 index) noexcept {
        if (!staging || index >= 4 || r.width != width || r.height != height)
            return false;
        ID3D11Resource* source = nullptr;
        r.graphics.target->GetResource(&source);
        r.graphics.context->CopyResource(staging, source);
        source->Release();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        staging->GetDesc(&desc);
        auto* destination = images + index * bytes;
        for (u32 y = 0; y < height; ++y) {
            const auto* row = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            for (u32 x = 0; x < width; ++x) {
                const u32 p = (y * width + x) * 4;
                destination[p] = row[x * 4 + (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? 2 : 0)];
                destination[p + 1] = row[x * 4 + 1];
                destination[p + 2] = row[x * 4 + (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 2)];
                destination[p + 3] = row[x * 4 + 3];
            }
        }
        r.graphics.context->Unmap(staging, 0);
        return true;
    }
    bool capture(u32 index, const wchar_t* filename) const noexcept {
        wchar_t path[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", path, 900);
        if (!length)
            return true;
        if (length >= 900)
            return false;
        u32 end = length;
        path[end++] = L'/';
        for (u32 i = 0; filename[i] && end < 1023; ++i)
            path[end++] = filename[i];
        HANDLE file =
            CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        BITMAPFILEHEADER header{};
        BITMAPINFOHEADER info{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + bytes;
        info.biSize = sizeof(info);
        info.biWidth = width;
        info.biHeight = -static_cast<LONG>(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        DWORD written = 0;
        const bool ok =
            WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
            WriteFile(file, &info, sizeof(info), &written, nullptr) && written == sizeof(info) &&
            WriteFile(file, images + index * bytes, bytes, &written, nullptr) && written == bytes;
        CloseHandle(file);
        return ok;
    }
};

struct Ranges {
    u32 paint_first, paint_end, text_first, text_end, media_first, media_end;
};
inline bool draw_fixed(D3D11Renderer& renderer) noexcept {
    const float clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                        kDefaultTheme.base_lowest.blue, 1.f};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
    return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, false, false, nullptr, 10000);
}
inline Ranges ranges(const D3D11Renderer& r, RendererLayerSurface surface) noexcept {
    switch (surface) {
    case RendererLayerSurface::profile:
        return {r.profile_paint_first_batch, r.profile_paint_end_batch,
                r.profile_text_first_batch, r.profile_text_end_batch,
                r.profile_media_first_batch, r.profile_media_end_batch};
    case RendererLayerSurface::gallery:
        return {r.media_picker_paint_first_batch, r.media_picker_paint_end_batch,
                r.media_picker_text_first_batch,  r.media_picker_text_end_batch,
                r.media_picker_media_first_batch, r.media_picker_media_end_batch};
    case RendererLayerSurface::popup:
        return {r.popup_paint_first_batch, r.popup_paint_end_batch,         r.popup_text_first_batch,
                r.popup_text_end_batch,    r.popup_scene_media_first_batch, r.popup_scene_media_end_batch};
    case RendererLayerSurface::forum_form:
        return {r.form_paint_first_batch, r.forum_paint_end_batch,   r.form_text_first_batch,
                r.forum_text_end_batch,   r.forum_media_first_batch, r.forum_media_end_batch};
    default:
        return {r.settings_choice_paint_first_batch, r.settings_choice_paint_end_batch,
                r.settings_choice_text_first_batch,  r.settings_choice_text_end_batch,
                r.settings_choice_media_first_batch, r.settings_choice_media_end_batch};
    }
}

inline u32 distance(const u8* a, const u8* b) noexcept {
    u32 result = 0;
    for (u32 i = 0; i < 3; ++i)
        result += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
    return result;
}

// Inspect real raster ink inside retained primitive/scissor intersections.
// The half sample is not asserted to be a flattened-group interpolation:
// overlapping native primitives correctly compose their own faded alpha.
inline bool alpha_region(const Readback& rb, UiRect rectangle, float scale, u32 kind, u32* contrast,
                         const Color* fill = nullptr) noexcept {
    const auto clip = raster_clip_rectangle(rectangle, scale, rb.width, rb.height);
    u32 strongest = 0, selected = 0;
    for (u32 y = static_cast<u32>(maximum(clip.top, 0)); y < static_cast<u32>(maximum(clip.bottom, 0)); ++y)
        for (u32 x = static_cast<u32>(maximum(clip.left, 0)); x < static_cast<u32>(maximum(clip.right, 0));
             ++x) {
            const u32 p = (y * rb.width + x) * 4;
            const auto* one = rb.images + p;
            const auto* zero = rb.images + rb.bytes + p;
            if (fill) {
                const u8 expected[]{static_cast<u8>(fill->blue * 255.f + .5f),
                                    static_cast<u8>(fill->green * 255.f + .5f),
                                    static_cast<u8>(fill->red * 255.f + .5f)};
                if (distance(one, expected) > 3)
                    continue;
            }
            if (kind == 1 && (one[0] < 170 || one[1] < 170 || one[2] < 170 || zero[0] > 155 ||
                              zero[1] > 155 || zero[2] > 155))
                continue;
            // The original fixture is solid magenta, so panel/text pixels
            // inside a media rectangle cannot masquerade as media opacity.
            if (kind == 2 && (one[0] < 170 || one[1] > 90 || one[2] < 230))
                continue;
            const u32 value = distance(one, zero);
            if (value > strongest) {
                strongest = value;
                selected = p;
            }
        }
    const u32 required = kind == 0 ? 15 : 90;
    if (strongest < required)
        return false;
    const auto* one = rb.images + selected;
    const auto* zero = rb.images + rb.bytes + selected;
    const auto* half = rb.images + rb.bytes * 2 + selected;
    const u32 from_zero = distance(half, zero), from_one = distance(half, one);
    *contrast = maximum(*contrast, strongest);
    return from_zero * 5 >= strongest && from_one * 5 >= strongest && from_zero * 5 <= strongest * 4 &&
           from_one * 5 <= strongest * 4;
}

template <typename GeometryGuard>
bool measure(D3D11Renderer& r, RendererLayerSurface surface, UiRect bounds, bool require_media,
             const wchar_t* const (&names)[3], GeometryGuard guard) noexcept {
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "surface_motion_surface", static_cast<u32>(surface));
        log_u64(LogLevel::error, "probe", "surface_motion_failure_stage", stage);
        return false;
    };
    if (!renderer_rebuild_content_text(&r, kDefaultTheme) || !probe_present(&r, kDefaultTheme) ||
        !probe_present(&r, kDefaultTheme))
        return fail();
    const auto baseline = snapshot(r);
    const auto batches = ranges(r, surface);
    if (batches.paint_first >= batches.paint_end || batches.paint_end > r.paint_stream.batch_count ||
        !r.content_text || batches.text_first >= batches.text_end ||
        batches.text_end > r.content_text->batch_count || batches.media_first > batches.media_end ||
        batches.media_end > r.media_batch_count ||
        (require_media && batches.media_first == batches.media_end))
        return fail();
    const auto stamp = renderer_capture_presentation_stamp(r, kDefaultTheme);
    if (renderer_set_layer_opacity(nullptr, surface, .5f) ||
        renderer_set_layer_opacity(&r, RendererLayerSurface::count, .5f) ||
        !renderer_presentation_stamp_equal(stamp, renderer_capture_presentation_stamp(r, kDefaultTheme)))
        return fail();
    Readback rb;
    stage = 2;
    if (!rb.initialize(r))
        return fail();
    const float values[]{1.f, 0.f, .5f, 1.f};
    for (u32 i = 0; i < 4; ++i) {
        stage = 10 + i;
        renderer_set_layer_opacity(&r, surface, values[i]);
        if (!guard() || !probe_present(&r, kDefaultTheme) || !draw_fixed(r) ||
            !same(baseline, snapshot(r)) || !guard() ||
            !renderer_presentation_stamp_equal(stamp,
                                               renderer_capture_presentation_stamp(r, kDefaultTheme)) ||
            !rb.read(r, i))
            return fail();
    }
    // Preserve the actual samples even when a subsequent pixel assertion
    // fails, so a renderer failure remains visually reviewable.
    stage = 14;
    for (u32 i = 0; i < 3; ++i)
        if (!rb.capture(i == 0 ? 1 : i == 1 ? 2 : 0, names[i]))
            return fail();
    stage = 20;
    if (bytes_hash(0, rb.images, rb.bytes) != bytes_hash(0, rb.images + rb.bytes * 3, rb.bytes))
        return fail();
    const float scale = static_cast<float>(r.dpi) / 96.f;
    const auto outside =
        raster_clip_rectangle({bounds.left - 32, bounds.top - 32, bounds.right + 32, bounds.bottom + 32},
                              scale, rb.width, rb.height);
    u32 unrelated = 0;
    for (u32 y = 0; y < rb.height; ++y)
        for (u32 x = 0; x < rb.width; ++x) {
            if (static_cast<i32>(x) >= outside.left && static_cast<i32>(x) < outside.right &&
                static_cast<i32>(y) >= outside.top && static_cast<i32>(y) < outside.bottom)
                continue;
            const u32 p = (y * rb.width + x) * 4;
            ++unrelated;
            if (distance(rb.images + p, rb.images + rb.bytes + p) ||
                distance(rb.images + p, rb.images + rb.bytes * 2 + p))
                return fail();
        }
    // A forum form includes its viewport-wide dimmer; every pixel belongs to
    // this modal surface, so there is no unrelated exterior to compare.
    if (!unrelated && surface != RendererLayerSurface::forum_form)
        return fail();
    bool paint = false, glyph = false, media = false;
    u32 contrasts[3]{};
    stage = 21;
    for (u32 b = batches.paint_first; b < batches.paint_end; ++b) {
        const auto batch = r.paint_stream.batches[b];
        const auto clip = r.paint_stream.clips[batch.clip_index];
        for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
            const auto& quad = r.paint_stream.quads[q];
            if (quad.fill.alpha < .9f)
                continue;
            paint = alpha_region(rb,
                                 ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom},
                                                      {clip.left, clip.top, clip.right, clip.bottom}),
                                 scale, 0, &contrasts[0], &quad.fill) ||
                    paint;
        }
    }
    stage = 22;
    for (u32 b = batches.text_first; b < batches.text_end; ++b) {
        const auto batch = r.content_text->batches[b];
        for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
            const auto& quad = r.content_text->quads[q];
            glyph =
                alpha_region(
                    rb, ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom}, batch.clip),
                    scale, 1, &contrasts[1]) ||
                glyph;
        }
    }
    stage = 23;
    for (u32 b = batches.media_first; b < batches.media_end; ++b) {
        const auto batch = r.media_draw.batch_data[b];
        for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
            const auto& quad = r.media_draw.quad_data[q];
            media =
                alpha_region(
                    rb, ui_rect_intersection({quad.left, quad.top, quad.right, quad.bottom}, batch.clip),
                    scale, 2, &contrasts[2]) ||
                media;
        }
    }
    if (!paint || !glyph || (require_media && !media)) {
        log_u64(LogLevel::error, "probe", "surface_motion_paint_pixels", paint);
        log_u64(LogLevel::error, "probe", "surface_motion_glyph_pixels", glyph);
        log_u64(LogLevel::error, "probe", "surface_motion_media_pixels", media);
        return fail();
    }
    // Invalid values snap to opaque, including a quiet NaN. They must not
    // disturb source stamps, draw clipping, or any retained CPU/GPU stream.
    u32 nan_bits = 0x7fc00000;
    float nan = 0;
    CopyMemory(&nan, &nan_bits, sizeof(nan));
    const float invalid[]{-1.f, 2.f, nan};
    for (u32 invalid_index = 0; invalid_index < 3; ++invalid_index) {
        const float value = invalid[invalid_index];
        stage = 25 + invalid_index;
        renderer_set_layer_opacity(&r, surface, .5f);
        renderer_set_layer_opacity(&r, surface, value);
        const bool drew = probe_present(&r, kDefaultTheme) && draw_fixed(r) && rb.read(r, 3);
        const bool retained = same(baseline, snapshot(r));
        const bool stable = guard() && renderer_presentation_stamp_equal(stamp,
            renderer_capture_presentation_stamp(r, kDefaultTheme));
        const bool identical = bytes_hash(0, rb.images, rb.bytes) == bytes_hash(0, rb.images + rb.bytes * 3, rb.bytes);
        if (!drew || !retained || !stable || !identical) {
            log_u64(LogLevel::error, "probe", "surface_motion_invalid_drew", drew);
            log_u64(LogLevel::error, "probe", "surface_motion_invalid_retained", retained);
            log_u64(LogLevel::error, "probe", "surface_motion_invalid_stable", stable);
            log_u64(LogLevel::error, "probe", "surface_motion_invalid_identical", identical);
            return fail();
        }
    }
    log_u64(LogLevel::info, "probe", "surface_motion_surface", static_cast<u32>(surface));
    log_u64(LogLevel::info, "probe", "surface_motion_paint_contrast", contrasts[0]);
    log_u64(LogLevel::info, "probe", "surface_motion_glyph_contrast", contrasts[1]);
    log_u64(LogLevel::info, "probe", "surface_motion_media_contrast", contrasts[2]);
    log_u64(LogLevel::info, "probe", "surface_motion_unrelated_pixels", unrelated);
    log_u64(LogLevel::info, "probe", "surface_motion_resident_atlas_refreshes",
            r.media_source_refreshes - baseline.source_refreshes);
    log_write(LogLevel::info, "probe",
              "surface_motion_native_alpha_retained_geometry_uploads_text_sources_passed");
    return true;
}
} // namespace surface_motion_probe_detail
} // namespace maicord
