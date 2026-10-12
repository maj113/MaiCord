#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
// For the app's fixed LTR paragraph base, this closed set resolves entirely
// to level zero: W1/W4-W7 remove weak types, N0-N2 resolve neutrals to L.
// AN, RTL, embedding/isolate controls and X9 removals require full analysis.
constexpr bool native_text_ltr_class(UnicodeBidiClass type) noexcept {
    using B=UnicodeBidiClass;
    constexpr u32 mask=(1u<<static_cast<u8>(B::l))|(1u<<static_cast<u8>(B::en))|
        (1u<<static_cast<u8>(B::es))|(1u<<static_cast<u8>(B::et))|(1u<<static_cast<u8>(B::cs))|
        (1u<<static_cast<u8>(B::nsm))|(1u<<static_cast<u8>(B::on))|(1u<<static_cast<u8>(B::ws))|
        (1u<<static_cast<u8>(B::b))|(1u<<static_cast<u8>(B::s));
    const auto index=static_cast<u8>(type);return index<32&&(mask&(1u<<index));
}
}
