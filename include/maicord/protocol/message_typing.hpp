#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kMessageTypingMaximumTargetBytes = 96;
inline constexpr u32 kMessageTypingMaximumResponseBytes = 512;
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
// Reads the cooldowns of a 200 response; false when the body is not an object of that shape.
struct MessageTypingCooldowns {
    u64 message_send_ms, thread_create_ms;
};
bool decode_message_typing_cooldowns(Utf8View body, Slice<u32> structural_scratch,
                                     MessageTypingCooldowns* output) noexcept;

} // namespace maicord
