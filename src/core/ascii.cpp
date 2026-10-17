#include "maicord/core/ascii.hpp"

namespace maicord {

Utf8View ascii_trim_c_space(Utf8View text) noexcept {
    u32 start = 0, end = text.bytes;
    while (start < end && ascii_is_c_space(text.data[start]))
        ++start;
    while (end > start && ascii_is_c_space(text.data[end - 1]))
        --end;
    return {text.data + start, end - start};
}

} // namespace maicord
