#include "maicord/ui/activity_status.hpp"
#include "background_theme_probe.hpp"
#include "settings_theme_probe.hpp"
#include "theme_views_probe.hpp"
#include "probe_present.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "maicord/core/log.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "maicord/render/content_text_retention.hpp"
#include "maicord/render/background_fx.hpp"
#include "maicord/render/renderer_settings_visibility.hpp"
#include "maicord/ui/member_list_scene.hpp"

#include "maicord/sys/d3d11.hpp"

#include "maicord_fx_plugin_reference_ps.hpp"

namespace maicord {
namespace {
bool near_background(float a, float b) noexcept {
    return a - b < .0001f && b - a < .0001f;
}
bool failure(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "background_theme_failure_stage", stage);
    return false;
}
struct PixelReadback {
    ID3D11Texture2D* staging{};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    D3D11Renderer& renderer;
    u32 width{}, height{};
    bool bgra{}, readable{};
    explicit PixelReadback(D3D11Renderer& r) noexcept : renderer(r) {
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* source = nullptr;
        r.graphics.target->GetResource(&resource);
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
    u32 visible_difference(const PixelReadback& other, u32 threshold = 2) const noexcept {
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
                    visible = visible || difference >= static_cast<i32>(threshold) ||
                              difference <= -static_cast<i32>(threshold);
                }
                if (visible)
                    ++changed;
            }
        }
        return changed;
    }
    u32 directed_difference(const PixelReadback& other, bool brighter) const noexcept {
        if (!readable || !other.readable || width != other.width || height != other.height)
            return 0;
        u32 count = 0;
        for (u32 y = 0; y < height; ++y) {
            const auto* a = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
            const auto* b = static_cast<const u8*>(other.mapped.pData) + y * other.mapped.RowPitch;
            for (u32 x = 0; x < width; ++x) {
                bool changed = false;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 difference = static_cast<i32>(a[x * 4 + channel]) - b[x * 4 + channel];
                    changed |= brighter ? difference >= 2 : difference <= -2;
                }
                count += changed ? 1u : 0u;
            }
        }
        return count;
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
    bool color(float x, float y, const float* expected) const noexcept {
        if (!readable || x < 0 || x >= 1 || y < 0 || y >= 1)
            return false;
        const u32 px = minimum(width - 1, static_cast<u32>(width * x));
        const u32 py = minimum(height - 1, static_cast<u32>(height * y));
        const auto* pixel = static_cast<const u8*>(mapped.pData) + py * mapped.RowPitch + px * 4;
        const u8 rgb[]{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
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
    bool secondary_ink(UiRect rectangle, Color ink) const noexcept {
        if (!readable || !ui_rect_has_area(rectangle))
            return false;
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
                    matching = matching && delta >= -20 && delta <= 20;
                }
                if (matching)
                    ++matching_ink;
            }
        log_u64(LogLevel::info, "probe", "theme_editor_light_actual_styled_timestamp_ink_pixels",
                matching_ink);
        return matching_ink >= 4;
    }
    bool dark_text_ink(UiRect rectangle, Color ink) const noexcept {
        if (!readable || !ui_rect_has_area(rectangle))
            return false;
        const float scale = renderer.dpi / 96.f;
        const u32 left = static_cast<u32>(maximum(0.f, rectangle.left * scale));
        const u32 top = static_cast<u32>(maximum(0.f, rectangle.top * scale));
        const u32 right = minimum(width, static_cast<u32>(maximum(0.f, rectangle.right * scale)));
        const u32 bottom = minimum(height, static_cast<u32>(maximum(0.f, rectangle.bottom * scale)));
        const i32 expected[]{static_cast<i32>(ink.red * 255 + .5f), static_cast<i32>(ink.green * 255 + .5f),
                             static_cast<i32>(ink.blue * 255 + .5f)};
        u32 solid_ink = 0, canvas = 0;
        for (u32 y = top; y < bottom; ++y)
            for (u32 x = left; x < right; ++x) {
                const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                const i32 rgb[]{pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
                bool matching = true;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 delta = rgb[channel] - expected[channel];
                    matching = matching && delta >= -24 && delta <= 24;
                }
                if (matching)
                    ++solid_ink;
                if (rgb[0] >= 200 && rgb[1] >= 200 && rgb[2] >= 200)
                    ++canvas;
            }
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
void reference(const Theme& theme, float position, float* rgb) noexcept {
    const float base[]{theme.base_low.red, theme.base_low.green, theme.base_low.blue};
    if (!background_theme_valid(theme.background)) {
        for (u32 c = 0; c < 3; ++c)
            rgb[c] = base[c];
        return;
    }
    Color colors[5]{};
    ui_background_palette(theme, colors);
    if (theme.background.color_count == 1) {
        rgb[0] = colors[0].red;
        rgb[1] = colors[0].green;
        rgb[2] = colors[0].blue;
        return;
    }
    const float selected = position * (theme.background.color_count - 1);
    const u32 first =
        minimum(static_cast<u32>(selected), static_cast<u32>(theme.background.color_count - 2));
    const float blend = selected - first;
    const float a[]{colors[first].red, colors[first].green, colors[first].blue};
    const float b[]{colors[first + 1].red, colors[first + 1].green, colors[first + 1].blue};
    for (u32 c = 0; c < 3; ++c)
        rgb[c] = a[c] + (b[c] - a[c]) * blend;
}
bool shader_pixels(D3D11Renderer& r, const Theme& theme, float seconds, u64* hash = nullptr,
                   const Theme& restore_theme = kDefaultTheme, bool canvas = false,
                   u8 (*sampled)[3] = nullptr, ID3D11Texture2D** snapshot = nullptr) noexcept {
    PaintStream isolated{};
    if (!paint_stream_initialize(&isolated))
        return false;
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    if (!paint_stream_reset(&isolated, width, height) ||
        !paint_stream_push(&isolated, 0, 0, width, height, theme.base_low)) {
        paint_stream_release(&isolated);
        return false;
    }
    if (background_theme_valid(theme.background))
        isolated.quads[0].border_width = -3;
    if (canvas && !ui_background_paint_canvas(&isolated, {0, 0, width, height}, theme, theme.base_low)) {
        paint_stream_release(&isolated);
        return false;
    }
    const auto saved_paint = r.paint_stream;
    auto* saved_text = r.content_text;
    const u32 saved_quads = r.media_quad_count, saved_batches = r.media_batch_count;
    RendererTextStorage empty{};
    r.paint_stream = isolated;
    r.content_text = &empty;
    r.media_quad_count = r.media_batch_count = 0;
    struct Restore {
        D3D11Renderer& r;
        const PaintStream& paint;
        RendererTextStorage* text;
        u32 quads, batches;
        PaintStream& isolated;
        const Theme& theme;
        ~Restore() noexcept {
            r.paint_stream = paint;
            r.content_text = text;
            r.media_quad_count = quads;
            r.media_batch_count = batches;
            paint_stream_release(&isolated);
            // Restore the original instance buffer before another retained
            // present; otherwise this temporary one-quad stream could survive.
            d3d11_renderer_draw_stream(&r, theme, true, true, nullptr);
        }
    } restore{r, saved_paint, saved_text, saved_quads, saved_batches, isolated, restore_theme};
    if (!renderer_set_background_phase(&r, seconds) ||
        !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr))
        return false;
    PixelReadback pixels{r};
    if (snapshot) {
        if (!pixels.readable || !pixels.staging)
            return false;
        pixels.staging->AddRef();
        *snapshot = pixels.staging;
        return true;
    }
    if (hash) {
        *hash = pixels.hash();
        return *hash != 0;
    }
    float direction_x = 0, direction_y = 0;
    if (!ui_background_direction(theme.background.angle_degrees, &direction_x, &direction_y))
        return false;
    const auto& controls = r.pipeline.background_constants.controls;
    if (controls[0] - direction_x > .0001f || direction_x - controls[0] > .0001f ||
        controls[1] - direction_y > .0001f || direction_y - controls[1] > .0001f)
        return false;
    const float projection = width * (direction_x < 0 ? -direction_x : direction_x) +
                             height * (direction_y < 0 ? -direction_y : direction_y);
    const float samples[]{.05f, .25f, .5f, .75f, .95f};
    u32 sample_index = 0;
    for (float t : samples) {
        float expected[3]{};
        const bool vertical = theme.background.angle_degrees == 0;
        const float x = vertical ? .5f : t, y = vertical ? t : .5f;
        const float position =
            .5f + ((x - .5f) * width * direction_x + (y - .5f) * height * direction_y) / projection;
        reference(theme, position, expected);
        if (canvas) {
            const float multiplier = ((1000u - 7u * theme.background.intensity + 5u) / 10u) / 100.f;
            const bool light = theme.appearance == ThemeAppearance::light;
            const float alpha = light ? .95f * (.6f + .4f * multiplier) : .48f + .32f * multiplier;
            for (u32 channel = 0; channel < 3; ++channel)
                expected[channel] = (light ? 1.f : 0.f) * alpha + expected[channel] * (1 - alpha);
        }
        if (!pixels.color(x, y, expected) || (sampled && !pixels.read(x, y, sampled[sample_index])))
            return false;
        ++sample_index;
    }
    return true;
}
bool overlay_surface_pixels(D3D11Renderer& r, bool solid = false) noexcept {
    PaintStream isolated{};
    if (!paint_stream_initialize(&isolated))
        return failure(65);
    const auto original_paint = r.paint_stream;
    auto* original_text = r.content_text;
    const u32 original_media_count = r.media_quad_count, original_media_batches = r.media_batch_count;
    RendererTextStorage empty{};
    r.content_text = &empty;
    r.media_quad_count = r.media_batch_count = 0;
    struct Restore {
        D3D11Renderer& r;
        const PaintStream& paint;
        RendererTextStorage* text;
        u32 media_count, media_batches;
        PaintStream& isolated;
        ~Restore() noexcept {
            r.paint_stream = paint;
            r.content_text = text;
            r.media_quad_count = media_count;
            r.media_batch_count = media_batches;
            paint_stream_release(&isolated);
            d3d11_renderer_draw_stream(&r, kDefaultTheme, true, true, nullptr);
        }
    } restore{r, original_paint, original_text, original_media_count, original_media_batches, isolated};
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect cards[]{{32, 32, width * .4f, 192},
                         {width * .56f, height * .38f, width * .92f, height * .68f}};
    const Color border{.9f, .6f, .1f, 1};
    for (u32 mode = 0; mode < 2; ++mode) {
        Theme theme = mode ? kLightTheme : kDefaultTheme;
        theme.background = background_theme_defaults();
        theme.background.effect = BackgroundEffect::none;
        theme.background.angle_degrees = 120;
        if (solid) {
            theme.background.color_count = 1;
            theme.background.colors[0] = 0x15304f;
            theme.background.colors[1] = theme.background.colors[2] = 0;
        }
        for (u32 enabled = 0; enabled < 2; ++enabled) {
            theme.background = background_theme_defaults();
            theme.background.angle_degrees = 120;
            theme.background.intensity = 60;
            if (solid) {
                theme.background.color_count = 1;
                theme.background.colors[0] = 0x15304f;
                theme.background.colors[1] = theme.background.colors[2] = 0;
            }
            if (!enabled)
                theme.background = {};
            u8 covered[2][3]{};
            for (u32 underlay = 0; underlay < 2; ++underlay) {
                const Color content = underlay ? Color{0, .8f, .1f, 1} : Color{.8f, .05f, .05f, 1};
                if (!paint_stream_reset(&isolated, width, height) ||
                    !paint_stream_push(&isolated, 0, 0, width, height, theme.base_low))
                    return failure(66);
                if (enabled)
                    isolated.quads[0].border_width = -3;
                if (!paint_stream_push(&isolated, 0, 0, width, height, content))
                    return failure(67);
                for (u32 card = 0; card < 2; ++card)
                    if (!ui_background_paint_surface(
                            &isolated, cards[card], theme, theme.surface, border, 16, 3, .9f,
                            UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                            card ? UiBackgroundBorderTone::palette : UiBackgroundBorderTone::semantic))
                        return failure(68);
                if (isolated.count != 4 || isolated.quads[2].border_width != (enabled ? -7.f : 3.f) ||
                    isolated.quads[2].fill.alpha != 1)
                    return failure(69);
                r.paint_stream = isolated;
                if (!renderer_set_background_phase(&r, 0) ||
                    !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr))
                    return failure(70);
                PixelReadback pixels{r};
                for (u32 card = 0; card < 2; ++card) {
                    const auto rectangle = cards[card];
                    UiPoint point{rectangle.left + ui_rect_width(rectangle) * .45f,
                                  rectangle.top + ui_rect_height(rectangle) * .5f};
                    point.x = (static_cast<u32>(point.x * r.dpi / 96.f) + .5f) * 96.f / r.dpi;
                    point.y = (static_cast<u32>(point.y * r.dpi / 96.f) + .5f) * 96.f / r.dpi;
                    float expected[]{theme.surface.red, theme.surface.green, theme.surface.blue};
                    if (enabled) {
                        float dx = 0, dy = 0;
                        if (!ui_background_direction(theme.background.angle_degrees, &dx, &dy))
                            return failure(71);
                        const float position =
                            .5f + ((point.x - width * .5f) * dx + (point.y - height * .5f) * dy) /
                                      (width * (dx < 0 ? -dx : dx) + height * (dy < 0 ? -dy : dy));
                        float gradient[3]{};
                        reference(theme, position, gradient);
                        const float multiplier = .58f;
                        const float alpha = mode ? .8f + .2f * multiplier : .39f + .26f * multiplier;
                        const float reveal = (1 - alpha) * .9f;
                        for (u32 channel = 0; channel < 3; ++channel)
                            expected[channel] =
                                (mode ? 1.f : 0.f) * (1 - reveal) + gradient[channel] * reveal;
                    }
                    u8 actual[3]{};
                    if (!pixels.color(point.x / width, point.y / height, expected) ||
                        !pixels.read(point.x / width, point.y / height, actual))
                        return failure(71);
                    if (!underlay)
                        copy_bytes(covered[card], actual, sizeof(actual));
                    else if (!bytes_equal(covered[card], actual, sizeof(actual)))
                        return failure(72);
                    float border_rgb[]{border.red, border.green, border.blue};
                    if (enabled && card) {
                        const float bx =
                            (static_cast<u32>((rectangle.left + 1.5f) * r.dpi / 96.f) + .5f) * 96.f / r.dpi;
                        const float by =
                            (static_cast<u32>((rectangle.top + ui_rect_height(rectangle) * .5f) * r.dpi /
                                              96.f) +
                             .5f) *
                            96.f / r.dpi;
                        float dx = 0, dy = 0;
                        ui_background_direction(theme.background.angle_degrees, &dx, &dy);
                        float gradient[3]{};
                        reference(theme,
                                  .5f + ((bx - width * .5f) * dx + (by - height * .5f) * dy) /
                                            (width * (dx < 0 ? -dx : dx) + height * (dy < 0 ? -dy : dy)),
                                  gradient);
                        const float alpha = mode ? .8f + .2f * .58f : .39f + .26f * .58f;
                        const float reveal = (1 - alpha) * .9f;
                        for (u32 c = 0; c < 3; ++c) {
                            const float fill = (mode ? 1.f : 0.f) * (1 - reveal) + gradient[c] * reveal;
                            border_rgb[c] = fill * .88f + (mode ? 0.f : 1.f) * .12f;
                        }
                    }
                    const float content_rgb[]{content.red, content.green, content.blue};
                    if (!pixels.color((rectangle.left + 1.5f) / width,
                                      (rectangle.top + ui_rect_height(rectangle) * .5f) / height,
                                      border_rgb) ||
                        !pixels.color((rectangle.left + 1) / width, (rectangle.top + 1) / height,
                                      content_rgb))
                        return failure(73);
                }
            }
        }
    }
    log_write(LogLevel::info, "probe",
              "background_opaque_overlay_global_palette_border_corners_content_occlusion_flat_descriptor_"
              "native_pixels_passed");
    return true;
}
bool intensity_pixels(D3D11Renderer& r) noexcept {
    constexpr u8 levels[]{0, 10, 20, 40, 60, 74, 80, 100};
    for (u32 mode = 0; mode < 2; ++mode) {
        Theme theme = mode ? kLightTheme : kDefaultTheme;
        theme.background = background_theme_defaults();
        theme.background.angle_degrees = 0;
        theme.background.intensity = 100;
        u8 endpoint[5][3]{}, baseline[5][3]{}, previous[5][3]{};
        if (!shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, endpoint))
            return failure(52);
        theme.background.intensity = 0;
        if (!shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, baseline))
            return failure(53);
        copy_bytes(previous, baseline, sizeof(previous));
        for (const auto level : levels) {
            theme.background.intensity = level;
            u8 current[5][3]{};
            if (!shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, current))
                return failure(54);
            for (u32 sample = 0; sample < 5; ++sample) {
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 delta =
                        static_cast<i32>(current[sample][channel]) - previous[sample][channel];
                    const i32 direction =
                        static_cast<i32>(endpoint[sample][channel]) - baseline[sample][channel];
                    if ((direction > 0 && delta < -1) || (direction < 0 && delta > 1) ||
                        (!direction && (delta < -1 || delta > 1)))
                        return failure(56);
                }
            }
            copy_bytes(previous, current, sizeof(previous));
        }
    }
    log_write(LogLevel::info, "probe",
              "background_intensity_0_10_20_40_60_74_80_100_source_tier_dark_light_native_pixels_passed");
    return true;
}
bool dither_pixels(D3D11Renderer& r) noexcept {
    Theme theme = kDefaultTheme;
    theme.background = {{0, 0x101010, 0, 0, 0}, 90, 2, 100, BackgroundEffect::none, 50, 25, 35};
    ID3D11Texture2D *first_texture = nullptr, *second_texture = nullptr;
    const bool rendered =
        shader_pixels(r, theme, 0, nullptr, kDefaultTheme, false, nullptr, &first_texture) &&
        shader_pixels(r, theme, 7, nullptr, kDefaultTheme, false, nullptr, &second_texture);
    PixelReadback first{r, first_texture}, second{r, second_texture};
    com_release(first_texture);
    com_release(second_texture);
    if (!rendered || !first.readable || first.hash() != second.hash())
        return failure(97);
    const auto* row =
        static_cast<const u8*>(first.mapped.pData) + (first.height / 2) * first.mapped.RowPitch;
    u32 transitions = 0;
    u8 previous = row[0];
    for (u32 x = 0; x < first.width; ++x) {
        const float expected = 16.f * (x + .5f) / first.width;
        const float difference = row[x * 4] - expected;
        if (difference < -1.01f || difference > 1.01f)
            return failure(98);
        if (x && row[x * 4] != previous)
            ++transitions;
        previous = row[x * 4];
    }
    log_u64(LogLevel::info, "probe", "background_static_dither_ramp_transitions", transitions);
    if (transitions <= 32)
        return failure(99);
    log_write(LogLevel::info, "probe",
              "background_half_byte_fixed_dither_breaks_ramp_bands_without_phase_noise_passed");
    return true;
}
bool star_independence_pixels(D3D11Renderer& r) noexcept {
    Theme theme = kDefaultTheme;
    theme.background = {{0, 0, 0, 0, 0}, 90, 1, 100, BackgroundEffect::stars, 35, 40, 60};
    ID3D11Texture2D *first_texture = nullptr, *second_texture = nullptr;
    const bool rendered =
        shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &first_texture) &&
        shader_pixels(r, theme, 7, nullptr, kDefaultTheme, true, nullptr, &second_texture);
    PixelReadback first{r, first_texture}, second{r, second_texture};
    com_release(first_texture);
    com_release(second_texture);
    u32 brighter = 0, dimmer = 0;
    if (!rendered || !first.independent_regions(second, &brighter, &dimmer) || brighter < 2 || dimmer < 2) {
        log_u64(LogLevel::error, "probe", "background_stars_independent_brightening_regions", brighter);
        log_u64(LogLevel::error, "probe", "background_stars_independent_dimming_regions", dimmer);
        return failure(105);
    }
    log_write(LogLevel::info, "probe",
              "background_stars_independent_opposing_twinkle_regions_native_pixels_passed");
    return true;
}
bool comet_density_pixels(D3D11Renderer& r) noexcept {
    constexpr u8 intensities[]{20, 100};
    for (u32 mode = 0; mode < 2; ++mode)
        for (const auto intensity : intensities) {
            Theme theme = mode ? kLightTheme : kDefaultTheme;
            theme.background = {{0, 0, 0, 0, 0}, 90, 1, intensity, BackgroundEffect::none, 24, 18, 16};
            ID3D11Texture2D *baseline_texture = nullptr, *low_texture = nullptr, *high_texture = nullptr;
            bool rendered =
                shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &baseline_texture);
            theme.background.effect = BackgroundEffect::comets;
            rendered =
                rendered && shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &low_texture);
            theme.background.density = 100;
            rendered = rendered &&
                       shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &high_texture);
            PixelReadback baseline{r, baseline_texture}, low{r, low_texture}, high{r, high_texture};
            com_release(baseline_texture);
            com_release(low_texture);
            com_release(high_texture);
            const u32 low_population = low.visible_difference(baseline),
                      high_population = high.visible_difference(baseline);
            log_u64(LogLevel::info, "probe", "background_comet_density_low_pixel_population",
                    low_population);
            log_u64(LogLevel::info, "probe", "background_comet_density_high_pixel_population",
                    high_population);
            if (!rendered || !low_population || high_population < low_population * 2u)
                return failure(102);
        }
    log_write(LogLevel::info, "probe",
              "background_comet_density_changes_pixel_population_at_fixed_strength_and_phase_passed");
    return true;
}
bool default_effect_pixels(D3D11Renderer& r) noexcept {
    constexpr BackgroundEffect effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                         BackgroundEffect::stars_and_comets};
    constexpr u8 intensities[]{20, 100};
    constexpr u32 solids[]{0xffffff, 0x3c3c3c, 0};
    for (u32 mode = 0; mode < 2; ++mode)
        for (const auto intensity : intensities)
            for (const auto solid : solids) {
                Theme theme = mode ? kLightTheme : kDefaultTheme;
                theme.background = background_theme_defaults();
                theme.background.color_count = 1;
                theme.background.colors[0] = solid;
                theme.background.colors[1] = theme.background.colors[2] = 0;
                theme.background.intensity = intensity;
                ID3D11Texture2D* baseline_texture = nullptr;
                if (!shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &baseline_texture))
                    return failure(95);
                PixelReadback baseline{r, baseline_texture};
                com_release(baseline_texture);
                for (const auto effect : effects) {
                    const auto settings = background_effect_settings_defaults(effect);
                    theme.background.effect = effect;
                    theme.background.speed = settings.speed;
                    theme.background.strength = settings.strength;
                    theme.background.density = settings.density;
                    ID3D11Texture2D *first_texture = nullptr, *second_texture = nullptr;
                    const bool rendered =
                        shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &first_texture) &&
                        shader_pixels(r, theme, 7, nullptr, kDefaultTheme, true, nullptr, &second_texture);
                    PixelReadback first{r, first_texture}, second{r, second_texture};
                    com_release(first_texture);
                    com_release(second_texture);
                    const u32 visible = first.visible_difference(baseline),
                              moving = first.visible_difference(second);
                    if (!rendered || !visible || !moving) {
                        log_u64(LogLevel::error, "probe", "background_default_effect",
                                static_cast<u32>(effect));
                        log_u64(LogLevel::error, "probe", "background_default_effect_light", mode);
                        log_u64(LogLevel::error, "probe", "background_default_effect_intensity", intensity);
                        log_u64(LogLevel::error, "probe", "background_default_effect_solid_case",
                                solid == 0xffffff ? 0
                                : solid           ? 1
                                                  : 2);
                        log_u64(LogLevel::error, "probe", "background_default_effect_visible_pixels",
                                visible);
                        log_u64(LogLevel::error, "probe", "background_default_effect_moving_pixels",
                                moving);
                        return failure(96);
                    }
                }
            }
    log_write(LogLevel::info, "probe",
              "background_all_three_default_effects_visible_and_moving_two_byte_pixels_dark_light_solid_20_"
              "100_passed");
    return true;
}
bool solid_pixels(D3D11Renderer& r) noexcept {
    constexpr u16 angles[]{0, 90, 120};
    for (u32 mode = 0; mode < 2; ++mode) {
        Theme theme = mode ? kLightTheme : kDefaultTheme;
        theme.background = background_theme_defaults();
        theme.background.color_count = 1;
        theme.background.colors[0] = 0x15304f;
        theme.background.colors[1] = theme.background.colors[2] = 0;
        theme.background.intensity = 100;
        u64 first = 0;
        for (const auto angle : angles) {
            theme.background.angle_degrees = angle;
            u64 current = 0;
            if (!shader_pixels(r, theme, 0) || !shader_pixels(r, theme, 0, &current))
                return failure(92);
            if (first && first != current)
                return failure(93);
            first = current;
        }
        theme.background.effect = BackgroundEffect::comets;
        theme.background.strength = theme.background.density = 100;
        u64 phase_a = 0, phase_b = 0;
        if (!shader_pixels(r, theme, 0, &phase_a, kDefaultTheme, true) ||
            !shader_pixels(r, theme, 7, &phase_b, kDefaultTheme, true) || phase_a == phase_b)
            return failure(94);
    }
    if (!overlay_surface_pixels(r, true))
        return false;
    log_write(
        LogLevel::info, "probe",
        "background_single_color_dark_light_angle_independent_surface_and_effect_native_pixels_passed");
    return true;
}
bool effect_canvas_pixels(D3D11Renderer& r) noexcept {
    const Theme modes[]{kDefaultTheme, ui_theme_amoled(), kLightTheme};
    constexpr BackgroundEffect effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                         BackgroundEffect::stars_and_comets};
    for (u32 mode = 0; mode < 4; ++mode) {
        Theme theme = modes[mode < 3 ? mode : 0];
        if (mode == 3) {
            theme.background = {{0xffffff, 0, 0, 0, 0}, 90, 1, 100, BackgroundEffect::none, 0, 0, 0};
        }
        ID3D11Texture2D* baseline_texture = nullptr;
        if (!shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &baseline_texture))
            return failure(109);
        PixelReadback baseline{r, baseline_texture};
        com_release(baseline_texture);
        const auto foreground_key = content_text_theme_signature(theme);
        for (const auto effect : effects) {
            const auto defaults = background_effect_settings_defaults(effect);
            theme.background.effect = effect;
            theme.background.speed = defaults.speed;
            theme.background.strength = defaults.strength;
            theme.background.density = defaults.density;
            ID3D11Texture2D *first_texture = nullptr, *second_texture = nullptr;
            const bool rendered =
                shader_pixels(r, theme, 0, nullptr, kDefaultTheme, true, nullptr, &first_texture) &&
                shader_pixels(r, theme, 7, nullptr, kDefaultTheme, true, nullptr, &second_texture);
            PixelReadback first{r, first_texture}, second{r, second_texture};
            com_release(first_texture);
            com_release(second_texture);
            const u32 visible = first.directed_difference(baseline, mode != 2);
            if (!rendered || !visible || !first.visible_difference(second) ||
                foreground_key != content_text_theme_signature(theme)) {
                log_u64(LogLevel::error, "probe", "background_canvas_effect_mode", mode);
                log_u64(LogLevel::error, "probe", "background_canvas_effect", static_cast<u8>(effect));
                log_u64(LogLevel::error, "probe", "background_canvas_directed_pixel_count", visible);
                return failure(110);
            }
            // Raw root gradients contain no effects; canvas-only composition
            // keeps their phase identity and avoids duplicated detail.
            if (mode == 3) {
                u64 root_first = 0, root_second = 0;
                if (!shader_pixels(r, theme, 0, &root_first) || !shader_pixels(r, theme, 7, &root_second) ||
                    root_first != root_second)
                    return failure(111);
            }
        }
    }
    log_write(LogLevel::info, "probe",
              "background_flat_dark_amoled_light_and_bright_stop_dark_six_effects_canvas_ink_phase_root_"
              "isolation_passed");
    return true;
}
// A plugin effect (third-party mc.fx@1 bytecode) on a flat theme with no
// built-in effect: it draws once per canvas after the canvas fill and under
// the widget that follows; parameters repaint; None skips the pass; the
// device's rejection of bad bytecode reaches the host.
bool plugin_effect_pixels(D3D11Renderer& r) noexcept {
    PaintStream isolated{};
    if (!paint_stream_initialize(&isolated))
        return failure(120);
    const auto saved_paint = r.paint_stream;
    auto* saved_text = r.content_text;
    const u32 saved_quads = r.media_quad_count, saved_batches = r.media_batch_count;
    RendererTextStorage empty{};
    struct Restore {
        D3D11Renderer& r;
        const PaintStream& paint;
        RendererTextStorage* text;
        u32 quads, batches;
        PaintStream& isolated;
        ~Restore() noexcept {
            renderer_clear_plugin_effect(&r);
            r.paint_stream = paint;
            r.content_text = text;
            r.media_quad_count = quads;
            r.media_batch_count = batches;
            paint_stream_release(&isolated);
            d3d11_renderer_draw_stream(&r, kDefaultTheme, true, true, nullptr);
        }
    } restore{r, saved_paint, saved_text, saved_quads, saved_batches, isolated};
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const Theme theme = kDefaultTheme;
    const UiRect canvas{width * .25f, height * .25f, width * .75f, height * .75f};
    const Color widget{.8f, .05f, .05f, 1};
    if (!paint_stream_reset(&isolated, width, height) ||
        !paint_stream_push(&isolated, 0, 0, width, height, theme.base_lowest) ||
        !ui_background_paint_canvas(&isolated, canvas, theme, theme.base_low) ||
        !paint_stream_push(&isolated, canvas.left, canvas.top, canvas.left + (canvas.right - canvas.left) * .2f,
                           canvas.top + (canvas.bottom - canvas.top) * .2f, widget) ||
        isolated.count != 3 || !ui_background_canvas_quad(isolated.quads[1]) ||
        ui_background_canvas_quad(isolated.quads[0]) || ui_background_canvas_quad(isolated.quads[2]))
        return failure(121);
    r.paint_stream = isolated;
    r.content_text = &empty;
    r.media_quad_count = r.media_batch_count = 0;
    const ConstSlice<u8> code{g_maicord_fx_plugin_reference_shader, sizeof(g_maicord_fx_plugin_reference_shader)};
    float parameters[kBackgroundFxParameters]{1};
    if (FAILED(renderer_select_plugin_effect(&r, 0x51, 7, 1, code, parameters)) ||
        !d3d11_background_fx_plugin_active(r.background_fx) || !renderer_set_background_phase(&r, 0) ||
        !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr) || r.background_fx.draws_last_frame != 1)
        return failure(122);
    const float accent[]{background_fx_linear(theme.accent.red), background_fx_linear(theme.accent.green),
                         background_fx_linear(theme.accent.blue)};
    const float base[]{theme.base_low.red, theme.base_low.green, theme.base_low.blue};
    const float outside[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue};
    const float ink[]{widget.red, widget.green, widget.blue};
    {
        PixelReadback pixels{r};
        if (!pixels.color(.25f + .5f * .35f, .6f, accent) || !pixels.color(.25f + .5f * .8f, .6f, base) ||
            !pixels.color(.25f + .5f * .1f, .25f + .5f * .1f, ink) || !pixels.color(.1f, .1f, outside))
            return failure(123);
    }
    // New parameters repaint with the same shader object.
    auto* const shader = r.background_fx.plugin_shader;
    parameters[0] = 0;
    if (FAILED(renderer_select_plugin_effect(&r, 0x51, 7, 2, code, parameters)) ||
        r.background_fx.plugin_shader != shader || !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr))
        return failure(124);
    {
        PixelReadback pixels{r};
        if (!pixels.color(.25f + .5f * .35f, .6f, base))
            return failure(125);
    }
    // A few frames feed the GPU guard without tripping it on a cheap effect.
    parameters[0] = 1;
    if (FAILED(renderer_select_plugin_effect(&r, 0x51, 7, 3, code, parameters)))
        return failure(126);
    for (u32 frame = 0; frame < 8; ++frame)
        if (!renderer_set_background_phase(&r, frame * .05f) ||
            !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr) || r.background_fx.draws_last_frame != 1 ||
            r.background_fx.tripped)
            return failure(127);
    // None: no effect pass at all.
    renderer_clear_plugin_effect(&r);
    if (d3d11_background_fx_plugin_active(r.background_fx) ||
        !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr) || r.background_fx.draws_last_frame ||
        r.background_fx.source != BackgroundFxSource::none)
        return failure(128);
    {
        PixelReadback pixels{r};
        if (!pixels.color(.25f + .5f * .35f, .6f, base))
            return failure(129);
    }
    // Bytecode the device rejects: the failure reaches the host, nothing draws.
    u8 corrupt[sizeof(g_maicord_fx_plugin_reference_shader)];
    copy_bytes(corrupt, g_maicord_fx_plugin_reference_shader, sizeof(corrupt));
    for (u32 i = 32; i < sizeof(corrupt); ++i)
        corrupt[i] ^= 0x5a;
    if (SUCCEEDED(renderer_select_plugin_effect(&r, 0x51, 8, 4, {corrupt, sizeof(corrupt)}, parameters)) ||
        d3d11_background_fx_plugin_active(r.background_fx))
        return failure(130);
    log_write(LogLevel::info, "probe",
              "background_plugin_effect_once_per_canvas_under_widgets_parameters_none_and_rejection_passed");
    return true;
}
bool retained_frames(D3D11Renderer& r, const Theme& theme) noexcept {
    if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme))
        return false;
    const u32 paint_builds = r.paint_stream_builds, text_builds = r.content_text->rebuilds;
    const u32 media_uploads = r.media_instance_uploads;
    const auto stamp = renderer_capture_presentation_stamp(r, theme);
    auto* quads = r.paint_stream.quads;
    const auto first_count = r.paint_stream.count;
    const auto* text_quads = r.content_text->quads;
    const auto copied = r.content_text_prefix;
    u64 images[3]{};
    const float phases[]{1, 3, 6};
    for (u32 i = 0; i < 3; ++i) {
        if (!renderer_set_background_phase(&r, phases[i]) || !probe_present(&r, theme) ||
            !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
            return false;
        PixelReadback pixels{r};
        images[i] = pixels.hash();
        const auto current = renderer_capture_presentation_stamp(r, theme);
        if (!images[i] || stamp.primary != current.primary || stamp.secondary != current.secondary ||
            stamp.count != current.count || r.paint_stream_builds != paint_builds ||
            r.content_text->rebuilds != text_builds || r.media_instance_uploads != media_uploads ||
            r.paint_stream.quads != quads || r.paint_stream.count != first_count ||
            r.content_text->quads != text_quads ||
            !bytes_equal(&copied, &r.content_text_prefix, sizeof(copied)))
            return false;
    }
    return images[0] != images[1] && images[1] != images[2];
}
bool full_scene_visible(D3D11Renderer& r, const Theme& theme) noexcept {
    if (!r.ui_scene)
        return false;
    constexpr UiSemantic surfaces[]{UiSemantic::message_view, UiSemantic::channel_sidebar,
                                    UiSemantic::member_sidebar};
    float points[3][2][2]{};
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    for (u32 s = 0; s < 3; ++s) {
        const auto* node = ui_scene_resolve(*r.ui_scene, ui_scene_find(*r.ui_scene, surfaces[s]));
        if (!node || !ui_rect_has_area(node->rectangle))
            return false;
        for (u32 p = 0; p < 2; ++p) {
            points[s][p][0] = (node->rectangle.left + ui_rect_width(node->rectangle) * .5f) / width;
            points[s][p][1] =
                (node->rectangle.top + ui_rect_height(node->rectangle) * (p ? .78f : .55f)) / height;
        }
    }
    u8 baseline[3][2][3]{};
    if (!renderer_rebuild_content_text(&r, kDefaultTheme) || !probe_present(&r, kDefaultTheme) ||
        !d3d11_renderer_draw_stream(&r, kDefaultTheme, false, false, nullptr))
        return false;
    {
        PixelReadback pixels{r};
        for (u32 s = 0; s < 3; ++s)
            for (u32 p = 0; p < 2; ++p)
                if (!pixels.read(points[s][p][0], points[s][p][1], baseline[s][p]))
                    return false;
    }
    if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
        return false;
    if (!r.paint_stream.count || r.paint_stream.quads[0].border_width != -3 ||
        r.paint_stream.quads[0].fill.alpha < 1 || r.paint_stream.quads[0].radius != 0)
        return false;
    PixelReadback pixels{r};
    for (u32 s = 0; s < 3; ++s) {
        u32 minimum_delta = ~0u;
        for (u32 p = 0; p < 2; ++p) {
            u8 rgb[3]{};
            if (!pixels.read(points[s][p][0], points[s][p][1], rgb))
                return false;
            u32 delta = 0;
            for (u32 c = 0; c < 3; ++c) {
                const i32 difference = static_cast<i32>(rgb[c]) - baseline[s][p][c];
                delta += difference < 0 ? -difference : difference;
            }
            minimum_delta = minimum(minimum_delta, delta);
        }
        log_u64(LogLevel::info, "probe", "background_theme_full_scene_surface", s);
        log_u64(LogLevel::info, "probe", "background_theme_full_scene_minimum_rgb_delta", minimum_delta);
        // Real blank message/nav/member canvases must carry visible color,
        // not merely a gradient in an isolated shader or composer margin.
        if (minimum_delta < 12)
            return false;
    }
    return true;
}
bool chrome_native_pixels(D3D11Renderer& r, const Theme& theme) noexcept {
    if (!r.ui_scene || !r.message_composer_scene || !r.message_composer_scene->active)
        return failure(40);
    const auto node = [&](UiSemantic semantic) noexcept {
        return ui_scene_resolve(*r.ui_scene, ui_scene_find(*r.ui_scene, semantic));
    };
    const auto* messages = node(UiSemantic::message_view);
    const auto* root = node(UiSemantic::root);
    const auto* content = node(UiSemantic::content);
    const auto* header = node(UiSemantic::channel_header);
    const auto* footer = node(UiSemantic::channel_footer);
    const auto* composer = node(UiSemantic::composer);
    const auto* surface =
        ui_widget_manager_layout_node(r.message_composer_scene->widgets, r.message_composer_scene->surface);
    const auto* edit =
        message_composer_scene_layout_node(*r.message_composer_scene, MessageComposerControl::edit);
    if (!messages || !root || !header || !footer || !composer || !surface || !edit)
        return failure(41);
    const bool background = background_theme_valid(theme.background);
    const auto exact_quad = [&](UiRect rectangle) noexcept -> const PaintQuad* {
        for (u32 i = 0; i < r.paint_stream.count; ++i) {
            const auto& q = r.paint_stream.quads[i];
            if (ui_rect_equal({q.left, q.top, q.right, q.bottom}, rectangle))
                return &q;
        }
        return nullptr;
    };
    const auto* footer_quad = exact_quad(footer->rectangle);
    const auto* surface_quad = exact_quad(surface->rectangle);
    const auto* edit_quad = exact_quad(edit->rectangle);
    if (!footer_quad || !surface_quad || !edit_quad)
        return failure(42);
    if (background) {
        UiRect column = messages->rectangle;
        column.bottom = content ? content->rectangle.bottom : root->rectangle.bottom;
        const auto* plate = exact_quad(column);
        if (!plate || plate->border_width != -8 || plate->fill.alpha != 1 || plate->radius != 0 ||
            !near_background(plate->reserved_y,
                             1 - ui_background_tier_opacity(theme, UiBackgroundTier::chat)) ||
            footer_quad->border_width != -7 || surface_quad->border_width != -7 ||
            footer_quad->fill.alpha != 1 || surface_quad->fill.alpha != 1 || edit_quad->fill.alpha != 0)
            return failure(43);
    } else if (footer_quad->fill.alpha != 1 || surface_quad->fill.alpha != 1 || edit_quad->fill.alpha != 1)
        return failure(44);
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const float gap_x = (messages->rectangle.left + composer->rectangle.left) * .5f;
    const UiPoint points[]{
        {gap_x, (composer->rectangle.top + composer->rectangle.bottom) * .5f},
        {gap_x, messages->rectangle.bottom - 2},
        {gap_x, messages->rectangle.bottom + 2},
        {header->rectangle.right - 12, header->rectangle.top + 6},
        {(footer->rectangle.left + footer->rectangle.right) * .5f, footer->rectangle.top + 6},
        {(surface->rectangle.left + surface->rectangle.right) * .5f, surface->rectangle.top + 2.5f}};
    if (!d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
        return failure(45);
    PixelReadback pixels{r};
    u8 seam[2][3]{};
    for (u32 sample = 0; sample < 6; ++sample) {
        // Evaluate at the physical pixel center used by readback, so DPI and
        // fractional retained geometry do not introduce an oracle offset.
        const UiPoint point{(static_cast<u32>(points[sample].x * r.dpi / 96.f) + .5f) * 96.f / r.dpi,
                            (static_cast<u32>(points[sample].y * r.dpi / 96.f) + .5f) * 96.f / r.dpi};
        if (!ui_rect_contains(root->rectangle, point.x, point.y))
            return failure(46);
        float rgb[]{theme.base_low.red, theme.base_low.green, theme.base_low.blue};
        if (background) {
            const auto& constants = r.pipeline.background_constants;
            const float half_width = ui_rect_width(root->rectangle) * .5f,
                        half_height = ui_rect_height(root->rectangle) * .5f;
            const float dx = constants.controls[0], dy = constants.controls[1];
            const float denominator =
                2 * (half_width * (dx < 0 ? -dx : dx) + half_height * (dy < 0 ? -dy : dy));
            const float position = .5f + ((point.x - root->rectangle.left - half_width) * dx +
                                          (point.y - root->rectangle.top - half_height) * dy) /
                                             denominator;
            reference(theme, maximum(0.f, minimum(position, 1.f)), rgb);
        }
        u32 plates = 0;
        for (u32 batch = 0; batch < r.paint_stream.batch_count; ++batch) {
            const auto& b = r.paint_stream.batches[batch];
            const auto& clip = r.paint_stream.clips[b.clip_index];
            if (point.x < clip.left || point.x >= clip.right || point.y < clip.top ||
                point.y >= clip.bottom)
                continue;
            for (u32 i = b.first_quad; i < b.first_quad + b.quad_count; ++i) {
                const auto& q = r.paint_stream.quads[i];
                if (!i || q.right - q.left < 48 || q.bottom - q.top < 20 || q.fill.alpha <= 0 ||
                    !ui_rect_contains({q.left, q.top, q.right, q.bottom}, point.x, point.y))
                    continue;
                const float border_width = q.border_width == -8 ? 0
                                           : q.border_width == -7
                                               ? (q.reserved_x < 0 ? -q.reserved_x : q.reserved_x)
                                               : q.border_width;
                if ((q.border_width < 0 && q.border_width != -7 && q.border_width != -8) ||
                    point.x < q.left + border_width + 1 || point.x > q.right - border_width - 1 ||
                    point.y < q.top + border_width + 1 || point.y > q.bottom - border_width - 1) {
                    log_u64(LogLevel::error, "probe", "background_chrome_sample", sample);
                    log_u64(LogLevel::error, "probe", "background_chrome_edge_quad", i);
                    log_u64(LogLevel::error, "probe", "background_chrome_background_enabled", background);
                    log_u64(LogLevel::error, "probe", "background_chrome_point_x_millidip",
                            static_cast<u64>(point.x * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_point_y_millidip",
                            static_cast<u64>(point.y * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_left_millidip",
                            static_cast<u64>(q.left * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_top_millidip",
                            static_cast<u64>(q.top * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_right_millidip",
                            static_cast<u64>(q.right * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_bottom_millidip",
                            static_cast<u64>(q.bottom * 1000));
                    log_u64(LogLevel::error, "probe", "background_chrome_quad_border_millidip",
                            static_cast<u64>(maximum(0.f, q.border_width) * 1000));
                    return failure(47);
                }
                float fill[]{q.fill.red, q.fill.green, q.fill.blue};
                if (q.border_width == -7 || q.border_width == -8) {
                    const float dx = r.pipeline.background_constants.controls[0],
                                dy = r.pipeline.background_constants.controls[1];
                    const float position =
                        .5f + ((point.x - width * .5f) * dx + (point.y - height * .5f) * dy) /
                                  (width * (dx < 0 ? -dx : dx) + height * (dy < 0 ? -dy : dy));
                    float gradient[3]{};
                    reference(theme, maximum(0.f, minimum(position, 1.f)), gradient);
                    const float mode = theme.appearance == ThemeAppearance::light ? 1.f : 0.f;
                    for (u32 c = 0; c < 3; ++c)
                        fill[c] = mode * (1 - q.reserved_y) + gradient[c] * q.reserved_y;
                }
                for (u32 c = 0; c < 3; ++c)
                    rgb[c] = fill[c] * q.fill.alpha + rgb[c] * (1 - q.fill.alpha);
                ++plates;
            }
        }
        if (background && ((sample <= 2 && plates != 1) || (sample >= 3 && plates != 2))) {
            log_u64(LogLevel::error, "probe", "background_chrome_sample", sample);
            log_u64(LogLevel::error, "probe", "background_chrome_plate_count", plates);
            return failure(48);
        }
        if (!pixels.color(point.x / width, point.y / height, rgb)) {
            log_u64(LogLevel::error, "probe", "background_chrome_sample", sample);
            return failure(49);
        }
        if (sample == 1 || sample == 2)
            if (!pixels.read(point.x / width, point.y / height, seam[sample - 1]))
                return failure(50);
    }
    if (background) {
        u32 discontinuity = 0;
        for (u32 c = 0; c < 3; ++c) {
            const i32 difference = seam[0][c] - seam[1][c];
            discontinuity += difference < 0 ? -difference : difference;
        }
        if (discontinuity > 12)
            return failure(51);
    }
    log_write(LogLevel::info, "probe",
              background ? "background_chrome_composer_gap_header_opaque_single_effective_tier_footer_"
                           "composer_gradient_pixels_passed"
                         : "background_chrome_disabled_original_opaque_controls_pixels_passed");
    return true;
}
struct FixtureMessageColors {
    MessageTextPresentation* messages{};
    Color colors[kMaximumMessageTextCommands]{};
    u32 count{};
    u64 signature{};
    explicit FixtureMessageColors(D3D11Renderer& r) noexcept {
        auto* value = const_cast<MessageTextPresentation*>(r.message_text);
        if (!value || value->command_count > value->commands.count ||
            value->command_count > kMaximumMessageTextCommands)
            return;
        messages = value;
        count = value->command_count;
        signature = value->projection_signature;
        for (u32 i = 0; i < count; ++i)
            colors[i] = value->commands[i].color;
    }
    ~FixtureMessageColors() noexcept {
        if (!messages || messages->command_count != count)
            return;
        for (u32 i = 0; i < count; ++i)
            messages->commands[i].color = colors[i];
        messages->projection_signature = signature;
    }
    u32 project(const Theme& theme) noexcept {
        if (!messages || messages->command_count != count)
            return 0;
        u32 primary = 0;
        for (u32 i = 0; i < count; ++i) {
            auto& color = messages->commands[i].color;
            color = colors[i];
            if (bytes_equal(&colors[i], &kDefaultTheme.text_primary, sizeof(Color))) {
                color = theme.text_primary;
                ++primary;
            } else if (bytes_equal(&colors[i], &kDefaultTheme.text_secondary, sizeof(Color)))
                color = theme.text_secondary;
        }
        messages->projection_signature = message_text_presentation_projection_signature(*messages);
        return primary;
    }
};
bool theme_editor_pixels(BackgroundThemeProbeContext c, Theme theme) noexcept {
    auto& r = c.renderer;
    FixtureMessageColors restore_colors{r};
    UiThemeEditorScene editor{};
    UiSettingsScene authority{};
    authority.active = true;
    authority.revision = 1;
    const auto* old_settings = r.settings_scene;
    const auto* old_editor = r.theme_editor_scene;
    const auto* old_focus = r.focus_owner;
    struct Retire {
        D3D11Renderer& r;
        UiThemeEditorScene& scene;
        const UiSettingsScene* settings;
        const UiThemeEditorScene* editor;
        const UiWidgetManager* focus;
        ~Retire() noexcept {
            renderer_bind_theme_editor_scene(&r, editor);
            r.settings_scene = settings;
            r.focus_owner = focus;
            ui_theme_editor_release(&scene);
        }
    } retire{r, editor, old_settings, old_editor, old_focus};
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    auto editor_preferences = client_preferences_defaults();
    editor_preferences.theme = ClientTheme::custom_gradient;
    editor_preferences.gradient = {
        {0xcc0000, 0x00cc00, 0x0000cc, 0, 0}, 0, 3, 65, BackgroundEffect::none, 50, 25, 35};
    theme = client_theme_projection(editor_preferences);
    if (!restore_colors.project(theme))
        return failure(36);
    if (!ui_theme_editor_update(&editor, theme.background, 2, {0, 0, width, height}))
        return failure(20);
    r.settings_scene = &authority;
    renderer_bind_theme_editor_scene(&r, &editor);
    r.focus_owner = &editor.widgets;
    if (renderer_settings_page_visible(r) || !renderer_theme_editor_visible(r) || !authority.active ||
        !renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr))
        return failure(21);
    if (r.theme_editor_paint_first_batch >= r.theme_editor_paint_end_batch ||
        r.theme_editor_text_first_batch >= r.theme_editor_text_end_batch ||
        r.theme_editor_paint_end_batch > r.form_paint_first_batch ||
        r.theme_editor_text_end_batch > r.content_text->batch_count)
        return failure(22);
    const auto* sv = ui_theme_editor_node(editor, UiThemeEditorElement::sv);
    const auto* hue = ui_theme_editor_node(editor, UiThemeEditorElement::hue);
    const auto* gradient = ui_theme_editor_node(editor, UiThemeEditorElement::gradient);
    if (!sv || !hue || !gradient)
        return failure(23);
    const auto sample = [&](const PixelReadback& pixels, UiRect rect, float x, float y, u32 rgb) noexcept {
        const float expected[]{((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f,
                               (rgb & 255) / 255.f};
        return pixels.color((rect.left + ui_rect_width(rect) * x) / width,
                            (rect.top + ui_rect_height(rect) * y) / height, expected);
    };
    {
        PixelReadback pixels{r};
        if (!sample(pixels, sv->rectangle, .25f, .25f, ui_theme_editor_hsv_rgb(editor.hue, .25f, .75f)))
            return failure(24);
        if (!sample(pixels, hue->rectangle, .08f, .5f, ui_theme_editor_hsv_rgb(.08f, 1, 1)))
            return failure(25);
        if (!sample(pixels, gradient->rectangle, .25f, .5f, 0x666600))
            return failure(26);
    }
    if (!ui_theme_editor_focus(&editor, UiThemeEditorElement::sv))
        return failure(27);
    ui_widget_manager_hover(&editor.widgets, editor.elements[static_cast<u32>(UiThemeEditorElement::sv)]);
    ++editor.revision;
    if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        (c.capture && !c.capture(L"background-theme-editor-dark.bmp", theme)))
        return failure(28);
    // Warm the real prefix after initial glyph-atlas admission, then mutate
    // only draft/background. The base shell/messages must stay retained.
    if (!renderer_rebuild_content_text(&r, theme))
        return failure(29);
    const u32 base_builds = r.content_text_prefix.base_builds;
    const auto base_key = r.content_text_retention.key;
    const auto* before_quads = r.content_text->quads;
    auto changed = editor.draft;
    const u32 text_builds_before = r.content_text->rebuilds;
    const u32 paint_builds_before = r.paint_stream_builds;
    const u32 text_uploads_before = r.text_instance_uploads;
    const u32 paint_uploads_before = r.paint_instance_uploads;
    const UiRect panel_before = editor.panel;
    const u32 generation_before = editor.generation;
    const u32 widgets_before = editor.widgets.count, quads_before = r.content_text->quad_count;
    UiRect rectangles_before[kUiThemeEditorElements]{}, clips_before[kUiThemeEditorElements]{};
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto* node = ui_theme_editor_node(editor, static_cast<UiThemeEditorElement>(i));
        if (node) {
            rectangles_before[i] = node->rectangle;
            clips_before[i] = node->clip_rectangle;
        }
    }
    u32 changed_moves = 0;
    for (u32 i = 0; i < 64; ++i) {
        const UiPoint pointer{sv->rectangle.left + ui_rect_width(sv->rectangle) * (.3f + .5f * i / 63.f),
                              sv->rectangle.top + ui_rect_height(sv->rectangle) * .3f};
        if (ui_theme_editor_pointer(&editor, UiThemeEditorElement::sv, pointer, &changed))
            ++changed_moves;
    }
    if (changed_moves < 32 || !ui_rect_equal(editor.panel, panel_before) ||
        editor.generation != generation_before || editor.widgets.count != widgets_before ||
        r.content_text->quad_count != quads_before || r.content_text->rebuilds != text_builds_before ||
        r.paint_stream_builds != paint_builds_before || r.text_instance_uploads != text_uploads_before ||
        r.paint_instance_uploads != paint_uploads_before ||
        !ui_theme_editor_update(&editor, changed, editor.selected_stop, {0, 0, width, height}))
        return failure(30);
    bool geometry_stable = ui_rect_equal(editor.panel, panel_before) &&
                           editor.generation == generation_before && editor.widgets.count == widgets_before;
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto* node = ui_theme_editor_node(editor, static_cast<UiThemeEditorElement>(i));
        geometry_stable =
            geometry_stable && (node ? ui_rect_equal(node->rectangle, rectangles_before[i]) &&
                                           ui_rect_equal(node->clip_rectangle, clips_before[i])
                                     : !ui_rect_has_area(rectangles_before[i]));
    }
    if (!geometry_stable)
        return failure(104);
    editor_preferences.gradient = changed;
    theme = client_theme_projection(editor_preferences);
    const bool foreground_changed = content_text_theme_signature(theme) != base_key.theme_signature;
    if (!restore_colors.project(theme))
        return failure(36);
    const bool published = renderer_rebuild_content_text(&r, theme) && probe_present(&r, theme);
    const u32 expected_base_builds = base_builds + (foreground_changed ? 1u : 0u);
    if (!published || r.content_text_prefix.base_builds != expected_base_builds ||
        r.content_text->quads != before_quads || r.content_text->quad_count != quads_before ||
        // A changed foreground republishes under the emitter's colour-hashed
        // key (checked for stability below), not the bare theme signature.
        (!foreground_changed &&
         r.content_text_retention.key.theme_signature != content_text_theme_signature(theme)) ||
        r.content_text_retention.key.message_signature != r.message_text->projection_signature ||
        !authority.active) {
        log_u64(LogLevel::error, "probe", "background_editor_prefix_publication_succeeded", published);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_base_before", base_builds);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_base_after",
                r.content_text_prefix.base_builds);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_base_expected", expected_base_builds);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_storage_stable",
                r.content_text->quads == before_quads);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_quad_count_before", quads_before);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_quad_count_after",
                r.content_text->quad_count);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_theme_current",
                r.content_text_retention.key.theme_signature == content_text_theme_signature(theme));
        log_u64(LogLevel::error, "probe", "background_editor_prefix_message_current",
                r.content_text_retention.key.message_signature == r.message_text->projection_signature);
        log_u64(LogLevel::error, "probe", "background_editor_prefix_authority_active", authority.active);
        return failure(31);
    }
    // The shell emitter hashes actual command colors too. A coalesced source
    // foreground publication legitimately replaces that key. Repeating the
    // identical publication must retain its newly committed prefix exactly.
    const auto published_key = r.content_text_retention.key;
    if (!renderer_rebuild_content_text(&r, theme) ||
        r.content_text_prefix.base_builds != expected_base_builds ||
        r.content_text->quads != before_quads || r.content_text->quad_count != quads_before ||
        r.content_text_retention.key.shell_signature != published_key.shell_signature ||
        r.content_text_retention.key.message_signature != published_key.message_signature ||
        r.content_text_retention.key.theme_signature != published_key.theme_signature)
        return failure(103);
    log_write(
        LogLevel::info, "probe",
        "theme_editor_64_moves_zero_work_one_foreground_publication_and_stable_repeated_prefix_passed");
    auto* messages = const_cast<MessageTextPresentation*>(r.message_text);
    if (!messages || messages->command_count > messages->commands.count ||
        messages->command_count > kMaximumMessageTextCommands)
        return failure(36);
    auto light_preferences = client_preferences_defaults();
    light_preferences.theme = ClientTheme::custom_light_gradient;
    light_preferences.gradient = changed;
    const Theme light = client_theme_projection(light_preferences);
    UiRect body_ink{};
    const u32 projected_primary = restore_colors.project(light);
    for (u32 i = 0; i < messages->command_count; ++i) {
        auto& command = messages->commands[i];
        if (!ui_rect_has_area(body_ink) &&
            command.measurement_kind == MessageTextMeasurementKind::content && command.text.bytes) {
            body_ink = ui_rect_intersection(command.rectangle, command.clip);
            body_ink.right = minimum(body_ink.right, minimum(body_ink.left + 300.f, editor.panel.left - 8));
            body_ink.bottom = minimum(body_ink.bottom, body_ink.top + 24);
        }
    }
    UiRect timestamp_ink{};
    if (!r.message_scene || !r.chat_state || !r.user_store)
        return failure(90);
    for (u32 i = 0; i < messages->command_count; ++i) {
        const auto& command = messages->commands[i];
        bool has_subtext = false;
        for (u32 span = 0; span < command.styles.count; ++span)
            has_subtext = has_subtext || (command.styles[span].flags & text_style_subtext) != 0;
        if (!has_subtext)
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(*r.message_scene, command, *r.chat_state, *r.user_store,
                                   r.member_list_scene ? r.member_list_scene->measure : nullptr, light,
                                   &header) ||
            !header.timestamp.text.bytes || !(header.timestamp.style_flags & text_style_subtext))
            continue;
        const auto expected_ink = ui_background_muted_text_color(light);
        if (!bytes_equal(&header.timestamp.color, &expected_ink, sizeof(Color)))
            return failure(90);
        timestamp_ink = ui_rect_intersection(header.timestamp.rectangle, header.timestamp.clip);
        timestamp_ink.right = minimum(timestamp_ink.right, editor.panel.left - 8);
        if (ui_rect_has_area(timestamp_ink))
            break;
    }
    if (!ui_rect_has_area(timestamp_ink))
        return failure(90);
    messages->projection_signature = message_text_presentation_projection_signature(*messages);
    if (!projected_primary || !ui_rect_has_area(body_ink))
        return failure(37);
    editor.light_mode = true;
    ++editor.revision;
    if (!ui_theme_editor_focus(&editor, UiThemeEditorElement::hue))
        return failure(32);
    ui_widget_manager_hover(&editor.widgets, editor.elements[static_cast<u32>(UiThemeEditorElement::hue)]);
    if (!renderer_rebuild_content_text(&r, light) || !probe_present(&r, light) ||
        !d3d11_renderer_draw_stream(&r, light, false, false, nullptr))
        return failure(33);
    {
        PixelReadback pixels{r};
        if (!pixels.dark_text_ink(body_ink, light.text_primary))
            return failure(38);
        if (!pixels.secondary_ink(timestamp_ink, ui_background_muted_text_color(light)))
            return failure(91);
    }
    if (c.capture && !c.capture(L"background-theme-editor-light.bmp", light))
        return failure(39);
    // Keep the prior Light view unchanged; the additional view exposes the
    // real inline effect controls through the drawer's own scroll projection.
    BackgroundTheme effect_draft{};
    if (!ui_theme_editor_action(&editor, UiThemeEditorElement::effect_comets, 0, &effect_draft) ||
        !ui_theme_editor_update(&editor, effect_draft, editor.selected_stop, {0, 0, width, height}) ||
        !ui_theme_editor_scroll(&editor, 5000) ||
        !ui_theme_editor_focus(&editor, UiThemeEditorElement::apply))
        return failure(74);
    const auto default_comets = background_effect_settings_defaults(BackgroundEffect::comets);
    if (effect_draft.speed != default_comets.speed || effect_draft.strength != default_comets.strength ||
        effect_draft.density != default_comets.density)
        return failure(74);
    const auto scrollbar = ui_theme_editor_scrollbar(editor);
    if (!scrollbar.visible || !ui_rect_has_area(scrollbar.thumb))
        return failure(75);
    constexpr UiThemeEditorElement effect_ranges[]{
        UiThemeEditorElement::speed, UiThemeEditorElement::strength, UiThemeEditorElement::density};
    for (const auto element : effect_ranges) {
        const auto* range = ui_theme_editor_node(editor, element);
        if (!range || !ui_rect_has_area(ui_rect_intersection(range->rectangle, range->clip_rectangle)) ||
            ui_rect_height(ui_rect_intersection(range->rectangle, range->clip_rectangle)) + .01f <
                ui_rect_height(range->rectangle) ||
            !ui_widget_manager_input_enabled(editor.widgets, editor.elements[static_cast<u32>(element)]))
            return failure(76);
    }
    ui_widget_manager_hover(&editor.widgets,
                            editor.elements[static_cast<u32>(UiThemeEditorElement::apply)]);
    ++editor.revision;
    auto effect_preferences = client_preferences_defaults();
    effect_preferences.theme = ClientTheme::custom_light_gradient;
    effect_preferences.gradient = effect_draft;
    const Theme effect_light = client_theme_projection(effect_preferences);
    if (!restore_colors.project(effect_light))
        return failure(77);
    if (!renderer_set_background_phase(&r, 80) || !renderer_rebuild_content_text(&r, effect_light) ||
        !probe_present(&r, effect_light) ||
        (c.capture && !c.capture(L"background-theme-editor-effects-light.bmp", effect_light)))
        return failure(77);
    renderer_bind_theme_editor_scene(&r, nullptr);
    if (!renderer_settings_page_visible(r) || !authority.active)
        return failure(34);
    // Dummy authority has no UI arena: restore real fixture before any draw.
    r.settings_scene = old_settings;
    ui_theme_editor_close(&editor);
    if (renderer_theme_editor_visible(r))
        return failure(35);
    log_u64(LogLevel::info, "probe", "theme_editor_scene_bytes", sizeof(editor));
    log_u64(LogLevel::info, "probe", "theme_editor_widget_committed_bytes", editor.storage.arena.committed);
    log_write(
        LogLevel::info, "probe",
        "theme_editor_native_SV_hue_stops_live_main_settings_authority_retained_prefix_dark_light_passed");
    return true;
}
} // namespace

bool background_theme_ui_probe(BackgroundThemeProbeContext c) noexcept {
    auto& r = c.renderer;
    if (!c.rebuild || !r.content_text || r.pipeline.background_buffer || !c.rebuild() ||
        !probe_present(&r, kDefaultTheme) || r.pipeline.background_buffer)
        return failure(1);
    const u32 width = r.width, height = r.height;
    struct Restore {
        BackgroundThemeProbeContext& c;
        u32 width, height;
        ~Restore() noexcept {
            renderer_resize(&c.renderer, width, height);
            renderer_set_background_phase(&c.renderer, 0);
            c.rebuild();
            probe_present(&c.renderer, kDefaultTheme);
        }
    } restore{c, width, height};
    if (!renderer_set_background_phase(&r, 0) || !presentation_damage_empty(r.presentation_damage) ||
        renderer_set_background_phase(nullptr, 0) || renderer_set_background_phase(&r, -1))
        return failure(2);
    Theme theme = kDefaultTheme;
    theme.background = {
        {0xcc0000, 0x00cc00, 0x0000cc, 0, 0}, 0, 3, 100, BackgroundEffect::none, 50, 25, 35};
    if (!shader_pixels(r, theme, 0))
        return failure(3);
    D3D11_BUFFER_DESC buffer{};
    r.pipeline.background_buffer->GetDesc(&buffer);
    if (buffer.ByteWidth != sizeof(D3D11BackgroundConstants) || buffer.Usage != D3D11_USAGE_DEFAULT ||
        buffer.BindFlags != D3D11_BIND_CONSTANT_BUFFER)
        return failure(4);
    theme.background.angle_degrees = 90;
    if (!shader_pixels(r, theme, 0))
        return failure(5);
    theme.background = {
        {0xcc0000, 0xcccc00, 0x00cc00, 0x00cccc, 0x0000cc}, 0, 5, 100, BackgroundEffect::none, 50, 25, 35};
    if (!shader_pixels(r, theme, 0))
        return failure(6);
    theme.background = {{0xffffff, 0xffffff, 0, 0, 0}, 0, 2, 50, BackgroundEffect::none, 50, 25, 35};
    if (!shader_pixels(r, theme, 0))
        return failure(7);
    theme.background.intensity = 0;
    if (!shader_pixels(r, theme, 0))
        return failure(8);
    if (!intensity_pixels(r) || !overlay_surface_pixels(r) || !solid_pixels(r) ||
        !default_effect_pixels(r) || !dither_pixels(r) ||
        !comet_density_pixels(r) || !star_independence_pixels(r) || !effect_canvas_pixels(r) ||
        !plugin_effect_pixels(r))
        return false;
    theme.background = background_theme_defaults();
    theme.background.intensity = 100;
    theme.background.strength = theme.background.density = 100;
    constexpr BackgroundEffect effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                         BackgroundEffect::stars_and_comets};
    for (u32 mode = 0; mode < 2; ++mode) {
        Theme effect_theme = mode ? kLightTheme : kDefaultTheme;
        effect_theme.background = theme.background;
        effect_theme.background.effect = BackgroundEffect::none;
        u64 still = 0;
        if (!shader_pixels(r, effect_theme, 0, &still, kDefaultTheme, true))
            return failure(57);
        for (const auto effect : effects) {
            effect_theme.background.effect = effect;
            u64 first = 0, second = 0;
            if (!shader_pixels(r, effect_theme, 0, &first, kDefaultTheme, true) ||
                !shader_pixels(r, effect_theme, 7, &second, kDefaultTheme, true) || first == second)
                return failure(58 + static_cast<u32>(effect));
            for (u32 disabled = 0; disabled < 2; ++disabled) {
                Theme zero = effect_theme;
                if (disabled)
                    zero.background.density = 0;
                else
                    zero.background.strength = 0;
                u64 inactive = 0;
                if (!shader_pixels(r, zero, 7, &inactive, kDefaultTheme, true) || inactive != still)
                    return failure(64);
            }
        }
    }
    theme.background.effect = BackgroundEffect::comets;
    if (!retained_frames(r, theme))
        return failure(13);
    const u32 updates = r.pipeline.background_updates;
    if (!renderer_set_background_phase(&r, r.background_phase_seconds) ||
        !d3d11_renderer_draw_stream(&r, theme, false, false, nullptr) ||
        r.pipeline.background_updates != updates)
        return failure(14);
    for (u32 step = 0; step < 4; ++step) {
        const u32 w = (step & 1) ? width - 1 : width + 1;
        const u32 h = (step & 1) ? height - 1 : height + 1;
        Theme static_theme = theme;
        static_theme.background.effect = BackgroundEffect::none;
        if (!renderer_resize(&r, w, h) || !c.rebuild() ||
            !renderer_rebuild_content_text(&r, static_theme) || !probe_present(&r, static_theme) ||
            !shader_pixels(r, static_theme, 0, nullptr, static_theme))
            return failure(15);
    }
    if (!renderer_resize(&r, width, height) || !c.rebuild())
        return failure(16);
    FixtureMessageColors capture_colors{r};
    auto capture_preferences = client_preferences_defaults();
    capture_preferences.theme = ClientTheme::custom_gradient;
    capture_preferences.gradient = background_theme_defaults();
    theme = client_theme_projection(capture_preferences);
    if (!capture_colors.project(theme))
        return failure(36);
    if (!full_scene_visible(r, theme))
        return failure(19);
    if (!capture_colors.project(kDefaultTheme))
        return failure(36);
    if (!renderer_rebuild_content_text(&r, kDefaultTheme) || !probe_present(&r, kDefaultTheme) ||
        !chrome_native_pixels(r, kDefaultTheme) || !capture_colors.project(theme) ||
        !renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        !chrome_native_pixels(r, theme))
        return false;
    if (!renderer_rebuild_content_text(&r, theme) || !probe_present(&r, theme) ||
        (c.capture && !c.capture(L"background-theme-gradient.bmp", theme)))
        return failure(17);
    theme.background.effect = BackgroundEffect::stars_and_comets;
    if (!renderer_set_background_phase(&r, 6) || !renderer_rebuild_content_text(&r, theme) ||
        !probe_present(&r, theme) || (c.capture && !c.capture(L"background-theme-stars-comets.bmp", theme)))
        return failure(18);
    auto stars_preferences = client_preferences_defaults();
    stars_preferences.theme = ClientTheme::custom_gradient;
    stars_preferences.gradient = background_theme_defaults();
    stars_preferences.gradient.effect = BackgroundEffect::stars;
    const auto stars_profile = background_effect_settings_defaults(BackgroundEffect::stars);
    stars_preferences.gradient.speed = stars_profile.speed;
    stars_preferences.gradient.strength = stars_profile.strength;
    stars_preferences.gradient.density = stars_profile.density;
    const Theme stars_theme = client_theme_projection(stars_preferences);
    const wchar_t* stars_names[]{L"background-theme-stars-twinkle-a.bmp",
                                 L"background-theme-stars-twinkle-b.bmp"};
    for (u32 frame = 0; frame < 2; ++frame)
        if (!capture_colors.project(stars_theme) || !renderer_set_background_phase(&r, frame ? 7.f : 0.f) ||
            !renderer_rebuild_content_text(&r, stars_theme) || !probe_present(&r, stars_theme) ||
            (c.capture && !c.capture(stars_names[frame], stars_theme)))
            return failure(106 + frame);
    constexpr BackgroundEffect flat_effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                              BackgroundEffect::stars_and_comets};
    const wchar_t* flat_names[]{L"background-theme-amoled-stars.bmp", L"background-theme-dark-comets.bmp",
                                L"background-theme-dark-stars-comets-high-strength.bmp"};
    for (u32 i = 0; i < 3; ++i) {
        auto preferences = client_preferences_defaults();
        preferences.theme = i == 0 ? ClientTheme::amoled : ClientTheme::dark;
        preferences.gradient.effect = flat_effects[i];
        const auto settings = background_effect_settings_defaults(flat_effects[i]);
        preferences.gradient.speed = settings.speed;
        preferences.gradient.strength = i == 2 ? 65 : settings.strength;
        preferences.gradient.density = settings.density;
        const Theme flat = client_theme_projection(preferences);
        if (ui_background_active(flat) || !background_effects_valid(flat.background) ||
            !capture_colors.project(flat) || !renderer_set_background_phase(&r, 7) ||
            !renderer_rebuild_content_text(&r, flat) || !probe_present(&r, flat) ||
            (i == 0 && !retained_frames(r, flat)) || (c.capture && !c.capture(flat_names[i], flat)))
            return failure(112 + i);
    }
    constexpr u8 comparison_intensities[]{20, 100};
    const wchar_t* comparison_names[]{L"background-theme-magenta-cyan-20.bmp",
                                      L"background-theme-magenta-cyan-100.bmp"};
    for (u32 i = 0; i < 2; ++i) {
        auto preferences = client_preferences_defaults();
        preferences.theme = ClientTheme::custom_gradient;
        preferences.gradient = {{0xff00ff, 0x00ffff, 0, 0, 0}, 180, 2,  comparison_intensities[i],
                                BackgroundEffect::none,        50,  25, 35};
        const Theme comparison = client_theme_projection(preferences);
        if (!capture_colors.project(comparison) || !renderer_set_background_phase(&r, 0) ||
            !renderer_rebuild_content_text(&r, comparison) || !probe_present(&r, comparison) ||
            !chrome_native_pixels(r, comparison) ||
            (c.capture && !c.capture(comparison_names[i], comparison)))
            return failure(100 + i);
    }
    if (!capture_colors.project(kDefaultTheme))
        return failure(36);
    // The Settings page and the themed Friends/DM/group views are independent
    // fixtures: they run (and capture) even when an editor check fails.
    const bool editor_passed = theme_editor_pixels(c, theme);
    const bool settings_passed = settings_theme_ui_probe(c);
    const bool views_passed = theme_views_ui_probe(c);
    if (!editor_passed || !settings_passed || !views_passed)
        return false;
    log_u64(LogLevel::info, "probe", "background_theme_lazy_gpu_constant_bytes", buffer.ByteWidth);
    log_u64(LogLevel::info, "probe", "background_theme_pipeline_owner_bytes",
            sizeof(D3D11DrawPipelineOwner));
    log_write(LogLevel::info, "probe",
              "background_theme_3_5_stops_direction_intensity_effect_pixels_retention_resize_passed");
    return true;
}
} // namespace maicord
