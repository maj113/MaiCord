#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class SecureBufferError : u8 {
    none,
    invalid_input,
    allocation_failed,
    lock_failed,
    output_too_small,
    protect_failed,
    unprotect_failed,
};

struct SecureBufferResult {
    u32 bytes;
    u32 native_error;
    SecureBufferError error;

    constexpr bool ok() const noexcept {
        return error == SecureBufferError::none;
    }
};

struct SecureBuffer {
    u8* data;
    u32 capacity;
    u32 bytes;
    bool locked;
};

SecureBufferResult secure_buffer_initialize(SecureBuffer* buffer, u32 capacity) noexcept;
SecureBufferResult secure_buffer_assign(SecureBuffer* buffer, ConstSlice<u8> value) noexcept;
bool secure_buffer_transfer(SecureBuffer* destination, SecureBuffer* source) noexcept;
void secure_buffer_clear(SecureBuffer* buffer) noexcept;
void secure_buffer_release(SecureBuffer* buffer) noexcept;
ConstSlice<u8> secure_buffer_view(const SecureBuffer& buffer) noexcept;

// DPAPI output is copied into caller storage, then the OS allocation is
// securely erased and released. Protect reports the required encrypted size
// without partially writing when output is too small.
SecureBufferResult secure_buffer_protect_user_bound(
    const SecureBuffer& plaintext, ConstSlice<u8> entropy,
    Slice<u8> protected_output) noexcept;
// Protects into a new zero-initialized VirtualAlloc block with header_bytes of
// caller-owned space in front, using one DPAPI call (two only when the output
// guess is short). On success *block holds header_bytes + result.bytes used
// bytes within *block_bytes allocated; the caller securely clears and frees
// it. Fails with output_too_small when the result would exceed maximum_bytes.
SecureBufferResult secure_buffer_protect_user_bound_block(
    const SecureBuffer& plaintext, ConstSlice<u8> entropy, u32 header_bytes,
    u32 maximum_bytes, u8** block, u32* block_bytes) noexcept;
SecureBufferResult secure_buffer_unprotect_user_bound(
    ConstSlice<u8> protected_input, ConstSlice<u8> entropy,
    SecureBuffer* plaintext) noexcept;

} // namespace maicord
