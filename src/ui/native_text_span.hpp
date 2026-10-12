#pragma once
#include "maicord/core/base.hpp"
namespace maicord {
inline constexpr u32 native_text_maximum_bytes=32*1024;
// Keep full source coordinates. Binding objects are eight-byte aligned on the
// supported x64 target; their three low address bits carry shaping flags.
template<class Binding> struct NativeStyledSpan {
    u16 first_byte,end_byte,first_unit,end_unit;
    u64 binding_bits;float em;u32 flags;
    Binding* binding() const noexcept {return reinterpret_cast<Binding*>(binding_bits&~7ull);}
    void set_binding(Binding* value) noexcept {static_assert(alignof(Binding)>=8);binding_bits=reinterpret_cast<u64>(value)|(binding_bits&7);}
    u16 shaping_flags() const noexcept {return static_cast<u16>(binding_bits&7);}
    void set_shaping_flags(u16 value) noexcept {binding_bits=(binding_bits&~7ull)|(value&7);}
};
static_assert(sizeof(NativeStyledSpan<void>)==24);
// Analysis uses an 8-bit script ID and three independent shaping flags. The
// transient byte array stores only the ID; flags travel with the span record.
// Expose the combined 11-bit value while the existing grouping passes run.
template<class Span> struct NativeSpanAnalysis {
    u8* scripts;Span* spans;
    struct Reference {
        u8& script;Span& span;
        operator u16() const noexcept {return static_cast<u16>((static_cast<u16>(script)<<3)|span.shaping_flags());}
        Reference& operator=(u16 value) noexcept {script=static_cast<u8>(value>>3);span.set_shaping_flags(value);return *this;}
        Reference& operator=(const Reference& value) noexcept {return *this=static_cast<u16>(value);}
        Reference& operator|=(u16 value) noexcept {return *this=static_cast<u16>(static_cast<u16>(*this)|value);}
        Reference& operator&=(u16 value) noexcept {return *this=static_cast<u16>(static_cast<u16>(*this)&value);}
    };
    Reference operator[](u32 index) const noexcept {return {scripts[index],spans[index]};}
};
}
