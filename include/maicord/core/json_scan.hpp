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
JsonScanResult json_scan_structurals_avx1(const Utf8View& input, const Slice<u32>& output) noexcept;

// Registers `index`, the exact structural index of `input`, for the calling thread: until it is cleared, a
// json_scan_structurals_avx1 of a whole object or array of at least 256 bytes inside `input` returns that
// container's entries rebased to its start instead of scanning it (the same result, several times cheaper).
// The registration ends by itself when a scan's output would overwrite `index`; the caller clears it before
// `input` or `index` change. Other threads are unaffected.
void json_scan_source_set(const Utf8View& input, const ConstSlice<u32>& index) noexcept;
void json_scan_source_clear() noexcept;

} // namespace maicord
