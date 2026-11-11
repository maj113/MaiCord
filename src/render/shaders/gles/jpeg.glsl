// twin: src/render/shaders/jpeg.hlsl f98b8be4ecd7074e
// JPEG decode. ENTRY_jpeg_idct (STAGE_CS): the dequantizing 8x8 inverse DCT of every coefficient block.
// ENTRY_jpeg_to_atlas (STAGE_CS): upsampling, colour conversion, ICC transform and orientation into the media atlas.
layout(local_size_x = 8, local_size_y = 8) in;

layout(std430, binding = 1) readonly buffer Auxiliary { uint jpeg_auxiliary[]; }; //! t1

uint load_u16(uint address) {
    return (jpeg_auxiliary[address >> 2u] >> ((address & 2u) * 8u)) & 65535u;
}

#if defined(ENTRY_jpeg_idct)

layout(std140, binding = 0) uniform JpegIdctConstants {
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
};

layout(std430, binding = 0) readonly buffer Coefficients { uint jpeg_coefficients[]; }; //! t0
layout(std430, binding = 2) buffer Samples { uint jpeg_samples[]; }; //! u0

const float idct_basis[64] = float[64](
    0.353553391, 0.353553391, 0.353553391, 0.353553391, 0.353553391, 0.353553391, 0.353553391, 0.353553391,
    0.490392640, 0.415734806, 0.277785117, 0.097545161, -0.097545161, -0.277785117, -0.415734806, -0.490392640,
    0.461939766, 0.191341716, -0.191341716, -0.461939766, -0.461939766, -0.191341716, 0.191341716, 0.461939766,
    0.415734806, -0.097545161, -0.490392640, -0.277785117, 0.277785117, 0.490392640, 0.097545161, -0.415734806,
    0.353553391, -0.353553391, -0.353553391, 0.353553391, 0.353553391, -0.353553391, -0.353553391, 0.353553391,
    0.277785117, -0.490392640, 0.097545161, 0.415734806, -0.415734806, -0.097545161, 0.490392640, -0.277785117,
    0.191341716, -0.461939766, 0.461939766, -0.191341716, -0.191341716, 0.461939766, -0.461939766, 0.191341716,
    0.097545161, -0.277785117, 0.415734806, -0.490392640, 0.490392640, -0.415734806, 0.277785117, -0.097545161);

int load_i16(uint address) {
    uint value = (jpeg_coefficients[address >> 2u] >> ((address & 2u) * 8u)) & 65535u;
    return int(value << 16u) >> 16;
}

// The coefficients as the decoder stores them (jpeg.hpp, JpegCoefficientWorkspace): whole blocks in block order, or
// with JPEG_STORED_ROWS one word per block (bits 0-3: how many of its rows are stored, from the first; bits 8-31: the
// index of its first stored row), then the stored rows, 16 bytes each, from the first 16-byte boundary after the
// words; a row not stored is zero.
//
// Two cooperative 1-D passes: one dequantization load per coefficient. Group-wide barriers are uniform; no hardware
// wave-width assumption.
shared float idct_dequantized[64];
shared float idct_horizontal[64];

void main() {
    uint block = gl_WorkGroupID.x;
    if (block >= coefficient_block_count)
        return;

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

    uint component_block = block - block_offset;
    uint block_x = component_block % block_columns;
    uint block_y = component_block / block_columns;
    uint x = gl_LocalInvocationID.x;
    uint y = gl_LocalInvocationID.y;
    uint coefficient = y * 8u + x;
#if defined(JPEG_STORED_ROWS)
    uint header = jpeg_coefficients[block];
    uint row_base = (coefficient_block_count * 4u + 15u) & ~15u;
    int value = 0;
    if (y < (header & 15u))
        value = load_i16(row_base + ((header >> 8u) + y) * 16u + x * 2u);
#else
    int value = load_i16(block * 128u + coefficient * 2u);
#endif
    uint quantization = load_u16((quantization_table * 64u + coefficient) * 2u);
    idct_dequantized[coefficient] = float(value * int(quantization));
    memoryBarrierShared();
    barrier();

    float horizontal = 0.0;
    for (uint u = 0u; u < 8u; ++u)
        horizontal += idct_dequantized[y * 8u + u] * idct_basis[u * 8u + x];
    idct_horizontal[coefficient] = horizontal;
    memoryBarrierShared();
    barrier();

    float result = 0.0;
    for (uint v = 0u; v < 8u; ++v)
        result += idct_horizontal[v * 8u + x] * idct_basis[v * 8u + y];
    uint reconstructed = uint(clamp(floor(result + 128.5), 0.0, 255.0));
    jpeg_samples[sample_offset + (block_y * 8u + y) * sample_stride + block_x * 8u + x] = reconstructed;
}

#else

layout(std140, binding = 0) uniform JpegAtlasConstants {
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
    vec4 icc_matrix_row0;
    vec4 icc_matrix_row1;
    vec4 icc_matrix_row2;
    uvec4 icc_curve_layouts[7];
    vec4 icc_curve_parameters0[7];
    vec4 icc_curve_parameters1[7];
};

layout(std430, binding = 0) readonly buffer Components { uint reconstructed_components[]; }; //! t0
layout(rgba8, binding = 0) uniform writeonly highp image2D media_atlas; //! u0

float component_sample(uint offset, uint stride, uint width, uint height, uint horizontal_sampling,
                       uint vertical_sampling, uint x, uint y) {
    float source_x = (float(x) + 0.5) * float(horizontal_sampling) / float(maximum_horizontal_sampling) - 0.5;
    float source_y = (float(y) + 0.5) * float(vertical_sampling) / float(maximum_vertical_sampling) - 0.5;
    int left = int(floor(source_x));
    int top = int(floor(source_y));
    uint x0 = uint(clamp(left, 0, int(width) - 1));
    uint x1 = uint(clamp(left + 1, 0, int(width) - 1));
    uint y0 = uint(clamp(top, 0, int(height) - 1));
    uint y1 = uint(clamp(top + 1, 0, int(height) - 1));
    float fx = source_x - floor(source_x);
    float fy = source_y - floor(source_y);
    float top_value = mix(float(reconstructed_components[offset + y0 * stride + x0]),
                          float(reconstructed_components[offset + y0 * stride + x1]), fx);
    float bottom_value = mix(float(reconstructed_components[offset + y1 * stride + x0]),
                             float(reconstructed_components[offset + y1 * stride + x1]), fx);
    return mix(top_value, bottom_value, fy);
}

uvec3 rounded_bytes(vec3 value) {
    return uvec3(clamp(floor(value + 0.5), 0.0, 255.0));
}

float icc_curve(uint curve_index, float x) {
    uvec4 curve = icc_curve_layouts[curve_index];
    uint kind = curve.x;
    uint table_offset = curve.y;
    uint table_count = curve.z;
    vec4 parameters0 = icc_curve_parameters0[curve_index];
    vec4 parameters1 = icc_curve_parameters1[curve_index];
    x = clamp(x, 0.0, 1.0);
    if (kind == 0u)
        return x;
    if (kind == 1u) {
        float g = parameters0.x;
        float a = parameters0.y;
        float b = parameters0.z;
        float c = parameters0.w;
        float d = parameters1.x;
        float e = parameters1.y;
        float f = parameters1.z;
        float value = x < d ? c * x + f : pow(max(a * x + b, 0.0), g) + e;
        return clamp(value, 0.0, 1.0);
    }
    float position = x * float(table_count - 1u);
    uint lower = min(uint(floor(position)), table_count - 1u);
    uint upper = min(lower + 1u, table_count - 1u);
    float fraction = position - float(lower);
    float low = float(load_u16((table_offset + lower) * 2u));
    float high = float(load_u16((table_offset + upper) * 2u));
    return mix(low, high, fraction) * (1.0 / 65535.0);
}

vec3 icc_clut(vec4 device) {
    uvec4 grid = uvec4(icc_grid0, icc_grid1, icc_grid2, icc_grid3);
    vec4 position = vec4(0.0);
    position.x = clamp(device.x, 0.0, 1.0) * float(grid.x - 1u);
    if (icc_input_channels > 1u)
        position.y = clamp(device.y, 0.0, 1.0) * float(grid.y - 1u);
    if (icc_input_channels > 2u)
        position.z = clamp(device.z, 0.0, 1.0) * float(grid.z - 1u);
    if (icc_input_channels > 3u)
        position.w = clamp(device.w, 0.0, 1.0) * float(grid.w - 1u);
    uvec4 lower = uvec4(floor(position));
    vec4 fraction = position - vec4(lower);
    vec3 result = vec3(0.0);
    uint corner_count = 1u << icc_input_channels;
    for (uint corner = 0u; corner < 16u; ++corner) {
        if (corner >= corner_count)
            break;
        uvec4 coordinate = lower;
        float weight = 1.0;
        for (uint channel = 0u; channel < 4u; ++channel) {
            if (channel < icc_input_channels) {
                bool upper = (corner & (1u << channel)) != 0u;
                coordinate[channel] = min(lower[channel] + (upper ? 1u : 0u), grid[channel] - 1u);
                weight *= upper ? fraction[channel] : 1.0 - fraction[channel];
            }
        }
        uint node_index = coordinate.x;
        if (icc_input_channels > 1u)
            node_index = node_index * grid.y + coordinate.y;
        if (icc_input_channels > 2u)
            node_index = node_index * grid.z + coordinate.z;
        if (icc_input_channels > 3u)
            node_index = node_index * grid.w + coordinate.w;
        uint entry = icc_clut_offset + node_index * 3u;
        result += weight *
                  vec3(float(load_u16(entry * 2u)), float(load_u16((entry + 1u) * 2u)),
                       float(load_u16((entry + 2u) * 2u))) * (1.0 / 65535.0);
    }
    return result;
}

vec3 icc_lab_to_xyz(vec3 lab) {
    float lightness = lab.x * 100.0;
    float a = lab.y * 255.0 - 128.0;
    float b = lab.z * 255.0 - 128.0;
    float y = (lightness + 16.0) / 116.0;
    vec3 root = vec3(y + a / 500.0, y, y - b / 200.0);
    vec3 xyz;
    for (uint channel = 0u; channel < 3u; ++channel) {
        float cube = root[channel] * root[channel] * root[channel];
        xyz[channel] = cube > 0.008856 ? cube : (root[channel] - 16.0 / 116.0) / 7.787;
    }
    xyz.x *= 0.9642;
    xyz.z *= 0.8249;
    return xyz;
}

vec3 icc_connection_to_xyz(vec3 connection) {
    return icc_pcs == 1u ? icc_lab_to_xyz(connection) : connection;
}

vec3 srgb_encode(vec3 value) {
    value = clamp(value, 0.0, 1.0);
    return mix(pow(1.137283325 * value, vec3(0.416666657)) - 0.054969788, 12.920000076 * value,
               lessThan(value, vec3(0.003130805)));
}

vec3 icc_transform(vec4 device) {
    vec3 connection = vec3(0.0);
    if (icc_transform_kind == 1u) {
        connection = vec3(icc_curve(0u, device.x), icc_curve(1u, device.y), icc_curve(2u, device.z));
    } else if (icc_transform_kind == 2u) {
        float gray = icc_curve(0u, device.x);
        connection = icc_pcs == 1u ? vec3(gray, 128.0 / 255.0, 128.0 / 255.0) : gray * vec3(0.9642, 1.0, 0.8249);
        connection = icc_connection_to_xyz(connection);
    } else {
        vec4 curved = vec4(0.0);
        for (uint channel = 0u; channel < 4u; ++channel) {
            if (channel < icc_input_channels)
                curved[channel] = icc_curve(channel, device[channel]);
        }
        connection = icc_clut(curved);
        connection = vec3(icc_curve(4u, connection.x), icc_curve(5u, connection.y), icc_curve(6u, connection.z));
        if (icc_precision_bytes == 2u) {
            connection *= icc_pcs == 1u ? 65535.0 / 65280.0 : 65535.0 / 32768.0;
            if (icc_pcs == 1u)
                connection = clamp(connection, 0.0, 1.0);
        }
        connection = icc_connection_to_xyz(connection);
    }
    vec3 linear_srgb = vec3(dot(icc_matrix_row0.xyz, connection), dot(icc_matrix_row1.xyz, connection),
                            dot(icc_matrix_row2.xyz, connection));
    return srgb_encode(linear_srgb);
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    uint jpeg_color_transform = color_transform_orientation & 255u;
    uint orientation = (color_transform_orientation >> 8u) & 255u;
    bool swapped = orientation >= 5u;
    uint output_width = swapped ? image_height : image_width;
    uint output_height = swapped ? image_width : image_height;
    if (id.x >= output_width || id.y >= output_height)
        return;
    uint x = id.x;
    uint y = id.y;
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
    float first = component_sample(a0_sample_offset, a0_sample_stride, a0_sample_width, a0_sample_height,
                                   a0_horizontal_sampling, a0_vertical_sampling, source_x, source_y);
    uvec3 rgb = rounded_bytes(vec3(first));
    float second = 0.0;
    float third = 0.0;
    float fourth = 0.0;
    vec4 icc_device = vec4(float(rgb.r) * (1.0 / 255.0), 0.0, 0.0, 0.0);
    if (atlas_component_count >= 3u) {
        second = component_sample(a1_sample_offset, a1_sample_stride, a1_sample_width, a1_sample_height,
                                  a1_horizontal_sampling, a1_vertical_sampling, source_x, source_y);
        third = component_sample(a2_sample_offset, a2_sample_stride, a2_sample_width, a2_sample_height,
                                 a2_horizontal_sampling, a2_vertical_sampling, source_x, source_y);
        if (jpeg_color_transform == 2u || jpeg_color_transform == 3u) {
            rgb = rounded_bytes(vec3(first, second, third));
        } else {
            float cb = second - 128.0;
            float cr = third - 128.0;
            rgb = rounded_bytes(vec3(first + 1.402 * cr, first - 0.34414 * cb - 0.71414 * cr, first + 1.772 * cb));
        }
        icc_device = vec4(vec3(rgb) * (1.0 / 255.0), 0.0);
        if (atlas_component_count == 4u) {
            fourth = component_sample(a3_sample_offset, a3_sample_stride, a3_sample_width, a3_sample_height,
                                      a3_horizontal_sampling, a3_vertical_sampling, source_x, source_y);
            if (jpeg_color_transform == 3u) {
                icc_device = 1.0 - vec4(first, second, third, fourth) * (1.0 / 255.0);
                float black = fourth * (1.0 / 255.0);
                rgb = rounded_bytes(vec3(first, second, third) * black);
            } else {
                icc_device = vec4(vec3(rgb) * (1.0 / 255.0), 1.0 - fourth * (1.0 / 255.0));
                float black = fourth * (1.0 / 255.0);
                rgb = rounded_bytes((255.0 - vec3(rgb)) * black);
            }
        }
    }
    if (icc_transform_kind != 0u) {
        if (icc_transform_kind != 3u)
            icc_device = vec4(vec3(rgb) * (1.0 / 255.0), 0.0);
        vec3 encoded_srgb = icc_transform(icc_device);
        rgb = rounded_bytes(encoded_srgb * 255.0);
    }
    imageStore(media_atlas, ivec2(destination_x + x, destination_y + y), vec4(vec3(rgb) * (1.0 / 255.0), 1.0));
}

#endif
