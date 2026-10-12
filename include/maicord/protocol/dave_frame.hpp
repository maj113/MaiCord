#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
constexpr u32 kDaveFrameTagBytes = 8;
// 255 supplemental bytes minus tag, minimum nonce, size and marker; each
// range requires at least two ULEB128 bytes. This is a wire-format bound.
constexpr u32 kDaveMaximumClearRanges = 121;
struct DaveClearRange {
    u32 offset, bytes;
};
struct DaveFrameView {
    ConstSlice<u8> frame;
    ConstSlice<DaveClearRange> clear_ranges;
    u32 body_bytes, clear_bytes, nonce;
};
enum class DaveFrameError : u8 {
    none,
    invalid_input,
    not_encrypted,
    malformed,
    range_capacity,
    output_capacity
};
struct DaveFrameParseResult {
    u32 range_count;
    DaveFrameError error;
    constexpr bool ok() const noexcept {
        return error == DaveFrameError::none;
    }
};
// Borrow input and caller range storage. Full validation precedes writes to
// either output; range_capacity reports the required count after validation.
// Keep metadata/ranges alive and unchanged through the cryptographic operation.
// not_encrypted is classification only, NOT permission for plaintext playback.
// Noncanonical ULEB encodings are accepted as in libdave; overflow is rejected.
DaveFrameParseResult dave_frame_parse(ConstSlice<u8> frame, Slice<DaveClearRange> ranges,
                                      DaveFrameView* output) noexcept;
// Materialize contiguous AAD/ciphertext only when the cryptor needs it. Tail
// capacity is untouched. For a wholly encrypted body, exact cipher/body alias
// is allowed (a no-copy path); all other frame/output aliases are rejected.
DaveFrameError dave_frame_gather(const DaveFrameView& view, Slice<u8> aad, Slice<u8> ciphertext) noexcept;
// Reassemble ONLY authenticated decrypted ciphertext bytes with clear headers.
// Can overwrite the original body in place using a separate plaintext span.
// No-clear-range frames also allow exact plaintext/output alias. Partial aliases
// are rejected and every error preserves the output. This does not authenticate.
DaveFrameError dave_frame_reconstruct(const DaveFrameView& view, ConstSlice<u8> plaintext,
                                      Slice<u8> output) noexcept;
} // namespace maicord
