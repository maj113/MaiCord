#pragma once

#include "maicord/model/chat/typing_store.hpp"
#include "maicord/model/user/store.hpp"

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

// One typed-out name inside the label: its byte span and its user id.
struct MessageTypingName {
    u64 user_id;
    u16 offset, bytes;
};
// names, when given, receives one span per name (up to three); it is filled only when
// the result's users <= 3 ("Several people are typing" names nobody).
MessageTypingPresentationResult message_typing_presentation_build(
    const TypingStore& typing, const UserStore& users,
    u64 channel_id, u64 self_user_id, Slice<char> output, MessageTypingName* names = nullptr) noexcept;

} // namespace maicord
