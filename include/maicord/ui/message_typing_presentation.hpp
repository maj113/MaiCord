#pragma once

#include "maicord/model/typing_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {

inline constexpr u32 kMessageTypingLabelMaximumBytes = 512;

enum class MessageTypingPresentationError : u8 {
    none,
    invalid_input,
    capacity,
};

struct MessageTypingPresentationResult {
    u32 bytes;
    u32 users;
    MessageTypingPresentationError error;

    constexpr bool ok() const noexcept {
        return error == MessageTypingPresentationError::none;
    }
};

MessageTypingPresentationResult message_typing_presentation_build(
    const TypingStore& typing, const UserStore& users,
    u64 channel_id, u64 self_user_id, Slice<char> output) noexcept;

} // namespace maicord
