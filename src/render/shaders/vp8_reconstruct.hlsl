// Residual inverse DCT pass (vp8_inverse_dct) for vp8_reconstruct_tiled.hlsl.
cbuffer Vp8ReconstructionConstants : register(b0) {
    uint image_width;
    uint image_height;
    uint macroblock_columns;
    uint macroblock_rows;
    uint final_wave_rank;
    uint luma_stride;
    uint luma_rows;
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

uint residual_address(uint macroblock, uint block) {
    return (v_plane_offset + chroma_stride * chroma_rows + macroblock * 384u + block * 16u) * 4u;
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
    const uint destination = residual_address(macroblock, block);
    const int ac = v0.y | v0.z | v0.w | v1.x | v1.y | v1.z | v1.w |
                   v2.x | v2.y | v2.z | v2.w | v3.x | v3.y | v3.z | v3.w;
    if (ac == 0) {
        const int dc = (v0.x + 4) >> 3;
        [unroll] for (uint y = 0u; y < 4u; ++y)
            vp8_yuv.Store4(destination + y * 16u, asuint(int4(dc, dc, dc, dc)));
        return;
    }
    // Pass 1 sees signed int16 inputs; unlike pass 2, direct products are safe.
    const int4 a = v0 + v2;
    const int4 b = v0 - v2;
    const int4 c = ((v1 * 35468) >> 16) - (v3 + ((v3 * 20091) >> 16));
    const int4 d = v1 + ((v1 * 20091) >> 16) + ((v3 * 35468) >> 16);
    vp8_yuv.Store4(destination, asuint(inverse_dct_row(a + d)));
    vp8_yuv.Store4(destination + 16u, asuint(inverse_dct_row(b + c)));
    vp8_yuv.Store4(destination + 32u, asuint(inverse_dct_row(b - c)));
    vp8_yuv.Store4(destination + 48u, asuint(inverse_dct_row(a - d)));
}
