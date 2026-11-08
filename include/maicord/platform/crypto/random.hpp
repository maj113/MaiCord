#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class SecureRandomError : u8 {
    none,
    invalid_input,
    system_failure,
};

struct SecureRandomResult {
    i32 platform_status;
    SecureRandomError error;

    constexpr bool ok() const noexcept {
        return error == SecureRandomError::none;
    }
};

SecureRandomResult secure_random_fill(Slice<u8> output) noexcept;
inline SecureRandomResult secure_random_u32(u32* output) noexcept {
    return output ? secure_random_fill(Slice<u8>{reinterpret_cast<u8*>(output), sizeof(*output)})
                  : SecureRandomResult{0, SecureRandomError::invalid_input};
}

} // namespace maicord
