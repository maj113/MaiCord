// Day and night: a sky that cross-fades between a day and a night gradient. Night comes from the local clock
// (dawn and dusk at the hours you set, with a warm band at the horizon), from the light or dark appearance, or
// from either; the host eases both, so a theme switch glides instead of cutting. Summer days run a deeper blue
// than winter ones (mc_ambient.year), stars twinkle at night and pale clouds drift by day.
//
// @effect   name="Day and night" authors="MaiCord" license="MPL-2.0" version="1.0.1"
// @knob follow  choice "Clock|Appearance|Clock or appearance" = 2 "Night follows"
// @knob sunrise float 4..10  = 6.5 "Sunrise (hour)"
// @knob sunset  float 15..22 = 19  "Sunset (hour)"
// @knob stars   float 0..1   = 0.6 "Stars"
// @knob drift   float 0..2   = 0.5 "Cloud drift"
// @uses blue_noise clock appearance
// @blend normal
#include <maicord/sdk/background.hlsli>

#define knob_follow  ((int)mc_knobs[0].x)
#define knob_sunrise (mc_knobs[0].y)
#define knob_sunset  (mc_knobs[0].z)
#define knob_stars   (mc_knobs[0].w)
#define knob_drift   (mc_knobs[1].x)

float4 background(float2 pixel) {
    const float2 uv = pixel * (1.0 / mc.resolution);
    const float depth = uv.y; // 0 at the top of the window, 1 at the horizon
    // How much of the night shows: the clock's, the appearance's, or the darker of the two.
    const float clock_night = 1.0 - mc_daylight(knob_sunrise, knob_sunset);
    const float night = knob_follow == 0 ? clock_night
                        : knob_follow == 1 ? mc_ambient.dark
                                           : max(clock_night, mc_ambient.dark);
    // Day: a summer sky is deeper than a winter one (mc_ambient.year peaks in early July).
    const float summer = 0.5 - 0.5 * cos(6.2831853 * (mc_ambient.year - 0.04));
    const float3 day_top = lerp(float3(0.56, 0.72, 0.90), float3(0.24, 0.50, 0.88), summer);
    const float3 day_sky = lerp(day_top, float3(0.87, 0.92, 0.97), depth * depth);
    // Night: deep blue over a horizon lit by the theme's accent.
    const float3 night_sky = lerp(float3(0.015, 0.02, 0.06), lerp(float3(0.05, 0.07, 0.16), mc_role(MC_ROLE_ACCENT), 0.18),
                                  depth);
    float3 colour = lerp(day_sky, night_sky, night);
    // Dawn and dusk warm the horizon for an hour either side when the clock leads.
    if (knob_follow != 1) {
        const float hour = mc_ambient.day * 24.0;
        const float twilight = max(1.0 - abs(hour - knob_sunrise), 1.0 - abs(hour - knob_sunset));
        colour += float3(0.55, 0.22, 0.08) * saturate(twilight) * depth * depth;
    }
    // Pale clouds drift by day and dim to grey at night.
    const float drift = mc.time * 0.02 * knob_drift;
    const float cloud = 0.5 + 0.5 * sin(uv.x * 4.0 + drift + 1.7 * sin(uv.y * 6.0 - drift * 0.7));
    colour = lerp(colour, lerp(float3(1.0, 1.0, 1.0), float3(0.18, 0.2, 0.28), night),
                  cloud * cloud * cloud * 0.18 * (1.0 - depth));
    // Stars: one in a few 24-DIP cells, twinkling, fading towards the horizon and out by day. Real branches: by day
    // (or with the knob at 0) no pixel looks for a star, and only the pixels of a star draw one.
    const float stars = knob_stars * night;
    [branch] if (stars > 0.0) {
        const float cell_size = 24.0 * mc.dip;
        const float2 cell = floor(pixel * (1.0 / cell_size));
        const float4 random = mc_hash4(cell);
        const float2 centre = (cell + 0.2 + 0.6 * random.xy) * cell_size;
        const float distance_dips = length(pixel - centre) * (1.0 / mc.dip);
        [branch] if (random.z >= 0.7 && distance_dips < 1.3) {
            const float twinkle = 0.55 + 0.45 * sin(mc.time * (1.0 + 2.0 * random.z) + random.w * 6.2831853);
            colour += saturate(1.3 - distance_dips) * twinkle * (1.0 - depth) * stars;
        }
    }
    // The theme's strength: a weaker theme lets more of its background through.
    colour = lerp(mc_role(MC_ROLE_BACKGROUND), colour, lerp(0.55, 0.9, mc.intensity));
    return float4(colour + mc_dither(pixel), 1.0);
}
