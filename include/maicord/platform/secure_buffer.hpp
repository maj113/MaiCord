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
SecureBufferResult secure_buffer_protect_user(const SecureBuffer& plaintext,
                                              Slice<u8> protected_output) noexcept;
SecureBufferResult secure_buffer_unprotect_user(ConstSlice<u8> protected_input,
                                                SecureBuffer* plaintext) noexcept;
SecureBufferResult secure_buffer_protect_user_bound(
    const SecureBuffer& plaintext, ConstSlice<u8> entropy,
    Slice<u8> protected_output) noexcept;
SecureBufferResult secure_buffer_unprotect_user_bound(
    ConstSlice<u8> protected_input, ConstSlice<u8> entropy,
    SecureBuffer* plaintext) noexcept;

} // namespace maicord
