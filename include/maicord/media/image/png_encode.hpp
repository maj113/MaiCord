#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Compact zlib (RFC 1950/1951) compressor: LZ77 over the 32 KiB window with
// hash chains of four-byte prefixes. Each block covers at most 65535 input
// bytes and is written as dynamic Huffman, fixed Huffman or stored,
// whichever is smallest, so output never exceeds zlib_compress_bound.
inline constexpr u32 kZlibCompressWorkspaceBytes = (16'384u + 32'768u + 16'384u) * 4u;

u64 zlib_compress_bound(u64 input_bytes) noexcept;
// workspace holds kZlibCompressWorkspaceBytes, 4-byte aligned. Returns the
// bytes written, zero on bad arguments or short output.
u32 zlib_compress(ConstSlice<u8> input, Slice<u8> workspace, Slice<u8> output) noexcept;

// The PNG filter of one row of `bytes` bytes, `bpp` bytes for each pixel: the
// filter type with the least sum of absolute differences over None, Sub, Up,
// Average and Paeth, then its residuals, go to out (1 + bytes). `row` and
// `above` each have 32 zero bytes before them. `candidates` holds 5 * bytes.
void png_filter_row(const u8* row, const u8* above, u32 bytes, u32 bpp, u8* candidates, u8* out) noexcept;
// The PNG bytes around one zlib stream: signature, IHDR, the IDAT chunk's
// header and CRC, and IEND.
inline constexpr u32 kPngFrameBytes = 8 + 25 + 12 + 12;
// Signature and IHDR (8 bits, no interlace) at out. Returns where the IDAT
// chunk starts. Its zlib stream goes 8 bytes after that.
u8* png_write_header(u8* out, u32 width, u32 height, u8 color_type) noexcept;
// Closes the IDAT chunk at idat around the zlib stream of zlib_bytes already
// in place, then writes IEND. Returns the end of the PNG.
u8* png_write_end(u8* idat, u32 zlib_bytes) noexcept;
// Signature, IHDR (colour type 2 or 6), one IDAT that compresses the
// `filtered` rows, and IEND. workspace as zlib_compress. Returns the PNG's
// size, zero on failure.
u32 png_encode_filtered(ConstSlice<u8> filtered, u32 width, u32 height, u8 color_type, Slice<u8> workspace,
                        Slice<u8> output) noexcept;

// PNG of straight RGBA8 pixels, as a canvas's toDataURL("image/png") writes
// it: colour type 2 (RGB8) when every pixel is opaque, otherwise 6 (RGBA8),
// one png_filter_row filter per row and a single IDAT.
inline constexpr u32 kPngEncodeDimensionMaximum = 16'384;

u64 png_encode_bound(u32 width, u32 height) noexcept;
u64 png_encode_workspace_bytes(u32 width, u32 height) noexcept;
// rgba holds width * height * 4 bytes; workspace png_encode_workspace_bytes
// (4-byte aligned). Returns the PNG's size, zero on failure.
u32 png_encode_rgba8(const u8* rgba, u32 width, u32 height, Slice<u8> workspace, Slice<u8> output) noexcept;

} // namespace maicord
