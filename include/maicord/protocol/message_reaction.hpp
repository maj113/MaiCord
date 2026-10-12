#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u64 kMessageReactionRouteId = 0x4d53475245414354ull;
inline constexpr u32 kMessageReactionMaximumEmojiNameBytes = 128;
inline constexpr u32 kMessageReactionMaximumTargetBytes = 512;

enum class MessageReactionType : u8 {
    normal = 0,
    burst = 1,
    vote = 2,
};

enum class MessageReactionWriteError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    output_too_small,
};

struct MessageReactionWriteResult {
    u32 bytes;
    MessageReactionWriteError error;

    constexpr bool ok() const noexcept {
        return error == MessageReactionWriteError::none;
    }
};

MessageReactionWriteResult write_message_reaction_target(
    u64 channel_id, u64 message_id, u64 emoji_id, Utf8View emoji_name,
    MessageReactionType type, bool add, Slice<u8> output) noexcept;

} // namespace maicord
