// The parts of secure_buffer.hpp that are the same on both systems, included by src/platform/crypto/secure_buffer.cpp
// (Windows) and src/platform/crypto/linux/secure_buffer.cpp. Each defines invalid_input, the result that carries its
// system's error number for a malformed argument.

namespace maicord {
namespace {

SecureBufferResult success(u32 bytes = 0) noexcept {
    return SecureBufferResult{bytes, 0, SecureBufferError::none};
}

SecureBufferResult failure(SecureBufferError error, u32 native_error = 0, u32 bytes = 0) noexcept {
    return SecureBufferResult{bytes, native_error, error};
}

SecureBufferResult invalid_input() noexcept;

} // namespace

SecureBufferResult secure_buffer_assign(SecureBuffer* buffer, ConstSlice<u8> value) noexcept {
    // Every release buffer with storage is locked: both initializers and the login coordinators' views set data and
    // locked together. An uninitialized output buffer stays refused (its callers come through continuations).
    if (!buffer->locked) {
        return invalid_input();
    }
    if (value.count > buffer->capacity) {
        return failure(SecureBufferError::output_too_small, 0, value.count);
    }
    secure_clear_bytes(buffer->data, buffer->capacity);
    copy_bytes(buffer->data, value.data, value.count);
    buffer->bytes = value.count;
    return success(value.count);
}

bool secure_buffer_transfer(SecureBuffer* destination, SecureBuffer* source) noexcept {
    if (!destination || !source || destination->data || destination->locked ||
        !source->data || !source->locked || source->capacity == 0 || source->bytes == 0) {
        return false;
    }
    *destination = *source;
    reset_value<SecureBuffer>(source);
    return true;
}

void secure_buffer_clear(SecureBuffer* buffer) noexcept {
    if (buffer && buffer->data) {
        secure_clear_bytes(buffer->data, buffer->capacity);
        buffer->bytes = 0;
    }
}

} // namespace maicord
