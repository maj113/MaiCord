// The Go Live encoder's colour conversion (Windows; VA-API's video processing does it on Linux): a captured BGRA
// picture, scaled to the encoded extent, into the two planes of the encoder's NV12 surface as BT.709 limited range.
// One draw per plane, each a triangle covering its render target: the R8 view of the luma plane at the full extent,
// the R8G8 view of the chroma plane at half of it. One pixel shader writes either (the constants say which; an R8
// target keeps the first component).
//
// Scaling averages Taps x Taps bilinear samples spread over the output texel's footprint in the source (a box filter of
// up to 2 * Taps source texels each way), so a reduced picture stays free of aliasing.

Texture2D<float4> Source : register(t0);
SamplerState Linear : register(s0);

cbuffer EncodeConstants : register(b0) {
    float2 Extent;  // the picture's bottom-right in source UV (the texture may be larger than the picture)
    float2 Step;    // one texel of the plane in source UV
    uint Taps;
    uint Chroma;    // the chroma plane, else the luma plane
};

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vs_main(uint id : SV_VertexID) {
    const float2 corner = float2((id << 1) & 2, id & 2);
    Vertex output;
    output.position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = corner * Extent;
    return output;
}

float3 footprint(float2 centre, float2 step, uint taps) {
    float3 sum = 0;
    [loop] for (uint y = 0; y < taps; ++y)
        [loop] for (uint x = 0; x < taps; ++x)
            sum += Source.SampleLevel(Linear, centre + ((float2(x, y) + 0.5) / taps - 0.5) * step, 0).rgb;
    return sum / (taps * taps);
}

// BT.709: Y' = 0.2126 R' + 0.7152 G' + 0.0722 B'; limited range puts Y' in 16-235 and the colour differences in 16-240.
float luma_of(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float2 ps_plane(Vertex input) : SV_Target {
    const float3 rgb = footprint(input.uv, Step, Taps);
    const float y = luma_of(rgb);
    const float2 chroma = 128.0 + 224.0 * float2((rgb.b - y) / 1.8556, (rgb.r - y) / 1.5748);
    return (Chroma ? chroma : 16.0 + 219.0 * y) / 255.0;
}
