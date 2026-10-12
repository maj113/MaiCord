cbuffer JpegIdctConstants : register(b0) {
    uint coefficient_block_count;
    uint component_count;
    uint c0_block_offset;
    uint c0_block_count;
    uint c0_block_columns;
    uint c0_sample_offset;
    uint c0_sample_stride;
    uint c0_quantization_table;
    uint c1_block_offset;
    uint c1_block_count;
    uint c1_block_columns;
    uint c1_sample_offset;
    uint c1_sample_stride;
    uint c1_quantization_table;
    uint c2_block_offset;
    uint c2_block_count;
    uint c2_block_columns;
    uint c2_sample_offset;
    uint c2_sample_stride;
    uint c2_quantization_table;
    uint c3_block_offset;
    uint c3_block_count;
    uint c3_block_columns;
    uint c3_sample_offset;
    uint c3_sample_stride;
    uint c3_quantization_table;
    uint idct_reserved0;
    uint idct_reserved1;
    uint idct_reserved2;
    uint idct_reserved3;
    uint idct_reserved4;
    uint idct_reserved5;
};

ByteAddressBuffer jpeg_coefficients : register(t0);
ByteAddressBuffer jpeg_auxiliary : register(t1);
RWByteAddressBuffer jpeg_samples : register(u0);

static const float idct_basis[64] = {
    0.353553391f,  0.353553391f,  0.353553391f,  0.353553391f,  0.353553391f,  0.353553391f,  0.353553391f,
    0.353553391f,  0.490392640f,  0.415734806f,  0.277785117f,  0.097545161f,  -0.097545161f, -0.277785117f,
    -0.415734806f, -0.490392640f, 0.461939766f,  0.191341716f,  -0.191341716f, -0.461939766f, -0.461939766f,
    -0.191341716f, 0.191341716f,  0.461939766f,  0.415734806f,  -0.097545161f, -0.490392640f, -0.277785117f,
    0.277785117f,  0.490392640f,  0.097545161f,  -0.415734806f, 0.353553391f,  -0.353553391f, -0.353553391f,
    0.353553391f,  0.353553391f,  -0.353553391f, -0.353553391f, 0.353553391f,  0.277785117f,  -0.490392640f,
    0.097545161f,  0.415734806f,  -0.415734806f, -0.097545161f, 0.490392640f,  -0.277785117f, 0.191341716f,
    -0.461939766f, 0.461939766f,  -0.191341716f, -0.191341716f, 0.461939766f,  -0.461939766f, 0.191341716f,
    0.097545161f,  -0.277785117f, 0.415734806f,  -0.490392640f, 0.490392640f,  -0.415734806f, 0.277785117f,
    -0.097545161f,
};

int load_i16(ByteAddressBuffer source, uint address) {
    const uint packed = source.Load(address & ~3u);
    const uint value = (packed >> ((address & 2u) * 8u)) & 65535u;
    return int(value << 16u) >> 16;
}

uint load_u16(ByteAddressBuffer source, uint address) {
    const uint packed = source.Load(address & ~3u);
    return (packed >> ((address & 2u) * 8u)) & 65535u;
}

[numthreads(8, 8, 1)] void jpeg_idct(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
    const uint block = group_id.x;
    if (block >= coefficient_block_count) {
        return;
    }

    uint block_offset = c0_block_offset;
    uint block_columns = c0_block_columns;
    uint sample_offset = c0_sample_offset;
    uint sample_stride = c0_sample_stride;
    uint quantization_table = c0_quantization_table;
    if (component_count > 1u && block >= c1_block_offset) {
        block_offset = c1_block_offset;
        block_columns = c1_block_columns;
        sample_offset = c1_sample_offset;
        sample_stride = c1_sample_stride;
        quantization_table = c1_quantization_table;
    }
    if (component_count > 2u && block >= c2_block_offset) {
        block_offset = c2_block_offset;
        block_columns = c2_block_columns;
        sample_offset = c2_sample_offset;
        sample_stride = c2_sample_stride;
        quantization_table = c2_quantization_table;
    }
    if (component_count > 3u && block >= c3_block_offset) {
        block_offset = c3_block_offset;
        block_columns = c3_block_columns;
        sample_offset = c3_sample_offset;
        sample_stride = c3_sample_stride;
        quantization_table = c3_quantization_table;
    }

    const uint component_block = block - block_offset;
    const uint block_x = component_block % block_columns;
    const uint block_y = component_block / block_columns;
    const uint x = group_thread.x;
    const uint y = group_thread.y;
    float sample = 0.0f;
    [unroll] for (uint v = 0u; v < 8u; ++v) {
        [unroll] for (uint u = 0u; u < 8u; ++u) {
            const uint coefficient = v * 8u + u;
            const int value = load_i16(jpeg_coefficients, block * 128u + coefficient * 2u);
            const uint quantization =
                load_u16(jpeg_auxiliary, (quantization_table * 64u + coefficient) * 2u);
            sample += float(value * int(quantization)) * idct_basis[u * 8u + x] * idct_basis[v * 8u + y];
        }
    }
    const uint reconstructed = uint(clamp(floor(sample + 128.5f), 0.0f, 255.0f));
    const uint destination = sample_offset + (block_y * 8u + y) * sample_stride + block_x * 8u + x;
    jpeg_samples.Store(destination * 4u, reconstructed);
}

