#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/message_reaction.hpp"

namespace maicord {

inline constexpr u64 kReactionUsersRouteId = 0x5245414354555352ull;
inline constexpr u32 kReactionUsersMaximumPage = 100;
inline constexpr u32 kReactionUsersMaximumTargetBytes = 512;
inline constexpr u32 kReactionUserMaximumNameBytes = 128;
inline constexpr u32 kReactionUserMaximumAvatarHashBytes = 34;

enum class ReactionUsersWriteError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    output_too_small,
};

struct ReactionUsersWriteResult {
    u32 bytes;
    ReactionUsersWriteError error;

    constexpr bool ok() const noexcept {
        return error == ReactionUsersWriteError::none;
    }
};

struct ReactionUser {
    u64 id;
    Utf8View username;
    Utf8View display_name;
    Utf8View avatar_hash;
    bool bot;
};

enum class ReactionUsersDecodeError : u8 {
    none,
    invalid_input,
    scan,
    json,
    invalid_user,
    user_capacity,
    text_capacity,
};

struct ReactionUsersDecodeResult {
    u32 user_count;
    u32 text_bytes;
    u32 error_offset;
    ReactionUsersDecodeError error;
    JsonScanError scan_error;
    JsonReadError read_error;

    constexpr bool ok() const noexcept {
        return error == ReactionUsersDecodeError::none;
    }
};

ReactionUsersWriteResult write_reaction_users_target(
    u64 channel_id, u64 message_id, u64 emoji_id, Utf8View emoji_name,
    MessageReactionType type, u64 after_user_id, u32 limit,
    Slice<u8> output) noexcept;

ReactionUsersDecodeResult decode_reaction_users(
    Utf8View input, Slice<u32> page_structural_scratch,
    Slice<u32> object_structural_scratch, u32 maximum_depth,
    Slice<ReactionUser> users, Slice<char> text_output) noexcept;

} // namespace maicord
