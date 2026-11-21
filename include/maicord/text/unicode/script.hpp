#pragma once
#include "maicord/text/unicode/generated/unicode_data.hpp"
#include "maicord/text/layout/visibility.hpp"
namespace maicord {
enum NativeScriptFlag:u8 {
    native_script_contextual=1,
    native_script_nonvisual=2,
    native_script_invalid=4,
    native_script_selector=8
};
struct NativeScriptClass {UnicodeScript script;u8 flags;};
static_assert(sizeof(NativeScriptClass)==2);
// Windows-compatible en-US script-analysis class; it does not change the Unicode
// Script property. Contextual cells can inherit a surrounding script; their
// stored script is the isolated-input fallback. A non-contextual Zzzz cell is an
// unknown-script boundary, not a neutral to merge across. Nonvisual items are
// separate from shaping controls such as ZWJ/selectors. Returns
// native_script_invalid for non-scalars. Paragraph context, font cuts and
// OpenType script-version selection come later; this class alone must not
// select a shaping profile.
NativeScriptClass native_script_class(u32 scalar) noexcept;
// native_script_class of a scalar from its property record, for a caller that
// already looked the record up.
inline NativeScriptClass native_script_class(u32 scalar,const UnicodeProperties& properties) noexcept {
    if(native_text_nonvisual(scalar))return {UnicodeScript::zyyy,native_script_nonvisual};
    const auto script=properties.script();
    const u8 selector=properties.has(UnicodeFlag::variation_selector)?native_script_selector:0;
    if(script==UnicodeScript::zyyy||script==UnicodeScript::zinh)return {UnicodeScript::zzzz,static_cast<u8>(native_script_contextual|selector)};
    return {script,static_cast<u8>(selector?native_script_contextual|selector:0)};
}
// Resolves one en-US analysis input in place, in logical scalar order: each
// cell holds the native_script_class of a scalar (none invalid). Handles
// nonvisual boundaries and one following variation selector per analysis
// item. Resolved flags keep only nonvisual; source positions and bidi order
// are unchanged.
void native_script_resolve(Slice<NativeScriptClass> classes) noexcept;
}
