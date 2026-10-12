// Frozen per-pixel NV12 converter (before the shared-chroma fast path). Test reference only.
Texture2D<float> Luma : register(t0);
Texture2D<float2> Chroma : register(t1);
RWTexture2D<unorm float4> Target : register(u0);

cbuffer VideoConstants : register(b0) {
    uint2 Source;
    uint2 Size;
    uint2 Destination;
    int YScale;
    int YOffset;
    int RedCr;
    int GreenCb;
    int GreenCr;
    int BlueCb;
    // Integer box reduction; Size is the reduced extent and Visible the
    // cropped source extent that bounds every Step x Step block.
    uint Step;
    uint2 Visible;
    uint Reserved;
};

float3 convert(uint2 position) {
    int luma = int(Luma.Load(int3(position, 0)) * 255.0 + 0.5);
    int2 uv = int2(Chroma.Load(int3(position / 2, 0)) * 255.0 + 0.5) - 128;
    int y = (luma - YOffset) * YScale;
    int3 rgb = int3(y + RedCr * uv.y,
                   y - GreenCb * uv.x - GreenCr * uv.y,
                   y + BlueCb * uv.x);
    return float3(clamp((rgb + 128) >> 8, 0, 255));
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    if (Step <= 1) {
        Target[Destination + id.xy] = float4(convert(Source + id.xy) / 255.0, 1.0);
        return;
    }
    // Poster thumbnails: average the block, clipped at the visible edge.
    const uint2 first = id.xy * Step;
    const uint2 last = min(first + Step, Visible);
    float3 sum = 0;
    [loop] for (uint y = first.y; y < last.y; ++y)
        [loop] for (uint x = first.x; x < last.x; ++x)
            sum += convert(Source + uint2(x, y));
    const uint2 count = last - first;
    Target[Destination + id.xy] = float4(sum / (255.0 * count.x * count.y), 1.0);
}
