#include "jpeg_internal.hpp"
#include "maicord/core/byte_order.hpp"

// Markers, tables and scan setup run once per image, around the entropy and
// sample loops of jpeg_entropy.cpp and jpeg_reconstruct.cpp: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace jpeg_detail {

u16 read_tiff16(const u8* bytes, bool big_endian) noexcept {
    return big_endian ? byte_load_be16(bytes) : byte_load_le16(bytes);
}

u32 read_tiff32(const u8* bytes, bool big_endian) noexcept {
    return big_endian ? byte_load_be32(bytes) : byte_load_le32(bytes);
}

JpegParseResult parse_failure(JpegParseResult result, JpegError error) noexcept {
    result.error = error;
    return result;
}

bool read_marker(ConstSlice<u8> encoded, u32* cursor, u8* marker) noexcept {
    if (*cursor >= encoded.count || encoded[*cursor] != 0xFF) {
        return false;
    }
    u32 offset = *cursor;
    while (offset < encoded.count && encoded[offset] == 0xFF) {
        ++offset;
    }
    if (offset >= encoded.count || encoded[offset] == 0) {
        return false;
    }
    *marker = encoded[offset++];
    *cursor = offset;
    return true;
}

bool segment_bounds(ConstSlice<u8> encoded, u32* cursor, u32* payload, u32* payload_bytes) noexcept {
    if (*cursor + 2u > encoded.count) {
        return false;
    }
    const u32 length = byte_load_be16(encoded.data + *cursor);
    if (length < 2 || static_cast<u64>(*cursor) + length > encoded.count) {
        return false;
    }
    *payload = *cursor + 2u;
    *payload_bytes = length - 2u;
    *cursor += length;
    return true;
}

bool parse_exif_orientation(const u8* payload, u32 bytes, JpegOrientation* orientation) noexcept {
    // The sixth identifier byte is a fill byte and is not checked, as in Chromium.
    if (bytes < 14 || !bytes_equal(payload, "Exif\0", 5)) {
        return false;
    }
    const u8* tiff = payload + 6;
    const u32 tiff_bytes = bytes - 6;
    const bool little_endian = tiff[0] == 'I' && tiff[1] == 'I';
    const bool big_endian = tiff[0] == 'M' && tiff[1] == 'M';
    if ((!little_endian && !big_endian) || read_tiff16(tiff + 2, big_endian) != 42) {
        return false;
    }
    const u32 directory_offset = read_tiff32(tiff + 4, big_endian);
    if (directory_offset > tiff_bytes - 2u) {
        return false;
    }
    const u16 entries = read_tiff16(tiff + directory_offset, big_endian);
    u32 cursor = directory_offset + 2u;
    const u32 available_entries = (tiff_bytes - cursor) / 12u;
    const u32 checked_entries = minimum<u32>(entries, available_entries);
    for (u32 index = 0; index < checked_entries; ++index) {
        const u8* entry = tiff + cursor;
        const u16 tag = read_tiff16(entry, big_endian);
        const u16 type = read_tiff16(entry + 2, big_endian);
        const u32 count = read_tiff32(entry + 4, big_endian);
        if (tag == 0x0112 && type == 3 && count == 1) {
            const u16 value = read_tiff16(entry + 8, big_endian);
            if (value >= static_cast<u16>(JpegOrientation::top_left) &&
                value <= static_cast<u16>(JpegOrientation::left_bottom)) {
                *orientation = static_cast<JpegOrientation>(value);
                return true;
            }
            return false;
        }
        cursor += 12u;
    }
    return false;
}

bool build_huffman_table(JpegHuffmanTable* table, const u8* counts, const u8* symbols,
                         u32 symbol_count) noexcept {
    reset_value<JpegHuffmanTable>(table);
    i32 code = 0;
    u32 symbol_offset = 0;
    for (u32 length = 0; length < 16; ++length) {
        const u32 count = counts[length];
        const u32 code_limit = 1u << (length + 1u);
        if (static_cast<u32>(code) + count > code_limit ||
            (count != 0 && static_cast<u32>(code) + count == code_limit)) {
            return false;
        }
        if (count != 0) {
            table->minimum_code[length] = static_cast<u16>(code);
            table->maximum_code[length] = static_cast<u16>(code + count - 1);
            table->value_offset[length] = static_cast<i32>(symbol_offset) - code;
        } else {
            table->minimum_code[length] = 0xffff;
            table->maximum_code[length] = 0;
        }
        code = (code + static_cast<i32>(count)) << 1;
        symbol_offset += count;
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 index = 0; index < symbol_count; ++index) {
        table->symbols[index] = symbols[index];
    }
    table->symbol_count = static_cast<u16>(symbol_count);
    table->present = true;
    return true;
}

JpegError parse_quantization(ConstSlice<u8> encoded, u32 payload, u32 bytes, u8 locked_tables,
                             JpegTables* tables) noexcept {
    u32 cursor = payload;
    const u32 end = payload + bytes;
    while (cursor < end) {
        const u8 information = encoded[cursor++];
        const u8 precision = information >> 4;
        const u8 identifier = information & 0x0F;
        if (precision != 0 || identifier >= kJpegTableMaximum ||
            (locked_tables & (1u << identifier)) != 0 || end - cursor < 64) {
            return JpegError::quantization;
        }
        JpegQuantizationTable& table = tables->quantization[identifier];
        reset_value<JpegQuantizationTable>(&table);
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (u32 index = 0; index < 64; ++index) {
            const u8 value = encoded[cursor++];
            if (value == 0) {
                return JpegError::quantization;
            }
            table.values[kZigzagToRaster[index]] = value;
        }
        table.present = true;
    }
    return cursor == end ? JpegError::none : JpegError::quantization;
}

JpegError parse_huffman(ConstSlice<u8> encoded, u32 payload, u32 bytes, JpegTables* tables) noexcept {
    u32 cursor = payload;
    const u32 end = payload + bytes;
    while (cursor < end) {
        if (end - cursor < 17) {
            return JpegError::huffman;
        }
        const u8 information = encoded[cursor++];
        const u8 table_class = information >> 4;
        const u8 identifier = information & 0x0F;
        if (table_class > 1 || identifier >= kJpegTableMaximum) {
            return JpegError::huffman;
        }
        JpegHuffmanTable* table = table_class == 0 ? &tables->dc[identifier] : &tables->ac[identifier];
        const u8* counts = encoded.data + cursor;
        u32 symbol_count = 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (u32 index = 0; index < 16; ++index) {
            symbol_count += counts[index];
        }
        cursor += 16;
        if (symbol_count == 0 || symbol_count > 256 || end - cursor < symbol_count) {
            return JpegError::huffman;
        }
        for (u32 index = 0; index < symbol_count; ++index) {
            const u8 symbol = encoded[cursor + index];
            const u8 magnitude = symbol & 0x0F;
            if ((table_class == 0 && symbol > 11) || (table_class == 1 && magnitude > 10)) {
                return JpegError::huffman;
            }
        }
        if (!build_huffman_table(table, counts, encoded.data + cursor, symbol_count)) {
            return JpegError::huffman;
        }
        cursor += symbol_count;
    }
    return cursor == end ? JpegError::none : JpegError::huffman;
}

JpegError parse_frame(ConstSlice<u8> encoded, u32 payload, u32 bytes, JpegImage* image,
                      bool thumbnail) noexcept {
    if (bytes < 9 || image->component_count != 0) {
        return image->component_count != 0 ? JpegError::duplicate : JpegError::segment;
    }
    const u8 precision = encoded[payload];
    const u32 height = byte_load_be16(encoded.data + payload + 1);
    const u32 width = byte_load_be16(encoded.data + payload + 3);
    const u8 components = encoded[payload + 5];
    if (precision != 8) {
        return JpegError::unsupported_precision;
    }
    if (components != 1 && components != 3 && components != 4) {
        return JpegError::unsupported_components;
    }
    if (bytes != 6u + static_cast<u32>(components) * 3u) {
        return JpegError::segment;
    }
    const u32 dimension_limit = thumbnail ? kJpegThumbnailDimensionMaximum : kJpegDimensionMaximum;
    if (width == 0 || height == 0 || width > dimension_limit || height > dimension_limit) {
        return JpegError::dimensions;
    }

    u8 maximum_h = 0;
    u8 maximum_v = 0;
    for (u32 index = 0; index < components; ++index) {
        const u32 offset = payload + 6u + index * 3u;
        const u8 identifier = encoded[offset];
        const u8 sampling = encoded[offset + 1];
        const u8 horizontal = sampling >> 4;
        const u8 vertical = sampling & 0x0F;
        const u8 quantization = encoded[offset + 2];
        if (horizontal == 0 || horizontal > 2 || vertical == 0 || vertical > 2 ||
            quantization >= kJpegTableMaximum) {
            return JpegError::unsupported_sampling;
        }
        for (u32 prior = 0; prior < index; ++prior) {
            if (image->components[prior].identifier == identifier) {
                return JpegError::duplicate;
            }
        }
        JpegComponent& component = image->components[index];
        component.identifier = identifier;
        component.horizontal_sampling = horizontal;
        component.vertical_sampling = vertical;
        component.quantization_table = quantization;
        maximum_h = maximum(maximum_h, horizontal);
        maximum_v = maximum(maximum_v, vertical);
    }
    if (components == 1) {
        // A single-component scan is non-interleaved: its MCU is one block
        // whatever factors the frame header declares (T.81 A.2.2). Some
        // encoders write 2x2 for grayscale; treat it as 1x1.
        image->components[0].horizontal_sampling = image->components[0].vertical_sampling = 1;
        maximum_h = maximum_v = 1;
    }

    const u32 mcu_width = 8u * maximum_h;
    const u32 mcu_height = 8u * maximum_v;
    const u32 mcu_columns = (width + mcu_width - 1u) / mcu_width;
    const u32 mcu_rows = (height + mcu_height - 1u) / mcu_height;
    u32 block_offset = 0;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 index = 0; index < components; ++index) {
        JpegComponent& component = image->components[index];
        const u32 columns = mcu_columns * component.horizontal_sampling;
        const u32 rows = mcu_rows * component.vertical_sampling;
        const u32 count = columns * rows;
        if (!thumbnail && static_cast<u64>(block_offset) + count > kJpegCoefficientBlockMaximum) {
            return JpegError::dimensions;
        }
        component.block_offset = block_offset;
        component.block_count = count;
        component.block_columns = static_cast<u16>(columns);
        component.block_rows = static_cast<u16>(rows);
        block_offset += count;
    }

    image->width = static_cast<u16>(width);
    image->height = static_cast<u16>(height);
    image->mcu_columns = static_cast<u16>(mcu_columns);
    image->mcu_rows = static_cast<u16>(mcu_rows);
    image->coefficient_blocks = block_offset;
    image->component_count = components;
    image->maximum_horizontal_sampling = maximum_h;
    image->maximum_vertical_sampling = maximum_v;
    return JpegError::none;
}

u32 scan_component_columns(const JpegImage& image, const JpegComponent& component) noexcept {
    const u64 numerator = static_cast<u64>(image.width) * component.horizontal_sampling;
    const u32 denominator = 8u * image.maximum_horizontal_sampling;
    return static_cast<u32>((numerator + denominator - 1u) / denominator);
}

u32 scan_component_rows(const JpegImage& image, const JpegComponent& component) noexcept {
    const u64 numerator = static_cast<u64>(image.height) * component.vertical_sampling;
    const u32 denominator = 8u * image.maximum_vertical_sampling;
    return static_cast<u32>((numerator + denominator - 1u) / denominator);
}

JpegError parse_scan_header(ConstSlice<u8> encoded, u32 payload, u32 bytes, const JpegImage& image,
                            const JpegTables& tables, u16 restart_interval, JpegScan* scan) noexcept {
    if (image.component_count == 0 || bytes < 6) {
        return JpegError::order;
    }
    reset_value<JpegScan>(scan);
    const u8 scan_components = encoded[payload];
    if (scan_components == 0 || scan_components > image.component_count ||
        bytes != 1u + static_cast<u32>(scan_components) * 2u + 3u ||
        (!image.progressive && scan_components != image.component_count)) {
        return JpegError::unsupported_process;
    }
    u32 sampling_sum = 0;
    for (u32 scan_index = 0; scan_index < scan_components; ++scan_index) {
        const u8 identifier = encoded[payload + 1u + scan_index * 2u];
        const u8 selectors = encoded[payload + 2u + scan_index * 2u];
        const u8 dc = selectors >> 4;
        const u8 ac = selectors & 0x0F;
        if (dc >= kJpegTableMaximum || ac >= kJpegTableMaximum) {
            return JpegError::huffman;
        }
        u32 component_index = image.component_count;
        for (u32 index = 0; index < image.component_count; ++index) {
            if (image.components[index].identifier == identifier) {
                component_index = index;
                break;
            }
        }
        if (component_index == image.component_count) {
            return JpegError::scan;
        }
        for (u32 prior = 0; prior < scan_index; ++prior) {
            if (scan->component_indices[prior] == component_index) {
                return JpegError::scan;
            }
        }
        const JpegComponent& component = image.components[component_index];
        if (!tables.quantization[component.quantization_table].present) {
            return JpegError::order;
        }
        sampling_sum += static_cast<u32>(component.horizontal_sampling) * component.vertical_sampling;
        scan->component_indices[scan_index] = static_cast<u8>(component_index);
        scan->dc_tables[scan_index] = dc;
        scan->ac_tables[scan_index] = ac;
    }
    if (scan_components > 1 && sampling_sum > 10) {
        return JpegError::unsupported_sampling;
    }
    const u32 parameters = payload + 1u + scan_components * 2u;
    const u8 spectral_start = encoded[parameters];
    const u8 spectral_end = encoded[parameters + 1];
    const u8 approximation = encoded[parameters + 2];
    const u8 approximation_high = approximation >> 4;
    const u8 approximation_low = approximation & 0x0F;
    if (image.progressive) {
        if (spectral_start > spectral_end || spectral_end >= 64 ||
            (spectral_start == 0 && spectral_end != 0) || (spectral_start != 0 && scan_components != 1) ||
            approximation_high > 13 || approximation_low > 13 ||
            (approximation_high != 0 && approximation_high != approximation_low + 1u)) {
            return JpegError::scan;
        }
    } else if (spectral_start != 0 || spectral_end != 63 || approximation != 0) {
        return JpegError::unsupported_process;
    }
    for (u32 index = 0; index < scan_components; ++index) {
        if (spectral_start == 0 && approximation_high == 0 && !tables.dc[scan->dc_tables[index]].present) {
            return JpegError::order;
        }
        if (spectral_start != 0 && !tables.ac[scan->ac_tables[index]].present) {
            return JpegError::order;
        }
        if (!image.progressive &&
            (!tables.dc[scan->dc_tables[index]].present || !tables.ac[scan->ac_tables[index]].present)) {
            return JpegError::order;
        }
    }
    scan->component_count = scan_components;
    scan->spectral_start = spectral_start;
    scan->spectral_end = spectral_end;
    scan->approximation_high = approximation_high;
    scan->approximation_low = approximation_low;
    scan->restart_interval = restart_interval;
    if (scan_components > 1) {
        scan->data_units = static_cast<u32>(image.mcu_columns) * image.mcu_rows;
    } else {
        const JpegComponent& component = image.components[scan->component_indices[0]];
        scan->data_units = scan_component_columns(image, component) * scan_component_rows(image, component);
    }
    return scan->data_units != 0 ? JpegError::none : JpegError::scan;
}

JpegParseResult parse_jpeg(ConstSlice<u8> encoded, JpegImage* image, JpegWorkspace* workspace,
                           bool thumbnail) noexcept {
    JpegParseResult result{};
    if (!image || !workspace || (!encoded.data && encoded.count != 0)) {
        return parse_failure(result, JpegError::invalid_input);
    }
    reset_value<JpegImage>(image);
    image->orientation = JpegOrientation::top_left;
    reset_value<JpegTables>(&workspace->tables);
    reset_value<JpegIccProfile>(&workspace->icc.profile);
    workspace->icc.profile.entries = workspace->icc.entries;
    workspace->tables.icc = &workspace->icc;
    image->tables = &workspace->tables;
    if (encoded.count > (thumbnail ? kJpegThumbnailEncodedMaximumBytes : kJpegEncodedMaximumBytes)) {
        return parse_failure(result, JpegError::encoded_limit);
    }
    if (encoded.count < 4 || encoded[0] != 0xFF || encoded[1] != 0xD8) {
        return parse_failure(result, JpegError::signature);
    }

    bool adobe_transform_valid = false;
    bool orientation_found = false;
    bool jfif_prefix_exif_only = true;
    bool icc_marker_seen = false;
    bool icc_transport_malformed = false;
    u8 adobe_transform = 0;
    u32 icc_declared_chunks = 0;
    u32 icc_seen_chunks = 0;
    u8 quantization_locked = 0;
    u16 restart_interval = 0;
    JpegIccChunk icc_chunks[kJpegIccChunkMaximum];
    u32 icc_chunk_presence[8]{};
    u8 progression[kJpegComponentMaximum][64];
    memset(progression, 0xFF, sizeof(progression));
    u32 cursor = 2;
    while (cursor < encoded.count) {
        const bool first_segment = result.segments == 0;
        u8 marker = 0;
        if (!read_marker(encoded, &cursor, &marker)) {
            return parse_failure(result, JpegError::marker);
        }
        if (marker == 0xD9) {
            if (result.scans == 0 || cursor != encoded.count) {
                return parse_failure(result,
                                     result.scans == 0 ? JpegError::order : JpegError::trailing_data);
            }
            if (image->progressive) {
                for (u32 component = 0; component < image->component_count; ++component) {
                    if (progression[component][0] == 0xFF) {
                        return parse_failure(result, JpegError::order);
                    }
                }
            }
            if (image->component_count == 1) {
                image->color_transform = JpegColorTransform::grayscale;
            } else if (image->component_count == 3 && adobe_transform_valid) {
                if (adobe_transform > 1) {
                    return parse_failure(result, JpegError::unsupported_color);
                }
                image->color_transform =
                    adobe_transform == 0 ? JpegColorTransform::rgb : JpegColorTransform::ycbcr;
            } else if (image->component_count == 3) {
                image->color_transform = JpegColorTransform::ycbcr;
            } else if (adobe_transform_valid) {
                if (adobe_transform != 0 && adobe_transform != 2) {
                    return parse_failure(result, JpegError::unsupported_color);
                }
                image->color_transform =
                    adobe_transform == 0 ? JpegColorTransform::cmyk : JpegColorTransform::ycck;
            } else {
                image->color_transform = JpegColorTransform::cmyk;
            }
            if (image->has_jfif && image->component_count == 4) {
                return parse_failure(result, JpegError::unsupported_color);
            }
            if (image->has_jfif && image->component_count == 3 &&
                (image->components[0].identifier != 1 || image->components[1].identifier != 2 ||
                 image->components[2].identifier != 3)) {
                return parse_failure(result, JpegError::unsupported_color);
            }
            if (image->color_transform == JpegColorTransform::rgb) {
                for (u32 index = 0; index < 3; ++index) {
                    if (image->components[index].horizontal_sampling != 1 ||
                        image->components[index].vertical_sampling != 1) {
                        return parse_failure(result, JpegError::unsupported_sampling);
                    }
                }
            }
            image->scan_count = static_cast<u16>(result.scans);
            image->restart_markers = result.restart_markers;
            finalize_icc_profile(encoded, icc_chunks, icc_seen_chunks, icc_declared_chunks, icc_marker_seen,
                                 icc_transport_malformed, *image, &workspace->icc);
            result.error = JpegError::none;
            return result;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            return parse_failure(result, JpegError::order);
        }
        u32 payload = 0;
        u32 payload_bytes = 0;
        if (!segment_bounds(encoded, &cursor, &payload, &payload_bytes)) {
            return parse_failure(result, JpegError::truncated);
        }
        ++result.segments;

        const bool exif_segment =
            marker == 0xE1 && payload_bytes >= 5 && bytes_equal(encoded.data + payload, "Exif\0", 5);
        const bool icc_segment = marker == 0xE2 && payload_bytes >= 12 &&
                                 bytes_equal(encoded.data + payload, "ICC_PROFILE\0", 12);
        JpegError error = JpegError::none;
        if (marker == 0xE0 && payload_bytes >= 5 && bytes_equal(encoded.data + payload, "JFIF\0", 5)) {
            if ((!first_segment && !jfif_prefix_exif_only) || image->has_jfif || payload_bytes < 14) {
                return parse_failure(result, JpegError::order);
            }
            const u8 major = encoded[payload + 5];
            const u8 units = encoded[payload + 7];
            const u16 density_x = byte_load_be16(encoded.data + payload + 8);
            const u16 density_y = byte_load_be16(encoded.data + payload + 10);
            const u32 thumbnail_x = encoded[payload + 12];
            const u32 thumbnail_y = encoded[payload + 13];
            const u64 expected = 14ull + 3ull * thumbnail_x * thumbnail_y;
            if (major != 1 || units > 2 || density_x == 0 || density_y == 0 || expected != payload_bytes) {
                return parse_failure(result, JpegError::segment);
            }
            image->has_jfif = true;
        } else if (marker == 0xEE && payload_bytes >= 5 &&
                   bytes_equal(encoded.data + payload, "Adobe", 5)) {
            if (image->has_adobe || payload_bytes < 12) {
                return parse_failure(result, JpegError::duplicate);
            }
            image->has_adobe = true;
            adobe_transform = encoded[payload + 11];
            adobe_transform_valid = true;
        } else if (exif_segment && !orientation_found) {
            JpegOrientation orientation = JpegOrientation::top_left;
            if (parse_exif_orientation(encoded.data + payload, payload_bytes, &orientation)) {
                image->orientation = orientation;
                orientation_found = true;
            }
        } else if (icc_segment) {
            icc_marker_seen = true;
            if (payload_bytes < 14) {
                icc_transport_malformed = true;
            } else {
                const u32 sequence = encoded[payload + 12u];
                const u32 total = encoded[payload + 13u];
                if (sequence == 0 || total == 0 || sequence > total ||
                    (icc_declared_chunks != 0 && icc_declared_chunks != total)) {
                    icc_transport_malformed = true;
                } else {
                    if (icc_declared_chunks == 0) {
                        icc_declared_chunks = total;
                    }
                    const u32 chunk_index = sequence - 1u;
                    const u32 presence_word = chunk_index >> 5u;
                    const u32 presence_mask = 1u << (chunk_index & 31u);
                    if ((icc_chunk_presence[presence_word] & presence_mask) != 0) {
                        icc_transport_malformed = true;
                    } else {
                        icc_chunk_presence[presence_word] |= presence_mask;
                        JpegIccChunk& chunk = icc_chunks[chunk_index];
                        chunk.encoded_offset = payload + 14u;
                        chunk.bytes = payload_bytes - 14u;
                        ++icc_seen_chunks;
                    }
                }
            }
        } else if (marker == 0xDB) {
            error = parse_quantization(encoded, payload, payload_bytes, quantization_locked, image->tables);
        } else if (marker == 0xC4) {
            error = parse_huffman(encoded, payload, payload_bytes, image->tables);
        } else if (marker == 0xC0 || marker == 0xC2) {
            error = parse_frame(encoded, payload, payload_bytes, image, thumbnail);
            if (error == JpegError::none) {
                image->progressive = marker == 0xC2;
            }
        } else if (marker >= 0xC1 && marker <= 0xCF) {
            return parse_failure(result, JpegError::unsupported_process);
        } else if (marker == 0xDD) {
            if (payload_bytes != 2) {
                return parse_failure(result, JpegError::segment);
            }
            restart_interval = byte_load_be16(encoded.data + payload);
        } else if (marker == 0xDA) {
            if (result.scans >= kJpegProgressiveScanMaximum || (!image->progressive && result.scans != 0)) {
                return parse_failure(result, JpegError::unsupported_process);
            }
            JpegScan scan{};
            error = parse_scan_header(encoded, payload, payload_bytes, *image, *image->tables,
                                      restart_interval, &scan);
            if (error != JpegError::none) {
                return parse_failure(result, error);
            }
            if (image->progressive) {
                error = validate_progression(scan, progression);
                if (error != JpegError::none) {
                    return parse_failure(result, error);
                }
            }
            u32 next_marker = 0;
            error = find_entropy_end(encoded, cursor, &scan, &next_marker);
            if (error != JpegError::none) {
                return parse_failure(result, error);
            }
            const u32 expected_restarts =
                scan.restart_interval == 0 ? 0 : (scan.data_units - 1u) / scan.restart_interval;
            if (scan.restart_markers != expected_restarts) {
                return parse_failure(result, JpegError::restart);
            }
            if (result.scans == 0) {
                image->entropy_offset = scan.entropy_offset;
                image->entropy_bytes = scan.entropy_bytes;
            }
            for (u32 index = 0; index < scan.component_count; ++index) {
                const u32 component_index = scan.component_indices[index];
                JpegComponent& component = image->components[component_index];
                component.dc_table = scan.dc_tables[index];
                component.ac_table = scan.ac_tables[index];
                image->scan_order[index] = static_cast<u8>(component_index);
                quantization_locked |= static_cast<u8>(1u << component.quantization_table);
            }
            image->restart_interval = scan.restart_interval;
            result.restart_markers += scan.restart_markers;
            ++result.scans;
            cursor = next_marker;
            continue;
        }
        if (error != JpegError::none) {
            return parse_failure(result, error);
        }
        if (!image->has_jfif && !exif_segment) {
            jfif_prefix_exif_only = false;
        }
    }
    return parse_failure(result, JpegError::truncated);
}

// Re-walks the markers of a progressive file and decodes every scan into blocks, which
// holds image.coefficient_blocks entries.
JpegDecodeResult decode_progressive_scans(ConstSlice<u8> encoded, const JpegImage& image,
                                          JpegCoefficientBlock* blocks) noexcept {
    JpegDecodeResult result{};
    if (image.scan_count == 0 || encoded.count < 4 || encoded[0] != 0xFF || encoded[1] != 0xD8) {
        result.error = JpegError::invalid_input;
        return result;
    }
    memset(blocks, 0, static_cast<usize>(image.coefficient_blocks) * sizeof(*blocks));
    JpegTables tables{};
    u16 restart_interval = 0;
    u8 quantization_locked = 0;
    u8 progression[kJpegComponentMaximum][64];
    memset(progression, 0xFF, sizeof(progression));
    u32 cursor = 2;
    u32 scans = 0;
    while (cursor < encoded.count) {
        u8 marker = 0;
        if (!read_marker(encoded, &cursor, &marker)) {
            result.error = JpegError::marker;
            return result;
        }
        if (marker == 0xD9) {
            if (cursor != encoded.count || scans != image.scan_count ||
                result.decoded_scans != image.scan_count) {
                result.error = cursor != encoded.count ? JpegError::trailing_data : JpegError::scan;
                return result;
            }
            result.error = JpegError::none;
            return result;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            result.error = JpegError::order;
            return result;
        }
        u32 payload = 0;
        u32 payload_bytes = 0;
        if (!segment_bounds(encoded, &cursor, &payload, &payload_bytes)) {
            result.error = JpegError::truncated;
            return result;
        }
        JpegError error = JpegError::none;
        if (marker == 0xDB) {
            error = parse_quantization(encoded, payload, payload_bytes, quantization_locked, &tables);
        } else if (marker == 0xC4) {
            error = parse_huffman(encoded, payload, payload_bytes, &tables);
        } else if (marker == 0xDD) {
            if (payload_bytes != 2) {
                result.error = JpegError::segment;
                return result;
            }
            restart_interval = byte_load_be16(encoded.data + payload);
        } else if (marker == 0xDA) {
            JpegScan scan{};
            error = parse_scan_header(encoded, payload, payload_bytes, image, tables, restart_interval,
                                      &scan);
            if (error != JpegError::none) {
                result.error = error;
                return result;
            }
            error = validate_progression(scan, progression);
            if (error != JpegError::none) {
                result.error = error;
                return result;
            }
            u32 next_marker = 0;
            error = find_entropy_end(encoded, cursor, &scan, &next_marker);
            if (error != JpegError::none ||
                !decode_progressive_scan(encoded, image, tables, scan, blocks, &result)) {
                if (result.error == JpegError::none) {
                    result.error = error == JpegError::none ? JpegError::coefficient : error;
                }
                return result;
            }
            for (u32 index = 0; index < scan.component_count; ++index) {
                const JpegComponent& component = image.components[scan.component_indices[index]];
                quantization_locked |= static_cast<u8>(1u << component.quantization_table);
            }
            ++scans;
            cursor = next_marker;
        }
        if (error != JpegError::none) {
            result.error = error;
            return result;
        }
    }
    result.error = JpegError::truncated;
    return result;
}

} // namespace jpeg_detail
using namespace jpeg_detail;

JpegParseResult jpeg_parse(ConstSlice<u8> encoded, JpegImage* image, JpegWorkspace* workspace) noexcept {
    return parse_jpeg(encoded, image, workspace, false);
}

JpegDecodeResult jpeg_decode_parsed_coefficients(ConstSlice<u8> encoded, const JpegImage& image,
                                                 JpegCoefficientWorkspace* coefficients) noexcept {
    JpegDecodeResult result{};
    if (!coefficients || !image.tables || (!encoded.data && encoded.count != 0) || image.width == 0 ||
        image.height == 0 || image.component_count == 0 || image.component_count > kJpegComponentMaximum ||
        !jpeg_orientation_valid(image.orientation) ||
        image.coefficient_blocks > kJpegCoefficientBlockMaximum ||
        static_cast<u64>(image.entropy_offset) + image.entropy_bytes > encoded.count) {
        result.error = JpegError::invalid_input;
        return result;
    }

    if (image.progressive) {
        return decode_progressive_scans(encoded, image, coefficients->blocks);
    }

    JpegEntropyReader reader = entropy_reader(ConstSlice<u8>{encoded.data + image.entropy_offset, image.entropy_bytes});
    JpegDecodeTable dc_decode[kJpegTableMaximum];
    JpegAcDecodeTable ac_decode[kJpegTableMaximum];
    build_component_tables(image, dc_decode, ac_decode);
    i32 predictor[kJpegComponentMaximum]{};
    const u32 total_mcus = static_cast<u32>(image.mcu_columns) * image.mcu_rows;
    u8 expected_restart = 0;
    u32 mcu_x = 0, mcu_y = 0;
    for (u32 mcu = 0; mcu < total_mcus; ++mcu) {
        if (image.restart_interval != 0 && mcu != 0 && mcu % image.restart_interval == 0) {
            if (!entropy_consume_restart(&reader, expected_restart)) {
                result.error = reader.error;
                return result;
            }
            expected_restart = static_cast<u8>((expected_restart + 1) & 7);
            for (u32 index = 0; index < image.component_count; ++index) {
                predictor[index] = 0;
            }
        }
        // mcu_x/mcu_y step with the MCU index instead of dividing it.
        for (u32 scan = 0; scan < image.component_count; ++scan) {
            const u32 component_index = image.scan_order[scan];
            const JpegComponent& component = image.components[component_index];
            const JpegDecodeTable& dc = dc_decode[component.dc_table];
            const JpegAcDecodeTable& ac = ac_decode[component.ac_table];
            for (u32 vertical = 0; vertical < component.vertical_sampling; ++vertical) {
                for (u32 horizontal = 0; horizontal < component.horizontal_sampling; ++horizontal) {
                    const u32 block_x = mcu_x * component.horizontal_sampling + horizontal;
                    const u32 block_y = mcu_y * component.vertical_sampling + vertical;
                    const u32 block_index =
                        component.block_offset + block_y * component.block_columns + block_x;
                    if (block_index >= image.coefficient_blocks ||
                        !decode_block(&reader, dc, ac, &predictor[component_index],
                                      &coefficients->blocks[block_index])) {
                        result.error =
                            reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                        return result;
                    }
                    ++result.decoded_blocks;
                }
            }
        }
        if (++mcu_x == image.mcu_columns) {
            mcu_x = 0;
            ++mcu_y;
        }
    }
    if (!entropy_at_end(&reader) || result.decoded_blocks != image.coefficient_blocks) {
        result.error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
        return result;
    }
    result.error = JpegError::none;
    result.decoded_scans = 1;
    return result;
}

JpegDecodeResult jpeg_decode_coefficients(ConstSlice<u8> encoded, JpegImage* image,
                                          JpegWorkspace* workspace) noexcept {
    JpegDecodeResult result{};
    result.parse = jpeg_parse(encoded, image, workspace);
    if (!result.parse.ok()) {
        result.error = result.parse.error;
        return result;
    }
    const JpegParseResult parse = result.parse;
    result = jpeg_decode_parsed_coefficients(encoded, *image, &workspace->coefficients);
    result.parse = parse;
    return result;
}

} // namespace maicord
#pragma clang attribute pop
