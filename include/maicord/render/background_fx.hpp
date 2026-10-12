#pragma once
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/theme.hpp"

// Background effect pass: CPU-side policy shared by the renderer and the
// plugin host. Effects draw over each background canvas after its base fill,
// with a fixed constant buffer (mc.fx@1 mc_fx_constants) and premultiplied
// output composited as dst = src.rgb + dst * (1 - src.a).
namespace maicord {

inline constexpr u32 kBackgroundFxParameters = 8;
inline constexpr u32 kBackgroundFxPaletteRoles = 8;

// mc_fx_constants (192 bytes, 16-byte rows), then one host-only row: the
// canvas in DIPs, read by the effect vertex shader (and the built-in pixel
// shader). Plugin shaders declare only the first 192 bytes.
struct BackgroundFxConstants {
    float time, delta, intensity, dark;
    float resolution[2], origin[2];
    float palette[kBackgroundFxPaletteRoles][4];
    float parameters[kBackgroundFxParameters];
    float canvas[4];
};
static_assert(sizeof(BackgroundFxConstants) == 208);
static_assert(__builtin_offsetof(BackgroundFxConstants, palette) == 32 &&
              __builtin_offsetof(BackgroundFxConstants, parameters) == 160 &&
              __builtin_offsetof(BackgroundFxConstants, canvas) == 192);

enum class BackgroundFxSource : u8 {
    none,
    builtin,
    plugin
};

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

// Bit test: /fp:fast may fold NaN comparisons.
constexpr bool background_fx_finite(float value) noexcept {
    return (__builtin_bit_cast(u32, value) & 0x7F80'0000u) != 0x7F80'0000u;
}
// sRGB to linear; cubic fit, within 0.2% of the exact curve over 0..1.
constexpr float background_fx_linear(float value) noexcept {
    const float c = !background_fx_finite(value) ? 0.f : value < 0 ? 0.f : value > 1 ? 1.f : value;
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

// Theme roles in mc.fx palette order (MC_FX_BACKGROUND..MC_FX_CRITICAL).
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
    if (!output || !background_fx_finite(pixel_scale) || !background_fx_finite(time) ||
        !background_fx_finite(delta) || !(pixel_scale > 0 && pixel_scale <= 16) || time < 0 ||
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

// GPU guard: a plugin effect averaging more than 2 ms of GPU time over a
// second, or taking more than 8 ms in three frames, is turned off.
inline constexpr float kBackgroundFxAverageLimitMs = 2;
inline constexpr float kBackgroundFxStallMs = 8;
inline constexpr u32 kBackgroundFxStallLimit = 3;
inline constexpr u64 kBackgroundFxWindowMs = 1000;
struct BackgroundFxGuard {
    u64 window_start_ms;
    float window_total_ms;
    u32 window_frames;
    u32 stalls;
};
// One measured frame (its CPU submission time and GPU milliseconds). True
// when the effect must be turned off.
constexpr bool background_fx_guard_sample(BackgroundFxGuard* guard, float gpu_ms, u64 frame_ms) noexcept {
    if (!guard || !background_fx_finite(gpu_ms) || gpu_ms < 0)
        return false;
    if (gpu_ms > kBackgroundFxStallMs && ++guard->stalls >= kBackgroundFxStallLimit)
        return true;
    if (!guard->window_frames || frame_ms < guard->window_start_ms)
        guard->window_start_ms = frame_ms, guard->window_total_ms = 0, guard->window_frames = 0;
    guard->window_total_ms += gpu_ms;
    ++guard->window_frames;
    if (frame_ms - guard->window_start_ms < kBackgroundFxWindowMs)
        return false;
    const bool slow = guard->window_total_ms > kBackgroundFxAverageLimitMs * guard->window_frames;
    guard->window_total_ms = 0;
    guard->window_frames = 0;
    return slow;
}

// Plugin effect choice: the picker lists the built-ins, then registered plugin
// effects ordered by name. A chosen plugin effect persists as owner+effect id,
// apart from the built-in preference, and applies whenever its plugin has it
// registered.
struct BackgroundFxChoice {
    u64 owner_id, effect_id;
};
inline constexpr u32 kBackgroundFxChoiceBytes = 24;
constexpr bool background_fx_choice_valid(BackgroundFxChoice choice) noexcept {
    return choice.owner_id && choice.effect_id;
}
constexpr void background_fx_choice_encode(BackgroundFxChoice choice, u8 (&output)[kBackgroundFxChoiceBytes]) noexcept {
    const u8 header[8]{'M', 'C', 'F', 'X', 1, 0, 0, 0};
    for (u32 i = 0; i < 8; ++i) {
        output[i] = header[i];
        output[8 + i] = static_cast<u8>(choice.owner_id >> (i * 8));
        output[16 + i] = static_cast<u8>(choice.effect_id >> (i * 8));
    }
}
constexpr bool background_fx_choice_decode(const u8* input, u32 bytes, BackgroundFxChoice* output) noexcept {
    if (!output)
        return false;
    *output = {};
    if (!input || bytes != kBackgroundFxChoiceBytes || input[0] != 'M' || input[1] != 'C' || input[2] != 'F' ||
        input[3] != 'X' || input[4] != 1 || input[5] || input[6] || input[7])
        return false;
    BackgroundFxChoice choice{};
    for (u32 i = 0; i < 8; ++i) {
        choice.owner_id |= static_cast<u64>(input[8 + i]) << (i * 8);
        choice.effect_id |= static_cast<u64>(input[16 + i]) << (i * 8);
    }
    if (!background_fx_choice_valid(choice))
        return false;
    *output = choice;
    return true;
}
// Picker order: ASCII case-insensitive name, then owner and effect id.
constexpr bool background_fx_choice_before(Utf8View a_name, BackgroundFxChoice a, Utf8View b_name,
                                           BackgroundFxChoice b) noexcept {
    const u32 shared = a_name.bytes < b_name.bytes ? a_name.bytes : b_name.bytes;
    for (u32 i = 0; i < shared; ++i) {
        u8 x = static_cast<u8>(a_name.data[i]), y = static_cast<u8>(b_name.data[i]);
        x = x >= 'A' && x <= 'Z' ? static_cast<u8>(x + 32) : x;
        y = y >= 'A' && y <= 'Z' ? static_cast<u8>(y + 32) : y;
        if (x != y)
            return x < y;
    }
    if (a_name.bytes != b_name.bytes)
        return a_name.bytes < b_name.bytes;
    return a.owner_id != b.owner_id ? a.owner_id < b.owner_id : a.effect_id < b.effect_id;
}

} // namespace maicord
