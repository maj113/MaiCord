#pragma once
#include "maicord/text/outline.hpp"
#include "maicord/text/font_run_handle.hpp"
namespace maicord {
struct NativeGlyphOutlines;
struct NativeFontSession;
struct ColrPaintProgram;
struct ColrColor;
// The caller retains the run; the outline owner retains its font session and prepared instances.
bool native_glyph_outline(NativeGlyphOutlines** owner,
    u16 face_id,u16 glyph,GlyphOutline* output,NativeFontSession* fonts,NativeFontRunHandle run) noexcept;
void native_glyph_outlines_release(NativeGlyphOutlines** owner) noexcept;
bool native_color_glyph_build(NativeGlyphOutlines** owner,
    u16 face_id,u16 glyph,const ColrColor& foreground,float pixels_per_em,
    ColrPaintProgram* program,bool* uses_foreground,NativeFontSession* fonts,NativeFontRunHandle run) noexcept;
} // namespace maicord
