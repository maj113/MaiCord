#include "maicord/protocol/auth/p256_signature.hpp"
#include "maicord/core/memory.hpp"

// Signature framing, once per signature: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace maicord {
// Both callers (mls/signature.cpp) give 64 raw bytes and kP256DerSignatureMaximum DER bytes in their own arrays.
SignatureEncodingResult p256_signature_to_der(ConstSlice<u8> raw, Slice<u8> der) noexcept {
    u8 staged[kP256DerSignatureMaximum]{0x30};
    u32 cursor = 2;
    for (u32 part = 0; part != 2; ++part) {
        const u8* value = raw.data + part * 32;
        u32 first = 0;
        while (first != 32 && !value[first])
            ++first;
        if (first == 32)
            return {0, SignatureEncodingError::malformed};
        const bool padding = (value[first] & 0x80) != 0;
        staged[cursor++] = 2;
        staged[cursor++] = static_cast<u8>(32 - first + padding);
        if (padding)
            staged[cursor++] = 0;
        copy_bytes(staged + cursor, value + first, 32 - first);
        cursor += 32 - first;
    }
    staged[1] = static_cast<u8>(cursor - 2);
    copy_bytes(der.data, staged, cursor);
    return {cursor, SignatureEncodingError::none};
}
SignatureEncodingResult p256_signature_from_der(ConstSlice<u8> der, Slice<u8> raw) noexcept {
    if (der.count < 8 || der.count > kP256DerSignatureMaximum || der.data[0] != 0x30 ||
        der.data[1] != der.count - 2)
        return {0, SignatureEncodingError::malformed};
    u8 staged[64]{};
    u32 cursor = 2;
    for (u32 part = 0; part != 2; ++part) {
        if (der.count - cursor < 2 || der.data[cursor++] != 2)
            return {0, SignatureEncodingError::malformed};
        u32 count = der.data[cursor++];
        if (!count || count > 33 || count > der.count - cursor || (der.data[cursor] & 0x80))
            return {0, SignatureEncodingError::malformed};
        if (!der.data[cursor]) {
            if (count == 1 || !(der.data[cursor + 1] & 0x80))
                return {0, SignatureEncodingError::malformed};
            ++cursor;
            --count;
        }
        if (count > 32)
            return {0, SignatureEncodingError::malformed};
        copy_bytes(staged + part * 32 + 32 - count, der.data + cursor, count);
        cursor += count;
    }
    if (cursor != der.count)
        return {0, SignatureEncodingError::malformed};
    copy_bytes(raw.data, staged, 64);
    return {64, SignatureEncodingError::none};
}
} // namespace maicord
#pragma clang attribute pop
