// One-slice array views: decoder arrays are read in place.
Texture2DArray<float> Luma : register(t0);
Texture2DArray<float2> Chroma : register(t1);
#define PLANE_TEXEL(position) int4(position, 0, 0)
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
#if defined(__spirv__)
    // std140 aligns a uint2 to 8 bytes, but the host packs it right after Step (offset 52): the reference layout
    // tools/check_cbuffer_layout.py compares the GLSL twin's block with (no compiler reads this branch).
    uint VisibleX, VisibleY;
#else
    uint2 Visible;
#endif
    // Clockwise quarter turns. Size stays the unrotated extent; odd turns
    // write it transposed at Destination.
    uint Rotation;
    // Threads that share one written pixel's block (large steps), else 1;
    // the host dispatches by it.
    uint Lanes;
};

// The integer conversion clamp(((Y - YOffset) * YScale + terms + 128) >> 8, 0, 255) in float32: every term is an
// integer below 2^18 and the shift a division by 256, so each step is exact. A chroma texel's share is
// pre-divided by 256 and shared by its luma samples.
float3 chroma_terms(uint2 texel) {
    const float2 uv = round(Chroma.Load(PLANE_TEXEL(texel)) * 255.0) - 128.0;
    const float3 terms = float3(RedCr * uv.y, -GreenCb * uv.x - GreenCr * uv.y, BlueCb * uv.x);
    return (terms + float(128 - YOffset * YScale)) * (1.0 / 256.0);
}

float3 convert(uint2 position, float3 terms) {
    const float luma = Luma.Load(PLANE_TEXEL(position));
    return clamp(floor(round(luma * 255.0) * (YScale * (1.0 / 256.0)) + terms), 0.0, 255.0);
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

// A complete S x S block for a literal S of 2 to 4, unrolled so that every load issues before the first
// conversion waits; it spans at most 2 x 2 chroma texels, and one when S is 2 (an even step starts on one).
float3 block_unrolled(uint2 first, uint S) {
    const uint2 phase = (S & 1) ? first & 1 : 0;
    const uint2 texel = (Source + first) / 2;
    const float3 t00 = chroma_terms(texel);
    float3 t10 = t00, t01 = t00, t11 = t00;
    if (S > 2) {
        t10 = chroma_terms(texel + uint2(1, 0));
        t01 = chroma_terms(texel + uint2(0, 1));
        t11 = chroma_terms(texel + 1);
    }
    float3 sum = 0;
    [unroll] for (uint y = 0; y < S; ++y) {
        const bool lower = phase.y + y >= 2;
        [unroll] for (uint x = 0; x < S; ++x) {
            const bool right = phase.x + x >= 2;
            sum += convert(Source + first + uint2(x, y), lower ? (right ? t11 : t01) : (right ? t10 : t00));
        }
    }
    return sum;
}

// The block's chroma texels on every `stride`-th quad row from y0 (a source row of even parity), each with its
// luma quad; pixels outside [first, last) weigh 0. A quad past an odd edge stays inside the plane: Source is
// even and the coded extent too.
float3 block_quads(uint2 first, uint2 last, uint y0, uint stride) {
    float3 sum = 0;
    [loop] for (uint y = y0; y < last.y; y += stride) {
        const float top = y >= first.y, bottom = y + 1 < last.y;
        [loop] for (uint x = first.x & ~1u; x < last.x; x += 2) {
            const float left = x >= first.x, right = x + 1 < last.x;
            const uint2 p = Source + uint2(x, y);
            const float3 terms = chroma_terms(p / 2);
            sum += convert(p, terms) * (top * left) + convert(p + uint2(1, 0), terms) * (top * right) +
                   convert(p + uint2(0, 1), terms) * (bottom * left) + convert(p + 1, terms) * (bottom * right);
        }
    }
    return sum;
}

void store_average(uint2 written, float3 sum, uint2 first, uint2 last) {
    const uint2 count = last - first;
    Target[Destination + written] = float4(sum / (255.0 * count.x * count.y), 1.0);
}

groupshared float3 Partial[64];

// Poster thumbnails and small tiles average Step x Step blocks, clipped at the visible edge; each RGB value is
// clamped before the sum. All sums are integers that fit in float32, so their order is free.
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
    const uint2 written_extent = (Rotation & 1) ? Size.yx : Size;
    if (Lanes > 1) {
        // Large steps: Lanes threads split one written pixel's block by quad rows, 64 / Lanes pixels a group.
        const uint lane = index % Lanes;
        const uint2 written = uint2(group.x * (64 / Lanes) + index / Lanes, group.y);
        const bool inside = all(written < written_extent);
        uint2 first = 0, last = 1;
        float3 sum = 0;
        if (inside) {
            first = source_pixel(written) * Step;
            last = min(first + Step, Visible);
            sum = block_quads(first, last, (first.y & ~1u) + 2 * lane, 2 * Lanes);
        }
        Partial[index] = sum;
        GroupMemoryBarrierWithGroupSync();
        if (inside && lane == 0) {
            [loop] for (uint i = 1; i < Lanes; ++i)
                sum += Partial[index + i];
            store_average(written, sum, first, last);
        }
        return;
    }
    if (any(id.xy >= written_extent)) return;
    const uint2 pixel = source_pixel(id.xy);
    if (Step <= 1) {
        const uint2 p = Source + pixel;
        Target[Destination + id.xy] = float4(convert(p, chroma_terms(p / 2)) / 255.0, 1.0);
        return;
    }
    const uint2 first = pixel * Step;
    const uint2 last = min(first + Step, Visible);
    float3 sum;
    [branch] if (Step <= 4 && all(last - first == Step)) {
        [branch] switch (Step) {
        case 2: sum = block_unrolled(first, 2); break;
        case 3: sum = block_unrolled(first, 3); break;
        default: sum = block_unrolled(first, 4); break;
        }
    } else {
        sum = block_quads(first, last, first.y & ~1u, 2);
    }
    store_average(id.xy, sum, first, last);
}
