#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u64 kMessageDeleteRouteId = 0x4d534744454c4554ull;

enum class MessageDeleteWriteError : u8 {
    none,
    invalid_input,
    output_too_small,
};

using MessageDeleteWriteResult = ByteResult<MessageDeleteWriteError>;

MessageDeleteWriteResult write_message_delete_target(
    u64 channel_id, u64 message_id, Slice<u8> output) noexcept;

} // namespace maicord
