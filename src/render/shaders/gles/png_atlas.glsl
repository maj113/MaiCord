// twin: src/render/shaders/png_atlas.hlsl 528181fdfd15d82b
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

// A big-endian 16-bit sample. (sample + 128) / 257 scales it to 8 bits with rounding.
uint load_sample16(uint offset) {
    return (load_byte(offset) << 8u) | load_byte(offset + 1u);
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
    uvec4 rgba = uvec4(0u, 0u, 0u, 255u);

    // The bit depth is uniform: each type branches on it before its loads, so a pixel reads and scales only the
    // samples of its depth. Color types 2, 4 and 6 have 8 or 16 bits a sample (png_image_configure).
    // 8-bit RGBA rows (PNG color type 6, raw color type 7): one word per pixel instead of four byte loads.
    if (color_type == 7u || (color_type == 6u && bit_depth == 8u)) {
        rgba = unpack_rgba(load_word(row + x * 4u));
    } else if (color_type == 0u) {
        uint gray, expanded;
        if (bit_depth == 16u) {
            gray = load_sample16(row + x * 2u);
            expanded = (gray + 128u) / 257u;
        } else if (bit_depth == 8u) {
            gray = load_byte(row + x);
            expanded = gray;
        } else {
            gray = packed_sample(row, x);
            expanded = gray * 255u / ((1u << bit_depth) - 1u);
        }
        rgba.rgb = uvec3(expanded);
        rgba.a = gray == transparent_gray ? 0u : 255u;
    } else if (color_type == 2u) {
        uvec3 samples;
        if (bit_depth == 16u) {
            uint offset = row + x * 6u;
            samples = uvec3(load_sample16(offset), load_sample16(offset + 2u), load_sample16(offset + 4u));
            rgba.rgb = (samples + 128u) / 257u;
        } else {
            uint offset = row + x * 3u;
            samples = uvec3(load_byte(offset), load_byte(offset + 1u), load_byte(offset + 2u));
            rgba.rgb = samples;
        }
        rgba.a = all(equal(samples, uvec3(transparent_red, transparent_green, transparent_blue))) ? 0u : 255u;
    } else if (color_type == 3u || color_type == 8u) {
        uint index;
        if (bit_depth < 8u)
            index = packed_sample(row, x);
        else
            index = load_byte(row + x);
        rgba = unpack_rgba(index < palette_count ? palette_rgba[index] : 0u);
    } else if (color_type == 4u) {
        if (bit_depth == 16u) {
            uint offset = row + x * 4u;
            rgba.ga = (uvec2(load_sample16(offset), load_sample16(offset + 2u)) + 128u) / 257u;
        } else {
            uint offset = row + x * 2u;
            rgba.ga = uvec2(load_byte(offset), load_byte(offset + 1u));
        }
        rgba.rb = uvec2(rgba.g);
    } else {
        // 16-bit RGBA: the 8-bit form takes the first branch.
        uint offset = row + x * 8u;
        rgba = (uvec4(load_sample16(offset), load_sample16(offset + 2u), load_sample16(offset + 4u),
                      load_sample16(offset + 6u)) + 128u) / 257u;
    }

    // Over-blend onto a premultiplied destination: a transparent source leaves it unchanged and an opaque one
    // replaces it, so neither reads the atlas.
    bool blend_over = reserved0 == 1u;
    if (blend_over && rgba.a == 0u)
        return;
    // (c * a + 127) / 255 without the division: exact for every 8-bit c and a.
    uvec3 scaled = rgba.rgb * rgba.a + 128u;
    rgba.rgb = (scaled + (scaled >> 8u)) >> 8u;
    ivec2 destination = ivec2(uvec2(destination_x, destination_y) + id);
    vec4 result = vec4(rgba) * (1.0 / 255.0);
    if (blend_over && rgba.a != 255u)
        result += imageLoad(media_atlas_read, destination) * (1.0 - result.a);
    imageStore(media_atlas, destination, result);
}
