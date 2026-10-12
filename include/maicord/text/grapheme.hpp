#pragma once
#include "maicord/text/unicode.hpp"

namespace maicord {
// Default extended grapheme clusters, UAX #29 revision 47 / Unicode 17.0.0.
// Zero-initialize at start of text; retain across input chunks. A successful
// call reports whether a boundary precedes this scalar, then advances state.
// Nonempty text always has an end boundary. No tailoring, shaping-cluster or
// caret policy is implied. No allocation or backward scanning is required.
struct UnicodeGraphemeState {
    UnicodeGraphemeBreak previous{};
    u8 flags{},indic{};
};
static_assert(sizeof(UnicodeGraphemeState)==3);
// Rejects invalid Unicode scalars and null arguments. Invalid scalar input
// leaves state unchanged; the UTF decoder owns replacement/error policy.
bool unicode_grapheme_next(UnicodeGraphemeState* state,u32 scalar,bool* boundary) noexcept;
} // namespace maicord
