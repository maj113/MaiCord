#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_kernel.hpp"

// Ordinal (code unit by code unit, no locale) comparison of UTF-16 strings, as the Windows call spells it:
// CompareStringOrdinal(a, a_count, b, b_count, ignore_case) == CSTR_EQUAL. A count of -1 means "to the terminator".
// Ignoring case follows the system's simple upper-case mapping: on Windows its table, on Linux the simple case
// folding of the Latin, Greek and Cyrillic blocks and ASCII, which is what font family names and file names need
// (src/os/linux/ordinal.cpp).

namespace maicord {

#if defined(_WIN32)
__forceinline bool ordinal_equal(const wchar_t* a, int a_count, const wchar_t* b, int b_count,
                                 bool ignore_case) noexcept {
    return CompareStringOrdinal(a, a_count, b, b_count, ignore_case ? TRUE : FALSE) == CSTR_EQUAL;
}
#else
bool ordinal_equal(const wchar_t* a, int a_count, const wchar_t* b, int b_count, bool ignore_case) noexcept;
#endif

} // namespace maicord
