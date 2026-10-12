#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kMessageTypingMaximumTargetBytes = 96;
inline constexpr u64 kMessageTypingRouteId = 0x4D5347545950494Eull;

enum class MessageTypingWriteError : u8 {
    none,
    invalid_input,
    capacity,
};

struct MessageTypingWriteResult {
    u32 bytes;
    MessageTypingWriteError error;

    constexpr bool ok() const noexcept {
        return error == MessageTypingWriteError::none;
    }
};

MessageTypingWriteResult write_message_typing_target(
    u64 channel_id, Slice<u8> output) noexcept;

} // namespace maicord
