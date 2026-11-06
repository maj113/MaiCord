#pragma once
#include "maicord/render/effects/background_fx_data.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/theme/theme.hpp"

// Built-in Stars/Comets pass: CPU-side policy shared by the renderer and the tests. The effect
// draws over each background canvas after its base fill, with a fixed constant buffer and premultiplied
// output composited as dst = src.rgb + dst * (1 - src.a).
namespace maicord {

// Built-in Stars/Comets parameter slots.
enum : u32 {
    kBackgroundFxSpeed = 0,
    kBackgroundFxDensity = 1,
    kBackgroundFxStrength = 2,
    kBackgroundFxMask = 3, // 1 stars, 2 comets
    kBackgroundFxWindowWidth = 4,
    kBackgroundFxWindowHeight = 5,
};

constexpr u32 background_fx_builtin_mask(BackgroundEffect effect) noexcept {
    return effect == BackgroundEffect::stars            ? 1u
           : effect == BackgroundEffect::comets         ? 2u
           : effect == BackgroundEffect::stars_and_comets ? 3u
                                                        : 0u;
}

// sRGB to linear; cubic fit, within 0.2% of the exact curve over 0..1.
constexpr float background_fx_linear(float value) noexcept {
    const float c = !is_finite(value) ? 0.f : value < 0 ? 0.f : value > 1 ? 1.f : value;
    return c * (c * (c * .305306011f + .682171111f) + .012522878f);
}

// 0..1 from the theme's effect strength; lower in a light theme. A flat theme
// with no built-in effect carries no strength, so the None profile applies.
constexpr float background_fx_intensity(const Theme& theme) noexcept {
    const auto& background = theme.background;
    const u8 strength = background.effect != BackgroundEffect::none || background.strength
                            ? background.strength
                            : background_effect_settings_defaults(BackgroundEffect::none).strength;
    const float value = (strength > 100 ? 100 : strength) / 100.f;
    return theme.appearance == ThemeAppearance::light ? value * .75f : value;
}

// Theme roles in palette order (background, surface, text, muted, accent, positive, warning, critical).
constexpr void background_fx_palette(const Theme& theme, float (&palette)[kBackgroundFxPaletteRoles][4]) noexcept {
    const Color roles[kBackgroundFxPaletteRoles]{
        theme.base_low,           theme.surface,         theme.text_primary,         theme.tokens.text.muted,
        theme.accent,             theme.tokens.accent.success, theme.tokens.accent.warning,
        theme.tokens.accent.danger};
    for (u32 i = 0; i < kBackgroundFxPaletteRoles; ++i) {
        palette[i][0] = background_fx_linear(roles[i].red);
        palette[i][1] = background_fx_linear(roles[i].green);
        palette[i][2] = background_fx_linear(roles[i].blue);
        palette[i][3] = 1;
    }
}

// Fields shared by every effect for one canvas (DIP rectangle). Parameters
// are left to the caller.
constexpr bool background_fx_pack(BackgroundFxConstants* output, const Theme& theme, float time, float delta,
                                  UiRect canvas, float pixel_scale) noexcept {
    if (!output || !is_finite(pixel_scale) || !is_finite(time) ||
        !is_finite(delta) || !(pixel_scale > 0 && pixel_scale <= 16) || time < 0 ||
        !ui_coordinate_valid(canvas.left) || !ui_coordinate_valid(canvas.top) ||
        !ui_coordinate_valid(canvas.right) || !ui_coordinate_valid(canvas.bottom) ||
        !(canvas.right > canvas.left && canvas.bottom > canvas.top))
        return false;
    output->time = time;
    output->delta = delta >= 0 && delta <= 1 ? delta : 0;
    output->intensity = background_fx_intensity(theme);
    output->dark = theme.appearance == ThemeAppearance::light ? 0.f : 1.f;
    output->resolution[0] = (canvas.right - canvas.left) * pixel_scale;
    output->resolution[1] = (canvas.bottom - canvas.top) * pixel_scale;
    output->origin[0] = canvas.left * pixel_scale;
    output->origin[1] = canvas.top * pixel_scale;
    background_fx_palette(theme, output->palette);
    output->canvas[0] = canvas.left;
    output->canvas[1] = canvas.top;
    output->canvas[2] = canvas.right;
    output->canvas[3] = canvas.bottom;
    for (auto& value : output->grid)
        value = 0;
    return true;
}

// Stars/Comets: phase seconds scaled by speed in the shader; effect geometry
// spans the whole window in DIPs so overlapping canvases stay continuous.
constexpr bool background_fx_pack_builtin(BackgroundFxConstants* output, const Theme& theme, float phase,
                                          UiRect canvas, float pixel_scale, float window_width,
                                          float window_height) noexcept {
    const auto& background = theme.background;
    const u32 mask = background_fx_builtin_mask(background.effect);
    if (!mask || !background_effects_active(background) || !(window_width > 0 && window_height > 0) ||
        !background_fx_pack(output, theme, phase, 0, canvas, pixel_scale))
        return false;
    for (auto& value : output->parameters)
        value = 0;
    output->parameters[kBackgroundFxSpeed] = background.speed / 100.f;
    output->parameters[kBackgroundFxDensity] = background.density / 100.f;
    output->parameters[kBackgroundFxStrength] = background.strength / 100.f;
    output->parameters[kBackgroundFxMask] = static_cast<float>(mask);
    output->parameters[kBackgroundFxWindowWidth] = window_width;
    output->parameters[kBackgroundFxWindowHeight] = window_height;
    return true;
}

constexpr i32 background_fx_floor(float value) noexcept {
    const i32 truncated = static_cast<i32>(value);
    return value < static_cast<float>(truncated) ? truncated - 1 : truncated;
}

// Staggered comet paths across the window: density admits one (to a quarter),
// then two, three and four.
constexpr u32 background_fx_comet_lanes(float density) noexcept {
    return 1u + (density > .25f) + (density > .5f) + (density > .75f);
}

// Stars/Comets sprites over one canvas (DIPs): fills output->grid for fx_vertex
// and returns the instance count, one per comet lane then one per star cell
// the canvas touches. Only the part of the canvas near the window can show, so
// an oversized canvas stays bounded. Returns 0 when nothing can show.
constexpr u32 background_fx_builtin_grid(BackgroundFxConstants* output, UiRect canvas) noexcept {
    const u32 mask = static_cast<u32>(output->parameters[kBackgroundFxMask]);
    const float density = output->parameters[kBackgroundFxDensity];
    const u32 lanes = mask & 2u ? background_fx_comet_lanes(density) : 0u;
    u32 columns = 0, rows = 0;
    i32 first_column = 0, first_row = 0;
    if (mask & 1u) {
        const float width = output->parameters[kBackgroundFxWindowWidth];
        const float height = output->parameters[kBackgroundFxWindowHeight];
        const float left = maximum(canvas.left, -width), top = maximum(canvas.top, -height);
        const float right = minimum(canvas.right, width * 2), bottom = minimum(canvas.bottom, height * 2);
        if (right > left && bottom > top) {
            const float pitch = 180.f + (48.f - 180.f) * (density > 1 ? 1.f : density < 0 ? 0.f : density);
            first_column = background_fx_floor(left / pitch);
            first_row = background_fx_floor(top / pitch);
            columns = static_cast<u32>(background_fx_floor(right / pitch) - first_column + 1);
            rows = static_cast<u32>(background_fx_floor(bottom / pitch) - first_row + 1);
        }
    }
    output->grid[0] = static_cast<float>(first_column);
    output->grid[1] = static_cast<float>(first_row);
    output->grid[2] = static_cast<float>(columns);
    output->grid[3] = static_cast<float>(lanes);
    return lanes + columns * rows;
}

} // namespace maicord
