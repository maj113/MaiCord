#pragma once
#include "maicord/text/unicode.hpp"

namespace maicord {
// Default extended grapheme clusters, UAX #29 revision 47 / Unicode 17.0.0.
// Zero-initialize at start of text; retain across input chunks. A call reports
// whether a boundary precedes this scalar, then advances the state.
// Nonempty text always has an end boundary. No tailoring, shaping-cluster or
// caret policy is implied. No allocation or backward scanning is required.
struct UnicodeGraphemeState {
    UnicodeGraphemeBreak previous{};
    u8 flags{},indic{};
};
static_assert(sizeof(UnicodeGraphemeState)==3);
// The scalar must be a valid Unicode scalar value, as a UTF decoder yields it.
bool unicode_grapheme_boundary(UnicodeGraphemeState& state,u32 scalar) noexcept;
} // namespace maicord
