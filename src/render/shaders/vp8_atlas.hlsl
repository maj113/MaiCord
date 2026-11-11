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

// Chroma samples x and x + 1 of row y, U in the low and V in the high halfword,
// so the upsampling filters both planes at once (libwebp's LOAD_UV): no
// halfword of its sums exceeds 16 bits. Column x + 1 may lie past the plane's
// width, where the caller ignores it (past the buffer's end it reads 0).
uint2 load_uv_pair(uint x, uint y) {
    const uint offset = y * chroma_stride + x;
    const uint2 u = vp8_yuv.Load2((u_plane_offset + offset) * 4u);
    const uint2 v = vp8_yuv.Load2((v_plane_offset + offset) * 4u);
    return u | (v << 16u);
}

uint3 clip_rgb(int3 value) {
    return uint3(clamp(value >> 6, 0, 255));
}

// One thread per pixel, a row of 64 per group: the loads of a group are runs of
// consecutive words. The host dispatches ((width + 63) / 64, height, 1).
[numthreads(64, 1, 1)]
void vp8_to_atlas(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint x = dispatch_id.x;
    const uint y = dispatch_id.y;
    if (x >= image_width || y >= image_height) {
        return;
    }
    // Every load is issued before any result is needed, except that an
    // over-blend first waits for the alpha to skip transparent pixels.
    const uint luma = vp8_yuv.Load((y * luma_stride + x) * 4u);
    uint alpha = 255u;
    [branch] if (has_alpha != 0u) {
        const uint address = y * alpha_stride + x;
        alpha = (vp8_alpha.Load(address & ~3u) >> ((address & 3u) * 8u)) & 255u;
    }
    // Over-blend onto a premultiplied destination: a transparent source leaves
    // it unchanged (and needs no other read) and an opaque one replaces it, so
    // only alpha 1..254 reads the atlas.
    const bool over = blend_over != 0u;
    [branch] if (over && alpha == 0u) {
        return;
    }
    // Fancy upsampling (libwebp's UpsampleRgbaLinePair): pixel (x, y) lies in
    // the square of chroma samples a b / c d between columns block.x - 1 and
    // block.x and rows block.y - 1 and block.y, and takes
    // (9 * nearest + 3 * each side + farthest + 8) >> 4 of them. Samples past
    // the picture's edges clamp to the edge, which gives the edge rows' and
    // columns' two-tap filter and the corners' copies.
    const uint2 block = (uint2(x, y) + 1u) >> 1u;
    const uint chroma_right = (image_width + 1u) / 2u - 1u;
    const uint chroma_bottom = (image_height + 1u) / 2u - 1u;
    const uint chroma_x = max(block.x, 1u) - 1u;
    const uint2 top = load_uv_pair(chroma_x, max(block.y, 1u) - 1u);
    const uint2 bottom = load_uv_pair(chroma_x, min(block.y, chroma_bottom));
    const uint2 destination = uint2(destination_x + x, destination_y + y);
    float4 under = 0.0f;
    [branch] if (over && alpha - 1u < 254u) {
        under = media_atlas[destination];
    }
    const bool inner_x = block.x != 0u && block.x <= chroma_right;
    const uint a = top.x;
    const uint b = inner_x ? top.y : top.x;
    const uint c = bottom.x;
    const uint d = inner_x ? bottom.y : bottom.x;
    const bool near_left = (x & 1u) != 0u;
    const bool near_top = (y & 1u) != 0u;
    const uint nearest = near_top ? (near_left ? a : b) : (near_left ? c : d);
    const uint sides = near_left == near_top ? b + c : a + d;
    // (9n + 3s + 3t + f + 8) >> 4 == (((n + 3s + 3t + f + 8) >> 3) + n) >> 1
    // exactly; the shift drops bits of V into U's halfword above bit 12,
    // which the final mask removes.
    const uint uv = ((((a + b + c + d + 0x00080008u + 2u * sides) >> 3u) + nearest) >> 1u);
    const int u = int(uv & 255u);
    const int v = int(uv >> 16u);
    const int scaled_y = (int(luma) * 19077) >> 8;
    uint3 rgb = clip_rgb(int3(scaled_y + ((v * 26149) >> 8) - 14234,
                              scaled_y - ((u * 6419) >> 8) - ((v * 13320) >> 8) + 8708,
                              scaled_y + ((u * 33050) >> 8) - 17685));
    // (rgb * alpha + 127) / 255 for products up to 255 * 255, without the
    // divide (Blinn, "Three Wrongs Make a Right").
    rgb = rgb * alpha + 128u;
    rgb = (rgb + (rgb >> 8u)) >> 8u;
    float4 result = float4(float3(rgb), float(alpha)) / 255.0f;
    if (over) {
        result += under * (1.0f - result.a);
    }
    media_atlas[destination] = result;
}
