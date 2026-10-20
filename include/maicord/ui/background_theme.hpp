#pragma once
#include "maicord/core/utf.hpp"
#include "maicord/core/color_hex.hpp"

namespace maicord {
enum class BackgroundEffect : u8 {
    none,
    stars,
    comets,
    stars_and_comets
};
inline constexpr u32 kBackgroundEffectCount = 4;
inline constexpr u32 kBackgroundEffectProfileCount = kBackgroundEffectCount - 1;
struct BackgroundEffectSettings {
    u8 speed, strength, density;
};
static_assert(sizeof(BackgroundEffectSettings) == 3);
constexpr BackgroundEffectSettings background_effect_settings_defaults(BackgroundEffect effect) noexcept {
    switch (effect) {
    case BackgroundEffect::stars:
        return {35, 22, 25};
    case BackgroundEffect::comets:
        return {24, 18, 16};
    case BackgroundEffect::stars_and_comets:
        return {28, 18, 22};
    default:
        return {50, 25, 35};
    }
}
// Stored ids outside the catalog (a removed effect) are not an error: callers
// restore BackgroundEffect::none with its default tuning.
constexpr bool background_effect_known(u32 stored) noexcept {
    return stored < kBackgroundEffectCount;
}
constexpr bool background_effect_settings_valid(BackgroundEffectSettings value) noexcept {
    return value.speed <= 100 && value.strength <= 100 && value.density <= 100;
}
constexpr u32 background_effect_profile_index(BackgroundEffect effect) noexcept {
    const u32 value = static_cast<u8>(effect);
    return value && value < kBackgroundEffectCount ? value - 1 : kBackgroundEffectProfileCount;
}
struct BackgroundTheme {
    u32 colors[5];
    u16 angle_degrees;
    u8 color_count, intensity;
    BackgroundEffect effect;
    u8 speed, strength, density;
};
static_assert(sizeof(BackgroundTheme) == 28);
constexpr BackgroundTheme background_theme_defaults() noexcept {
    return {{0x6935b8, 0x164d80, 0x16836f, 0, 0}, 120, 3, 40, BackgroundEffect::none, 50, 25, 35};
}
constexpr BackgroundEffectSettings background_effect_settings(const BackgroundTheme& value) noexcept {
    return {value.speed, value.strength, value.density};
}
__declspec(noinline) constexpr bool background_theme_valid(const BackgroundTheme& value) noexcept {
    if (value.color_count < 1 || value.color_count > 5 || value.angle_degrees > 360 ||
        value.intensity > 100 || !background_effect_settings_valid(background_effect_settings(value)) ||
        static_cast<u8>(value.effect) >= kBackgroundEffectCount)
        return false;
    for (u32 i = 0; i < 5; ++i)
        if (value.colors[i] > 0xffffff || (i >= value.color_count && value.colors[i]))
            return false;
    return true;
}
// Effects on a flat theme have no palette or direction. Keep that admission
// separate so selecting an effect cannot enable gradient surface materials.
constexpr bool background_effects_valid(const BackgroundTheme& value) noexcept {
    if (value.color_count || value.angle_degrees || value.intensity ||
        value.effect == BackgroundEffect::none || static_cast<u8>(value.effect) >= kBackgroundEffectCount ||
        !background_effect_settings_valid(background_effect_settings(value)))
        return false;
    for (const auto color : value.colors)
        if (color)
            return false;
    return true;
}
constexpr bool background_render_valid(const BackgroundTheme& value) noexcept {
    return background_theme_valid(value) || background_effects_valid(value);
}
constexpr bool background_effects_active(const BackgroundTheme& value) noexcept {
    return background_render_valid(value) && value.effect != BackgroundEffect::none && value.strength &&
           value.density;
}
constexpr bool background_theme_equal(const BackgroundTheme& a, const BackgroundTheme& b) noexcept {
    for (u32 i = 0; i < 5; ++i)
        if (a.colors[i] != b.colors[i])
            return false;
    return a.angle_degrees == b.angle_degrees && a.color_count == b.color_count &&
           a.intensity == b.intensity && a.effect == b.effect && a.speed == b.speed &&
           a.strength == b.strength && a.density == b.density;
}
// Accept one solid colour or up to five comma-separated gradient colours, each
// #RGB or #RRGGBB with an optional '#'.
// Rejected input preserves the caller's complete current descriptor.
inline bool background_theme_parse_colors(Utf8View text, BackgroundTheme* output) noexcept {
    if (!output || !text.data || text.bytes > 128)
        return false;
    BackgroundTheme next = *output;
    for (auto& color : next.colors)
        color = 0;
    u32 offset = 0, count = 0;
    const auto space = [](char c) noexcept { return c == ' ' || c == '\t'; };
    while (offset < text.bytes) {
        while (offset < text.bytes && space(text.data[offset]))
            ++offset;
        if (offset == text.bytes || count == 5)
            return false;
        const u32 start = offset;
        while (offset < text.bytes && !space(text.data[offset]) && text.data[offset] != ',')
            ++offset;
        u32 rgba = 0;
        if (!color_hex_parse({text.data + start, offset - start}, false, &rgba))
            return false;
        next.colors[count++] = rgba >> 8;
        while (offset < text.bytes && space(text.data[offset]))
            ++offset;
        if (offset == text.bytes)
            break;
        if (text.data[offset++] != ',' || offset == text.bytes)
            return false;
    }
    next.color_count = static_cast<u8>(count);
    if (!background_theme_valid(next))
        return false;
    *output = next;
    return true;
}
} // namespace maicord
