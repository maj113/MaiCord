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

// Two cooperative 1-D passes: one dequantization load per coefficient.
// Group-wide barriers are uniform; no hardware wave-width assumption.
groupshared float idct_dequantized[64];
groupshared float idct_horizontal[64];

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
    const uint coefficient = y * 8u + x;
    const int value = load_i16(jpeg_coefficients, block * 128u + coefficient * 2u);
    const uint quantization =
        load_u16(jpeg_auxiliary, (quantization_table * 64u + coefficient) * 2u);
    idct_dequantized[coefficient] = float(value * int(quantization));
    GroupMemoryBarrierWithGroupSync();

    float horizontal = 0.0f;
    [unroll] for (uint u = 0u; u < 8u; ++u) {
        horizontal += idct_dequantized[y * 8u + u] * idct_basis[u * 8u + x];
    }
    idct_horizontal[coefficient] = horizontal;
    GroupMemoryBarrierWithGroupSync();

    float sample = 0.0f;
    [unroll] for (uint v = 0u; v < 8u; ++v) {
        sample += idct_horizontal[v * 8u + x] * idct_basis[v * 8u + y];
    }
    const uint reconstructed = uint(clamp(floor(sample + 128.5f), 0.0f, 255.0f));
    const uint destination = sample_offset + (block_y * 8u + y) * sample_stride + block_x * 8u + x;
    jpeg_samples.Store(destination * 4u, reconstructed);
}

cbuffer JpegAtlasConstants : register(b0) {
    uint image_width;
    uint image_height;
    uint destination_x;
    uint destination_y;
    uint color_transform_orientation;
    uint maximum_horizontal_sampling;
    uint maximum_vertical_sampling;
    uint atlas_component_count;
    uint a0_sample_offset;
    uint a0_sample_stride;
    uint a0_sample_width;
    uint a0_sample_height;
    uint a0_horizontal_sampling;
    uint a0_vertical_sampling;
    uint a1_sample_offset;
    uint a1_sample_stride;
    uint a1_sample_width;
    uint a1_sample_height;
    uint a1_horizontal_sampling;
    uint a1_vertical_sampling;
    uint a2_sample_offset;
    uint a2_sample_stride;
    uint a2_sample_width;
    uint a2_sample_height;
    uint a2_horizontal_sampling;
    uint a2_vertical_sampling;
    uint a3_sample_offset;
    uint a3_sample_stride;
    uint a3_sample_width;
    uint a3_sample_height;
    uint a3_horizontal_sampling;
    uint a3_vertical_sampling;
    uint icc_transform_kind;
    uint icc_pcs;
    uint icc_input_channels;
    uint icc_clut_offset;
    uint icc_clut_entry_count;
    uint icc_grid0;
    uint icc_grid1;
    uint icc_grid2;
    uint icc_grid3;
    uint icc_precision_bytes;
    uint icc_reserved0;
    uint icc_reserved1;
    float4 icc_matrix_row0;
    float4 icc_matrix_row1;
    float4 icc_matrix_row2;
    uint4 icc_curve_layouts[7];
    float4 icc_curve_parameters0[7];
    float4 icc_curve_parameters1[7];
};

ByteAddressBuffer reconstructed_components : register(t0);
RWTexture2D<unorm float4> media_atlas : register(u0);

float component_sample(uint offset, uint stride, uint width, uint height, uint horizontal_sampling,
                       uint vertical_sampling, uint x, uint y) {
    const float source_x =
        (float(x) + 0.5f) * float(horizontal_sampling) / float(maximum_horizontal_sampling) - 0.5f;
    const float source_y =
        (float(y) + 0.5f) * float(vertical_sampling) / float(maximum_vertical_sampling) - 0.5f;
    const int left = int(floor(source_x));
    const int top = int(floor(source_y));
    const uint x0 = uint(clamp(left, 0, int(width) - 1));
    const uint x1 = uint(clamp(left + 1, 0, int(width) - 1));
    const uint y0 = uint(clamp(top, 0, int(height) - 1));
    const uint y1 = uint(clamp(top + 1, 0, int(height) - 1));
    const float fx = source_x - floor(source_x);
    const float fy = source_y - floor(source_y);
    const float top_value =
        lerp(float(reconstructed_components.Load((offset + y0 * stride + x0) * 4u)),
             float(reconstructed_components.Load((offset + y0 * stride + x1) * 4u)), fx);
    const float bottom_value =
        lerp(float(reconstructed_components.Load((offset + y1 * stride + x0) * 4u)),
             float(reconstructed_components.Load((offset + y1 * stride + x1) * 4u)), fx);
    return lerp(top_value, bottom_value, fy);
}

uint rounded_byte(float value) {
    return uint(clamp(floor(value + 0.5f), 0.0f, 255.0f));
}

float icc_curve(uint curve_index, float x) {
    const uint4 layout = icc_curve_layouts[curve_index];
    const uint kind = layout.x;
    const uint table_offset = layout.y;
    const uint table_count = layout.z;
    const float4 parameters0 = icc_curve_parameters0[curve_index];
    const float4 parameters1 = icc_curve_parameters1[curve_index];
    x = saturate(x);
    if (kind == 0u) {
        return x;
    }
    if (kind == 1u) {
        const float g = parameters0.x;
        const float a = parameters0.y;
        const float b = parameters0.z;
        const float c = parameters0.w;
        const float d = parameters1.x;
        const float e = parameters1.y;
        const float f = parameters1.z;
        const float value = x < d ? c * x + f : pow(max(a * x + b, 0.0f), g) + e;
        return saturate(value);
    }
    const float position = x * float(table_count - 1u);
    const uint lower = min(uint(floor(position)), table_count - 1u);
    const uint upper = min(lower + 1u, table_count - 1u);
    const float fraction = position - float(lower);
    const float low = float(load_u16(jpeg_auxiliary, (table_offset + lower) * 2u));
    const float high = float(load_u16(jpeg_auxiliary, (table_offset + upper) * 2u));
    return lerp(low, high, fraction) / 65535.0f;
}

float srgb_encode(float value);

float3 icc_clut(float4 input) {
    const uint4 grid = uint4(icc_grid0, icc_grid1, icc_grid2, icc_grid3);
    float4 position = 0.0f;
    position.x = saturate(input.x) * float(grid.x - 1u);
    if (icc_input_channels > 1u) {
        position.y = saturate(input.y) * float(grid.y - 1u);
    }
    if (icc_input_channels > 2u) {
        position.z = saturate(input.z) * float(grid.z - 1u);
    }
    if (icc_input_channels > 3u) {
        position.w = saturate(input.w) * float(grid.w - 1u);
    }
    const uint4 lower = uint4(floor(position));
    const float4 fraction = position - float4(lower);
    float3 result = 0.0f;
    const uint corner_count = 1u << icc_input_channels;
    [loop] for (uint corner = 0u; corner < 16u; ++corner) {
        if (corner >= corner_count) {
            break;
        }
        uint4 coordinate = lower;
        float weight = 1.0f;
        [unroll] for (uint channel = 0u; channel < 4u; ++channel) {
            if (channel < icc_input_channels) {
                const bool upper = (corner & (1u << channel)) != 0u;
                coordinate[channel] = min(lower[channel] + (upper ? 1u : 0u), grid[channel] - 1u);
                weight *= upper ? fraction[channel] : 1.0f - fraction[channel];
            }
        }
        uint node_index = coordinate.x;
        if (icc_input_channels > 1u) {
            node_index = node_index * grid.y + coordinate.y;
        }
        if (icc_input_channels > 2u) {
            node_index = node_index * grid.z + coordinate.z;
        }
        if (icc_input_channels > 3u) {
            node_index = node_index * grid.w + coordinate.w;
        }
        const uint entry = icc_clut_offset + node_index * 3u;
        result += weight *
                  float3(float(load_u16(jpeg_auxiliary, entry * 2u)),
                         float(load_u16(jpeg_auxiliary, (entry + 1u) * 2u)),
                         float(load_u16(jpeg_auxiliary, (entry + 2u) * 2u))) /
                  65535.0f;
    }
    return result;
}

float3 icc_lab_to_xyz(float3 lab) {
    const float lightness = lab.x * 100.0f;
    const float a = lab.y * 255.0f - 128.0f;
    const float b = lab.z * 255.0f - 128.0f;
    const float y = (lightness + 16.0f) / 116.0f;
    const float3 root = float3(y + a / 500.0f, y, y - b / 200.0f);
    float3 xyz;
    [unroll] for (uint channel = 0u; channel < 3u; ++channel) {
        const float cube = root[channel] * root[channel] * root[channel];
        xyz[channel] = cube > 0.008856f ? cube : (root[channel] - 16.0f / 116.0f) / 7.787f;
    }
    xyz.x *= 0.9642f;
    xyz.z *= 0.8249f;
    return xyz;
}

float3 icc_connection_to_xyz(float3 connection) {
    return icc_pcs == 1u ? icc_lab_to_xyz(connection) : connection;
}

float3 icc_transform(float4 device) {
    float3 connection = 0.0f;
    if (icc_transform_kind == 1u) {
        connection = float3(icc_curve(0u, device.x), icc_curve(1u, device.y), icc_curve(2u, device.z));
    } else if (icc_transform_kind == 2u) {
        const float gray = icc_curve(0u, device.x);
        connection = icc_pcs == 1u ? float3(gray, 128.0f / 255.0f, 128.0f / 255.0f)
                                   : gray * float3(0.9642f, 1.0f, 0.8249f);
        connection = icc_connection_to_xyz(connection);
    } else {
        float4 curved = 0.0f;
        [unroll] for (uint channel = 0u; channel < 4u; ++channel) {
            if (channel < icc_input_channels) {
                curved[channel] = icc_curve(channel, device[channel]);
            }
        }
        connection = icc_clut(curved);
        connection =
            float3(icc_curve(4u, connection.x), icc_curve(5u, connection.y), icc_curve(6u, connection.z));
        if (icc_precision_bytes == 2u) {
            connection *= icc_pcs == 1u ? 65535.0f / 65280.0f : 65535.0f / 32768.0f;
            if (icc_pcs == 1u) {
                connection = saturate(connection);
            }
        }
        connection = icc_connection_to_xyz(connection);
    }
    const float3 linear_srgb =
        float3(dot(icc_matrix_row0.xyz, connection), dot(icc_matrix_row1.xyz, connection),
               dot(icc_matrix_row2.xyz, connection));
    return float3(srgb_encode(linear_srgb.r), srgb_encode(linear_srgb.g), srgb_encode(linear_srgb.b));
}

float srgb_encode(float value) {
    value = saturate(value);
    return value < 0.003130805f ? 12.920000076f * value
                                : pow(1.137283325f * value, 0.416666657f) - 0.054969788f;
}

[numthreads(8, 8, 1)] void jpeg_to_atlas(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint jpeg_color_transform = color_transform_orientation & 255u;
    const uint orientation = (color_transform_orientation >> 8u) & 255u;
    const bool swapped = orientation >= 5u;
    const uint output_width = swapped ? image_height : image_width;
    const uint output_height = swapped ? image_width : image_height;
    if (dispatch_id.x >= output_width || dispatch_id.y >= output_height) {
        return;
    }
    const uint x = dispatch_id.x;
    const uint y = dispatch_id.y;
    uint source_x = x;
    uint source_y = y;
    if (orientation == 2u) {
        source_x = image_width - 1u - x;
    } else if (orientation == 3u) {
        source_x = image_width - 1u - x;
        source_y = image_height - 1u - y;
    } else if (orientation == 4u) {
        source_y = image_height - 1u - y;
    } else if (orientation == 5u) {
        source_x = y;
        source_y = x;
    } else if (orientation == 6u) {
        source_x = y;
        source_y = image_height - 1u - x;
    } else if (orientation == 7u) {
        source_x = image_width - 1u - y;
        source_y = image_height - 1u - x;
    } else if (orientation == 8u) {
        source_x = image_width - 1u - y;
        source_y = x;
    }
    const float first =
        component_sample(a0_sample_offset, a0_sample_stride, a0_sample_width, a0_sample_height,
                         a0_horizontal_sampling, a0_vertical_sampling, source_x, source_y);
    uint3 rgb = rounded_byte(first);
    float second = 0.0f;
    float third = 0.0f;
    float fourth = 0.0f;
    float4 icc_device = float4(float(rgb.r) / 255.0f, 0.0f, 0.0f, 0.0f);
    if (atlas_component_count >= 3u) {
        second = component_sample(a1_sample_offset, a1_sample_stride, a1_sample_width, a1_sample_height,
                                  a1_horizontal_sampling, a1_vertical_sampling, source_x, source_y);
        third = component_sample(a2_sample_offset, a2_sample_stride, a2_sample_width, a2_sample_height,
                                 a2_horizontal_sampling, a2_vertical_sampling, source_x, source_y);
        if (jpeg_color_transform == 2u || jpeg_color_transform == 3u) {
            rgb = uint3(rounded_byte(first), rounded_byte(second), rounded_byte(third));
        } else {
            const float cb = second - 128.0f;
            const float cr = third - 128.0f;
            rgb = uint3(rounded_byte(first + 1.402f * cr),
                        rounded_byte(first - 0.34414f * cb - 0.71414f * cr),
                        rounded_byte(first + 1.772f * cb));
        }
        icc_device = float4(float3(rgb) / 255.0f, 0.0f);
        if (atlas_component_count == 4u) {
            fourth = component_sample(a3_sample_offset, a3_sample_stride, a3_sample_width, a3_sample_height,
                                      a3_horizontal_sampling, a3_vertical_sampling, source_x, source_y);
            if (jpeg_color_transform == 3u) {
                icc_device = 1.0f - float4(first, second, third, fourth) / 255.0f;
                const float black = fourth / 255.0f;
                rgb = uint3(rounded_byte(first * black), rounded_byte(second * black),
                            rounded_byte(third * black));
            } else {
                icc_device = float4(float3(rgb) / 255.0f, 1.0f - fourth / 255.0f);
                const float black = fourth / 255.0f;
                rgb = uint3(rounded_byte((255.0f - float(rgb.r)) * black),
                            rounded_byte((255.0f - float(rgb.g)) * black),
                            rounded_byte((255.0f - float(rgb.b)) * black));
            }
        }
    }
    if (icc_transform_kind != 0u) {
        if (icc_transform_kind != 3u) {
            icc_device = float4(float3(rgb) / 255.0f, 0.0f);
        }
        const float3 encoded_srgb = icc_transform(icc_device);
        rgb = uint3(rounded_byte(encoded_srgb.r * 255.0f), rounded_byte(encoded_srgb.g * 255.0f),
                    rounded_byte(encoded_srgb.b * 255.0f));
    }
    media_atlas[uint2(destination_x + x, destination_y + y)] = float4(float3(rgb) / 255.0f, 1.0f);
}
