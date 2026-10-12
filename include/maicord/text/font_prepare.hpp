#pragma once
#include "maicord/text/font_session.hpp"
namespace maicord {
// Prepare an already loaded physical face with the given user-space settings.
// No font identity service or file open occurs here. The caller keeps the session
// and face alive; success returns one instance reference. Scratch is appended to,
// not reset, so settings may borrow its existing prefix. All new scratch is
// disposable after return. Outputs are disjoint and cleared on failure.
// Each simulation output allows that mode. Instruction/prep policy
// matches the application's current normalized-outline boundary.
NativeFontResult native_font_prepare(NativeFontSession*,NativeFontHandle,ConstSlice<FontAxisSetting>,
    u32 simulations,bool outline_instructions,Arena*,NativeFontInstanceHandle*,bool* oblique=nullptr,bool* bold=nullptr) noexcept;
// Compact borrowed selection for the current two-axis-or-less application fonts.
// A live instance/plan must hold a reference to face; settings are copied, independent of scratch.
struct NativeFontSelection {NativeFontHandle face;FontAxisSetting settings[2];u32 count;};
}
