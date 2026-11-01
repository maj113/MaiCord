#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class AeadError : u8 {
    none,
    invalid_input,
    not_initialized,
    output_too_small,
    storage,
    platform_failure,
    authentication_failed,
};

struct AeadResult {
    u32 bytes;
    i32 native_status;
    AeadError error;

    constexpr bool ok() const noexcept {
        return error == AeadError::none;
    }
};

} // namespace maicord
