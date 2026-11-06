// twin: src/render/shaders/vp8_atlas.hlsl 85ebf4a0764ab6d6
// vp8_to_atlas (STAGE_CS): reconstructed VP8 planes (fancy chroma upsampling, optional alpha) into the media atlas.
layout(local_size_x = 8, local_size_y = 8) in;

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

uint load_yuv(uint plane, uint stride, uint x, uint y) {
    return vp8_yuv[plane + y * stride + x];
}

uint load_alpha(uint x, uint y) {
    uint address = y * alpha_stride + x;
    return (vp8_alpha[address >> 2u] >> ((address & 3u) * 8u)) & 255u;
}

uint fancy_chroma(uint plane, uint x, uint y) {
    uint top_row = 0u;
    uint current_row = 0u;
    bool bottom_output = false;
    if (y == 0u) {
        top_row = 0u;
        current_row = 0u;
    } else if (y == image_height - 1u && (image_height & 1u) == 0u) {
        top_row = y >> 1u;
        current_row = top_row;
    } else if ((y & 1u) != 0u) {
        top_row = (y - 1u) >> 1u;
        current_row = top_row + 1u;
    } else {
        current_row = y >> 1u;
        top_row = current_row - 1u;
        bottom_output = true;
    }

    if (x == 0u || (x == image_width - 1u && (image_width & 1u) == 0u)) {
        uint chroma_x = x >> 1u;
        uint top = load_yuv(plane, chroma_stride, chroma_x, top_row);
        uint current = load_yuv(plane, chroma_stride, chroma_x, current_row);
        return bottom_output ? (3u * current + top + 2u) >> 2u : (3u * top + current + 2u) >> 2u;
    }

    uint right_x = (x + 1u) >> 1u;
    uint left_x = right_x - 1u;
    uint a = load_yuv(plane, chroma_stride, left_x, top_row);
    uint b = load_yuv(plane, chroma_stride, right_x, top_row);
    uint c = load_yuv(plane, chroma_stride, left_x, current_row);
    uint d = load_yuv(plane, chroma_stride, right_x, current_row);
    if (!bottom_output) {
        return (x & 1u) != 0u ? (9u * a + 3u * b + 3u * c + d + 8u) >> 4u
                              : (3u * a + 9u * b + c + 3u * d + 8u) >> 4u;
    }
    return (x & 1u) != 0u ? (3u * a + b + 9u * c + 3u * d + 8u) >> 4u
                          : (a + 3u * b + 3u * c + 9u * d + 8u) >> 4u;
}

uint clip_rgb(int value) {
    return uint(clamp(value >> 6, 0, 255));
}

uvec3 yuv_to_rgb(uint y, uint u, uint v) {
    int scaled_y = (int(y) * 19077) >> 8;
    int red = scaled_y + ((int(v) * 26149) >> 8) - 14234;
    int green = scaled_y - ((int(u) * 6419) >> 8) - ((int(v) * 13320) >> 8) + 8708;
    int blue = scaled_y + ((int(u) * 33050) >> 8) - 17685;
    return uvec3(clip_rgb(red), clip_rgb(green), clip_rgb(blue));
}

void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= image_width || y >= image_height)
        return;
    uint alpha = has_alpha != 0u ? load_alpha(x, y) : 255u;
    // Over-blend onto a premultiplied destination: a transparent source leaves it unchanged and an opaque one
    // replaces it, so neither reads the atlas.
    bool over = blend_over != 0u;
    if (over && alpha == 0u)
        return;
    uint luma = load_yuv(0u, luma_stride, x, y);
    uint u = fancy_chroma(u_plane_offset, x, y);
    uint v = fancy_chroma(v_plane_offset, x, y);
    uvec3 rgb = yuv_to_rgb(luma, u, v);
    rgb = (rgb * alpha + 127u) / 255u;
    ivec2 destination = ivec2(destination_x + x, destination_y + y);
    vec4 result = vec4(vec3(rgb), float(alpha)) * (1.0 / 255.0);
    if (over && alpha != 255u)
        result += imageLoad(media_atlas_read, destination) * (1.0 - result.a);
    imageStore(media_atlas, destination, result);
}
