// twin: src/render/shaders/vp8_atlas.hlsl 22dcd2790857eb8c
// vp8_to_atlas (STAGE_CS): reconstructed VP8 planes (fancy chroma upsampling, optional alpha) into the media atlas.
layout(local_size_x = 64, local_size_y = 1) in;

layout(std140, binding = 0) uniform Vp8AtlasConstants {
    uint image_width;
    uint image_height;
    uint luma_stride;
    uint chroma_stride;
    uint u_plane_offset;
    uint v_plane_offset;
    uint destination_x;
    uint destination_y;
    uint has_alpha;
    uint alpha_stride;
    uint blend_over;
};

layout(std430, binding = 0) readonly buffer Yuv { uint vp8_yuv[]; }; //! t0
layout(std430, binding = 1) readonly buffer Alpha { uint vp8_alpha[]; }; //! t1
// ES 3.1 images of this format are either read-only or write-only: the atlas is bound twice, and the over-blend reads
// through the second view.
layout(rgba8, binding = 0) uniform writeonly highp image2D media_atlas; //! u0
layout(rgba8, binding = 1) uniform readonly highp image2D media_atlas_read; //! u1

// Chroma samples x and x + step (0 or 1) of row y, U in the low and V in the high halfword, so the upsampling filters
// both planes at once (libwebp's LOAD_UV): no halfword of its sums exceeds 16 bits. The HLSL reads x + 1 always (a raw
// load past a buffer's end reads 0 there); here a read past the end of the buffer is undefined, so the edges read x.
uvec2 load_uv_pair(uint x, uint step_x, uint y) {
    uint offset = y * chroma_stride + x;
    uint u = u_plane_offset + offset;
    uint v = v_plane_offset + offset;
    return uvec2(vp8_yuv[u], vp8_yuv[u + step_x]) | (uvec2(vp8_yuv[v], vp8_yuv[v + step_x]) << 16u);
}

uvec3 clip_rgb(ivec3 value) {
    return uvec3(clamp(value >> 6, 0, 255));
}

// One thread per pixel, a row of 64 per group: the loads of a group are runs of consecutive words. The host dispatches
// ((width + 63) / 64, height, 1).
void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= image_width || y >= image_height)
        return;
    // Every load is issued before any result is needed, except that an over-blend first waits for the alpha to skip
    // transparent pixels.
    uint luma = vp8_yuv[y * luma_stride + x];
    uint alpha = 255u;
    if (has_alpha != 0u) {
        uint address = y * alpha_stride + x;
        alpha = (vp8_alpha[address >> 2u] >> ((address & 3u) * 8u)) & 255u;
    }
    // Over-blend onto a premultiplied destination: a transparent source leaves it unchanged (and needs no other read)
    // and an opaque one replaces it, so only alpha 1..254 reads the atlas.
    bool over = blend_over != 0u;
    if (over && alpha == 0u)
        return;
    // Fancy upsampling (libwebp's UpsampleRgbaLinePair): pixel (x, y) lies in the square of chroma samples a b / c d
    // between columns block.x - 1 and block.x and rows block.y - 1 and block.y, and takes
    // (9 * nearest + 3 * each side + farthest + 8) >> 4 of them. Samples past the picture's edges clamp to the edge,
    // which gives the edge rows' and columns' two-tap filter and the corners' copies.
    uvec2 block = (uvec2(x, y) + 1u) >> 1u;
    uint chroma_right = (image_width + 1u) / 2u - 1u;
    uint chroma_bottom = (image_height + 1u) / 2u - 1u;
    uint chroma_x = max(block.x, 1u) - 1u;
    bool inner_x = block.x != 0u && block.x <= chroma_right;
    uint step_x = inner_x ? 1u : 0u;
    uvec2 top = load_uv_pair(chroma_x, step_x, max(block.y, 1u) - 1u);
    uvec2 bottom = load_uv_pair(chroma_x, step_x, min(block.y, chroma_bottom));
    ivec2 destination = ivec2(destination_x + x, destination_y + y);
    vec4 under = vec4(0.0);
    if (over && alpha - 1u < 254u)
        under = imageLoad(media_atlas_read, destination);
    uint a = top.x;
    uint b = top.y;
    uint c = bottom.x;
    uint d = bottom.y;
    bool near_left = (x & 1u) != 0u;
    bool near_top = (y & 1u) != 0u;
    uint nearest = near_top ? (near_left ? a : b) : (near_left ? c : d);
    uint sides = near_left == near_top ? b + c : a + d;
    // (9n + 3s + 3t + f + 8) >> 4 == (((n + 3s + 3t + f + 8) >> 3) + n) >> 1 exactly; the shift drops bits of V into
    // U's halfword above bit 12, which the final mask removes.
    uint uv = ((((a + b + c + d + 0x00080008u + 2u * sides) >> 3u) + nearest) >> 1u);
    int u = int(uv & 255u);
    int v = int(uv >> 16u);
    int scaled_y = (int(luma) * 19077) >> 8;
    uvec3 rgb = clip_rgb(ivec3(scaled_y + ((v * 26149) >> 8) - 14234,
                               scaled_y - ((u * 6419) >> 8) - ((v * 13320) >> 8) + 8708,
                               scaled_y + ((u * 33050) >> 8) - 17685));
    // (rgb * alpha + 127) / 255 for products up to 255 * 255, without the divide (Blinn, "Three Wrongs Make a Right").
    rgb = rgb * alpha + 128u;
    rgb = (rgb + (rgb >> 8u)) >> 8u;
    vec4 result = vec4(vec3(rgb), float(alpha)) * (1.0 / 255.0);
    if (over)
        result += under * (1.0 - result.a);
    imageStore(media_atlas, destination, result);
}
