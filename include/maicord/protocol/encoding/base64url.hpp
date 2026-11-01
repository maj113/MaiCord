#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class Base64UrlError : u8 {
    none,
    invalid_input,
    invalid_length,
    invalid_character,
    invalid_padding,
    non_canonical,
    output_too_small,
};

struct Base64UrlResult {
    u32 bytes;
    u32 error_offset;
    Base64UrlError error;

    constexpr bool ok() const noexcept {
        return error == Base64UrlError::none;
    }
};

// RFC 4648 section 5, URL alphabet. Decode accepts canonical padded or
// unpadded input; encode always emits the WebAuthn/JOSE unpadded form. Both
// operations validate and size the complete transaction before touching the
// destination. A null, zero-sized destination is therefore a size query.
Base64UrlResult base64url_decode(const Utf8View& input,
                                 const Slice<u8>& output) noexcept;
Base64UrlResult base64url_encode(const ConstSlice<u8>& input,
                                 const Slice<char>& output) noexcept;

} // namespace maicord
