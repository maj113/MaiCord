// twin: src/render/shaders/png_atlas.hlsl 0cb614e92d13cd70
// png_expand (STAGE_CS): decodes reconstructed PNG scanlines into the premultiplied media atlas.
layout(local_size_x = 8, local_size_y = 8) in;

layout(std140, binding = 0) uniform PngAtlasConstants {
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
    uint reserved0; // 1: over-blend onto the atlas
};

layout(std430, binding = 0) readonly buffer Scanlines { uint scanlines[]; }; //! t0
layout(std430, binding = 1) readonly buffer Palette { uint palette_rgba[]; }; //! t1
// ES 3.1 images of this format are either read-only or write-only: the atlas is bound twice, and the over-blend reads
// through the second view.
layout(rgba8, binding = 0) uniform writeonly highp image2D media_atlas; //! u0
layout(rgba8, binding = 1) uniform readonly highp image2D media_atlas_read; //! u1

uint load_byte(uint offset) {
    return (scanlines[offset >> 2u] >> ((offset & 3u) * 8u)) & 255u;
}

// Four bytes starting at any byte offset, little-endian: one word load when the offset is aligned, two otherwise
// (the second is only read for its low bytes).
uint load_word(uint offset) {
    uint shift = (offset & 3u) * 8u;
    uint word = scanlines[offset >> 2u];
    if (shift != 0u)
        word = (word >> shift) | (scanlines[(offset >> 2u) + 1u] << (32u - shift));
    return word;
}

uint load_sample(uint offset) {
    return bit_depth == 16u ? (load_byte(offset) << 8u) | load_byte(offset + 1u) : load_byte(offset);
}

uint expand_sample(uint value) {
    // Bit depth is uniform; keep unused integer divisions off the 8-bit path.
    uint result = value;
    if (bit_depth != 8u) {
        if (bit_depth == 16u)
            result = (value + 128u) / 257u;
        else
            result = value * 255u / ((1u << bit_depth) - 1u);
    }
    return result;
}

uvec4 unpack_rgba(uint word) {
    return uvec4(word & 255u, (word >> 8u) & 255u, (word >> 16u) & 255u, word >> 24u);
}

uint packed_sample(uint row, uint x) {
    uint bit_offset = x * bit_depth;
    uint shift = 8u - bit_depth - (bit_offset & 7u);
    return (load_byte(row + (bit_offset >> 3u)) >> shift) & ((1u << bit_depth) - 1u);
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= image_width || id.y >= image_height)
        return;
    uint x = id.x;
    // Rows of raw RGBA8 (color type 7) carry no filter byte.
    uint row = id.y * row_stride + (color_type == 7u || color_type == 8u ? 0u : 1u);
    uint sample_bytes = bit_depth == 16u ? 2u : 1u;
    uvec4 rgba = uvec4(0u, 0u, 0u, 255u);

    // 8-bit RGBA rows (PNG color type 6, raw color type 7): one word per pixel instead of four byte loads.
    if (color_type == 7u || (color_type == 6u && bit_depth == 8u)) {
        rgba = unpack_rgba(load_word(row + x * 4u));
    } else if (color_type == 0u) {
        uint gray = bit_depth < 8u ? packed_sample(row, x) : load_sample(row + x * sample_bytes);
        rgba.rgb = uvec3(expand_sample(gray));
        rgba.a = gray == transparent_gray ? 0u : 255u;
    } else if (color_type == 2u) {
        uint offset = row + x * 3u * sample_bytes;
        uint red = load_sample(offset);
        uint green = load_sample(offset + sample_bytes);
        uint blue = load_sample(offset + sample_bytes * 2u);
        rgba.rgb = uvec3(expand_sample(red), expand_sample(green), expand_sample(blue));
        rgba.a = red == transparent_red && green == transparent_green && blue == transparent_blue ? 0u : 255u;
    } else if (color_type == 3u || color_type == 8u) {
        uint index = bit_depth < 8u ? packed_sample(row, x) : load_byte(row + x);
        rgba = unpack_rgba(index < palette_count ? palette_rgba[index] : 0u);
    } else if (color_type == 4u) {
        uint offset = row + x * 2u * sample_bytes;
        uint gray = expand_sample(load_sample(offset));
        rgba.rgb = uvec3(gray);
        rgba.a = expand_sample(load_sample(offset + sample_bytes));
    } else {
        uint offset = row + x * 4u * sample_bytes;
        rgba = uvec4(expand_sample(load_sample(offset)), expand_sample(load_sample(offset + sample_bytes)),
                     expand_sample(load_sample(offset + sample_bytes * 2u)),
                     expand_sample(load_sample(offset + sample_bytes * 3u)));
    }

    // Over-blend onto a premultiplied destination: a transparent source leaves it unchanged and an opaque one
    // replaces it, so neither reads the atlas.
    bool blend_over = reserved0 == 1u;
    if (blend_over && rgba.a == 0u)
        return;
    rgba.rgb = (rgba.rgb * rgba.a + 127u) / 255u;
    ivec2 destination = ivec2(uvec2(destination_x, destination_y) + id);
    vec4 result = vec4(rgba) * (1.0 / 255.0);
    if (blend_over && rgba.a != 255u)
        result += imageLoad(media_atlas_read, destination) * (1.0 - result.a);
    imageStore(media_atlas, destination, result);
}
