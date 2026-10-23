#pragma once
#include "maicord/ui/text_presentation.hpp"
namespace maicord {
inline constexpr u32 kTextEffectCapacity = 128;
static_assert(kTextEffectCapacity <= 255);
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
// A zero uploaded prefix invalidates all materials. Static effects do not use
// the clock; shrinking a prefix leaves the remaining uploaded entries valid.
inline bool text_effect_upload_required(const TextEffectConstants& uploaded, u32 uploaded_count,
                                        u32 count, bool animated, float seconds, float scale) noexcept {
    return count > uploaded_count || scale != uploaded.scale || (animated && seconds != uploaded.seconds);
}
} // namespace maicord
