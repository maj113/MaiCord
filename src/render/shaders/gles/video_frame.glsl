// twin: src/render/shaders/video_frame.hlsl 1371ec9f4e7c57a9
// cs_main (STAGE_CS): converts a decoder's luma and chroma planes into RGB at the target (rotation, poster box reduction).
layout(local_size_x = 8, local_size_y = 8) in;

// A decoder publishes plain 2D plane textures (dma-buf images, CPU planes), read without a sampler.
layout(binding = 0) uniform highp sampler2D luma_plane; //! t0
layout(binding = 1) uniform highp sampler2D chroma_plane; //! t1
layout(rgba8, binding = 0) uniform writeonly highp image2D target; //! u0

layout(std140, binding = 0) uniform VideoConstants {
    uvec2 Source;
    uvec2 Size;
    uvec2 Destination;
    int YScale;
    int YOffset;
    int RedCr;
    int GreenCb;
    int GreenCr;
    int BlueCb;
    // Integer box reduction; Size is the reduced extent and Visible the cropped source extent that bounds every
    // Step x Step block.
    uint Step;
    uint VisibleX, VisibleY;
    // Clockwise quarter turns. Size stays the unrotated extent; odd turns write it transposed at Destination.
    uint Rotation;
    // Threads that share one written pixel's block (large steps), else 1; the host dispatches by it.
    uint Lanes;
};

// The integer conversion clamp(((Y - YOffset) * YScale + terms + 128) >> 8, 0, 255) in float32: every term is an
// integer below 2^18 and the shift a division by 256, so each step is exact. A chroma texel's share is
// pre-divided by 256 and shared by its luma samples.
vec3 chroma_terms(uvec2 texel) {
    vec2 uv = roundEven(texelFetch(chroma_plane, ivec2(texel), 0).xy * 255.0) - 128.0;
    vec3 terms = vec3(float(RedCr) * uv.y, -float(GreenCb) * uv.x - float(GreenCr) * uv.y, float(BlueCb) * uv.x);
    return (terms + float(128 - YOffset * YScale)) * (1.0 / 256.0);
}

vec3 convert(uvec2 position, vec3 terms) {
    float luma = texelFetch(luma_plane, ivec2(position), 0).x;
    return clamp(floor(roundEven(luma * 255.0) * (float(YScale) * (1.0 / 256.0)) + terms), 0.0, 255.0);
}

// Written pixel -> unrotated (reduced) source pixel.
uvec2 source_pixel(uvec2 written) {
    if (Rotation == 1u)
        return uvec2(written.y, Size.y - 1u - written.x);
    if (Rotation == 2u)
        return Size - 1u - written;
    if (Rotation == 3u)
        return uvec2(Size.x - 1u - written.y, written.x);
    return written;
}

// A complete S x S block for a literal S of 2 to 4, unrolled so that every load issues before the first conversion
// waits; it spans at most 2 x 2 chroma texels, and one when S is 2 (an even step starts on one).
vec3 block_unrolled(uvec2 first, uint S) {
    uvec2 phase = (S & 1u) != 0u ? first & 1u : uvec2(0u);
    uvec2 texel = (Source + first) / 2u;
    vec3 t00 = chroma_terms(texel);
    vec3 t10 = t00, t01 = t00, t11 = t00;
    if (S > 2u) {
        t10 = chroma_terms(texel + uvec2(1u, 0u));
        t01 = chroma_terms(texel + uvec2(0u, 1u));
        t11 = chroma_terms(texel + 1u);
    }
    vec3 sum = vec3(0.0);
    for (uint y = 0u; y < S; ++y) {
        bool lower = phase.y + y >= 2u;
        for (uint x = 0u; x < S; ++x) {
            bool right = phase.x + x >= 2u;
            sum += convert(Source + first + uvec2(x, y), lower ? (right ? t11 : t01) : (right ? t10 : t00));
        }
    }
    return sum;
}

// The block's chroma texels on every `stride`-th quad row from y0 (a source row of even parity), each with its luma
// quad; pixels outside [first, last) weigh 0. A quad past an odd edge stays inside the plane: Source is even and the
// coded extent too.
vec3 block_quads(uvec2 first, uvec2 last, uint y0, uint stride) {
    vec3 sum = vec3(0.0);
    for (uint y = y0; y < last.y; y += stride) {
        float top = float(y >= first.y), bottom = float(y + 1u < last.y);
        for (uint x = first.x & ~1u; x < last.x; x += 2u) {
            float left = float(x >= first.x), right = float(x + 1u < last.x);
            uvec2 p = Source + uvec2(x, y);
            vec3 terms = chroma_terms(p / 2u);
            sum += convert(p, terms) * (top * left) + convert(p + uvec2(1u, 0u), terms) * (top * right) +
                   convert(p + uvec2(0u, 1u), terms) * (bottom * left) + convert(p + 1u, terms) * (bottom * right);
        }
    }
    return sum;
}

void store_average(uvec2 written, vec3 sum, uvec2 first, uvec2 last) {
    uvec2 count = last - first;
    imageStore(target, ivec2(Destination + written), vec4(sum / (255.0 * float(count.x) * float(count.y)), 1.0));
}

shared vec3 Partial[64];

// Poster thumbnails and small tiles average Step x Step blocks, clipped at the visible edge; each RGB value is
// clamped before the sum. All sums are integers that fit in float32, so their order is free.
void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    uint index = gl_LocalInvocationIndex;
    uvec2 written_extent = (Rotation & 1u) != 0u ? Size.yx : Size;
    if (Lanes > 1u) {
        // Large steps: Lanes threads split one written pixel's block by quad rows, 64 / Lanes pixels a group.
        uint lane = index % Lanes;
        uvec2 written = uvec2(gl_WorkGroupID.x * (64u / Lanes) + index / Lanes, gl_WorkGroupID.y);
        bool inside = all(lessThan(written, written_extent));
        uvec2 first = uvec2(0u), last = uvec2(1u);
        vec3 sum = vec3(0.0);
        if (inside) {
            first = source_pixel(written) * Step;
            last = min(first + Step, uvec2(VisibleX, VisibleY));
            sum = block_quads(first, last, (first.y & ~1u) + 2u * lane, 2u * Lanes);
        }
        Partial[index] = sum;
        memoryBarrierShared();
        barrier();
        if (inside && lane == 0u) {
            for (uint i = 1u; i < Lanes; ++i)
                sum += Partial[index + i];
            store_average(written, sum, first, last);
        }
        return;
    }
    if (any(greaterThanEqual(id, written_extent)))
        return;
    uvec2 pixel = source_pixel(id);
    if (Step <= 1u) {
        uvec2 p = Source + pixel;
        imageStore(target, ivec2(Destination + id), vec4(convert(p, chroma_terms(p / 2u)) * (1.0 / 255.0), 1.0));
        return;
    }
    uvec2 first = pixel * Step;
    uvec2 last = min(first + Step, uvec2(VisibleX, VisibleY));
    vec3 sum;
    if (Step <= 4u && all(equal(last - first, uvec2(Step)))) {
        if (Step == 2u)
            sum = block_unrolled(first, 2u);
        else if (Step == 3u)
            sum = block_unrolled(first, 3u);
        else
            sum = block_unrolled(first, 4u);
    } else {
        sum = block_quads(first, last, first.y & ~1u, 2u);
    }
    store_average(id, sum, first, last);
}
