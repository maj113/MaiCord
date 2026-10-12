#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Compact zlib (RFC 1950/1951) compressor: LZ77 over the 32 KiB window with
// hash chains and one-step lazy matching. Each block covers at most 65535
// input bytes and is written as dynamic Huffman, fixed Huffman or stored,
// whichever is smallest, so output never exceeds zlib_compress_bound.
inline constexpr u32 kZlibCompressWorkspaceBytes = (65'536u + 32'768u + 65'536u) * 4u;

u64 zlib_compress_bound(u64 input_bytes) noexcept;
// workspace holds kZlibCompressWorkspaceBytes, 4-byte aligned. Returns the
// bytes written, zero on bad arguments or short output.
u32 zlib_compress(ConstSlice<u8> input, Slice<u8> workspace, Slice<u8> output) noexcept;

// PNG of straight RGBA8 pixels, as a canvas's toDataURL("image/png") writes
// it: colour type 2 (RGB8) when every pixel is opaque, otherwise 6 (RGBA8),
// one adaptive filter per row (least sum of absolute differences over None,
// Sub, Up, Average and Paeth) and a single IDAT.
inline constexpr u32 kPngEncodeDimensionMaximum = 16'384;

u64 png_encode_bound(u32 width, u32 height) noexcept;
u64 png_encode_workspace_bytes(u32 width, u32 height) noexcept;
// rgba holds width * height * 4 bytes; workspace png_encode_workspace_bytes
// (4-byte aligned). Returns the PNG's size, zero on failure.
u32 png_encode_rgba8(const u8* rgba, u32 width, u32 height, Slice<u8> workspace, Slice<u8> output) noexcept;

} // namespace maicord
