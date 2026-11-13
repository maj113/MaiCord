// twin: src/render/shaders/vp8_reconstruct.hlsl a875fab014663f36
// vp8_inverse_dct (STAGE_CS): the residual inverse DCT pass for vp8_predict.glsl. Residuals are stored per
// macroblock in raster order, two 16-bit samples to a word: luma rows of 16 (8 words), then the U and the V rows of 8
// (4 words); 192 words a macroblock.
layout(local_size_x = 64) in;

layout(std140, binding = 0) uniform Vp8ReconstructionConstants {
    uint macroblock_columns;
    uint macroblock_rows;
    uint luma_stride;
    uint chroma_stride;
    uint chroma_rows;
    uint u_plane_offset;
    uint v_plane_offset;
    uint mode_record_bytes;
    uint coefficient_record_bytes;
};

layout(std430, binding = 0) readonly buffer Coefficients { uint vp8_coefficients[]; }; //! t1
layout(std430, binding = 1) buffer Yuv { uint vp8_yuv[]; }; //! u0

// Vp8MacroblockCoefficients is a multiple of four bytes, so a row of four coefficients is two words.
ivec4 load_coefficient_row(uint address) {
    uint at = address >> 2u;
    uvec2 halves = uvec2(vp8_coefficients[at], vp8_coefficients[at + 1u]);
    return ivec4(int(halves.x << 16u) >> 16, int(halves.x) >> 16, int(halves.y << 16u) >> 16, int(halves.y) >> 16);
}

// Exact floor(value * factor / 65536), including negative values. The low-word product is unsigned; a full signed
// product would overflow in pass 2.
int multiply_shift16(int value, uint factor) {
    return (value >> 16) * int(factor) + int(((uint(value) & 65535u) * factor) >> 16u);
}

ivec4 inverse_dct_row(ivec4 value) {
    int dc = value.x + 4;
    int a = dc + value.z;
    int b = dc - value.z;
    int c = multiply_shift16(value.y, 35468u) - (value.w + multiply_shift16(value.w, 20091u));
    int d = value.y + multiply_shift16(value.y, 20091u) + multiply_shift16(value.w, 35468u);
    return ivec4(a + d, b + c, b - c, a - d) >> 3;
}

// Word address of row 0 of 4x4 block `block` (16 luma blocks, then four U and four V).
uint residual_address(uint macroblock, uint block) {
    uint base = v_plane_offset + chroma_stride * chroma_rows + macroblock * 192u;
    if (block < 16u) return base + (block >> 2u) * 32u + (block & 3u) * 2u;
    uint chroma = block - 16u;
    return base + 128u + (chroma >> 2u) * 32u + ((chroma >> 1u) & 1u) * 16u + (chroma & 1u) * 2u;
}

// Prediction is 0..255, so clamping a residual to -256..255 leaves every reconstructed sample as it was and fits it
// in 16 bits.
void store_row(uint at, ivec4 row) {
    uvec4 bounded = uvec4(clamp(row, -256, 255));
    vp8_yuv[at] = (bounded.x & 65535u) | (bounded.y << 16u);
    vp8_yuv[at + 1u] = (bounded.z & 65535u) | (bounded.w << 16u);
}

// Transforms remain independent of the dependency-ordered prediction pass.
void main() {
    uint macroblock = gl_GlobalInvocationID.x / 24u;
    uint block = gl_GlobalInvocationID.x % 24u;
    if (macroblock >= macroblock_columns * macroblock_rows)
        return;
    // Prediction checks the same flag: stale residuals are left untouched and never consumed.
    if ((vp8_coefficients[(macroblock * coefficient_record_bytes + 768u) >> 2u] & 255u) == 0u)
        return;

    uint source = macroblock * coefficient_record_bytes + block * 32u;
    ivec4 v0 = load_coefficient_row(source);
    ivec4 v1 = load_coefficient_row(source + 8u);
    ivec4 v2 = load_coefficient_row(source + 16u);
    ivec4 v3 = load_coefficient_row(source + 24u);
    uint destination = residual_address(macroblock, block);
    uint row_words = block < 16u ? 8u : 4u;
    int ac = v0.y | v0.z | v0.w | v1.x | v1.y | v1.z | v1.w | v2.x | v2.y | v2.z | v2.w | v3.x | v3.y | v3.z | v3.w;
    if (ac == 0) {
        int dc = (v0.x + 4) >> 3;
        for (uint y = 0u; y < 4u; ++y)
            store_row(destination + y * row_words, ivec4(dc));
        return;
    }
    // Pass 1 sees signed int16 inputs; unlike pass 2, direct products are safe.
    ivec4 a = v0 + v2;
    ivec4 b = v0 - v2;
    ivec4 c = ((v1 * 35468) >> 16) - (v3 + ((v3 * 20091) >> 16));
    ivec4 d = v1 + ((v1 * 20091) >> 16) + ((v3 * 35468) >> 16);
    store_row(destination, inverse_dct_row(a + d));
    store_row(destination + row_words, inverse_dct_row(b + c));
    store_row(destination + 2u * row_words, inverse_dct_row(b - c));
    store_row(destination + 3u * row_words, inverse_dct_row(a - d));
}
