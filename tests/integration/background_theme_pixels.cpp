#include "background_theme_internal.hpp"
#include "theme_views_probe.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/render/effects/background_fx.hpp"

namespace maicord {
namespace background_theme_detail {

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

} // namespace background_theme_detail

namespace background_theme_detail {

void restore_scene(D3D11Renderer& r, const Theme& theme) noexcept {
    d3d11_renderer_draw_stream(&r, theme, true, true, nullptr);
}

bool shader_pixels(D3D11Renderer& r, const Theme& theme, float seconds, u64* hash,
                   const Theme& restore_theme, bool canvas,
                   u8 (*sampled)[3], ID3D11Texture2D** snapshot) noexcept {
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
    RendererTextStorage empty{};
    StreamSwap restore{r, isolated, restore_theme};
    r.paint_stream = isolated;
    r.content_text = &empty;
    r.media_quad_count = r.media_batch_count = 0;
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

bool overlay_surface_pixels(D3D11Renderer& r, bool solid) noexcept {
    PaintStream isolated{};
    if (!paint_stream_initialize(&isolated))
        return failure(65);
    RendererTextStorage empty{};
    StreamSwap restore{r, isolated, kDefaultTheme};
    r.content_text = &empty;
    r.media_quad_count = r.media_batch_count = 0;
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect cards[]{{32, 32, width * .4f, 192},
                         {width * .56f, height * .38f, width * .92f, height * .68f}};
    const Color border{.9f, .6f, .1f, 1};
    for (u32 mode = 0; mode < 2; ++mode) {
        Theme theme = mode ? kLightTheme : kDefaultTheme;
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
            const u32 visible = first.visible_difference(baseline, mode != 2 ? 1 : -1);
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
        const auto* node = ui_scene_node(*r.ui_scene, surfaces[s]);
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

} // namespace background_theme_detail
} // namespace maicord
