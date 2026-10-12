cbuffer Vp8AtlasConstants : register(b0) {
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
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
    uint reserved4;
};

ByteAddressBuffer vp8_yuv : register(t0);
ByteAddressBuffer vp8_alpha : register(t1);
RWTexture2D<unorm float4> media_atlas : register(u0);

uint load_yuv(uint plane, uint stride, uint x, uint y) {
    return vp8_yuv.Load((plane + y * stride + x) * 4u);
}

uint load_alpha(uint x, uint y) {
    const uint address = y * alpha_stride + x;
    const uint packed = vp8_alpha.Load(address & ~3u);
    return (packed >> ((address & 3u) * 8u)) & 255u;
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

    if (x == 0u ||
        (x == image_width - 1u && (image_width & 1u) == 0u)) {
        const uint chroma_x = x >> 1u;
        const uint top = load_yuv(plane, chroma_stride, chroma_x,
                                  top_row);
        const uint current = load_yuv(plane, chroma_stride, chroma_x,
                                      current_row);
        return bottom_output ? (3u * current + top + 2u) >> 2u
                             : (3u * top + current + 2u) >> 2u;
    }

    const uint right_x = (x + 1u) >> 1u;
    const uint left_x = right_x - 1u;
    const uint a = load_yuv(plane, chroma_stride, left_x, top_row);
    const uint b = load_yuv(plane, chroma_stride, right_x, top_row);
    const uint c = load_yuv(plane, chroma_stride, left_x, current_row);
    const uint d = load_yuv(plane, chroma_stride, right_x, current_row);
    if (!bottom_output) {
        return (x & 1u) != 0u
                   ? (9u * a + 3u * b + 3u * c + d + 8u) >> 4u
                   : (3u * a + 9u * b + c + 3u * d + 8u) >> 4u;
    }
    return (x & 1u) != 0u
               ? (3u * a + b + 9u * c + 3u * d + 8u) >> 4u
               : (a + 3u * b + 3u * c + 9u * d + 8u) >> 4u;
}

uint clip_rgb(int value) {
    return uint(clamp(value >> 6, 0, 255));
}

uint3 yuv_to_rgb(uint y, uint u, uint v) {
    const int scaled_y = (int(y) * 19077) >> 8;
    const int red = scaled_y + ((int(v) * 26149) >> 8) - 14234;
    const int green = scaled_y - ((int(u) * 6419) >> 8) -
                      ((int(v) * 13320) >> 8) + 8708;
    const int blue = scaled_y + ((int(u) * 33050) >> 8) - 17685;
    return uint3(clip_rgb(red), clip_rgb(green), clip_rgb(blue));
}

[numthreads(8, 8, 1)]
void vp8_to_atlas(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= image_width || dispatch_id.y >= image_height) {
        return;
    }
    const uint x = dispatch_id.x;
    const uint y = dispatch_id.y;
    const uint luma = load_yuv(0u, luma_stride, x, y);
    const uint u = fancy_chroma(u_plane_offset, x, y);
    const uint v = fancy_chroma(v_plane_offset, x, y);
    const uint alpha = has_alpha != 0u ? load_alpha(x, y) : 255u;
    uint3 rgb = yuv_to_rgb(luma, u, v);
    rgb = (rgb * alpha + 127u) / 255u;
    const uint2 destination =
        uint2(destination_x + x, destination_y + y);
    const float4 source =
        float4(float3(rgb), float(alpha)) / 255.0f;
    media_atlas[destination] =
        blend_over != 0u
            ? source + media_atlas[destination] * (1.0f - source.a)
            : source;
}
