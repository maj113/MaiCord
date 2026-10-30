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
    not_encrypted,
    malformed,
    range_capacity
};
struct DaveFrameParseResult {
    u32 range_count;
    DaveFrameError error;
    constexpr bool ok() const noexcept {
        return error == DaveFrameError::none;
    }
};
// Borrow input and caller range storage; frame, ranges and output are distinct
// valid caller objects. Full validation precedes writes to either output;
// range_capacity reports the required count after validation. Keep
// metadata/ranges alive and unchanged through the cryptographic operation.
// not_encrypted is classification only, NOT permission for plaintext playback.
// Noncanonical ULEB encodings are accepted as in libdave; overflow is rejected.
DaveFrameParseResult dave_frame_parse(ConstSlice<u8> frame, Slice<DaveClearRange> ranges,
                                      DaveFrameView* output) noexcept;
// The next two take only a view that dave_frame_parse published for a live
// frame (so its ranges are ordered and inside the body) and spans sized from
// it: aad holds clear_bytes, cipher body_bytes - clear_bytes. aad/cipher stay
// disjoint from the frame unless cipher is the whole untouched body.
//
// Copy the clear ranges to aad and every other body byte to cipher, in order.
void dave_frame_gather(const DaveFrameView& view, u8* aad, u8* cipher) noexcept;
// Interleave authenticated plaintext back between the clear ranges, which are
// copied from the frame unless output is the frame itself (the original body is
// then overwritten in place from a separate plaintext span). output holds
// body_bytes; plaintext may equal output exactly only without clear ranges.
// This does not authenticate.
void dave_frame_reconstruct(const DaveFrameView& view, const u8* plaintext, u8* output) noexcept;
} // namespace maicord
