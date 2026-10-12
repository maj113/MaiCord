#pragma once
#include "maicord/ui/text_presentation.hpp"
namespace maicord {
inline constexpr u32 kTextEffectCapacity = 128;
struct TextEffectMaterial {
    Color colors[8];
    float left, width, kind, count;
    float animate, reserved[3];
};
struct TextEffectConstants {
    float seconds, scale, reserved[2];
    TextEffectMaterial materials[kTextEffectCapacity];
};
static_assert(sizeof(TextEffectConstants) == 16 + 160 * kTextEffectCapacity);
} // namespace maicord
