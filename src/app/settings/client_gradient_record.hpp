#pragma once

#include "maicord/app/settings/client_preferences.hpp"

namespace maicord {

// preferences.bin and themes.bin share the layout of bytes 20..56: the gradient (five colors, angle,
// color count, intensity, effect, speed, strength, density) followed by the per-effect tuning profiles. Both lie
// there as they lie in memory: little-endian fields without padding.
static_assert(sizeof(BackgroundTheme) == 28 && __builtin_offsetof(BackgroundTheme, angle_degrees) == 20 &&
              __builtin_offsetof(BackgroundTheme, color_count) == 22 &&
              __builtin_offsetof(BackgroundTheme, intensity) == 23 && __builtin_offsetof(BackgroundTheme, effect) == 24 &&
              __builtin_offsetof(BackgroundTheme, speed) == 25 && __builtin_offsetof(BackgroundTheme, strength) == 26 &&
              __builtin_offsetof(BackgroundTheme, density) == 27 && sizeof(BackgroundEffectSettings) == 3 &&
              __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
inline constexpr u32 kGradientRecordProfileBytes = kBackgroundEffectProfileCount * sizeof(BackgroundEffectSettings);

inline void gradient_record_store(u8* record, const BackgroundTheme& gradient,
                                  const BackgroundEffectSettings* profiles) noexcept {
    copy_bytes(record + 20, &gradient, sizeof(gradient));
    copy_bytes(record + 48, profiles, kGradientRecordProfileBytes);
}

// An effect id outside the catalog (a removed effect) restores "none" with its default tuning.
inline void gradient_record_load(const u8* record, BackgroundTheme* gradient,
                                 BackgroundEffectSettings* profiles) noexcept {
    copy_bytes(gradient, record + 20, sizeof(*gradient));
    copy_bytes(profiles, record + 48, kGradientRecordProfileBytes);
    if (!background_effect_known(record[44])) {
        constexpr BackgroundEffectSettings tuning = background_effect_settings_defaults(BackgroundEffect::none);
        gradient->effect = BackgroundEffect::none;
        gradient->speed = tuning.speed;
        gradient->strength = tuning.strength;
        gradient->density = tuning.density;
    }
}

} // namespace maicord
