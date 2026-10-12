#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class JsonScanError : u8 {
    none,
    invalid_input,
    output_overflow,
    control_in_string,
    unterminated_string,
};

struct JsonScanResult {
    u32 count;
    u32 error_offset;
    JsonScanError error;

    constexpr bool ok() const noexcept {
        return error == JsonScanError::none;
    }
};

// Emits the offsets of unescaped quotes and of {}[]:, punctuation outside
// strings. The caller owns the bounded output buffer; no allocation occurs.
JsonScanResult json_scan_structurals_avx1(Utf8View input, Slice<u32> output) noexcept;

} // namespace maicord
