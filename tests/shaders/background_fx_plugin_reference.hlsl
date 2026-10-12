// A plugin background effect as a third party writes it against mc.fx@1:
// the left half of each canvas is the accent role at parameters[0].x
// coverage, the right half untouched. The UI probe reads the result.
cbuffer McFx : register(b0) {
    float time, delta, intensity, dark;
    float2 resolution, origin;
    float4 palette[8];
    float4 parameters[2];
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const float alpha = uv.x < 0.5f ? saturate(parameters[0].x) : 0.0f;
    return float4(palette[4].rgb * alpha, alpha);
}
