#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u64 kMessageDeleteRouteId = 0x4d534744454c4554ull;

enum class MessageDeleteWriteError : u8 {
    none,
    invalid_input,
    output_too_small,
};

struct MessageDeleteWriteResult {
    u32 bytes;
    MessageDeleteWriteError error;

    constexpr bool ok() const noexcept {
        return error == MessageDeleteWriteError::none;
    }
};

MessageDeleteWriteResult write_message_delete_target(
    u64 channel_id, u64 message_id, Slice<u8> output) noexcept;

} // namespace maicord
