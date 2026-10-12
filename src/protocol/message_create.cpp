#include "maicord/protocol/message_create.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

MessageCreateWriteResult error(MessageCreateWriteError value) noexcept {
    return MessageCreateWriteResult{0, value};
}

} // namespace

MessageCreateWriteResult write_message_create_target(u64 channel_id,
                                                     Slice<u8> output) noexcept {
    if (channel_id == 0 || !output.data) {
        return error(MessageCreateWriteError::invalid_input);
    }
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, channel_id) ||
        !protocol_writer_append_literal(&writer, "/messages")) {
        return error(MessageCreateWriteError::output_too_small);
    }
    return MessageCreateWriteResult{writer.used, MessageCreateWriteError::none};
}

MessageCreateWriteResult write_message_create_body(Utf8View content, u64 nonce,
                                                   Slice<u8> output) noexcept {
    return write_message_create_body(content, nonce, 0, 0, output);
}

MessageCreateWriteResult write_message_create_body(Utf8View content, u64 nonce,
                                                   u64 channel_id, u64 reply_message_id,
                                                   Slice<u8> output, bool mention_reply_author) noexcept {
    return write_message_create_body(content, nonce, channel_id, reply_message_id, {}, output,
        mention_reply_author);
}

MessageCreateWriteResult write_message_create_body(Utf8View content, u64 nonce,
    u64 channel_id, u64 reply_message_id, ConstSlice<UploadedAttachment> attachments,
    Slice<u8> output, bool mention_reply_author) noexcept {
    return write_message_create_body(content, nonce, channel_id, reply_message_id, attachments, {}, output,
        mention_reply_author);
}

MessageCreateWriteResult write_message_create_body(Utf8View content, u64 nonce,
    u64 channel_id, u64 reply_message_id, ConstSlice<UploadedAttachment> attachments,
    ConstSlice<u64> sticker_ids, Slice<u8> output, bool mention_reply_author, u32 flags) noexcept {
    if (!output.data || (!content.data && content.bytes) || (!content.bytes && !attachments.count && !sticker_ids.count) ||
        (flags & ~(kMessageFlagSuppressNotifications | kMessageFlagVoiceMessage)) != 0 ||
        content.bytes > kMessageCreateMaximumContentBytes || nonce == 0 ||
        ((channel_id == 0) != (reply_message_id == 0)) ||
        attachments.count > kAttachmentUploadMaximumCount || (!attachments.data && attachments.count) ||
        sticker_ids.count > kMessageCreateMaximumStickerCount || (!sticker_ids.data && sticker_ids.count)) {
        return error(MessageCreateWriteError::invalid_input);
    }
    for (u32 i = 0; i < sticker_ids.count; ++i) {
        if (!sticker_ids[i]) return error(MessageCreateWriteError::invalid_input);
        for (u32 j = 0; j < i; ++j)
            if (sticker_ids[i] == sticker_ids[j]) return error(MessageCreateWriteError::invalid_input);
    }
    // A voice message is one voice attachment and nothing else; a waveform
    // never rides on an ordinary send.
    const bool voice = (flags & kMessageFlagVoiceMessage) != 0;
    if (voice && (content.bytes || attachments.count != 1 || sticker_ids.count))
        return error(MessageCreateWriteError::invalid_input);
    for (u32 i = 0; i < attachments.count; ++i) {
        if (!attachment_upload_valid_reference(attachments[i]) || attachment_upload_is_voice(attachments[i]) != voice)
            return error(MessageCreateWriteError::invalid_input);
        for (u32 j = 0; j < i; ++j)
            if (attachments[i].id == attachments[j].id) return error(MessageCreateWriteError::invalid_input);
    }
    if (content.bytes && !utf8_validate(content).valid) {
        return error(MessageCreateWriteError::invalid_utf8);
    }
    if (!content.bytes) content = utf8_literal("");
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "{\"content\":") ||
        !protocol_writer_append_json_string(&writer, content) ||
        !protocol_writer_append_literal(&writer, ",\"nonce\":\"") ||
        !protocol_writer_append_u64(&writer, nonce)) {
        return error(MessageCreateWriteError::output_too_small);
    }
    if (reply_message_id != 0 &&
        (!protocol_writer_append_literal(&writer,
                                         "\",\"message_reference\":{\"type\":0,\"message_id\":\"") ||
         !protocol_writer_append_u64(&writer, reply_message_id) ||
         !protocol_writer_append_literal(&writer, "\",\"channel_id\":\"") ||
         !protocol_writer_append_u64(&writer, channel_id) ||
         !protocol_writer_append_literal(&writer,
                                         "\",\"fail_if_not_exists\":false},\"allowed_mentions\":{\"parse\":"
                                         "[\"users\",\"roles\",\"everyone\"],\"replied_user\":") ||
         !protocol_writer_append_c_string(&writer, mention_reply_author ? "true}" : "false}"))) {
        return error(MessageCreateWriteError::output_too_small);
    }
    if (!(reply_message_id != 0
              ? protocol_writer_append_literal(&writer, ",\"tts\":false,\"flags\":")
              : protocol_writer_append_literal(&writer, "\",\"tts\":false,\"flags\":")) ||
        !protocol_writer_append_u64(&writer, flags)) {
        return error(MessageCreateWriteError::output_too_small);
    }
    if (attachments.count) {
        if (!protocol_writer_append_literal(&writer, ",\"attachments\":"))
            return error(MessageCreateWriteError::output_too_small);
        const u32 bytes = attachment_upload_write_references(attachments,
            {output.data + writer.used, output.count - writer.used});
        if (!bytes) return error(MessageCreateWriteError::output_too_small);
        writer.used += bytes;
    }
    if (sticker_ids.count) {
        if (!protocol_writer_append_literal(&writer, ",\"sticker_ids\":["))
            return error(MessageCreateWriteError::output_too_small);
        for (u32 i = 0; i < sticker_ids.count; ++i) {
            if ((i && !protocol_writer_append_literal(&writer, ",")) ||
                !protocol_writer_append_literal(&writer, "\"") ||
                !protocol_writer_append_u64(&writer, sticker_ids[i]) ||
                !protocol_writer_append_literal(&writer, "\""))
                return error(MessageCreateWriteError::output_too_small);
        }
        if (!protocol_writer_append_literal(&writer, "]"))
            return error(MessageCreateWriteError::output_too_small);
    }
    // Desktop has no mobile-network classification to supply.
    if (!protocol_writer_append_literal(&writer, ",\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}")) return error(MessageCreateWriteError::output_too_small);
    return MessageCreateWriteResult{writer.used, MessageCreateWriteError::none};
}

bool message_silent_prefix(Utf8View content, Utf8View* remainder) noexcept {
    constexpr Utf8View prefix = utf8_literal("@silent");
    const auto space = [](char value) noexcept {
        return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
               value == '\v';
    };
    if (!content.data || content.bytes < prefix.bytes ||
        !bytes_equal(content.data, prefix.data, prefix.bytes) ||
        (content.bytes > prefix.bytes && !space(content.data[prefix.bytes])))
        return false;
    u32 first = prefix.bytes, last = content.bytes;
    while (first < last && space(content.data[first])) ++first;
    while (last > first && space(content.data[last - 1])) --last;
    if (remainder) *remainder = {content.data + first, last - first};
    return true;
}

namespace {
bool poll_text_valid(Utf8View text, u32 maximum_codepoints) noexcept {
    if (!text.data || !text.bytes || text.bytes > maximum_codepoints * 4)
        return false;
    const auto validation = utf8_validate(text);
    return validation.valid && validation.codepoints <= maximum_codepoints;
}
} // namespace

bool poll_create_valid(const PollCreate& poll) noexcept {
    if (!poll_text_valid(poll.question, kPollQuestionMaximumCodepoints) ||
        poll.answer_count < kPollCreateMinimumAnswers || poll.answer_count > kPollCreateMaximumAnswers)
        return false;
    bool duration = false;
    for (const u16 hours : kPollDurations)
        duration = duration || hours == poll.duration_hours;
    if (!duration)
        return false;
    for (u32 i = 0; i < poll.answer_count; ++i) {
        const auto& answer = poll.answers[i];
        if (!poll_text_valid(answer.text, kPollAnswerMaximumCodepoints) ||
            (!answer.emoji_id && answer.emoji_name.bytes &&
             (!answer.emoji_name.data || answer.emoji_name.bytes > kPollEmojiMaximumBytes ||
              !utf8_validate(answer.emoji_name).valid)))
            return false;
    }
    return true;
}

MessageCreateWriteResult write_message_poll_body(const PollCreate& poll, u64 nonce,
                                                 Slice<u8> output) noexcept {
    if (!output.data || !nonce || !poll_create_valid(poll))
        return error(MessageCreateWriteError::invalid_input);
    ProtocolWriter writer{output, 0};
    bool ok = protocol_writer_append_literal(&writer, "{\"content\":\"\",\"nonce\":\"") &&
              protocol_writer_append_u64(&writer, nonce) &&
              protocol_writer_append_literal(&writer,
                                             "\",\"tts\":false,\"flags\":0,\"poll\":{\"question\":{\"text\":") &&
              protocol_writer_append_json_string(&writer, poll.question) &&
              protocol_writer_append_literal(&writer, "},\"answers\":[");
    for (u32 i = 0; ok && i < poll.answer_count; ++i) {
        const auto& answer = poll.answers[i];
        ok = (!i || protocol_writer_append_literal(&writer, ",")) &&
             protocol_writer_append_literal(&writer, "{\"poll_media\":{\"text\":") &&
             protocol_writer_append_json_string(&writer, answer.text);
        // Custom emoji carry an empty name, as the client sends them.
        if (ok && answer.emoji_id)
            ok = protocol_writer_append_literal(&writer, ",\"emoji\":{\"id\":\"") &&
                 protocol_writer_append_u64(&writer, answer.emoji_id) &&
                 protocol_writer_append_literal(&writer, "\",\"name\":\"\"}");
        else if (ok && answer.emoji_name.bytes)
            ok = protocol_writer_append_literal(&writer, ",\"emoji\":{\"name\":") &&
                 protocol_writer_append_json_string(&writer, answer.emoji_name) &&
                 protocol_writer_append_literal(&writer, "}");
        ok = ok && protocol_writer_append_literal(&writer, "}}");
    }
    ok = ok && protocol_writer_append_literal(&writer, "],\"allow_multiselect\":") &&
         protocol_writer_append_c_string(&writer, poll.allow_multiselect ? "true" : "false") &&
         protocol_writer_append_literal(&writer, ",\"duration\":") &&
         protocol_writer_append_u64(&writer, poll.duration_hours) &&
         protocol_writer_append_literal(
             &writer, ",\"layout_type\":1},\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}");
    return ok ? MessageCreateWriteResult{writer.used, MessageCreateWriteError::none}
              : error(MessageCreateWriteError::output_too_small);
}

MessageCreateWriteResult write_message_forward_body(Utf8View caption, u64 nonce,
    MessageForwardReference source, Slice<u8> output) noexcept {
    if (!output.data || (!caption.data && caption.bytes) ||
        caption.bytes > kMessageCreateMaximumContentBytes || !nonce ||
        !message_forward_reference_valid(source))
        return error(MessageCreateWriteError::invalid_input);
    if (caption.bytes && !utf8_validate(caption).valid)
        return error(MessageCreateWriteError::invalid_utf8);
    if (!caption.bytes) caption = utf8_literal("");
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "{\"content\":") ||
        !protocol_writer_append_json_string(&writer, caption) ||
        !protocol_writer_append_literal(&writer, ",\"nonce\":\"") ||
        !protocol_writer_append_u64(&writer, nonce) ||
        !protocol_writer_append_literal(&writer, "\",\"message_reference\":{\"type\":1,\"message_id\":\"") ||
        !protocol_writer_append_u64(&writer, source.message_id) ||
        !protocol_writer_append_literal(&writer, "\",\"channel_id\":\"") ||
        !protocol_writer_append_u64(&writer, source.channel_id) ||
        !protocol_writer_append_literal(&writer, "\""))
        return error(MessageCreateWriteError::output_too_small);
    if (source.guild_id && (!protocol_writer_append_literal(&writer, ",\"guild_id\":\"") ||
        !protocol_writer_append_u64(&writer, source.guild_id) ||
        !protocol_writer_append_literal(&writer, "\"")))
        return error(MessageCreateWriteError::output_too_small);
    // A missing source must fail, and a caption must never create implicit pings.
    if (!protocol_writer_append_literal(&writer,
        ",\"fail_if_not_exists\":true},\"allowed_mentions\":{\"parse\":[],\"replied_user\":false},"
        "\"tts\":false,\"flags\":0,\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}"))
        return error(MessageCreateWriteError::output_too_small);
    return {writer.used, MessageCreateWriteError::none};
}

} // namespace maicord
