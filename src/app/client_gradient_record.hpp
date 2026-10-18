#pragma once

#include "maicord/app/client_preferences.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {

// preferences.bin and themes.bin share the layout of bytes 20..56: the gradient (five colors, angle,
// color count, intensity, effect, speed, strength, density) followed by the per-effect tuning profiles.
inline void gradient_record_store(u8* record, const BackgroundTheme& gradient,
                                  const BackgroundEffectSettings* profiles) noexcept {
    for (u32 i = 0; i < 5; ++i)
        byte_store_le32(record + 20 + i * 4, gradient.colors[i]);
    byte_store_le16(record + 40, gradient.angle_degrees);
    record[42] = gradient.color_count;
    record[43] = gradient.intensity;
    record[44] = static_cast<u8>(gradient.effect);
    record[45] = gradient.speed;
    record[46] = gradient.strength;
    record[47] = gradient.density;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        record[48 + i * 3] = profiles[i].speed;
        record[49 + i * 3] = profiles[i].strength;
        record[50 + i * 3] = profiles[i].density;
    }
}

// An effect id outside the catalog (a removed effect) restores "none" with its default tuning.
inline void gradient_record_load(const u8* record, BackgroundTheme* gradient,
                                 BackgroundEffectSettings* profiles) noexcept {
    for (u32 i = 0; i < 5; ++i)
        gradient->colors[i] = byte_load_le32(record + 20 + i * 4);
    gradient->angle_degrees = byte_load_le16(record + 40);
    gradient->color_count = record[42];
    gradient->intensity = record[43];
    const bool known = background_effect_known(record[44]);
    const BackgroundEffectSettings tuning =
        known ? BackgroundEffectSettings{record[45], record[46], record[47]}
              : background_effect_settings_defaults(BackgroundEffect::none);
    gradient->effect = known ? static_cast<BackgroundEffect>(record[44]) : BackgroundEffect::none;
    gradient->speed = tuning.speed;
    gradient->strength = tuning.strength;
    gradient->density = tuning.density;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
        profiles[i] = {record[48 + i * 3], record[49 + i * 3], record[50 + i * 3]};
}

} // namespace maicord
