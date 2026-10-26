#pragma once

#include "maicord/ui/geometry.hpp"

namespace maicord {

// Whether every length is a finite, nonnegative coordinate. Style structs that
// declare their lengths as leading consecutive floats pass them in place.
__declspec(noinline) inline bool ui_lengths_valid(const float* lengths, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index)
        if (!ui_coordinate_valid(lengths[index]) || lengths[index] < 0)
            return false;
    return true;
}

} // namespace maicord
