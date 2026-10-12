#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/protocol/attachment_upload.hpp"

namespace maicord {

inline constexpr u64 kMessageCreateRouteId = 0x4d53474352454154ull;
inline constexpr u32 kMessageCreateMaximumContentBytes = 16 * 1024;
inline constexpr u32 kMessageCreateMaximumBodyBytes = 64 * 1024;
inline constexpr u32 kMessageCreateMaximumStickerCount = 3;

// These IDs always identify the originating message, never the destination.
// A zero guild_id denotes a private-channel source.
struct MessageForwardReference {
    u64 message_id;
    u64 channel_id;
    u64 guild_id;
};

inline constexpr bool message_forward_reference_valid(MessageForwardReference source) noexcept {
    return source.message_id != 0 && source.channel_id != 0;
}

enum class MessageCreateWriteError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    output_too_small,
};

struct MessageCreateWriteResult {
    u32 bytes;
    MessageCreateWriteError error;

    constexpr bool ok() const noexcept {
        return error == MessageCreateWriteError::none;
    }
};

MessageCreateWriteResult write_message_create_target(
    u64 channel_id, Slice<u8> output) noexcept;
MessageCreateWriteResult write_message_create_body(
    Utf8View content, u64 nonce, Slice<u8> output) noexcept;
MessageCreateWriteResult write_message_create_body(
    Utf8View content, u64 nonce, u64 channel_id, u64 reply_message_id,
    Slice<u8> output, bool mention_reply_author = true) noexcept;

MessageCreateWriteResult write_message_create_body(
    Utf8View content, u64 nonce, u64 channel_id, u64 reply_message_id,
    ConstSlice<UploadedAttachment> attachments, Slice<u8> output,
    bool mention_reply_author = true) noexcept;
MessageCreateWriteResult write_message_create_body(
    Utf8View content, u64 nonce, u64 channel_id, u64 reply_message_id,
    ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sticker_ids,
    Slice<u8> output, bool mention_reply_author = true, u32 flags = 0) noexcept;

// Message flag SUPPRESS_NOTIFICATIONS, written by "@silent" sends.
inline constexpr u32 kMessageFlagSuppressNotifications = 1u << 12;
// IS_VOICE_MESSAGE: exactly one voice attachment (waveform and duration) and
// no content or stickers.
inline constexpr u32 kMessageFlagVoiceMessage = 1u << 13;
// The leading "@silent" prefix (^@silent(\s|$)). On a match the
// remainder after the prefix, trimmed of surrounding whitespace, is returned.
bool message_silent_prefix(Utf8View content, Utf8View* remainder) noexcept;

// Poll creation (POST /channels/{id}/messages with an empty content and a
// poll object). Text is trimmed by the caller; lengths are in code points.
inline constexpr u32 kPollCreateMinimumAnswers = 2;
inline constexpr u32 kPollCreateMaximumAnswers = 10;
inline constexpr u32 kPollQuestionMaximumCodepoints = 300;
inline constexpr u32 kPollAnswerMaximumCodepoints = 55;
inline constexpr u32 kPollEmojiMaximumBytes = 64;
// Hours offered by the client: 1 h, 4 h, 8 h, 24 h, 3 d, 1 w.
inline constexpr u16 kPollDurations[] = {1, 4, 8, 24, 72, 168};
inline constexpr u32 kPollDurationCount = sizeof(kPollDurations) / sizeof(kPollDurations[0]);
struct PollCreateAnswer {
    Utf8View text;
    // Optional: a custom emoji id, or a Unicode emoji name with id zero.
    u64 emoji_id;
    Utf8View emoji_name;
};
struct PollCreate {
    Utf8View question;
    PollCreateAnswer answers[kPollCreateMaximumAnswers];
    u32 answer_count;
    u16 duration_hours;
    bool allow_multiselect;
};
bool poll_create_valid(const PollCreate&) noexcept;
MessageCreateWriteResult write_message_poll_body(const PollCreate& poll, u64 nonce,
                                                 Slice<u8> output) noexcept;

// The server constructs the immutable snapshot. No client-provided snapshot,
// uploaded attachments, or reply authority is accepted by this writer.
MessageCreateWriteResult write_message_forward_body(
    Utf8View caption, u64 nonce, MessageForwardReference source,
    Slice<u8> output) noexcept;

} // namespace maicord
