#pragma once
// A CPU model of the background-effect composite (fx_composite_pixel in src/render/shaders/background_fx.hlsl and the
// blend states) for one texel over an opaque background: `source` is the target's premultiplied colour at the user's
// opacity, `layer` the paint layer's opacity. Without adaptation, normal is source + background * (1 - alpha), screen
// source + background * (1 - source), add source + background, the source at the layer's opacity. Adapted, normal
// flips the source's colour (premultiplied: by alpha - 2y, within 0..alpha) and screen and add multiply the background
// by the flipped light; `light` cross-fades.
#include "maicord/render/effects/background_effect.hpp"

namespace maicord {

constexpr void effect_light_composite(const float (&source)[4], const float (&background)[3], EffectBlend blend,
                                      float light, float layer, float (&out)[3]) noexcept {
    float color[4]{};
    for (u32 c = 0; c < 4; ++c)
        color[c] = effect_unit(source[c]);
    const float y = effect_luma(color);
    for (u32 c = 0; c < 3; ++c) {
        float add = color[c], weight = blend == EffectBlend::normal ? 1 - color[3]
                                       : blend == EffectBlend::screen ? 1 - color[c]
                                                                      : 1.f;
        if (light > 0 && blend == EffectBlend::normal) {
            const float flipped = color[c] + color[3] - 2 * y;
            add += (minimum(maximum(flipped, 0.f), color[3]) - add) * light;
        } else if (light > 0) {
            const float multiply = 1 - layer * (1 - effect_unit(1 + color[c] - 2 * y));
            add *= 1 - light;
            weight += (multiply - weight) * light;
        }
        out[c] = effect_unit(layer * add + background[c] * weight);
    }
}

} // namespace maicord
