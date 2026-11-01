#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
// Straight (non-premultiplied) RGBA8 rows, top-down.
inline constexpr u32 kClipboardImageMaximumExtent = 8192;
// Uncompressed PNG (stored deflate blocks) for the registered "PNG" format.
// Returns the encoded size, or zero when the input or capacity is invalid.
u64 clipboard_image_png_bytes(u32 width, u32 height) noexcept;
u64 clipboard_image_encode_png(const u8* rgba, u32 width, u32 height, u32 stride, Slice<u8> output) noexcept;
// Bottom-up 32-bit BITMAPV5HEADER image with explicit alpha mask (CF_DIBV5 payload).
u64 clipboard_image_dib_bytes(u32 width, u32 height) noexcept;
u64 clipboard_image_build_dib(const u8* rgba, u32 width, u32 height, u32 stride, Slice<u8> output) noexcept;

// Pasting: format choice and conversion stay free of OLE so tests can drive
// them with in-memory payloads.
enum class ClipboardPasteKind : u8 { png, dibv5, dib, files, virtual_files };
struct ClipboardPasteFormats { bool png, dibv5, dib, files, virtual_files; };
inline constexpr u32 kClipboardPastePlanCapacity = 5;
// Image data wins over file lists: screenshot tools (Snipping Tool, browsers)
// offer a bitmap and sometimes a temporary file, and the bitmap is what was
// copied. "PNG" bytes go up unchanged; DIBs become PNG. Every later entry is
// a fallback when an earlier one cannot be read.
u32 clipboard_paste_plan(ClipboardPasteFormats formats,
                         ClipboardPasteKind (&plan)[kClipboardPastePlanCapacity]) noexcept;
// Byte length of a structurally valid PNG through IEND (trailing GlobalAlloc
// padding excluded), or zero.
u64 clipboard_image_png_length(ConstSlice<u8> png) noexcept;
// Decoded DIB extent limits for paste; larger screenshots should use "PNG".
inline constexpr u32 kClipboardPasteMaximumExtent = 16384;
inline constexpr u64 kClipboardPasteMaximumPixels = 64ull * 1024 * 1024;
struct ClipboardPng { u8* data; u64 bytes; };
// Owned PNG for a png/dibv5/dib payload (V3/V4/V5 headers, 1/4/8-bit palettes,
// 16/24/32-bit with BI_RGB or BI_BITFIELDS, either row order). Returns
// ERROR_SUCCESS, ERROR_INVALID_DATA, ERROR_FILE_TOO_LARGE or ERROR_NOT_ENOUGH_MEMORY.
DWORD clipboard_paste_png(ClipboardPasteKind kind, ConstSlice<u8> payload, u64 limit, ClipboardPng* output) noexcept;
void clipboard_png_release(ClipboardPng* png) noexcept;
} // namespace maicord
