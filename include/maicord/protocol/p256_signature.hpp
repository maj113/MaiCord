#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
constexpr u32 kP256DerSignatureMaximum = 72;
enum class SignatureEncodingError : u8 {
    none,
    invalid_input,
    malformed,
    output_capacity
};
struct SignatureEncodingResult {
    u32 bytes;
    SignatureEncodingError error;
    constexpr bool ok() const noexcept {
        return error == SignatureEncodingError::none;
    }
};
// Strict DER SEQUENCE of two minimally encoded positive, nonzero integers,
// each at most 256 bits. This validates encoding, not signature arithmetic.
// Raw representation is 64-byte P1363 r||s. All failures preserve output;
// success preserves excess capacity. Input/output overlap is supported.
SignatureEncodingResult p256_signature_to_der(ConstSlice<u8> raw, Slice<u8> der) noexcept;
SignatureEncodingResult p256_signature_from_der(ConstSlice<u8> der, Slice<u8> raw) noexcept;
} // namespace maicord
