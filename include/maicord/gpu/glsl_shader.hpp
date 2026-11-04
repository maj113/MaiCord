#pragma once

// The shader payload of the OpenGL ES backend, as .mcfx interface 4 carries it (section GLSL, mcfx.hpp) and as
// gpu_vertex_shader_create_blob / gpu_pixel_shader_create_blob take it: one shader per entry, little-endian,
//   u32 entry_bytes    the whole entry, this header included
//   u8  stage          1 pixel, 2 vertex
//   u8  binding_count  rows of the binding manifest below
//   u16 0
//   u32 raw_bytes      the GLSL ES 3.10 text
//   u32 packed_bytes   its zlib stream
//   binding_count x { u8 kind, u8 slot, u8 binding, u8 sampler }   (GlesBindingKind, tools/build_gles_shader_pack.py)
//   packed_bytes of zlib stream
// The translation conventions are those of the built-in pack (design: Y-flipped vertex output, clip-space fix-up, UBO
// bindings 0..7 vertex and 8..15 pixel, texture units, dual-source outputs declared with index 1). Parsing here only
// checks the container; the backend checks the text before the driver sees it.

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kGlslEntryHeaderBytes = 16;
inline constexpr u8 kGlslStagePixel = 1, kGlslStageVertex = 2;
inline constexpr u32 kGlslPackedMaximumBytes = 64u * 1024u;
inline constexpr u32 kGlslRawMaximumBytes = 256u * 1024u;
inline constexpr u32 kGlslBindingMaximum = 64;

struct GlslEntry {
    u8 stage;
    u8 binding_count;
    u32 raw_bytes, packed_bytes;
    const u8* bindings; // binding_count rows of 4 bytes
    const u8* packed;
};

inline u32 glsl_u32(const u8* bytes) noexcept {
    return bytes[0] | u32{bytes[1]} << 8 | u32{bytes[2]} << 16 | u32{bytes[3]} << 24;
}

// True when `entry` is exactly one well-formed entry.
inline bool glsl_entry_parse(ConstSlice<u8> entry, GlslEntry* out) noexcept {
    if (entry.count < kGlslEntryHeaderBytes || glsl_u32(entry.data) != entry.count)
        return false;
    const u8 stage = entry.data[4], binding_count = entry.data[5];
    const u32 raw = glsl_u32(entry.data + 8), packed = glsl_u32(entry.data + 12);
    if ((stage != kGlslStagePixel && stage != kGlslStageVertex) || entry.data[6] || entry.data[7] ||
        binding_count > kGlslBindingMaximum || raw == 0 || raw > kGlslRawMaximumBytes || packed == 0 ||
        packed > kGlslPackedMaximumBytes ||
        entry.count != kGlslEntryHeaderBytes + u32{binding_count} * 4 + packed)
        return false;
    *out = {stage, binding_count, raw, packed, entry.data + kGlslEntryHeaderBytes,
            entry.data + kGlslEntryHeaderBytes + u32{binding_count} * 4};
    return true;
}

} // namespace maicord
