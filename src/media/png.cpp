#include "maicord/media/png.hpp"

#include "png_common.hpp"

#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

enum class ParseStage : u8 {
    header,
    before_idat,
    idat,
    after_idat,
    done,
};

PngParseResult parse_failure(PngParseResult result,
                             PngError error) noexcept {
    result.error = error;
    return result;
}

bool initialize_header(PngImage* image, const u8* data) noexcept {
    return data[10] == 0 && data[11] == 0 &&
           png_image_configure(image, byte_load_be32(data), byte_load_be32(data + 4),
                               data[8], data[9]);
}

// Rows of row_bytes + 1 (filter byte first), reconstructed in place; the
// filter byte of a reconstructed row becomes 0.
bool reconstruct_rows(u8* data, u32 rows, u32 row_bytes, u32 bytes_per_pixel) noexcept {
    const u64 row_stride = u64{row_bytes} + 1;
    for (u32 y = 0; y < rows; ++y) {
        u8* row = data + y * row_stride;
        const u8* previous = y == 0 ? nullptr : data + (y - 1) * row_stride + 1;
        if (!png_unfilter_row(row[0], row + 1, previous, row_bytes, bytes_per_pixel)) {
            return false;
        }
        row[0] = 0;
    }
    return true;
}

// Adam7 pass origin and step: x0, y0, dx, dy.
constexpr u8 kAdam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                             {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

u32 pass_extent(u32 size, u32 origin, u32 step) noexcept {
    return size > origin ? (size - origin + step - 1) / step : 0;
}

u64 pass_row_bytes(const PngImage& image, u32 width) noexcept {
    return (u64{width} * image.channels * image.bit_depth + 7) / 8;
}

u64 interlaced_bytes(const PngImage& image) noexcept {
    u64 total = 0;
    for (const auto& pass : kAdam7) {
        const u32 width = pass_extent(image.width, pass[0], pass[2]);
        const u32 height = pass_extent(image.height, pass[1], pass[3]);
        if (width && height) {
            total += u64{height} * (pass_row_bytes(image, width) + 1);
        }
    }
    return total;
}

u64 span_table_offset(const PngImage& image) noexcept {
    const u64 used = u64{image.inflated_bytes} + (image.interlace ? interlaced_bytes(image) : 0);
    return (used + 15u) & ~u64{15};
}

// Copies width packed pixels of Bytes each to every step-th output pixel.
template <u32 Bytes> void scatter_pixels(const u8* source, u8* target, u32 width, u64 step) noexcept {
    for (u32 i = 0; i < width; ++i, source += Bytes, target += step) {
        for (u32 b = 0; b < Bytes; ++b) {
            target[b] = source[b];
        }
    }
}

// Expands reconstructed Adam7 passes (stored back to back after the output)
// into the progressive layout. Packed sub-byte pixels need zeroed rows.
void deinterlace(const PngImage& image, const u8* passes, u8* output) noexcept {
    const u32 bits = static_cast<u32>(image.channels) * image.bit_depth;
    if (bits < 8)
        clear_bytes(output, image.inflated_bytes);
    for (const auto& pass : kAdam7) {
        const u32 width = pass_extent(image.width, pass[0], pass[2]);
        const u32 height = pass_extent(image.height, pass[1], pass[3]);
        if (!width || !height) {
            continue;
        }
        const u64 stride = pass_row_bytes(image, width) + 1;
        for (u32 j = 0; j < height; ++j) {
            const u8* source = passes + j * stride + 1;
            u8* target = output + u64{pass[1] + j * pass[3]} * image.row_stride + 1;
            target[-1] = 0;
            if (bits >= 8) {
                const u32 bytes = bits / 8;
                u8* to = target + u64{pass[0]} * bytes;
                const u64 step = u64{pass[2]} * bytes;
                switch (bytes) {
                case 1: scatter_pixels<1>(source, to, width, step); break;
                case 2: scatter_pixels<2>(source, to, width, step); break;
                case 3: scatter_pixels<3>(source, to, width, step); break;
                case 4: scatter_pixels<4>(source, to, width, step); break;
                case 6: scatter_pixels<6>(source, to, width, step); break;
                default: scatter_pixels<8>(source, to, width, step); break;
                }
            } else {
                const u32 mask = (1u << bits) - 1u;
                for (u32 i = 0; i < width; ++i) {
                    const u32 from = i * bits;
                    const u32 value = source[from >> 3] >> (8u - bits - (from & 7u)) & mask;
                    const u32 to = (pass[0] + i * pass[2]) * bits;
                    target[to >> 3] = static_cast<u8>(target[to >> 3] | value << (8u - bits - (to & 7u)));
                }
            }
        }
        passes += height * stride;
    }
}

bool validate_palette_indices(const PngImage& image,
                              ConstSlice<u8> scanlines) noexcept {
    // Every index the bit depth can express names a palette entry.
    if (image.color_type != 3 || image.palette_count >= (1u << image.bit_depth)) {
        return true;
    }
    if (image.bit_depth == 8) {
        const __m128i limit = _mm_set1_epi8(static_cast<char>(image.palette_count - 1u));
        for (u32 y = 0; y < image.height; ++y) {
            const u8* row = scanlines.data + y * image.row_stride + 1;
            u32 x = 0;
            __m128i largest = _mm_setzero_si128();
            for (; x + 16 <= image.width; x += 16) {
                largest = _mm_max_epu8(largest, _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x)));
            }
            // An index above palette_count - 1 leaves max(index, limit) != limit.
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_max_epu8(largest, limit), limit)) != 0xFFFF) {
                return false;
            }
            for (; x < image.width; ++x) {
                if (row[x] >= image.palette_count) {
                    return false;
                }
            }
        }
        return true;
    }
    const u32 mask = (1u << image.bit_depth) - 1u;
    for (u32 y = 0; y < image.height; ++y) {
        const u8* row = scanlines.data + y * image.row_stride + 1;
        for (u32 x = 0; x < image.width; ++x) {
            const u32 bit = x * image.bit_depth;
            const u32 shift = 8u - image.bit_depth - (bit & 7u);
            const u32 index = row[bit >> 3] >> shift & mask;
            if (index >= image.palette_count) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

PngParseResult png_parse(ConstSlice<u8> encoded,
                         PngImage* image) noexcept {
    PngParseResult result{};
    if (!image || (!encoded.data && encoded.count != 0)) {
        return parse_failure(result, PngError::invalid_input);
    }
    *image = PngImage{};
    image->transparent_gray = ~0u;
    image->transparent_red = ~0u;
    image->transparent_green = ~0u;
    image->transparent_blue = ~0u;
    if (encoded.count > kPngEncodedMaximumBytes) {
        return parse_failure(result, PngError::encoded_limit);
    }
    if (encoded.count < sizeof(kPngSignature)) {
        return parse_failure(result, PngError::truncated);
    }
    for (u32 index = 0; index < sizeof(kPngSignature); ++index) {
        if (encoded[index] != kPngSignature[index]) {
            return parse_failure(result, PngError::signature);
        }
    }

    ParseStage stage = ParseStage::header;
    bool palette_seen = false;
    bool transparency_seen = false;
    bool idat_seen = false;
    u32 cursor = sizeof(kPngSignature);
    while (cursor < encoded.count) {
        if (encoded.count - cursor < 12) {
            return parse_failure(result, PngError::truncated);
        }
        const u32 length = byte_load_be32(encoded.data + cursor);
        const u64 end = static_cast<u64>(cursor) + 12u + length;
        if (length > 0x7FFFFFFFu || end > encoded.count) {
            return parse_failure(result, PngError::truncated);
        }
        const u8* type_bytes = encoded.data + cursor + 4;
        if (!png_chunk_type_valid(type_bytes) || (type_bytes[2] & 0x20u) != 0) {
            return parse_failure(result, PngError::chunk_type);
        }
        const u32 type = byte_load_be32(type_bytes);
        const u8* data = type_bytes + 4;
        const u32 expected_crc = byte_load_be32(data + length);
        if (png_crc32(type_bytes, length + 4) != expected_crc) {
            return parse_failure(result, PngError::chunk_crc);
        }
        ++result.chunks;

        if (stage == ParseStage::header && type != kChunkIhdr) {
            return parse_failure(result, PngError::chunk_order);
        }
        if (stage == ParseStage::idat && type != kChunkIdat) {
            stage = ParseStage::after_idat;
        }
        if (type == kChunkIhdr) {
            if (stage != ParseStage::header || length != 13) {
                return parse_failure(result,
                                     stage == ParseStage::header
                                         ? PngError::unsupported_format
                                         : PngError::duplicate_chunk);
            }
            if (data[12] > 1) {
                return parse_failure(result, PngError::unsupported_format);
            }
            if (!initialize_header(image, data)) {
                return parse_failure(
                    result,
                    image->width > kPngDimensionMaximum ||
                            image->height > kPngDimensionMaximum
                        ? PngError::dimensions
                        : PngError::unsupported_format);
            }
            image->interlace = data[12];
            stage = ParseStage::before_idat;
        } else if (type == kChunkPlte) {
            if (stage != ParseStage::before_idat || palette_seen ||
                transparency_seen) {
                return parse_failure(result,
                                     palette_seen ? PngError::duplicate_chunk
                                                  : PngError::chunk_order);
            }
            if (!png_apply_palette(image, data, length)) {
                return parse_failure(result, PngError::palette);
            }
            palette_seen = true;
        } else if (type == kChunkTrns) {
            if (stage != ParseStage::before_idat || transparency_seen) {
                return parse_failure(result,
                                     transparency_seen
                                         ? PngError::duplicate_chunk
                                         : PngError::chunk_order);
            }
            if (!png_apply_transparency(image, data, length)) {
                return parse_failure(result, PngError::transparency);
            }
            transparency_seen = true;
        } else if (type == kChunkIdat) {
            if (stage == ParseStage::after_idat ||
                (image->color_type == 3 && !palette_seen)) {
                return parse_failure(result, PngError::chunk_order);
            }
            idat_seen = true;
            stage = ParseStage::idat;
            if (length != 0) {
                if (image->idat_span_count >= kPngIdatChunkMaximum ||
                    length > kPngEncodedMaximumBytes -
                                 image->compressed_bytes) {
                    return parse_failure(result,
                                         PngError::idat_capacity);
                }
                // Encoders split IDAT freely; chunks past the inline table are
                // found again at decode.
                if (image->idat_span_count < kPngIdatSpanCapacity) {
                    image->idat_spans[image->idat_span_count] =
                        DeflateInputSpan{data, length};
                }
                ++image->idat_span_count;
                image->compressed_bytes += length;
            }
        } else if (type == kChunkIend) {
            if (!idat_seen || length != 0 ||
                end != encoded.count) {
                return parse_failure(result, PngError::chunk_order);
            }
            stage = ParseStage::done;
        } else if (type == kChunkActl || type == kChunkFctl ||
                   type == kChunkFdat) {
            return parse_failure(result,
                                 PngError::unsupported_animation);
        } else {
            const bool ancillary = (type_bytes[0] & 0x20u) != 0;
            if (!ancillary) {
                return parse_failure(
                    result, PngError::unknown_critical_chunk);
            }
            ++result.ancillary_chunks;
            image->has_color_metadata =
                image->has_color_metadata || png_color_metadata_chunk(type);
        }
        cursor = static_cast<u32>(end);
        if (stage == ParseStage::done) {
            break;
        }
    }
    if (stage != ParseStage::done || image->idat_span_count == 0 ||
        image->compressed_bytes < 6 ||
        (image->color_type == 3 && image->palette_count == 0)) {
        return parse_failure(result, PngError::chunk_order);
    }
    result.error = PngError::none;
    return result;
}

bool png_image_configure(PngImage* image, u32 width, u32 height,
                         u8 bit_depth, u8 color_type) noexcept {
    image->width = width;
    image->height = height;
    u8 channels = 0;
    if (width == 0 || height == 0 || width > kPngDimensionMaximum ||
        height > kPngDimensionMaximum ||
        !png_format_channels(color_type, bit_depth, &channels)) {
        return false;
    }
    // Valid formats use at most eight bytes per pixel; admitted dimensions
    // already bound every row and the complete image to the configured budget.
    static_assert(u64{kPngDimensionMaximum} * (u64{kPngDimensionMaximum} * 8 + 1) ==
                  kPngInflatedMaximumBytes);
    const u32 row_bytes = (width * channels * bit_depth + 7) / 8;
    image->row_bytes = row_bytes;
    image->row_stride = row_bytes + 1;
    image->inflated_bytes = (row_bytes + 1) * height;
    image->bit_depth = bit_depth;
    image->color_type = color_type;
    image->channels = channels;
    image->bytes_per_pixel = static_cast<u8>(maximum<u32>(
        1, (static_cast<u32>(channels) * bit_depth + 7) / 8));
    return true;
}

u32 png_decode_buffer_bytes(const PngImage& image) noexcept {
    if (image.interlace > 1 || !image.inflated_bytes) {
        return 0;
    }
    u64 total = u64{image.inflated_bytes} + (image.interlace ? interlaced_bytes(image) : 0);
    if (image.idat_span_count > kPngIdatSpanCapacity) {
        total = span_table_offset(image) + u64{image.idat_span_count} * sizeof(DeflateInputSpan);
    }
    return total <= kPngDecodeBufferMaximumBytes ? static_cast<u32>(total) : 0;
}

PngDecodeResult png_decode_parsed_scanlines(
    PngImage* image, DeflateWorkspace* workspace,
    Slice<u8> scanlines) noexcept {
    PngDecodeResult result{};
    PngImage configured{};
    if (!image || !workspace || !scanlines.data ||
        !png_image_configure(&configured, image->width, image->height, image->bit_depth,
                             image->color_type) ||
        image->idat_span_count == 0 || image->compressed_bytes < 6 ||
        image->interlace > 1 ||
        png_decode_buffer_bytes(*image) == 0 ||
        scanlines.count < png_decode_buffer_bytes(*image) ||
        image->inflated_bytes == 0 ||
        image->row_bytes != configured.row_bytes ||
        image->row_stride != configured.row_stride ||
        image->inflated_bytes != configured.inflated_bytes ||
        image->channels != configured.channels ||
        image->bytes_per_pixel != configured.bytes_per_pixel ||
        image->palette_count > kPngPaletteCapacity ||
        (image->color_type == 3 &&
         image->palette_count > (1u << image->bit_depth)) ||
        (image->color_type == 3 && image->palette_count == 0)) {
        result.error = PngError::inflated_limit;
        return result;
    }
    u64 compressed_bytes = 0;
    const u32 inline_spans = minimum<u32>(image->idat_span_count, kPngIdatSpanCapacity);
    for (u32 index = 0; index < inline_spans; ++index) {
        if (!image->idat_spans[index].data ||
            image->idat_spans[index].bytes == 0) {
            result.error = PngError::idat_capacity;
            return result;
        }
        compressed_bytes += image->idat_spans[index].bytes;
    }
    const DeflateInputSpan* spans = image->idat_spans;
    if (image->idat_span_count > kPngIdatSpanCapacity) {
        // png_parse validated the run: IDAT chunks are consecutive, so walk
        // their framing from the first one into the buffer's span table.
        auto* table = reinterpret_cast<DeflateInputSpan*>(scanlines.data + span_table_offset(*image));
        const u8* chunk = image->idat_spans[0].data - 8;
        u32 count = 0;
        compressed_bytes = 0;
        while (byte_load_be32(chunk + 4) == kChunkIdat) {
            const u32 length = byte_load_be32(chunk);
            if (length != 0) {
                if (count == image->idat_span_count) {
                    break;
                }
                table[count++] = DeflateInputSpan{chunk + 8, length};
                compressed_bytes += length;
            }
            chunk += 12u + u64{length};
        }
        if (count != image->idat_span_count) {
            result.error = PngError::idat_capacity;
            return result;
        }
        spans = table;
    }
    if (compressed_bytes != image->compressed_bytes) {
        result.error = PngError::idat_capacity;
        return result;
    }
    const u32 inflate_bytes = image->interlace
                                  ? static_cast<u32>(interlaced_bytes(*image))
                                  : image->inflated_bytes;
    u8* inflate_target = scanlines.data + (image->interlace ? image->inflated_bytes : 0);
    result.inflate = zlib_inflate(
        ConstSlice<DeflateInputSpan>{spans, image->idat_span_count},
        Slice<u8>{inflate_target, inflate_bytes}, inflate_bytes, workspace);
    if (!result.inflate.ok()) {
        result.error = PngError::inflate;
        return result;
    }
    if (image->interlace) {
        u8* pass = inflate_target;
        for (const auto& geometry : kAdam7) {
            const u32 width = pass_extent(image->width, geometry[0], geometry[2]);
            const u32 height = pass_extent(image->height, geometry[1], geometry[3]);
            if (!width || !height) {
                continue;
            }
            const u32 row_bytes = static_cast<u32>(pass_row_bytes(*image, width));
            if (!reconstruct_rows(pass, height, row_bytes, image->bytes_per_pixel)) {
                result.error = PngError::filter;
                return result;
            }
            pass += u64{height} * (row_bytes + 1u);
        }
        deinterlace(*image, inflate_target, scanlines.data);
    } else if (!reconstruct_rows(scanlines.data, image->height, image->row_bytes, image->bytes_per_pixel)) {
        result.error = PngError::filter;
        return result;
    }
    if (!validate_palette_indices(
            *image,
            ConstSlice<u8>{scanlines.data, image->inflated_bytes})) {
        result.error = PngError::palette_index;
        return result;
    }
    result.reconstructed_bytes = image->inflated_bytes;
    result.error = PngError::none;
    return result;
}

PngDecodeResult png_decode_scanlines(
    ConstSlice<u8> encoded, PngImage* image,
    DeflateWorkspace* workspace, Slice<u8> scanlines) noexcept {
    PngDecodeResult result{};
    result.parse = png_parse(encoded, image);
    if (!result.parse.ok()) {
        result.error = result.parse.error;
        return result;
    }
    const PngParseResult parse = result.parse;
    result = png_decode_parsed_scanlines(image, workspace, scanlines);
    result.parse = parse;
    return result;
}

} // namespace maicord
