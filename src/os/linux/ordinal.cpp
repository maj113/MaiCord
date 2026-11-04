// The Linux body of ordinal_equal (include/maicord/os/ordinal.hpp).
#include "maicord/os/ordinal.hpp"

namespace maicord {
namespace {

// The simple lower-case form of a UTF-16 unit for the scripts family and file names are written in: ASCII, Latin-1,
// Latin Extended-A, Greek and Cyrillic. Every other unit is its own form (the comparison is then exact).
u32 fold(u32 unit) noexcept {
    if (unit < 0x80)
        return unit >= 'A' && unit <= 'Z' ? unit + 32 : unit;
    if (unit >= 0xC0 && unit <= 0xDE && unit != 0xD7)
        return unit + 32;
    if (unit >= 0x100 && unit <= 0x17F) {
        // Latin Extended-A alternates upper and lower case except in a few runs.
        if ((unit >= 0x139 && unit <= 0x148) || (unit >= 0x179 && unit <= 0x17E))
            return (unit & 1) ? unit + 1 : unit;
        if (unit == 0x130 || unit == 0x138 || unit == 0x149 || unit == 0x178 || unit == 0x17F)
            return unit == 0x178 ? 0xFF : unit;
        return (unit & 1) ? unit : unit + 1;
    }
    if (unit >= 0x391 && unit <= 0x3A9 && unit != 0x3A2)
        return unit + 32;
    if (unit >= 0x410 && unit <= 0x42F)
        return unit + 32;
    if (unit >= 0x400 && unit <= 0x40F)
        return unit + 80;
    return unit;
}

} // namespace

bool ordinal_equal(const wchar_t* a, int a_count, const wchar_t* b, int b_count, bool ignore_case) noexcept {
    if (!a || !b)
        return false;
    u32 a_length = 0;
    u32 b_length = 0;
    if (a_count < 0)
        while (a[a_length])
            ++a_length;
    else
        a_length = static_cast<u32>(a_count);
    if (b_count < 0)
        while (b[b_length])
            ++b_length;
    else
        b_length = static_cast<u32>(b_count);
    if (a_length != b_length)
        return false;
    for (u32 i = 0; i < a_length; ++i) {
        const u32 x = static_cast<u16>(a[i]);
        const u32 y = static_cast<u16>(b[i]);
        if (x == y)
            continue;
        if (!ignore_case || fold(x) != fold(y))
            return false;
    }
    return true;
}

} // namespace maicord
