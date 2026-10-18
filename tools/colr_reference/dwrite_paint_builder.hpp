#pragma once
// Test/tool-only DirectWrite COLRv1 paint-program builder (the native COLR
// compiler's reference). Never linked into MaiCord.exe or MaiCordAccount.dll.
#include "maicord/render/colr_program.hpp"
#include "maicord/sys/d2d1.hpp"
#include "maicord/sys/dwrite.hpp"

namespace maicord {
// Windows resolves COLRv1 variations and palette entries through the paint
// reader; the program then renders through colr_paint_rasterize.
bool colr_paint_build(ColrPaintProgram* program, IDWritePaintReader* reader, IDWriteFontFace* face,
                      const DWRITE_PAINT_ELEMENT& root, D2D_RECT_F clip, float pixels_per_em) noexcept;
} // namespace maicord
