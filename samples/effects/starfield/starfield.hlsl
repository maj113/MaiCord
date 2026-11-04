// Starfield: three parallax layers of twinkling stars drifting sideways, sized in DIPs so they look the same
// at every DPI and resolution. Stars are the theme's text colour; the window behind shows through.
//
// @effect   name="Starfield" authors="MaiCord" license="MPL-2.0"
// @knob density float 0.02..0.5 = 0.14 "Density"
// @knob speed   float 0..3      = 0.5  "Drift speed"
// @knob size    float 0.5..3    = 1.2  "Star size"
// @knob twinkle float 0..1      = 0.6  "Twinkle"
// @blend add
#include <maicord/sdk/background.hlsli>

#define knob_density (mc_knobs[0].x)
#define knob_speed   (mc_knobs[0].y)
#define knob_size    (mc_knobs[0].z)
#define knob_twinkle (mc_knobs[0].w)

float4 background(float2 pixel) {
    float3 colour = float3(0.0, 0.0, 0.0);
    [unroll] for (int layer = 0; layer < 3; ++layer) {
        const float depth = (float)(layer + 1);
        // Nearer layers are sparser-looking, bigger and faster.
        const float pitch_dips = 22.0 * depth;
        const float2 scrolled =
            pixel * (1.0 / (pitch_dips * mc.dip)) + float2(mc.time * knob_speed * depth * 0.12, depth * 17.0);
        const float2 cell = floor(scrolled);
        const float4 n = mc_hash4(cell);
        const float2 centre = cell + 0.2 + 0.6 * n.xy;
        // Squared distance in DIPs from the star, against its radius.
        const float2 offset = (scrolled - centre) * pitch_dips;
        const float distance_squared = dot(offset, offset);
        const float radius = knob_size * (0.55 + 0.25 * depth) * (0.6 + 0.8 * n.z);
        // Most cells hold no star, and beyond 6.5 radii its halo is under 0.0005 and its core is gone: a real
        // branch, so whole blocks of pixels skip the rest.
        [branch] if (n.w <= knob_density && distance_squared <= radius * radius * 42.0) {
            const float core = saturate(1.6 - sqrt(distance_squared) / radius);
            const float halo = exp(-distance_squared / (radius * radius * 6.0)) * 0.35;
            const float pulse = 1.0 - knob_twinkle * (0.5 + 0.5 * sin(mc.time * (1.0 + 3.0 * n.z) + n.x * 6.2831853));
            const float3 tint = lerp(mc_role(MC_ROLE_TEXT), mc_role(MC_ROLE_ACCENT), n.y * 0.35);
            colour += tint * (core + halo) * pulse * (0.5 + 0.17 * depth);
        }
    }
    const float coverage = saturate(max(colour.r, max(colour.g, colour.b)));
    // Straight colour, so the interface premultiplies back to the light we summed.
    return float4(colour * (1.0 / max(coverage, 1e-4)), coverage);
}
