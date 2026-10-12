#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
enum NativeScriptFlag:u8 {
    native_script_contextual=1,
    native_script_nonvisual=2,
    native_script_invalid=4,
    native_script_selector=8
};
struct NativeScriptClass {UnicodeScript script;u8 flags;};
static_assert(sizeof(NativeScriptClass)==2);
// MaiCord's en-US script-analysis compatibility class. This does not
// change the Unicode Script property. Contextual cells can inherit a surrounding
// script; their stored script is the isolated-input fallback. A non-contextual
// Zzzz cell is an unknown-script boundary, not a neutral to merge across.
// Nonvisual items are separate from shaping controls such as ZWJ/selectors.
// Returns native_script_invalid for non-scalars. No allocation or OS call.
// Paragraph context, font cuts and OpenType script-version selection are later
// stages; this classification alone must not select a shaping profile.
NativeScriptClass native_script_class(u32 scalar) noexcept;
enum class NativeScriptStatus:u8 {ok,invalid_input,output_too_small};
// Resolve one en-US analysis input in logical scalar order, including nonvisual
// boundaries and one following variation selector per analysis item. Output
// flags keep only nonvisual; source positions and bidi order are unchanged.
// Uses two bytes per input scalar and constant additional state, with no heap
// or OS call. Input/output are disjoint caller-owned arrays. A failure leaves
// output scratch unusable. Font cuts and script-version planning are separate.
NativeScriptStatus native_script_resolve(ConstSlice<u32>,Slice<NativeScriptClass>) noexcept;
}
