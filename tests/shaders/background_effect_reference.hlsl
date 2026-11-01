// The reference package effect the background probe checks the effect pass against: the left half of the
// window is the accent role at knob 0's coverage, the right half untouched. Knob 1 adds loop iterations that
// cost GPU time and change nothing (the budget tests use them).
#include <maicord/sdk/background.hlsli>

float4 background(float2 pixel) {
    const float2 uv = mc_uv(pixel);
    const float alpha = uv.x < 0.5 ? saturate(mc_knobs[0].x) : 0.0;
    float burn = 0.0;
    [loop] for (int i = 0; i < (int)mc_knobs[1].x; ++i)
        burn = burn * 1.00001 + sin(uv.x * 0.1 + burn + (float)i * 0.7) * 0.001;
    // burn stays far below 1000 for any iteration count the tests use, so this adds nothing; the compiler cannot
    // know it, and each iteration depends on the last, so none can be dropped.
    return float4(mc_role(MC_ROLE_ACCENT), alpha + step(1000.0, burn));
}
