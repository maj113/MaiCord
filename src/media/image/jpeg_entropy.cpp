#include "jpeg_internal.hpp"

namespace maicord {
namespace jpeg_detail {

JpegError validate_progression(const JpegScan& scan, u8 progression[kJpegComponentMaximum][64]) noexcept {
    for (u32 scan_index = 0; scan_index < scan.component_count; ++scan_index) {
        const u32 component = scan.component_indices[scan_index];
        if (scan.spectral_start != 0 && progression[component][0] == 0xFF) {
            return JpegError::order;
        }
        for (u32 coefficient = scan.spectral_start; coefficient <= scan.spectral_end; ++coefficient) {
            const u8 previous = progression[component][coefficient];
            if ((scan.approximation_high == 0 && previous != 0xFF) ||
                (scan.approximation_high != 0 && previous != scan.approximation_high)) {
                return JpegError::scan;
            }
        }
    }
    for (u32 scan_index = 0; scan_index < scan.component_count; ++scan_index) {
        const u32 component = scan.component_indices[scan_index];
        for (u32 coefficient = scan.spectral_start; coefficient <= scan.spectral_end; ++coefficient) {
            progression[component][coefficient] = scan.approximation_low;
        }
    }
    return JpegError::none;
}

JpegError find_entropy_end(ConstSlice<u8> encoded, u32 start, JpegScan* scan, u32* next_marker) noexcept {
    u32 cursor = start;
    u32 restart_markers = 0;
    const __m128i marker_byte = _mm_set1_epi8(static_cast<char>(0xFF));
    while (cursor < encoded.count) {
        if (encoded[cursor] != 0xFF) {
            // Skip entropy bytes sixteen at a time up to the next 0xFF.
            ++cursor;
            while (cursor + 16u <= encoded.count) {
                const u32 mask = static_cast<u32>(_mm_movemask_epi8(_mm_cmpeq_epi8(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(encoded.data + cursor)), marker_byte)));
                if (mask != 0) {
                    unsigned long first = 0;
                    _BitScanForward(&first, mask);
                    cursor += first;
                    break;
                }
                cursor += 16u;
            }
            continue;
        }
        const u32 marker_start = cursor;
        while (cursor < encoded.count && encoded[cursor] == 0xFF) {
            ++cursor;
        }
        if (cursor >= encoded.count) {
            return JpegError::truncated;
        }
        const u8 marker = encoded[cursor++];
        if (marker == 0) {
            continue;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            ++restart_markers;
            continue;
        }
        scan->entropy_offset = start;
        scan->entropy_bytes = marker_start - start;
        if (restart_markers > 65'535u) {
            return JpegError::restart;
        }
        scan->restart_markers = static_cast<u16>(restart_markers);
        *next_marker = marker_start;
        return JpegError::none;
    }
    return JpegError::truncated;
}

// Bit-exact with a bit-serial reader that loads one entropy byte only when it
// needs its first bit: refills run ahead, but a refill that reaches the end of
// the segment or a marker only records why (stop), and the error is raised
// when a decode actually needs a bit past that point.
JpegEntropyReader entropy_reader(ConstSlice<u8> bytes) noexcept {
    return JpegEntropyReader{bytes, 0, 0, 0, JpegError::none, JpegError::none};
}

void build_decode_table(const JpegHuffmanTable& table, JpegDecodeTable* decode,
                         u32* coefficients = nullptr) noexcept {
    decode->table = &table;
    memset(decode->lookahead, 0, sizeof(decode->lookahead));
    if (!table.present) {
        if (coefficients) {
            memset(coefficients, 0, (1u << kJpegLookaheadBits) * sizeof(*coefficients));
        }
        return;
    }
    for (u32 length = 0; length < kJpegLookaheadBits; ++length) {
        const u32 shift = kJpegLookaheadBits - 1u - length;
        for (i32 code = table.minimum_code[length]; code <= table.maximum_code[length]; ++code) {
            const u16 entry = static_cast<u16>((length + 1u) << 8 | table.symbols[table.value_offset[length] + code]);
            const u32 first = static_cast<u32>(code) << shift;
#pragma clang loop vectorize(disable) interleave(disable)
#pragma clang loop unroll(disable)
            for (u32 fill = 0; fill < (1u << shift); ++fill) {
                if (decode->lookahead[first + fill] == 0) {
                    decode->lookahead[first + fill] = entry;
                }
            }
        }
    }
    if (!coefficients) {
        return;
    }
    for (u32 index = 0; index < (1u << kJpegLookaheadBits); ++index) {
        const u32 entry = decode->lookahead[index];
        const u32 length = entry >> 8;
        const u32 size = entry & 0x0Fu;
        coefficients[index] = 0;
        if (entry == 0) {
            continue;
        }
        if ((entry & 0xFFu) == 0) {
            coefficients[index] = kJpegEndOfBlock | length;
            continue;
        }
        if (size == 0 || length + size > kJpegLookaheadBits) {
            continue;
        }
        const u32 raw = index >> (kJpegLookaheadBits - length - size) & ((1u << size) - 1u);
        const i32 value = raw < (1u << (size - 1u)) ? static_cast<i32>(raw) - static_cast<i32>((1u << size) - 1u)
                                                    : static_cast<i32>(raw);
        coefficients[index] =
            static_cast<u32>(value) << 16 | (entry >> 4 & 0x0Fu) << 5 | (length + size);
    }
}

// Lookahead tables for the Huffman tables a baseline scan's components use.
void build_component_tables(const JpegImage& image, JpegDecodeTable* dc, JpegAcDecodeTable* ac) noexcept {
    u32 built = 0;
    for (u32 index = 0; index < image.component_count && index < kJpegComponentMaximum; ++index) {
        const JpegComponent& component = image.components[index];
        if (component.dc_table < kJpegTableMaximum && (built & (1u << component.dc_table)) == 0) {
            build_decode_table(image.tables->dc[component.dc_table], &dc[component.dc_table]);
            built |= 1u << component.dc_table;
        }
        if (component.ac_table < kJpegTableMaximum && (built & (16u << component.ac_table)) == 0) {
            build_decode_table(image.tables->ac[component.ac_table], &ac[component.ac_table],
                               ac[component.ac_table].coefficient);
            built |= 16u << component.ac_table;
        }
    }
}

// Bit-serial canonical match over a 16-bit window: length << 8 | symbol, or 0
// if no codeword of up to 16 bits matches. Table symbol indices are in range:
// the code ranges come from build_huffman_table.
__declspec(noinline) u32 canonical_entry(const JpegHuffmanTable& table, u32 window) noexcept {
    for (u32 length = 0; length < 16; ++length) {
        const i32 code = static_cast<i32>(window >> (15u - length));
        if (code >= table.minimum_code[length] && code <= table.maximum_code[length]) {
            return (length + 1u) << 8 | table.symbols[table.value_offset[length] + code];
        }
    }
    return 0;
}

__forceinline bool decode_huffman_symbol(JpegEntropyReader* reader, const JpegDecodeTable& decode,
                                         u8* symbol) noexcept {
    if (!decode.table->present) {
        reader->error = JpegError::huffman;
        return false;
    }
    if (reader->error != JpegError::none) {
        return false;
    }
    if (reader->bits < 16) [[unlikely]] {
        entropy_refill(reader);
    }
    u32 entry = decode.lookahead[reader->buffer >> (64u - kJpegLookaheadBits)];
    if (entry == 0) {
        entry = canonical_entry(*decode.table, static_cast<u32>(reader->buffer >> 48));
        if (entry == 0) {
            // A bit-serial reader runs out of input before it has read 16 bits.
            reader->error = reader->bits < 16 ? reader->stop : JpegError::huffman;
            return false;
        }
    }
    const u32 length = entry >> 8 & 0x1Fu;
    if (length > reader->bits) {
        reader->error = reader->stop;
        return false;
    }
    entropy_consume(reader, length);
    *symbol = static_cast<u8>(entry);
    return true;
}

__forceinline i32 receive_extend(JpegEntropyReader* reader, u32 bits) noexcept {
    if (bits == 0) {
        return 0;
    }
    const u32 value = entropy_read_bits(reader, bits);
    const u32 threshold = 1u << (bits - 1u);
    return value < threshold ? static_cast<i32>(value) - static_cast<i32>((1u << bits) - 1u)
                             : static_cast<i32>(value);
}

__forceinline bool decode_block_local(JpegEntropyReader* reader, const JpegDecodeTable& dc_table,
                                      const JpegAcDecodeTable& ac_table, i32* predictor, JpegCoefficientBlock* block) noexcept {
    for (u32 index = 0; index < 64; ++index) {
        block->values[index] = 0;
    }
    u8 symbol = 0;
    if (!decode_huffman_symbol(reader, dc_table, &symbol) || symbol > 11) {
        reader->error = JpegError::coefficient;
        return false;
    }
    *predictor += receive_extend(reader, symbol);
    if (reader->error != JpegError::none || *predictor < -32'768 || *predictor > 32'767) {
        reader->error = JpegError::coefficient;
        return false;
    }
    block->values[0] = static_cast<i16>(*predictor);

    u32 coefficient = 1;
    while (coefficient < 64) {
        if (reader->bits < 16) [[unlikely]] {
            entropy_refill(reader);
        }
        // Codeword and magnitude bits in one lookup: the same consumption,
        // checks and value as the symbol path below.
        const u32 fast = ac_table.coefficient[reader->buffer >> (64u - kJpegLookaheadBits)];
        if (fast != 0) {
            const u32 total = fast & 0x1Fu;
            const u32 skip = fast >> 5 & 0x0Fu;
            if ((fast & kJpegEndOfBlock) != 0) {
                if (total <= reader->bits) {
                    entropy_consume(reader, total);
                    break;
                }
            } else if (total <= reader->bits && coefficient + skip < 64) {
                entropy_consume(reader, total);
                coefficient += skip;
                block->values[kZigzagToRaster[coefficient]] = static_cast<i16>(static_cast<i32>(fast) >> 16);
                ++coefficient;
                continue;
            }
        }
        if (!decode_huffman_symbol(reader, ac_table, &symbol)) {
            return false;
        }
        const u32 run = symbol >> 4;
        const u32 bits = symbol & 0x0F;
        if (bits == 0) {
            if (run == 0) {
                break;
            }
            if (run != 15 || coefficient + 16 > 64) {
                reader->error = JpegError::coefficient;
                return false;
            }
            coefficient += 16;
            continue;
        }
        if (bits > 10 || coefficient + run >= 64) {
            reader->error = JpegError::coefficient;
            return false;
        }
        coefficient += run;
        const i32 value = receive_extend(reader, bits);
        if (reader->error != JpegError::none || value < -1'023 || value > 1'023) {
            reader->error = JpegError::coefficient;
            return false;
        }
        block->values[kZigzagToRaster[coefficient]] = static_cast<i16>(value);
        ++coefficient;
    }
    return true;
}

// The reader state lives in a local copy (registers) for the whole block.
bool decode_block(JpegEntropyReader* reader, const JpegDecodeTable& dc_table, const JpegAcDecodeTable& ac_table,
                  i32* predictor, JpegCoefficientBlock* block) noexcept {
    JpegEntropyReader local = *reader;
    const bool ok = decode_block_local(&local, dc_table, ac_table, predictor, block);
    *reader = local;
    return ok;
}

__forceinline bool store_coefficient(JpegEntropyReader* reader, i32 value, i16* destination) noexcept {
    if (value < -32'768 || value > 32'767) {
        reader->error = JpegError::coefficient;
        return false;
    }
    *destination = static_cast<i16>(value);
    return true;
}

__declspec(noinline) bool decode_progressive_dc_initial(JpegEntropyReader* reader, const JpegDecodeTable& table, i32* predictor,
                                   u8 approximation_low, JpegCoefficientBlock* block) noexcept {
    u8 symbol = 0;
    if (!decode_huffman_symbol(reader, table, &symbol) || symbol > 11) {
        if (reader->error == JpegError::none) {
            reader->error = JpegError::coefficient;
        }
        return false;
    }
    *predictor += receive_extend(reader, symbol);
    const i64 scaled = static_cast<i64>(*predictor) * (1ull << approximation_low);
    return reader->error == JpegError::none && scaled >= -32'768 && scaled <= 32'767 &&
           store_coefficient(reader, static_cast<i32>(scaled), &block->values[0]);
}

__declspec(noinline) bool decode_progressive_dc_refinement(JpegEntropyReader* reader, u8 approximation_low,
                                      JpegCoefficientBlock* block) noexcept {
    const i32 delta = 1u << approximation_low;
    const u32 bit = entropy_read_bit(reader);
    i32 value = block->values[0];
    if (reader->error != JpegError::none) {
        return false;
    }
    if (bit != 0) {
        if ((value & delta) != 0) {
            reader->error = JpegError::coefficient;
            return false;
        }
        value |= delta;
    }
    return store_coefficient(reader, value, &block->values[0]);
}

__declspec(noinline) bool decode_progressive_ac_initial(JpegEntropyReader* reader, const JpegAcDecodeTable& table,
                                   u8 spectral_start, u8 spectral_end, u8 approximation_low,
                                   JpegCoefficientBlock* block, u32* eob_run) noexcept {
    if (*eob_run != 0) {
        --*eob_run;
        return true;
    }
    u32 coefficient = spectral_start;
    while (coefficient <= spectral_end) {
        if (reader->bits < 16) [[unlikely]] {
            entropy_refill(reader);
        }
        // One lookup for codeword and magnitude bits when every check below passes.
        const u32 fast = table.coefficient[reader->buffer >> (64u - kJpegLookaheadBits)];
        if (fast != 0 && (fast & kJpegEndOfBlock) == 0) {
            const u32 total = fast & 0x1Fu;
            const u32 skip = fast >> 5 & 0x0Fu;
            const i32 scaled = (static_cast<i32>(fast) >> 16) * (1 << approximation_low);
            if (total <= reader->bits && coefficient + skip <= spectral_end && scaled >= -32'768 &&
                scaled <= 32'767 && block->values[kZigzagToRaster[coefficient + skip]] == 0) {
                entropy_consume(reader, total);
                coefficient += skip;
                block->values[kZigzagToRaster[coefficient]] = static_cast<i16>(scaled);
                ++coefficient;
                continue;
            }
        }
        u8 symbol = 0;
        if (!decode_huffman_symbol(reader, table, &symbol)) {
            return false;
        }
        const u32 run = symbol >> 4;
        const u32 bits = symbol & 0x0F;
        if (bits == 0) {
            if (run == 15) {
                if (coefficient + 16u > static_cast<u32>(spectral_end) + 1u) {
                    reader->error = JpegError::coefficient;
                    return false;
                }
                coefficient += 16;
                continue;
            }
            *eob_run = (1u << run) + entropy_read_bits(reader, run);
            if (reader->error != JpegError::none || *eob_run == 0) {
                reader->error = JpegError::coefficient;
                return false;
            }
            --*eob_run;
            return true;
        }
        if (bits > 10 || coefficient + run > spectral_end) {
            reader->error = JpegError::coefficient;
            return false;
        }
        coefficient += run;
        const i32 unscaled = receive_extend(reader, bits);
        const i64 scaled = static_cast<i64>(unscaled) * (1ull << approximation_low);
        i16& destination = block->values[kZigzagToRaster[coefficient]];
        if (reader->error != JpegError::none || destination != 0 || scaled < -32'768 || scaled > 32'767 ||
            !store_coefficient(reader, static_cast<i32>(scaled), &destination)) {
            if (reader->error == JpegError::none) {
                reader->error = JpegError::coefficient;
            }
            return false;
        }
        ++coefficient;
    }
    return true;
}

__forceinline bool refine_nonzeroes(JpegEntropyReader* reader, JpegCoefficientBlock* block, u32* coefficient,
                      u32 spectral_end, i32 zeroes_to_skip, i32 delta) noexcept {
    if (delta <= 0) {
        return false;
    }
    while (*coefficient <= spectral_end) {
        i16& stored = block->values[kZigzagToRaster[*coefficient]];
        i32 value = stored;
        if (value == 0) {
            if (zeroes_to_skip == 0) {
                return true;
            }
            if (zeroes_to_skip > 0) {
                --zeroes_to_skip;
            }
            ++*coefficient;
            continue;
        }
        const u32 bit = entropy_read_bit(reader);
        if (reader->error != JpegError::none) {
            return false;
        }
        if (bit != 0) {
            const i32 magnitude = value < 0 ? -value : value;
            if ((magnitude & delta) != 0) {
                reader->error = JpegError::coefficient;
                return false;
            }
            value += value < 0 ? -delta : delta;
            if (!store_coefficient(reader, value, &stored)) {
                return false;
            }
        }
        ++*coefficient;
    }
    if (zeroes_to_skip >= 0) {
        reader->error = JpegError::coefficient;
        return false;
    }
    return true;
}

__declspec(noinline) bool decode_progressive_ac_refinement(JpegEntropyReader* reader, const JpegDecodeTable& table,
                                      u8 spectral_start, u8 spectral_end, u8 approximation_low,
                                      JpegCoefficientBlock* block, u32* eob_run) noexcept {
    const i32 delta = 1u << approximation_low;
    u32 coefficient = spectral_start;
    if (*eob_run == 0) {
        while (coefficient <= spectral_end) {
            u8 symbol = 0;
            if (!decode_huffman_symbol(reader, table, &symbol)) {
                return false;
            }
            const u32 zeroes = symbol >> 4;
            const u32 magnitude = symbol & 0x0F;
            i32 new_value = 0;
            if (magnitude == 0) {
                if (zeroes != 15) {
                    *eob_run = (1u << zeroes) + entropy_read_bits(reader, zeroes);
                    if (reader->error != JpegError::none || *eob_run == 0) {
                        reader->error = JpegError::coefficient;
                        return false;
                    }
                    break;
                }
            } else if (magnitude == 1) {
                new_value = entropy_read_bit(reader) != 0 ? delta : -delta;
                if (reader->error != JpegError::none) {
                    return false;
                }
            } else {
                reader->error = JpegError::coefficient;
                return false;
            }
            if (!refine_nonzeroes(reader, block, &coefficient, spectral_end, static_cast<i32>(zeroes),
                                  delta) ||
                coefficient > spectral_end) {
                if (reader->error == JpegError::none) {
                    reader->error = JpegError::coefficient;
                }
                return false;
            }
            if (new_value != 0) {
                i16& destination = block->values[kZigzagToRaster[coefficient]];
                if (destination != 0 || !store_coefficient(reader, new_value, &destination)) {
                    reader->error = JpegError::coefficient;
                    return false;
                }
            }
            ++coefficient;
        }
    }
    if (*eob_run != 0) {
        --*eob_run;
        if (!refine_nonzeroes(reader, block, &coefficient, spectral_end, -1, delta)) {
            return false;
        }
    }
    return true;
}

__forceinline bool decode_progressive_block(JpegEntropyReader* reader, const JpegDecodeTable* dc,
                              const JpegAcDecodeTable* ac,
                              const JpegScan& scan, u32 scan_component, i32* predictor, JpegCoefficientBlock* block,
                              u32* eob_run) noexcept {
    if (scan.spectral_start == 0) {
        return scan.approximation_high == 0
                   ? decode_progressive_dc_initial(reader, dc[scan.dc_tables[scan_component]],
                                                   predictor, scan.approximation_low, block)
                   : decode_progressive_dc_refinement(reader, scan.approximation_low, block);
    }
    return scan.approximation_high == 0
               ? decode_progressive_ac_initial(reader, ac[scan.ac_tables[scan_component]],
                                               scan.spectral_start, scan.spectral_end,
                                               scan.approximation_low, block, eob_run)
               : decode_progressive_ac_refinement(reader, ac[scan.ac_tables[scan_component]],
                                                  scan.spectral_start, scan.spectral_end,
                                                  scan.approximation_low, block, eob_run);
}

bool decode_progressive_scan(ConstSlice<u8> encoded, const JpegImage& image, const JpegTables& tables,
                             const JpegScan& scan, JpegCoefficientBlock* blocks,
                             JpegDecodeResult* result) noexcept {
    if (static_cast<u64>(scan.entropy_offset) + scan.entropy_bytes > encoded.count) {
        return false;
    }
    JpegEntropyReader reader = entropy_reader(ConstSlice<u8>{encoded.data + scan.entropy_offset, scan.entropy_bytes});
    // Lookahead tables for the Huffman tables this scan references only.
    JpegDecodeTable dc[kJpegTableMaximum];
    JpegAcDecodeTable ac[kJpegTableMaximum];
    u8 built = 0;
    for (u32 index = 0; index < scan.component_count; ++index) {
        if (scan.spectral_start == 0 && scan.approximation_high == 0) {
            const u8 table = scan.dc_tables[index];
            if (table < kJpegTableMaximum && (built & (1u << table)) == 0) {
                build_decode_table(tables.dc[table], &dc[table]);
                built |= static_cast<u8>(1u << table);
            }
        } else if (scan.spectral_start != 0) {
            const u8 table = scan.ac_tables[index];
            if (table < kJpegTableMaximum && (built & (16u << table)) == 0) {
                // First AC scans take codeword and magnitude in one lookup.
                build_decode_table(tables.ac[table], &ac[table],
                                   scan.approximation_high == 0 ? ac[table].coefficient : nullptr);
                built |= static_cast<u8>(16u << table);
            }
        }
    }
    i32 predictor[kJpegComponentMaximum]{};
    u32 eob_run = 0;
    u8 expected_restart = 0;
    // An interleaved scan walks MCUs (each component's sampling factors); a
    // single-component scan walks that component's blocks, one per data unit.
    // One loop serves both so the block decoders are expanded once.
    const bool interleaved = scan.component_count > 1;
    const u32 unit_columns =
        interleaved ? image.mcu_columns : scan_component_columns(image, image.components[scan.component_indices[0]]);
    u32 unit_x = 0;
    u32 unit_y = 0;
    for (u32 unit = 0; unit < scan.data_units; ++unit) {
        if (scan.restart_interval != 0 && unit != 0 && unit % scan.restart_interval == 0) {
            if (!entropy_consume_restart(&reader, expected_restart)) {
                result->error = reader.error;
                return false;
            }
            expected_restart = static_cast<u8>((expected_restart + 1u) & 7u);
            for (u32 index = 0; index < image.component_count; ++index) {
                predictor[index] = 0;
            }
            eob_run = 0;
        }
        for (u32 scan_component = 0; scan_component < scan.component_count; ++scan_component) {
            const u32 component_index = scan.component_indices[scan_component];
            const JpegComponent& component = image.components[component_index];
            const u32 horizontal_blocks = interleaved ? component.horizontal_sampling : 1u;
            const u32 vertical_blocks = interleaved ? component.vertical_sampling : 1u;
            for (u32 vertical = 0; vertical < vertical_blocks; ++vertical) {
                for (u32 horizontal = 0; horizontal < horizontal_blocks; ++horizontal) {
                    const u32 block_x = unit_x * horizontal_blocks + horizontal;
                    const u32 block_y = unit_y * vertical_blocks + vertical;
                    const u32 block_index = component.block_offset + block_y * component.block_columns + block_x;
                    if (block_index >= image.coefficient_blocks ||
                        !decode_progressive_block(&reader, dc, ac, scan, scan_component, &predictor[component_index],
                                                  &blocks[block_index], &eob_run)) {
                        result->error = reader.error == JpegError::none ? JpegError::coefficient : reader.error;
                        return false;
                    }
                    ++result->decoded_blocks;
                }
            }
        }
        ++result->decoded_mcus;
        if (++unit_x == unit_columns) {
            unit_x = 0;
            ++unit_y;
        }
    }
    if (eob_run != 0 || !entropy_at_end(&reader)) {
        result->error = reader.error == JpegError::none ? JpegError::entropy : reader.error;
        return false;
    }
    ++result->decoded_scans;
    return true;
}

} // namespace jpeg_detail
} // namespace maicord
