#pragma once

#include "maicord/media/vp8.hpp"

namespace maicord {

// CPU decode of still lossy WebPs beyond the 512 px GPU path. VP8 keyframes
// are parsed, reconstructed and loop-filtered one macroblock row at a time, so
// memory is a few rows wide whatever the image height. Lossless (VP8L) and
// animated images are reported unsupported; uncompressed ALPH planes are
// applied, and a losslessly compressed one is left to the caller (alpha_chunk).
inline constexpr u32 kWebpThumbnailEncodedMaximumBytes = 16u * 1024u * 1024u;

enum class WebpThumbnailError : u8 {
    none,
    invalid_input,
    container,
    unsupported_lossless,
    unsupported_animation,
    unsupported_alpha,
    vp8_header,
    vp8_control,
    vp8_syntax,
    workspace,
    sink,
};

struct WebpThumbnailLayout {
    ConstSlice<u8> vp8;   // the VP8 chunk payload
    ConstSlice<u8> alpha; // raw alpha samples (width * height), or empty
    // A compressed ALPH payload (with its header byte): decode it with
    // webp_alpha_decode and set alpha (unfiltered, alpha_filter 0) first.
    ConstSlice<u8> alpha_chunk;
    u32 width;
    u32 height;
    u32 workspace_bytes; // caller-owned scratch for webp_thumbnail_decode
    u8 alpha_filter;     // WebP ALPH filtering method 0-3
    u8 reserved[3];
};

WebpThumbnailError webp_thumbnail_parse(ConstSlice<u8> encoded, WebpThumbnailLayout* layout) noexcept;

// One straight RGBA8 row of width pixels, top to bottom.
using WebpThumbnailRowSink = bool (*)(void* context, u32 y, const u8* rgba) noexcept;
// workspace holds at least layout.workspace_bytes, 16-byte aligned.
WebpThumbnailError webp_thumbnail_decode(const WebpThumbnailLayout& layout, Slice<u8> workspace,
                                         WebpThumbnailRowSink sink, void* context) noexcept;

} // namespace maicord
