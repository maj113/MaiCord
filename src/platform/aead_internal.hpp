#pragma once

#include "maicord/platform/aead.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {

inline AeadResult aead_result(AeadError error, u32 bytes = 0, i32 status = 0) noexcept {
    return {bytes, status, error};
}

// Both algorithms share argument/alias rules and native failure erasure. The
// initialized owner must validate before deriving/importing or changing storage.
AeadResult aead_validate(ConstSlice<u8> owner, ConstSlice<u8> storage, u32 nonce_bytes,
                         ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input, Slice<u8> output,
                         ConstSlice<u8> tag, bool encrypt, u32 tag_bytes = 16) noexcept;
AeadResult aead_cng_crypt(void* handle, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                          Slice<u8> output, u8* tag, bool encrypt) noexcept;

} // namespace maicord
