// Showcase gradient: a slow accent-to-positive wash over MaiCord's background
// canvases. Compiled with fxc /T ps_5_0 and registered through mc.fx.
cbuffer McFx : register(b0) {
    float time, delta, intensity, dark;
    float2 resolution, origin;
    float4 palette[8];    // MC_FX_BACKGROUND .. MC_FX_CRITICAL, linear RGB
    float4 parameters[2]; // parameters[0].x: speed
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float phase = time * parameters[0].x;
    float wave = 0.5 + 0.5 * sin(uv.x * 2.5 + uv.y * 1.5 + phase);
    float3 color = lerp(palette[4].rgb, palette[5].rgb, wave);
    // Softer in light themes; strongest near the bottom edge.
    float alpha = intensity * lerp(0.08, 0.16, dark) * (0.55 + 0.45 * uv.y);
    return float4(color * alpha, alpha); // premultiplied
}
