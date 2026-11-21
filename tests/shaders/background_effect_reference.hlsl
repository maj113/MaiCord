// The reference package effect the background probe checks the effect pass against: the left half of the
// window is the accent role at knob 0's coverage, the right half untouched.
#include <maicord/sdk/background.hlsli>

float4 background(float2 pixel) {
    const float2 uv = mc_uv(pixel);
    const float alpha = uv.x < 0.5 ? saturate(mc_knobs[0].x) : 0.0;
    return float4(mc_role(MC_ROLE_ACCENT), alpha);
}
