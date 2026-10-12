cbuffer PngAtlasConstants : register(b0) {
    uint row_stride;
    uint image_width;
    uint image_height;
    uint bit_depth;
    uint color_type;
    uint destination_x;
    uint destination_y;
    uint palette_count;
    uint transparent_gray;
    uint transparent_red;
    uint transparent_green;
    uint transparent_blue;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
};

ByteAddressBuffer reconstructed_scanlines : register(t0);
Buffer<uint> palette_rgba : register(t1);
RWTexture2D<unorm float4> media_atlas : register(u0);

uint load_byte(uint offset) {
    const uint packed = reconstructed_scanlines.Load(offset & ~3u);
    return (packed >> ((offset & 3u) * 8u)) & 255u;
}

uint load_sample(uint offset) {
    return bit_depth == 16u
               ? (load_byte(offset) << 8u) | load_byte(offset + 1u)
               : load_byte(offset);
}

uint expand_sample(uint sample) {
    if (bit_depth == 16u) {
        return (sample * 255u + 32767u) / 65535u;
    }
    if (bit_depth == 8u) {
        return sample;
    }
    return sample * 255u / ((1u << bit_depth) - 1u);
}

uint packed_sample(uint row, uint x) {
    const uint bit_offset = x * bit_depth;
    const uint shift = 8u - bit_depth - (bit_offset & 7u);
    return (load_byte(row + (bit_offset >> 3u)) >> shift) &
           ((1u << bit_depth) - 1u);
}

[numthreads(8, 8, 1)]
void png_expand(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= image_width || dispatch_id.y >= image_height) {
        return;
    }

    const uint x = dispatch_id.x;
    const uint row = dispatch_id.y * row_stride +
                     (color_type == 7u || color_type == 8u ? 0u : 1u);
    const uint sample_bytes = bit_depth == 16u ? 2u : 1u;
    uint4 rgba = uint4(0u, 0u, 0u, 255u);

    if (color_type == 0u) {
        const uint gray = bit_depth < 8u
                              ? packed_sample(row, x)
                              : load_sample(row + x * sample_bytes);
        const uint expanded = expand_sample(gray);
        rgba.rgb = expanded;
        rgba.a = gray == transparent_gray ? 0u : 255u;
    } else if (color_type == 2u) {
        const uint offset = row + x * 3u * sample_bytes;
        const uint red = load_sample(offset);
        const uint green = load_sample(offset + sample_bytes);
        const uint blue = load_sample(offset + sample_bytes * 2u);
        rgba.rgb = uint3(expand_sample(red), expand_sample(green),
                         expand_sample(blue));
        rgba.a = red == transparent_red && green == transparent_green &&
                         blue == transparent_blue
                     ? 0u
                     : 255u;
    } else if (color_type == 3u || color_type == 8u) {
        const uint index = bit_depth < 8u
                               ? packed_sample(row, x)
                               : load_byte(row + x);
        const uint packed = palette_rgba[index];
        rgba = uint4(packed & 255u, (packed >> 8u) & 255u,
                     (packed >> 16u) & 255u, packed >> 24u);
    } else if (color_type == 4u) {
        const uint offset = row + x * 2u * sample_bytes;
        const uint gray = expand_sample(load_sample(offset));
        rgba.rgb = gray;
        rgba.a = expand_sample(load_sample(offset + sample_bytes));
    } else {
        const uint offset = row + x * 4u * sample_bytes;
        rgba = uint4(expand_sample(load_sample(offset)),
                     expand_sample(load_sample(offset + sample_bytes)),
                     expand_sample(load_sample(offset + sample_bytes * 2u)),
                     expand_sample(load_sample(offset + sample_bytes * 3u)));
    }

    rgba.rgb = (rgba.rgb * rgba.a + 127u) / 255u;
    const uint2 destination =
        uint2(destination_x + x, destination_y + dispatch_id.y);
    const float4 source = float4(rgba) / 255.0f;
    media_atlas[destination] = reserved0 == 1u
                                   ? source + media_atlas[destination] *
                                                  (1.0f - source.a)
                                   : source;
}
