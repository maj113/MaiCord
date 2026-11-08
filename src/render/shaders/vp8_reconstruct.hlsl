// Residual inverse DCT pass (vp8_inverse_dct) for vp8_predict.hlsl. Residuals are stored per
// macroblock in raster order, two 16-bit samples to a word: luma rows of 16 (8 words), then the U and the V
// rows of 8 (4 words); 192 words a macroblock.
cbuffer Vp8ReconstructionConstants : register(b0) {
    uint macroblock_columns;
    uint macroblock_rows;
    uint luma_stride;
    uint chroma_stride;
    uint chroma_rows;
    uint u_plane_offset;
    uint v_plane_offset;
    uint mode_record_bytes;
    uint coefficient_record_bytes;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

ByteAddressBuffer vp8_coefficients : register(t1);
RWByteAddressBuffer vp8_yuv : register(u0);

uint load_byte(ByteAddressBuffer source, uint address) {
    const uint packed = source.Load(address & ~3u);
    return (packed >> ((address & 3u) * 8u)) & 255u;
}

// Vp8MacroblockCoefficients is 770 bytes, NOT dword aligned. Odd records
// need a three-dword load and a halfword splice; never assume Load2(src).
int4 load_coefficient_row(uint address) {
    uint2 packed;
    if ((address & 2u) == 0u) {
        packed = vp8_coefficients.Load2(address);
    } else {
        const uint3 words = vp8_coefficients.Load3(address & ~3u);
        packed = uint2((words.x >> 16u) | (words.y << 16u),
                       (words.y >> 16u) | (words.z << 16u));
    }
    return int4(asint(packed.x << 16u) >> 16, asint(packed.x) >> 16,
                asint(packed.y << 16u) >> 16, asint(packed.y) >> 16);
}

// Word address of row 0 of 4x4 block `block` (16 luma blocks, then four U and four V).
uint residual_address(uint macroblock, uint block) {
    const uint base = v_plane_offset + chroma_stride * chroma_rows + macroblock * 192u;
    if (block < 16u) return base + (block >> 2u) * 32u + (block & 3u) * 2u;
    const uint chroma = block - 16u;
    return base + 128u + (chroma >> 2u) * 32u + ((chroma >> 1u) & 1u) * 16u + (chroma & 1u) * 2u;
}

// Prediction is 0..255, so clamping a residual to -256..255 leaves every reconstructed sample as it was and
// fits it in 16 bits.
uint2 pack_residual(int4 value) {
    const int4 bounded = clamp(value, -256, 255);
    return uint2((uint(bounded.x) & 65535u) | (uint(bounded.y) << 16u),
                 (uint(bounded.z) & 65535u) | (uint(bounded.w) << 16u));
}

// Exact floor(value * factor / 65536), including negative values. The
// low-word product is unsigned; a full signed product would overflow in pass 2.
int multiply_shift16(int value, uint factor) {
    return (value >> 16) * int(factor) +
           int(((uint(value) & 65535u) * factor) >> 16u);
}

int4 inverse_dct_row(int4 value) {
    const int dc = value.x + 4;
    const int a = dc + value.z;
    const int b = dc - value.z;
    const int c = multiply_shift16(value.y, 35468u) -
                 (value.w + multiply_shift16(value.w, 20091u));
    const int d = value.y + multiply_shift16(value.y, 20091u) +
                 multiply_shift16(value.w, 35468u);
    return int4(a + d, b + c, b - c, a - d) >> 3;
}

// Transforms remain independent of the dependency-ordered prediction pass.
[numthreads(64, 1, 1)]
void vp8_inverse_dct(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint macroblock = dispatch_id.x / 24u;
    const uint block = dispatch_id.x % 24u;
    if (macroblock >= macroblock_columns * macroblock_rows) return;
    // Prediction checks the same flag: stale residuals are left untouched and
    // never consumed.
    if (load_byte(vp8_coefficients, macroblock * coefficient_record_bytes + 768u) == 0u) return;

    const uint source = macroblock * coefficient_record_bytes + block * 32u;
    const int4 v0 = load_coefficient_row(source);
    const int4 v1 = load_coefficient_row(source + 8u);
    const int4 v2 = load_coefficient_row(source + 16u);
    const int4 v3 = load_coefficient_row(source + 24u);
    const uint destination = residual_address(macroblock, block) * 4u;
    const uint row_bytes = block < 16u ? 32u : 16u;
    const int ac = v0.y | v0.z | v0.w | v1.x | v1.y | v1.z | v1.w |
                   v2.x | v2.y | v2.z | v2.w | v3.x | v3.y | v3.z | v3.w;
    if (ac == 0) {
        const int dc = (v0.x + 4) >> 3;
        const uint2 packed = pack_residual(int4(dc, dc, dc, dc));
        [unroll] for (uint y = 0u; y < 4u; ++y)
            vp8_yuv.Store2(destination + y * row_bytes, packed);
        return;
    }
    // Pass 1 sees signed int16 inputs; unlike pass 2, direct products are safe.
    const int4 a = v0 + v2;
    const int4 b = v0 - v2;
    const int4 c = ((v1 * 35468) >> 16) - (v3 + ((v3 * 20091) >> 16));
    const int4 d = v1 + ((v1 * 20091) >> 16) + ((v3 * 35468) >> 16);
    vp8_yuv.Store2(destination, pack_residual(inverse_dct_row(a + d)));
    vp8_yuv.Store2(destination + row_bytes, pack_residual(inverse_dct_row(b + c)));
    vp8_yuv.Store2(destination + 2u * row_bytes, pack_residual(inverse_dct_row(b - c)));
    vp8_yuv.Store2(destination + 3u * row_bytes, pack_residual(inverse_dct_row(a - d)));
}
