#include "maicord/media/png_stream.hpp"

#include "maicord/core/memory.hpp"
#include "png_common.hpp"

namespace maicord {
namespace {

PngParseResult parse_failure(PngParseResult result, PngError error) noexcept {
    result.error = error;
    return result;
}

bool configure(PngImage* image, PngStreamLayout* layout, const u8* data) noexcept {
    const u32 width = byte_load_be32(data), height = byte_load_be32(data + 4);
    const u8 depth = data[8], color = data[9];
    u8 channels = 0;
    if (!png_format_channels(color, depth, &channels) || !width || !height ||
        width > kPngStreamDimensionMaximum || height > kPngStreamDimensionMaximum || data[10] != 0 ||
        data[11] != 0)
        return false;
    const u64 row_bytes = (static_cast<u64>(width) * channels * depth + 7) / 8;
    image->width = width;
    image->height = height;
    image->bit_depth = depth;
    image->color_type = color;
    image->channels = channels;
    image->bytes_per_pixel = static_cast<u8>(maximum<u32>(1, (static_cast<u32>(channels) * depth + 7) / 8));
    image->row_bytes = static_cast<u32>(row_bytes);
    image->row_stride = static_cast<u32>(row_bytes + 1);
    // Saturates at 32 bits; layout->inflated_bytes holds the exact size.
    image->inflated_bytes = static_cast<u32>(minimum<u64>((row_bytes + 1) * height, ~0u));
    layout->inflated_bytes = (row_bytes + 1) * height;
    layout->row_buffer_bytes = static_cast<u32>((row_bytes + 1) * 2);
    return true;
}

struct Assembler {
    const PngImage* image;
    u8* current;
    u8* previous;
    PngStreamRowSink sink;
    void* context;
    u32 filled;
    u32 y;
    PngError error;
};

bool assemble(void* opaque, ConstSlice<u8> bytes) noexcept {
    auto& a = *static_cast<Assembler*>(opaque);
    const u32 stride = a.image->row_stride;
    u32 offset = 0;
    while (offset < bytes.count) {
        if (a.y >= a.image->height) {
            a.error = PngError::inflate; // data past the last scanline
            return false;
        }
        const u32 take = minimum(stride - a.filled, bytes.count - offset);
        copy_bytes(a.current + a.filled, bytes.data + offset, take);
        a.filled += take;
        offset += take;
        if (a.filled < stride)
            continue;
        if (!png_unfilter_row(a.current[0], a.current + 1, a.y ? a.previous + 1 : nullptr,
                              a.image->row_bytes, a.image->bytes_per_pixel)) {
            a.error = PngError::filter;
            return false;
        }
        if (!a.sink(a.context, a.y, a.current + 1)) {
            a.error = PngError::palette_index;
            return false;
        }
        u8* swap = a.previous;
        a.previous = a.current;
        a.current = swap;
        a.filled = 0;
        ++a.y;
    }
    return true;
}

} // namespace

PngParseResult png_stream_parse(ConstSlice<u8> encoded, PngImage* image, PngStreamLayout* layout) noexcept {
    PngParseResult result{};
    if (!image || !layout || !encoded.data || encoded.count > 0x7FFFFFFFu)
        return parse_failure(result, PngError::invalid_input);
    *image = PngImage{};
    *layout = PngStreamLayout{};
    image->transparent_gray = image->transparent_red = image->transparent_green = image->transparent_blue = ~0u;
    if (encoded.count < sizeof(kPngSignature) + 12)
        return parse_failure(result, PngError::truncated);
    for (u32 i = 0; i < sizeof(kPngSignature); ++i)
        if (encoded[i] != kPngSignature[i])
            return parse_failure(result, PngError::signature);
    bool header = false, palette = false, transparency = false, idat = false, after_idat = false, end = false;
    u64 compressed = 0;
    u32 cursor = sizeof(kPngSignature);
    while (cursor < encoded.count && !end) {
        if (encoded.count - cursor < 12)
            return parse_failure(result, PngError::truncated);
        const u32 length = byte_load_be32(encoded.data + cursor);
        const u64 next = static_cast<u64>(cursor) + 12u + length;
        if (length > 0x7FFFFFFFu || next > encoded.count)
            return parse_failure(result, PngError::truncated);
        const u8* type_bytes = encoded.data + cursor + 4;
        if (!png_chunk_type_valid(type_bytes))
            return parse_failure(result, PngError::chunk_type);
        const u32 type = byte_load_be32(type_bytes);
        const u8* data = type_bytes + 4;
        if (png_crc32(type_bytes, length + 4) != byte_load_be32(data + length))
            return parse_failure(result, PngError::chunk_crc);
        ++result.chunks;
        if (!header && type != kChunkIhdr)
            return parse_failure(result, PngError::chunk_order);
        if (idat && type != kChunkIdat)
            after_idat = true;
        if (type == kChunkIhdr) {
            if (header || length != 13)
                return parse_failure(result, header ? PngError::duplicate_chunk : PngError::unsupported_format);
            if (data[12] != 0)
                return parse_failure(result,
                                     data[12] == 1 ? PngError::unsupported_interlace : PngError::unsupported_format);
            if (!configure(image, layout, data))
                return parse_failure(result, byte_load_be32(data) > kPngStreamDimensionMaximum ||
                                                     byte_load_be32(data + 4) > kPngStreamDimensionMaximum
                                                 ? PngError::dimensions
                                                 : PngError::unsupported_format);
            header = true;
        } else if (type == kChunkPlte) {
            if (idat || palette || transparency)
                return parse_failure(result, palette ? PngError::duplicate_chunk : PngError::chunk_order);
            if (!png_apply_palette(image, data, length))
                return parse_failure(result, PngError::palette);
            palette = true;
        } else if (type == kChunkTrns) {
            if (idat || transparency)
                return parse_failure(result, transparency ? PngError::duplicate_chunk : PngError::chunk_order);
            if (!png_apply_transparency(image, data, length))
                return parse_failure(result, PngError::transparency);
            transparency = true;
        } else if (type == kChunkIdat) {
            if (after_idat || (image->color_type == 3 && !palette))
                return parse_failure(result, PngError::chunk_order);
            idat = true;
            if (length) {
                ++layout->idat_chunks;
                compressed += length;
            }
        } else if (type == kChunkIend) {
            if (!idat || length)
                return parse_failure(result, PngError::chunk_order);
            end = true;
        } else if (type == kChunkActl || type == kChunkFctl || type == kChunkFdat) {
            // APNG: the thumbnail is the default image; frame data is skipped.
            ++result.ancillary_chunks;
        } else if (!(type_bytes[0] & 0x20u)) {
            return parse_failure(result, PngError::unknown_critical_chunk);
        } else {
            ++result.ancillary_chunks;
        }
        cursor = static_cast<u32>(next);
    }
    if (!end || !layout->idat_chunks || compressed < 6 || (image->color_type == 3 && !image->palette_count))
        return parse_failure(result, PngError::chunk_order);
    image->compressed_bytes = static_cast<u32>(minimum<u64>(compressed, ~0u));
    return result;
}

PngDecodeResult png_stream_decode(ConstSlice<u8> encoded, const PngImage& image, const PngStreamLayout& layout,
                                  Slice<DeflateInputSpan> spans, Slice<u8> window, Slice<u8> rows,
                                  PngStreamRowSink sink, void* context, DeflateWorkspace* workspace) noexcept {
    PngDecodeResult result{};
    result.error = PngError::invalid_input;
    if (!encoded.data || !sink || !workspace || !spans.data || spans.count < layout.idat_chunks ||
        !layout.idat_chunks || !window.data || window.count < kPngStreamWindowBytes || !rows.data ||
        rows.count < layout.row_buffer_bytes || !image.row_stride ||
        layout.row_buffer_bytes != image.row_stride * 2u)
        return result;
    // Gather the IDAT payloads in order; png_stream_parse validated the chunks.
    u32 count = 0;
    for (u32 cursor = sizeof(kPngSignature); cursor + 12 <= encoded.count && count < layout.idat_chunks;) {
        const u32 length = byte_load_be32(encoded.data + cursor);
        if (static_cast<u64>(cursor) + 12u + length > encoded.count)
            return result;
        if (byte_load_be32(encoded.data + cursor + 4) == kChunkIdat && length)
            spans[count++] = {encoded.data + cursor + 8, length};
        cursor += 12u + length;
    }
    if (count != layout.idat_chunks)
        return result;
    Assembler assembler{&image, rows.data, rows.data + image.row_stride, sink, context, 0, 0, PngError::none};
    result.inflate = zlib_inflate_windowed({spans.data, count}, window, ~0u, assemble, &assembler, workspace);
    result.reconstructed_bytes = assembler.y;
    if (assembler.error != PngError::none) {
        result.error = assembler.error;
        return result;
    }
    if (!result.inflate.ok()) {
        result.error = PngError::inflate;
        return result;
    }
    result.error = assembler.y == image.height && assembler.filled == 0 ? PngError::none : PngError::inflate;
    return result;
}

} // namespace maicord
