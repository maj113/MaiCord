#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
// Internal primitives for bounded Thai/space/ZWSP spans. Results are relative
// to first. The signed grammar result is retained separately for differential
// QA. Recovery returns consumed length plus one, or zero when no recovery is
// available. Neither primitive decides when the word breaker should call it.
i32 native_thai_syllable_result(ConstSlice<u32> text,u32 first) noexcept;
u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first) noexcept;
// Reference byte-class predicate used when skipping an unknown fragment.
bool native_thai_boundary_class(u32 scalar) noexcept;
}
