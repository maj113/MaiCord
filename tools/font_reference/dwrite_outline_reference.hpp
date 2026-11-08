#pragma once
// DirectWrite glyph outlines and face binding for MaiCordFontReference (cmake/reference_tools.cmake), which compares
// the native SFNT, outline and COLR readers with Windows. Never linked into MaiCord.exe or the test suite.
#include "maicord/ui/text/text.hpp"
#include "maicord/text/fonts/prepare.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/dwrite.hpp"

namespace maicord {

enum class GlyphOutlineError : u8 {
    none,
    invalid_input,
    output_too_small,
    directwrite,
};
struct GlyphOutlineResult {
    HRESULT native_error;
    u32 command_count;
    GlyphOutlineError error;
    constexpr bool ok() const noexcept {
        return error == GlyphOutlineError::none;
    }
};
// Captures normalized (1 em) line/cubic contours from DirectWrite.
GlyphOutlineResult text_outline_glyph(IDWriteFontFace* font_face, u16 glyph_index, GlyphOutline* output) noexcept;

// Binds a DirectWrite-selected face to a native instance by its file path.
// Each optional style output permits its draw-time simulation. Optional source
// copies the actual fvar axes (up to two).
NativeFontResult native_font_bind_selected(NativeFontSession*, IDWriteFontFace*, bool outline_instructions,
                                           Arena* scratch, NativeFontInstanceHandle*, bool* oblique = nullptr,
                                           bool* bold = nullptr, NativeFontSelection* source = nullptr) noexcept;

} // namespace maicord
