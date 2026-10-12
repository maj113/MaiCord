#pragma once
#include "maicord/text/font_session.hpp"
namespace maicord {
// Prepare an already loaded physical face with the given user-space settings.
// The caller keeps the session and face alive. Scratch is appended to, not
// reset, so settings may borrow its existing prefix; the new scratch is
// disposable after return. Outputs are disjoint and cleared on failure.
// simulations: bit 0 requests bold, bit 1 oblique; each requested bit needs its
// non-null output pointer. outline_instructions prepares TrueType glyph programs
// when the font has them.
NativeFontResult native_font_prepare(NativeFontSession*,NativeFontHandle,ConstSlice<FontAxisSetting>,
    u32 simulations,bool outline_instructions,Arena*,NativeFontInstanceHandle*,bool* oblique=nullptr,bool* bold=nullptr) noexcept;
// Compact borrowed selection of at most two axis settings.
// A live instance/plan must hold a reference to face; settings are copied, independent of scratch.
struct NativeFontSelection {NativeFontHandle face;FontAxisSetting settings[2];u32 count;};
}
