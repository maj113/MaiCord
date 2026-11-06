// twin: src/render/shaders/video_frame.hlsl 46f1661ac3d6b6a7
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
};

ivec3 chroma_terms(uvec2 position) {
    ivec2 uv = ivec2(texelFetch(chroma_plane, ivec2(position / 2u), 0).xy * 255.0 + 0.5) - 128;
    return ivec3(RedCr * uv.y, -GreenCb * uv.x - GreenCr * uv.y, BlueCb * uv.x);
}

vec3 convert_luma(uvec2 position, ivec3 terms) {
    int luma = int(texelFetch(luma_plane, ivec2(position), 0).x * 255.0 + 0.5);
    int y = (luma - YOffset) * YScale;
    return vec3(clamp((y + terms + 128) >> 8, 0, 255));
}

vec3 convert(uvec2 position) {
    return convert_luma(position, chroma_terms(position));
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

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(id, (Rotation & 1u) != 0u ? Size.yx : Size)))
        return;
    uvec2 pixel = source_pixel(id);
    ivec2 out_pixel = ivec2(Destination + id);
    if (Step <= 1u) {
        imageStore(target, out_pixel, vec4(convert(Source + pixel) * (1.0 / 255.0), 1.0));
        return;
    }
    // Poster thumbnails: average the block, clipped at the visible edge.
    uvec2 first = pixel * Step;
    uvec2 last = min(first + Step, uvec2(VisibleX, VisibleY));
    vec3 sum = vec3(0.0);
    // Only complete, even-aligned blocks share one chroma load per 2x2 group. Clamp each RGB value before the sum.
    // All integer sums fit in float32.
    if ((Step & 1u) == 0u && all(equal(Source & 1u, uvec2(0u))) && all(equal(last - first, uvec2(Step)))) {
        for (uint y = first.y; y < last.y; y += 2u) {
            for (uint x = first.x; x < last.x; x += 2u) {
                uvec2 p = Source + uvec2(x, y);
                ivec3 terms = chroma_terms(p);
                sum += convert_luma(p, terms);
                sum += convert_luma(p + uvec2(1u, 0u), terms);
                sum += convert_luma(p + uvec2(0u, 1u), terms);
                sum += convert_luma(p + uvec2(1u, 1u), terms);
            }
        }
    } else {
        for (uint y = first.y; y < last.y; ++y)
            for (uint x = first.x; x < last.x; ++x)
                sum += convert(Source + uvec2(x, y));
    }
    uvec2 count = last - first;
    imageStore(target, out_pixel, vec4(sum / (255.0 * float(count.x) * float(count.y)), 1.0));
}
