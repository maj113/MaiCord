#pragma once

#include "maicord/core/log.hpp"
#include "maicord/render/d3d11/renderer_settings_visibility.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace background_theme_detail {

bool failure(u32 stage) noexcept;

struct PixelReadback {
    ID3D11Texture2D* staging{};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    D3D11Renderer& renderer;
    u32 width{}, height{};
    bool bgra{}, readable{};
    explicit PixelReadback(D3D11Renderer& r) noexcept : renderer(r) {
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* source = nullptr;
        r.graphics.surface.target->GetResource(&resource);
        const HRESULT result = resource ? resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                   reinterpret_cast<void**>(&source))
                                        : E_FAIL;
        com_release(resource);
        if (FAILED(result))
            return;
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!bgra && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            com_release(source);
            return;
        }
        width = minimum(description.Width, r.width);
        height = minimum(description.Height, r.height);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = description.MiscFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(r.graphics.device->CreateTexture2D(&description, nullptr, &staging))) {
            r.graphics.context->CopyResource(staging, source);
            readable = SUCCEEDED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        }
        com_release(source);
    }
    PixelReadback(D3D11Renderer& r, ID3D11Texture2D* copy) noexcept : renderer(r) {
        if (!copy)
            return;
        D3D11_TEXTURE2D_DESC description{};
        copy->GetDesc(&description);
        staging = copy;
        staging->AddRef();
        width = description.Width;
        height = description.Height;
        bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        readable = SUCCEEDED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    }
    // Pixels where some channel differs by 2 or more: brighter (+1), dimmer (-1) or either (0).
    u32 visible_difference(const PixelReadback& other, i32 direction = 0) const noexcept {
        if (!readable || !other.readable || width != other.width || height != other.height)
            return 0;
        u32 changed = 0;
        for (u32 y = 0; y < height; ++y) {
            const auto* a = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            const auto* b = static_cast<const u8*>(other.mapped.pData) + y * other.mapped.RowPitch;
            for (u32 x = 0; x < width; ++x) {
                bool visible = false;
                for (u32 c = 0; c < 3; ++c) {
                    const i32 difference = static_cast<i32>(a[x * 4 + c]) - b[x * 4 + c];
                    visible = visible || (direction >= 0 && difference >= 2) ||
                              (direction <= 0 && difference <= -2);
                }
                changed += visible ? 1u : 0u;
            }
        }
        return changed;
    }
    bool independent_regions(const PixelReadback& other, u32* brighter, u32* dimmer) const noexcept {
        if (!readable || !other.readable || width != other.width || height != other.height || !brighter ||
            !dimmer)
            return false;
        u32 bright_regions = 0, dim_regions = 0;
        for (u32 y = 0; y < height; ++y) {
            const auto* a = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            const auto* b = static_cast<const u8*>(other.mapped.pData) + y * other.mapped.RowPitch;
            for (u32 x = 0; x < width; ++x) {
                const u32 region = (minimum(y * 3 / height, 2u) * 3) + minimum(x * 3 / width, 2u);
                for (u32 c = 0; c < 3; ++c) {
                    const i32 difference = static_cast<i32>(b[x * 4 + c]) - a[x * 4 + c];
                    if (difference >= 2)
                        bright_regions |= 1u << region;
                    if (difference <= -2)
                        dim_regions |= 1u << region;
                }
            }
        }
        *brighter = *dimmer = 0;
        for (u32 region = 0; region < 9; ++region) {
            if (bright_regions & (1u << region))
                ++*brighter;
            if (dim_regions & (1u << region))
                ++*dimmer;
        }
        return true;
    }
    ~PixelReadback() noexcept {
        if (readable)
            renderer.graphics.context->Unmap(staging, 0);
        com_release(staging);
    }
    bool read(float x, float y, u8* rgb) const noexcept {
        if (!readable || !width || !height || !rgb || x < 0 || x >= 1 || y < 0 || y >= 1)
            return false;
        const u32 px = minimum(width - 1, static_cast<u32>(width * x));
        const u32 py = minimum(height - 1, static_cast<u32>(height * y));
        const auto* pixel = static_cast<const u8*>(mapped.pData) + py * mapped.RowPitch + px * 4;
        rgb[0] = pixel[bgra ? 2 : 0];
        rgb[1] = pixel[1];
        rgb[2] = pixel[bgra ? 0 : 2];
        return true;
    }
    bool color(float x, float y, const float* expected) const noexcept {
        u8 rgb[3];
        if (!read(x, y, rgb))
            return false;
        for (u32 i = 0; i < 3; ++i) {
            const i32 want = static_cast<i32>(expected[i] * 255 + .5f);
            const i32 delta = static_cast<i32>(rgb[i]) - want;
            if (delta < -3 || delta > 3) {
                log_u64(LogLevel::error, "probe", "background_theme_pixel_channel", i);
                log_u64(LogLevel::error, "probe", "background_theme_pixel_actual", rgb[i]);
                log_u64(LogLevel::error, "probe", "background_theme_pixel_expected", want);
                return false;
            }
        }
        return true;
    }
    // Pixels of the region within `tolerance` of `ink` per channel; `canvas` (optional) counts near-white pixels.
    u32 ink_pixels(UiRect rectangle, Color ink, i32 tolerance, u32* canvas) const noexcept {
        const float scale = renderer.dpi / 96.f;
        const u32 left = static_cast<u32>(maximum(0.f, rectangle.left * scale));
        const u32 top = static_cast<u32>(maximum(0.f, rectangle.top * scale));
        const u32 right = minimum(width, static_cast<u32>(maximum(0.f, rectangle.right * scale)));
        const u32 bottom = minimum(height, static_cast<u32>(maximum(0.f, rectangle.bottom * scale)));
        const i32 expected[]{static_cast<i32>(ink.red * 255 + .5f), static_cast<i32>(ink.green * 255 + .5f),
                             static_cast<i32>(ink.blue * 255 + .5f)};
        u32 matching_ink = 0;
        for (u32 y = top; y < bottom; ++y)
            for (u32 x = left; x < right; ++x) {
                const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                const i32 rgb[]{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
                bool matching = true;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 delta = rgb[channel] - expected[channel];
                    matching = matching && delta >= -tolerance && delta <= tolerance;
                }
                if (matching)
                    ++matching_ink;
                if (canvas && rgb[0] >= 200 && rgb[1] >= 200 && rgb[2] >= 200)
                    ++*canvas;
            }
        return matching_ink;
    }
    bool secondary_ink(UiRect rectangle, Color ink) const noexcept {
        if (!readable || !ui_rect_has_area(rectangle))
            return false;
        const u32 matching_ink = ink_pixels(rectangle, ink, 20, nullptr);
        log_u64(LogLevel::info, "probe", "theme_editor_light_actual_styled_timestamp_ink_pixels",
                matching_ink);
        return matching_ink >= 4;
    }
    bool dark_text_ink(UiRect rectangle, Color ink) const noexcept {
        if (!readable || !ui_rect_has_area(rectangle))
            return false;
        u32 canvas = 0;
        const u32 solid_ink = ink_pixels(rectangle, ink, 24, &canvas);
        log_u64(LogLevel::info, "probe", "theme_editor_light_actual_body_ink_pixels", solid_ink);
        return solid_ink >= 8 && canvas >= 32;
    }
    u64 hash() const noexcept {
        if (!readable || !width || !height)
            return 0;
        u64 value = 0xcbf29ce484222325ull;
        for (u32 y = 0; y < height; ++y) {
            const auto* row = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            for (u32 x = 0; x < width * 4; ++x)
                value = (value ^ row[x]) * 0x100000001b3ull;
        }
        return value;
    }
};

// Draws the retained scene again under `theme` (defined with the pixel probes).
void restore_scene(D3D11Renderer& r, const Theme& theme) noexcept;

// Saves the renderer's paint stream, text and media counts so a probe can draw an isolated
// stream; restores them, and redraws the retained scene under `theme`, when it goes out of scope.
struct StreamSwap {
    D3D11Renderer& r;
    PaintStream saved;
    RendererTextStorage* text;
    u32 quads, batches;
    PaintStream& isolated;
    const Theme& theme;
    bool effect;
    StreamSwap(D3D11Renderer& renderer, PaintStream& stream, const Theme& restore_theme,
               bool clear_effect = false) noexcept
        : r(renderer), saved(renderer.paint_stream), text(renderer.content_text),
          quads(renderer.media_quad_count), batches(renderer.media_batch_count), isolated(stream),
          theme(restore_theme), effect(clear_effect) {}
    ~StreamSwap() noexcept {
        if (effect)
            renderer_clear_effect(&r);
        r.paint_stream = saved;
        r.content_text = text;
        r.media_quad_count = quads;
        r.media_batch_count = batches;
        paint_stream_release(&isolated);
        // Restore the original instance buffer before another retained
        // present; otherwise this temporary one-quad stream could survive.
        restore_scene(r, theme);
    }
};


void reference(const Theme& theme, float position, float* rgb) noexcept;

bool shader_pixels(D3D11Renderer& r, const Theme& theme, float seconds, u64* hash = nullptr,
                   const Theme& restore_theme = kDefaultTheme, bool canvas = false,
                   u8 (*sampled)[3] = nullptr, ID3D11Texture2D** snapshot = nullptr) noexcept;

bool overlay_surface_pixels(D3D11Renderer& r, bool solid = false) noexcept;

bool intensity_pixels(D3D11Renderer& r) noexcept;

bool dither_pixels(D3D11Renderer& r) noexcept;

bool star_independence_pixels(D3D11Renderer& r) noexcept;

bool comet_density_pixels(D3D11Renderer& r) noexcept;

bool default_effect_pixels(D3D11Renderer& r) noexcept;

bool solid_pixels(D3D11Renderer& r) noexcept;

bool effect_canvas_pixels(D3D11Renderer& r) noexcept;

// Package background effects against the renderer: the pass draws once per target frame under the frame cap,
// canvases sample one window-space target (continuity, widgets above), the resolution, pause, blend, opacity and
// budget behave, a lost device is rebuilt, and the shipped samples render and move (captured when
// MAICORD_UI_PROBE_CAPTURE_DIR is set).
bool effect_package_pixels(D3D11Renderer& r) noexcept;

bool retained_frames(D3D11Renderer& r, const Theme& theme) noexcept;

bool full_scene_visible(D3D11Renderer& r, const Theme& theme) noexcept;

} // namespace background_theme_detail

using namespace background_theme_detail;
} // namespace maicord
