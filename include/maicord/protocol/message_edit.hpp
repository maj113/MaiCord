#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u64 kMessageEditRouteId = 0x4d53474544495431ull;
inline constexpr u32 kMessageEditMaximumContentBytes = 16 * 1024;
inline constexpr u32 kMessageEditMaximumBodyBytes = 64 * 1024;

enum class MessageEditWriteError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    output_too_small,
};

struct MessageEditWriteResult {
    u32 bytes;
    MessageEditWriteError error;

    constexpr bool ok() const noexcept {
        return error == MessageEditWriteError::none;
    }
};

MessageEditWriteResult write_message_edit_target(
    u64 channel_id, u64 message_id, Slice<u8> output) noexcept;
MessageEditWriteResult write_message_edit_body(
    Utf8View content, Slice<u8> output) noexcept;

} // namespace maicord
