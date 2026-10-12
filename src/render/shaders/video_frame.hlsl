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
    // Clockwise quarter turns. Size stays the unrotated extent; odd turns
    // write it transposed at Destination.
    uint Rotation;
};

int3 chroma_terms(uint2 position) {
    int2 uv = int2(Chroma.Load(int3(position / 2, 0)) * 255.0 + 0.5) - 128;
    return int3(RedCr * uv.y, -GreenCb * uv.x - GreenCr * uv.y, BlueCb * uv.x);
}

float3 convert_luma(uint2 position, int3 terms) {
    int luma = int(Luma.Load(int3(position, 0)) * 255.0 + 0.5);
    int y = (luma - YOffset) * YScale;
    return float3(clamp((y + terms + 128) >> 8, 0, 255));
}

float3 convert(uint2 position) {
    int luma = int(Luma.Load(int3(position, 0)) * 255.0 + 0.5);
    int2 uv = int2(Chroma.Load(int3(position / 2, 0)) * 255.0 + 0.5) - 128;
    int y = (luma - YOffset) * YScale;
    int3 rgb = int3(y + RedCr * uv.y,
                   y - GreenCb * uv.x - GreenCr * uv.y,
                   y + BlueCb * uv.x);
    return float3(clamp((rgb + 128) >> 8, 0, 255));
}

// Written pixel -> unrotated (reduced) source pixel.
uint2 source_pixel(uint2 written) {
    [branch] switch (Rotation) {
    case 1: return uint2(written.y, Size.y - 1 - written.x);
    case 2: return Size - 1 - written;
    case 3: return uint2(Size.x - 1 - written.y, written.x);
    default: return written;
    }
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ((Rotation & 1) ? Size.yx : Size))) return;
    const uint2 pixel = source_pixel(id.xy);
    if (Step <= 1) {
        Target[Destination + id.xy] = float4(convert(Source + pixel) / 255.0, 1.0);
        return;
    }
    // Poster thumbnails: average the block, clipped at the visible edge.
    const uint2 first = pixel * Step;
    const uint2 last = min(first + Step, Visible);
    float3 sum = 0;
    // Only complete, even-aligned blocks share one chroma load per 2x2 group.
    // Clamp each RGB value before the sum. All integer sums fit in float32.
    [branch] if ((Step & 1) == 0 && all((Source & 1) == 0) && all(last - first == Step)) {
        [loop] for (uint y = first.y; y < last.y; y += 2) {
            [loop] for (uint x = first.x; x < last.x; x += 2) {
                const uint2 p = Source + uint2(x, y);
                const int3 terms = chroma_terms(p);
                sum += convert_luma(p, terms);
                sum += convert_luma(p + uint2(1, 0), terms);
                sum += convert_luma(p + uint2(0, 1), terms);
                sum += convert_luma(p + uint2(1, 1), terms);
            }
        }
    } else {
        [loop] for (uint y = first.y; y < last.y; ++y)
            [loop] for (uint x = first.x; x < last.x; ++x)
                sum += convert(Source + uint2(x, y));
    }
    const uint2 count = last - first;
    Target[Destination + id.xy] = float4(sum / (255.0 * count.x * count.y), 1.0);
}
