#pragma once

#include "maicord/model/chat/typing_store.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/chat/messages/typing_indicator.hpp"

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
    u32 named; // Spans written to `names`.
    MessageTypingPresentationError error;

    constexpr bool ok() const noexcept {
        return error == MessageTypingPresentationError::none;
    }
};

// names, when given, receives one bold span per name (up to three), or the one span of "Several people"
// when users > 3. Only labels of one to three names collapse when they do not fit (users <= 3).
MessageTypingPresentationResult message_typing_presentation_build(
    const TypingStore& typing, const UserStore& users,
    u64 channel_id, u64 self_user_id, Slice<char> output, MessageTypingName* names = nullptr) noexcept;

} // namespace maicord
