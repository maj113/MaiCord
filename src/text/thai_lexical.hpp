#pragma once
#include "maicord/core/utf.hpp"
#include "thai_word_selection.hpp"

namespace maicord {
// A bounded Thai/space scalar span, already selected by the caller's script
// policy. Returns signed lexical lengths, not complete word-break decisions.
NativeThaiWordCandidates native_thai_lexical_candidates(ConstSlice<u32> text,u32 first) noexcept;
}
