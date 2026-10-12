#pragma once

#include "maicord/media/png.hpp"

namespace maicord {

// Streaming PNG decode for thumbnails, including images png_parse refuses:
// up to 32768 px per side, any number of IDAT chunks, and the default image
// of an APNG. Only the encoded file, a sliding inflate window and two
// scanlines are resident; no scanline buffer or RGBA copy of the image.
// Interlaced (Adam7) images remain unsupported.
inline constexpr u32 kPngStreamDimensionMaximum = 32768;
inline constexpr u32 kPngStreamWindowBytes = kZlibWindowedMinimumBytes;

struct PngStreamLayout {
    u64 inflated_bytes;   // row_stride * height
    u32 idat_chunks;      // non-empty IDAT chunks (span storage needed)
    u32 row_buffer_bytes; // two scanlines including filter bytes
};

// Validates signature, chunk CRCs and order, IHDR, PLTE and tRNS. Fills the
// image's format, palette and transparency fields; idat_spans stay unused.
PngParseResult png_stream_parse(ConstSlice<u8> encoded, PngImage* image, PngStreamLayout* layout) noexcept;

// One reconstructed scanline (filter byte removed), top to bottom.
using PngStreamRowSink = bool (*)(void* context, u32 y, const u8* row) noexcept;
// spans holds layout.idat_chunks entries, window kPngStreamWindowBytes and
// rows layout.row_buffer_bytes. The sink sees each row once, before the
// stream's Adler-32 is verified; a later error still fails the decode.
PngDecodeResult png_stream_decode(ConstSlice<u8> encoded, const PngImage& image, const PngStreamLayout& layout,
                                  Slice<DeflateInputSpan> spans, Slice<u8> window, Slice<u8> rows,
                                  PngStreamRowSink sink, void* context, DeflateWorkspace* workspace) noexcept;

} // namespace maicord
