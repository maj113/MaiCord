#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u64 kMessageEditRouteId = 0x4d53474544495431ull;
inline constexpr u32 kMessageEditMaximumBodyBytes = 64 * 1024;

enum class MessageEditWriteError : u8 {
    none,
    output_too_small,
};

using MessageEditWriteResult = ByteResult<MessageEditWriteError>;

// The PATCH target is the message route that write_message_delete_target writes. Content is valid UTF-8 of at most
// kMessageComposerMaximumBytes (the composer or queue_message_edit_content checked it).
MessageEditWriteResult write_message_edit_body(
    Utf8View content, Slice<u8> output) noexcept;

} // namespace maicord
