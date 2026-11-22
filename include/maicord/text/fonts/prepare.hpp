#pragma once
#include "maicord/text/fonts/session.hpp"
#include "maicord/core/arena.hpp"
namespace maicord {
// Prepare an already loaded physical face with the given user-space settings.
// The caller keeps the session and face alive. Scratch is appended to, not
// reset, so settings may borrow its existing prefix; the new scratch is
// disposable after return. Outputs are disjoint; the instance output is cleared on failure.
// simulations (below 4): bit 0 requests bold, bit 1 oblique; on success *oblique and *bold tell which the instance
// applies. outline_instructions prepares TrueType glyph programs when the font has them. Scratch is initialized.
NativeFontResult native_font_prepare(NativeFontSession*,NativeFontHandle,ConstSlice<FontAxisSetting>,
    u32 simulations,bool outline_instructions,Arena*,NativeFontInstanceHandle*,bool* oblique,bool* bold) noexcept;
// Compact borrowed selection of at most two axis settings.
// A live instance/plan must hold a reference to face; settings are copied, independent of scratch.
struct NativeFontSelection {NativeFontHandle face;FontAxisSetting settings[2];u32 count;};
}
