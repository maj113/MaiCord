#pragma once
// The engine's only entry to the host's core vocabulary: integer types,
// slices, memory, byte-order and byte-range helpers, UTF-8 decoding, arenas and
// the Win32 types and records named in engine declarations.
#include "maicord/core/base.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
// A slice whose data pointer is null must be empty.
template<class T> constexpr bool filled(Slice<T> values) noexcept {return !values.count||values.data;}
template<class T> constexpr bool filled(ConstSlice<T> values) noexcept {return !values.count||values.data;}
// Charges n units against a work budget; false (budget untouched) when it can't cover them.
inline bool spend(u32* work, u32 n = 1) noexcept {
    if (n > *work) return false;
    *work -= n;
    return true;
}
inline bool spend(u64* work, u64 n = 1) noexcept {
    if (n > *work) return false;
    *work -= n;
    return true;
}
} // namespace maicord
