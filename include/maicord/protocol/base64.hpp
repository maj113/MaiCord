#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class Base64Error : u8 {
    none,
    invalid_input,
    invalid_length,
    invalid_character,
    invalid_padding,
    non_canonical,
    output_too_small,
};

struct Base64Result {
    u32 bytes;
    u32 error_offset;
    Base64Error error;

    constexpr bool ok() const noexcept {
        return error == Base64Error::none;
    }
};

constexpr u32 base64_encoded_size(u32 input_bytes) noexcept {
    return input_bytes > (~0u - 2u) / 4u * 3u ? 0u : ((input_bytes + 2u) / 3u) * 4u;
}

// RFC 4648 section 4. Encoding always emits canonical padding. Decoding
// requires canonical padded input, validates the complete input before
// touching output, and supports a null/zero output size query.
Base64Result base64_decode(const Utf8View& input, const Slice<u8>& output) noexcept;
Base64Result base64_encode(const ConstSlice<u8>& input, const Slice<char>& output) noexcept;

} // namespace maicord
