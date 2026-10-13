#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/base.hpp"

namespace maicord {
struct NativeThaiWordCandidates { i32 lengths[8]{}; };
// A bounded Thai/space scalar span, already selected by the caller's script
// policy. Returns the lengths of up to eight dictionary words starting at
// first, longest first; candidates, not complete word-break decisions.
NativeThaiWordCandidates native_thai_lexical_candidates(ConstSlice<u32> text,u32 first) noexcept;
}
